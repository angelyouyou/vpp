/*
 *------------------------------------------------------------------
 * Copyright (c) 2016 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *------------------------------------------------------------------
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/uio.h>

#include <vlib/vlib.h>
#include <vlib/unix/unix.h>
#include <vnet/ethernet/ethernet.h>
#include <vnet/interface/rx_queue_funcs.h>
#include <vnet/feature/feature.h>

#include <memif/memif.h>
#include <memif/private.h>

#define foreach_memif_input_error                                             \
  _ (BUFFER_ALLOC_FAIL, buffer_alloc, ERROR, "buffer allocation failed")      \
  _ (BAD_DESC, bad_desc, ERROR, "bad descriptor")                             \
  _ (NOT_IP, not_ip, INFO, "not ip packet")

typedef enum
{
#define _(f, n, s, d) MEMIF_INPUT_ERROR_##f,
  foreach_memif_input_error
#undef _
    MEMIF_INPUT_N_ERROR,
} memif_input_error_t;

static vlib_error_desc_t memif_input_error_counters[] = {
#define _(f, n, s, d) { #n, d, VL_COUNTER_SEVERITY_##s },
  foreach_memif_input_error
#undef _
};

typedef struct
{
  u32 next_index;
  u32 hw_if_index;
  u16 ring;
} memif_input_trace_t;

static __clib_unused u8 *
format_memif_input_trace (u8 * s, va_list * args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  memif_input_trace_t *t = va_arg (*args, memif_input_trace_t *);
  u32 indent = format_get_indent (s);

  s = format (s, "memif: hw_if_index %d next-index %d",
	      t->hw_if_index, t->next_index);
  s = format (s, "\n%Uslot: ring %u", format_white_space, indent + 2,
	      t->ring);
  return s;
}

static_always_inline u32
memif_next_from_ip_hdr (vlib_node_runtime_t * node, vlib_buffer_t * b)
{
  u8 *ptr = vlib_buffer_get_current (b);
  u8 v = *ptr & 0xf0;

  if (PREDICT_TRUE (v == 0x40))
    return VNET_DEVICE_INPUT_NEXT_IP4_NCS_INPUT;
  else if (PREDICT_TRUE (v == 0x60))
    return VNET_DEVICE_INPUT_NEXT_IP6_INPUT;

  b->error = node->errors[MEMIF_INPUT_ERROR_NOT_IP];
  return VNET_DEVICE_INPUT_NEXT_DROP;
}

static_always_inline void
memif_trace_buffer (vlib_main_t * vm, vlib_node_runtime_t * node,
		    memif_if_t * mif, vlib_buffer_t * b, u32 next, u16 qid,
		    uword * n_tracep)
{
  if (PREDICT_TRUE
      (b != 0 && vlib_trace_buffer (vm, node, next, b, /* follow_chain */ 0)))
    {
      memif_input_trace_t *tr;
      vlib_set_trace_count (vm, node, --(*n_tracep));
      tr = vlib_add_trace (vm, node, b, sizeof (*tr));
      tr->next_index = next;
      tr->hw_if_index = mif->hw_if_index;
      tr->ring = qid;
    }
}

static_always_inline void
memif_add_copy_op (memif_per_thread_data_t * ptd, void *data, u32 len,
		   u16 buffer_offset, u16 buffer_vec_index)
{
  memif_copy_op_t *co;
  vec_add2_aligned (ptd->copy_ops, co, 1, CLIB_CACHE_LINE_BYTES);
  co->data = data;
  co->data_len = len;
  co->buffer_offset = buffer_offset;
  co->buffer_vec_index = buffer_vec_index;
}

static_always_inline void
memif_add_to_chain (vlib_main_t * vm, vlib_buffer_t * b, u32 * buffers,
		    u32 buffer_size)
{
  vlib_buffer_t *seg = b;
  i32 bytes_left = b->current_length - buffer_size + b->current_data;

  if (PREDICT_TRUE (bytes_left <= 0))
    return;

  b->current_length -= bytes_left;
  b->total_length_not_including_first_buffer = bytes_left;

  while (bytes_left)
    {
      seg->flags |= VLIB_BUFFER_NEXT_PRESENT;
      seg->next_buffer = buffers[0];
      seg = vlib_get_buffer (vm, buffers[0]);
      buffers++;
      seg->current_data = 0;
      seg->current_length = clib_min (buffer_size, bytes_left);
      bytes_left -= seg->current_length;
    }
}

static_always_inline uword
memif_device_input_inline (vlib_main_t *vm, vlib_node_runtime_t *node,
			   memif_if_t *mif, memif_ring_type_t type, u16 qid,
			   memif_interface_mode_t mode)
{
  vnet_main_t *vnm = vnet_get_main ();
  memif_main_t *mm = &memif_main;
  memif_ring_t *ring;
  memif_queue_t *mq;
  u16 buffer_size = vlib_buffer_get_default_data_size (vm);
  uword n_trace;
  u16 nexts[MEMIF_RX_VECTOR_SZ], *next = nexts;
  u32 _to_next_bufs[MEMIF_RX_VECTOR_SZ], *to_next_bufs = _to_next_bufs, *bi;
  u32 n_rx_packets = 0, n_rx_bytes = 0;
  u32 n_left, n_left_to_next;
  u32 next_index = VNET_DEVICE_INPUT_NEXT_ETHERNET_INPUT;
  vlib_buffer_t *b0, *b1, *b2, *b3;
  u32 thread_index = vm->thread_index;
  memif_per_thread_data_t *ptd = vec_elt_at_index (mm->per_thread_data,
						   thread_index);
  vlib_buffer_t bt;
  u16 cur_slot, last_slot, ring_size, n_slots, mask;
  i16 start_offset;
  u16 n_buffers = 0, n_alloc;
  memif_copy_op_t *co;
  memif_packet_op_t *po;
  memif_region_index_t last_region = ~0;
  void *last_region_shm = 0;
  void *last_region_max = 0;

  mq = vec_elt_at_index (mif->rx_queues, qid);
  ring = mq->ring;
  ring_size = 1 << mq->log2_ring_size;
  mask = ring_size - 1;

  /* assume that somebody will want to add ethernet header on the packet
     so start with IP header at offset 14 */
  start_offset = (mode == MEMIF_INTERFACE_MODE_IP) ? 14 : 0;

  /* for S2M rings, we are consumers of packet buffers, and for M2S rings we
     are producers of empty buffers */
  cur_slot = (type == MEMIF_RING_S2M) ? mq->last_head : mq->last_tail;

  if (type == MEMIF_RING_S2M)
    last_slot = __atomic_load_n (&ring->head, __ATOMIC_ACQUIRE);
  else
    last_slot = __atomic_load_n (&ring->tail, __ATOMIC_ACQUIRE);

  if (cur_slot == last_slot)
    goto refill;
  n_slots = last_slot - cur_slot;

  /* construct copy and packet vector out of ring slots */
  while (n_slots && n_rx_packets < MEMIF_RX_VECTOR_SZ)
    {
      u32 dst_off, src_off, n_bytes_left;
      u16 s0;
      memif_desc_t *d0;
      void *mb0;
      po = ptd->packet_ops + n_rx_packets;
      n_rx_packets++;
      po->first_buffer_vec_index = n_buffers++;
      po->packet_len = 0;
      src_off = 0;
      dst_off = start_offset;

    next_slot:
      CLIB_PREFETCH (&ring->desc[(cur_slot + 8) & mask],
		     CLIB_CACHE_LINE_BYTES, LOAD);
      s0 = cur_slot & mask;
      d0 = &ring->desc[s0];
      n_bytes_left = d0->length;

      /* slave resets buffer length,
       * so it can produce full size buffer for master
       */
      if (type == MEMIF_RING_M2S)
	d0->length = mif->run.buffer_size;

      po->packet_len += n_bytes_left;
      if (PREDICT_FALSE (last_region != d0->region))
	{
	  last_region_shm = mif->regions[d0->region].shm;
	  last_region = d0->region;
	  last_region_max =
	    last_region_shm + mif->regions[last_region].region_size;
	}
      mb0 = last_region_shm + d0->offset;

      if (PREDICT_FALSE (mb0 + n_bytes_left > last_region_max))
	vlib_error_count (vm, node->node_index, MEMIF_INPUT_ERROR_BAD_DESC, 1);
      else
	do
	  {
	    u32 dst_free = buffer_size - dst_off;
	    if (dst_free == 0)
	      {
		dst_off = 0;
		dst_free = buffer_size;
		n_buffers++;
	      }
	    u32 bytes_to_copy = clib_min (dst_free, n_bytes_left);
	    memif_add_copy_op (ptd, mb0 + src_off, bytes_to_copy, dst_off,
			       n_buffers - 1);
	    n_bytes_left -= bytes_to_copy;
	    src_off += bytes_to_copy;
	    dst_off += bytes_to_copy;
	  }
	while (PREDICT_FALSE (n_bytes_left));

      cur_slot++;
      n_slots--;
      if ((d0->flags & MEMIF_DESC_FLAG_NEXT) && n_slots)
	{
	  src_off = 0;
	  goto next_slot;
	}
    }

  /* allocate free buffers */
  vec_validate_aligned (ptd->buffers, n_buffers - 1, CLIB_CACHE_LINE_BYTES);
  n_alloc = vlib_buffer_alloc_from_pool (vm, ptd->buffers, n_buffers,
					 mq->buffer_pool_index);
  if (PREDICT_FALSE (n_alloc != n_buffers))
    {
      if (n_alloc)
	vlib_buffer_free (vm, ptd->buffers, n_alloc);
      vlib_error_count (vm, node->node_index,
			MEMIF_INPUT_ERROR_BUFFER_ALLOC_FAIL, 1);
      goto refill;
    }

  /* copy data */
  n_left = vec_len (ptd->copy_ops);
  co = ptd->copy_ops;
  while (n_left >= 8)
    {
      CLIB_PREFETCH (co[4].data, CLIB_CACHE_LINE_BYTES, LOAD);
      CLIB_PREFETCH (co[5].data, CLIB_CACHE_LINE_BYTES, LOAD);
      CLIB_PREFETCH (co[6].data, CLIB_CACHE_LINE_BYTES, LOAD);
      CLIB_PREFETCH (co[7].data, CLIB_CACHE_LINE_BYTES, LOAD);

      b0 = vlib_get_buffer (vm, ptd->buffers[co[0].buffer_vec_index]);
      b1 = vlib_get_buffer (vm, ptd->buffers[co[1].buffer_vec_index]);
      b2 = vlib_get_buffer (vm, ptd->buffers[co[2].buffer_vec_index]);
      b3 = vlib_get_buffer (vm, ptd->buffers[co[3].buffer_vec_index]);

      clib_memcpy_fast (b0->data + co[0].buffer_offset, co[0].data,
			co[0].data_len);
      clib_memcpy_fast (b1->data + co[1].buffer_offset, co[1].data,
			co[1].data_len);
      clib_memcpy_fast (b2->data + co[2].buffer_offset, co[2].data,
			co[2].data_len);
      clib_memcpy_fast (b3->data + co[3].buffer_offset, co[3].data,
			co[3].data_len);

      co += 4;
      n_left -= 4;
    }
  while (n_left)
    {
      b0 = vlib_get_buffer (vm, ptd->buffers[co[0].buffer_vec_index]);
      clib_memcpy_fast (b0->data + co[0].buffer_offset, co[0].data,
			co[0].data_len);
      co += 1;
      n_left -= 1;
    }

  /* release slots from the ring */
  if (type == MEMIF_RING_S2M)
    {
      __atomic_store_n (&ring->tail, cur_slot, __ATOMIC_RELEASE);
      mq->last_head = cur_slot;
    }
  else
    {
      mq->last_tail = cur_slot;
    }

  /* prepare buffer template and next indices */
  vnet_buffer (&ptd->buffer_template)->sw_if_index[VLIB_RX] =
    mif->sw_if_index;
  vnet_buffer (&ptd->buffer_template)->feature_arc_index = 0;
  ptd->buffer_template.current_data = start_offset;
  ptd->buffer_template.current_config_index = 0;
  ptd->buffer_template.buffer_pool_index = mq->buffer_pool_index;
  ptd->buffer_template.ref_count = 1;

  if (mode == MEMIF_INTERFACE_MODE_ETHERNET)
    {
      next_index = VNET_DEVICE_INPUT_NEXT_ETHERNET_INPUT;
      if (mif->per_interface_next_index != ~0)
	next_index = mif->per_interface_next_index;
      else
	vnet_feature_start_device_input_x1 (mif->sw_if_index, &next_index,
					    &ptd->buffer_template);

      vlib_get_new_next_frame (vm, node, next_index, to_next_bufs,
			       n_left_to_next);
      if (PREDICT_TRUE (next_index == VNET_DEVICE_INPUT_NEXT_ETHERNET_INPUT))
	{
	  vlib_next_frame_t *nf;
	  vlib_frame_t *f;
	  ethernet_input_frame_t *ef;
	  nf = vlib_node_runtime_get_next_frame (vm, node, next_index);
	  f = vlib_get_frame (vm, nf->frame);
	  f->flags = ETH_INPUT_FRAME_F_SINGLE_SW_IF_IDX;

	  ef = vlib_frame_scalar_args (f);
	  ef->sw_if_index = mif->sw_if_index;
	  ef->hw_if_index = mif->hw_if_index;
	  vlib_frame_no_append (f);
	}
    }

  /* process buffer metadata */
  u32 n_from = n_rx_packets;
  po = ptd->packet_ops;
  bi = to_next_bufs;

  /* copy template into local variable - will save per packet load */
  vlib_buffer_copy_template (&bt, &ptd->buffer_template);

  while (n_from >= 8)
    {
      b0 = vlib_get_buffer (vm, ptd->buffers[po[4].first_buffer_vec_index]);
      b1 = vlib_get_buffer (vm, ptd->buffers[po[5].first_buffer_vec_index]);
      b2 = vlib_get_buffer (vm, ptd->buffers[po[6].first_buffer_vec_index]);
      b3 = vlib_get_buffer (vm, ptd->buffers[po[7].first_buffer_vec_index]);

      vlib_prefetch_buffer_header (b0, STORE);
      vlib_prefetch_buffer_header (b1, STORE);
      vlib_prefetch_buffer_header (b2, STORE);
      vlib_prefetch_buffer_header (b3, STORE);

      /* enqueue buffer */
      u32 fbvi[4];
      fbvi[0] = po[0].first_buffer_vec_index;
      fbvi[1] = po[1].first_buffer_vec_index;
      fbvi[2] = po[2].first_buffer_vec_index;
      fbvi[3] = po[3].first_buffer_vec_index;

      bi[0] = ptd->buffers[fbvi[0]];
      bi[1] = ptd->buffers[fbvi[1]];
      bi[2] = ptd->buffers[fbvi[2]];
      bi[3] = ptd->buffers[fbvi[3]];

      b0 = vlib_get_buffer (vm, bi[0]);
      b1 = vlib_get_buffer (vm, bi[1]);
      b2 = vlib_get_buffer (vm, bi[2]);
      b3 = vlib_get_buffer (vm, bi[3]);

      vlib_buffer_copy_template (b0, &bt);
      vlib_buffer_copy_template (b1, &bt);
      vlib_buffer_copy_template (b2, &bt);
      vlib_buffer_copy_template (b3, &bt);

      b0->current_length = po[0].packet_len;
      n_rx_bytes += b0->current_length;
      b1->current_length = po[1].packet_len;
      n_rx_bytes += b1->current_length;
      b2->current_length = po[2].packet_len;
      n_rx_bytes += b2->current_length;
      b3->current_length = po[3].packet_len;
      n_rx_bytes += b3->current_length;

      memif_add_to_chain (vm, b0, ptd->buffers + fbvi[0] + 1, buffer_size);
      memif_add_to_chain (vm, b1, ptd->buffers + fbvi[1] + 1, buffer_size);
      memif_add_to_chain (vm, b2, ptd->buffers + fbvi[2] + 1, buffer_size);
      memif_add_to_chain (vm, b3, ptd->buffers + fbvi[3] + 1, buffer_size);

      if (mode == MEMIF_INTERFACE_MODE_IP)
	{
	  next[0] = memif_next_from_ip_hdr (node, b0);
	  next[1] = memif_next_from_ip_hdr (node, b1);
	  next[2] = memif_next_from_ip_hdr (node, b2);
	  next[3] = memif_next_from_ip_hdr (node, b3);
	}

      /* next */
      n_from -= 4;
      po += 4;
      bi += 4;
      next += 4;
    }
  while (n_from)
    {
      u32 fbvi[4];
      /* enqueue buffer */
      fbvi[0] = po[0].first_buffer_vec_index;
      bi[0] = ptd->buffers[fbvi[0]];
      b0 = vlib_get_buffer (vm, bi[0]);
      vlib_buffer_copy_template (b0, &bt);
      b0->current_length = po->packet_len;
      n_rx_bytes += b0->current_length;

      memif_add_to_chain (vm, b0, ptd->buffers + fbvi[0] + 1, buffer_size);

      if (mode == MEMIF_INTERFACE_MODE_IP)
	{
	  next[0] = memif_next_from_ip_hdr (node, b0);
	}

      /* next */
      n_from -= 1;
      po += 1;
      bi += 1;
      next += 1;
    }

  /* packet trace if enabled */
  if (PREDICT_FALSE ((n_trace = vlib_get_trace_count (vm, node))))
    {
      u32 n_left = n_rx_packets;
      bi = to_next_bufs;
      next = nexts;
      u32 ni = next_index;
      while (n_trace && n_left)
	{
	  vlib_buffer_t *b;
	  memif_input_trace_t *tr;
	  if (mode != MEMIF_INTERFACE_MODE_ETHERNET)
	    ni = next[0];
	  b = vlib_get_buffer (vm, bi[0]);
	  if (PREDICT_TRUE
	      (vlib_trace_buffer (vm, node, ni, b, /* follow_chain */ 0)))
	    {
	      tr = vlib_add_trace (vm, node, b, sizeof (*tr));
	      tr->next_index = ni;
	      tr->hw_if_index = mif->hw_if_index;
	      tr->ring = qid;
	      n_trace--;
	    }

	  /* next */
	  n_left--;
	  bi++;
	  next++;
	}
      vlib_set_trace_count (vm, node, n_trace);
    }

  if (mode == MEMIF_INTERFACE_MODE_ETHERNET)
    {
      n_left_to_next -= n_rx_packets;
      vlib_put_next_frame (vm, node, next_index, n_left_to_next);
    }
  else
    vlib_buffer_enqueue_to_next (vm, node, to_next_bufs, nexts, n_rx_packets);

  vlib_increment_combined_counter (vnm->interface_main.combined_sw_if_counters
				   + VNET_INTERFACE_COUNTER_RX, thread_index,
				   mif->sw_if_index, n_rx_packets,
				   n_rx_bytes);

  /* refill ring with empty buffers */
refill:
  vec_reset_length (ptd->buffers);
  vec_reset_length (ptd->copy_ops);

  if (type == MEMIF_RING_M2S)
    {
      u16 head = ring->head;
      n_slots = ring_size - head + mq->last_tail;

      while (n_slots--)
	{
	  u16 s = head++ & mask;
	  memif_desc_t *d = &ring->desc[s];
	  d->length = mif->run.buffer_size;
	}

      __atomic_store_n (&ring->head, head, __ATOMIC_RELEASE);
    }

  return n_rx_packets;
}

static_always_inline uword
memif_device_input_zc_inline (vlib_main_t *vm, vlib_node_runtime_t *node,
			      memif_if_t *mif, u16 qid,
			      memif_interface_mode_t mode)
{
  vnet_main_t *vnm = vnet_get_main ();
  memif_main_t *mm = &memif_main;
  memif_ring_t *ring;
  memif_queue_t *mq;
  u32 next_index;
  uword n_trace = vlib_get_trace_count (vm, node);
  u32 n_rx_packets = 0, n_rx_bytes = 0;
  u32 *to_next = 0, *buffers;
  u32 bi0, bi1, bi2, bi3;
  u16 slot, s0;
  memif_desc_t *d0;
  vlib_buffer_t *b0, *b1, *b2, *b3;
  u32 thread_index = vm->thread_index;
  memif_per_thread_data_t *ptd = vec_elt_at_index (mm->per_thread_data,
						   thread_index);
  u16 cur_slot, last_slot, ring_size, n_slots, mask, head;
  i16 start_offset;
  u64 offset;
  u32 buffer_length;
  u16 n_alloc, n_from;

  mq = vec_elt_at_index (mif->rx_queues, qid);
  ring = mq->ring;
  ring_size = 1 << mq->log2_ring_size;
  mask = ring_size - 1;

  next_index = (mode == MEMIF_INTERFACE_MODE_IP) ?
    VNET_DEVICE_INPUT_NEXT_IP6_INPUT : VNET_DEVICE_INPUT_NEXT_ETHERNET_INPUT;

  /* asume that somebody will want to add ethernet header on the packet
     so start with IP header at offset 14 */
  start_offset = (mode == MEMIF_INTERFACE_MODE_IP) ? 14 : 0;
  buffer_length = vlib_buffer_get_default_data_size (vm) - start_offset;

  cur_slot = mq->last_tail;
  last_slot = __atomic_load_n (&ring->tail, __ATOMIC_ACQUIRE);
  if (cur_slot == last_slot)
    goto refill;
  n_slots = last_slot - cur_slot;

  /* process ring slots */
  vec_validate_aligned (ptd->buffers, MEMIF_RX_VECTOR_SZ,
			CLIB_CACHE_LINE_BYTES);
  while (n_slots && n_rx_packets < MEMIF_RX_VECTOR_SZ)
    {
      vlib_buffer_t *hb;

      s0 = cur_slot & mask;
      bi0 = mq->buffers[s0];
      ptd->buffers[n_rx_packets++] = bi0;

      CLIB_PREFETCH (&ring->desc[(cur_slot + 8) & mask],
		     CLIB_CACHE_LINE_BYTES, LOAD);
      d0 = &ring->desc[s0];
      hb = b0 = vlib_get_buffer (vm, bi0);
      b0->current_data = start_offset;
      b0->current_length = d0->length;
      n_rx_bytes += d0->length;

      cur_slot++;
      n_slots--;
      if (PREDICT_FALSE ((d0->flags & MEMIF_DESC_FLAG_NEXT) && n_slots))
	{
	  hb->flags |= VLIB_BUFFER_TOTAL_LENGTH_VALID;
	next_slot:
	  s0 = cur_slot & mask;
	  d0 = &ring->desc[s0];
	  bi0 = mq->buffers[s0];

	  /* previous buffer */
	  b0->next_buffer = bi0;
	  b0->flags |= VLIB_BUFFER_NEXT_PRESENT;

	  /* current buffer */
	  b0 = vlib_get_buffer (vm, bi0);
	  b0->current_data = start_offset;
	  b0->current_length = d0->length;
	  hb->total_length_not_including_first_buffer += d0->length;
	  n_rx_bytes += d0->length;

	  cur_slot++;
	  n_slots--;
	  if ((d0->flags & MEMIF_DESC_FLAG_NEXT) && n_slots)
	    goto next_slot;
	}
    }

  /* release slots from the ring */
  mq->last_tail = cur_slot;

  n_from = n_rx_packets;
  buffers = ptd->buffers;

  while (n_from)
    {
      u32 n_left_to_next;
      u32 next0, next1, next2, next3;

      vlib_get_next_frame (vm, node, next_index, to_next, n_left_to_next);
      while (n_from >= 8 && n_left_to_next >= 4)
	{
	  b0 = vlib_get_buffer (vm, buffers[4]);
	  b1 = vlib_get_buffer (vm, buffers[5]);
	  b2 = vlib_get_buffer (vm, buffers[6]);
	  b3 = vlib_get_buffer (vm, buffers[7]);
	  vlib_prefetch_buffer_header (b0, STORE);
	  vlib_prefetch_buffer_header (b1, STORE);
	  vlib_prefetch_buffer_header (b2, STORE);
	  vlib_prefetch_buffer_header (b3, STORE);

	  /* enqueue buffer */
	  to_next[0] = bi0 = buffers[0];
	  to_next[1] = bi1 = buffers[1];
	  to_next[2] = bi2 = buffers[2];
	  to_next[3] = bi3 = buffers[3];
	  to_next += 4;
	  n_left_to_next -= 4;
	  buffers += 4;

	  b0 = vlib_get_buffer (vm, bi0);
	  b1 = vlib_get_buffer (vm, bi1);
	  b2 = vlib_get_buffer (vm, bi2);
	  b3 = vlib_get_buffer (vm, bi3);

	  vnet_buffer (b0)->sw_if_index[VLIB_RX] = mif->sw_if_index;
	  vnet_buffer (b0)->sw_if_index[VLIB_TX] = ~0;
	  vnet_buffer (b1)->sw_if_index[VLIB_RX] = mif->sw_if_index;
	  vnet_buffer (b1)->sw_if_index[VLIB_TX] = ~0;
	  vnet_buffer (b2)->sw_if_index[VLIB_RX] = mif->sw_if_index;
	  vnet_buffer (b2)->sw_if_index[VLIB_TX] = ~0;
	  vnet_buffer (b3)->sw_if_index[VLIB_RX] = mif->sw_if_index;
	  vnet_buffer (b3)->sw_if_index[VLIB_TX] = ~0;

	  if (mode == MEMIF_INTERFACE_MODE_IP)
	    {
	      next0 = memif_next_from_ip_hdr (node, b0);
	      next1 = memif_next_from_ip_hdr (node, b1);
	      next2 = memif_next_from_ip_hdr (node, b2);
	      next3 = memif_next_from_ip_hdr (node, b3);
	    }
	  else if (mode == MEMIF_INTERFACE_MODE_ETHERNET)
	    {
	      if (PREDICT_FALSE (mif->per_interface_next_index != ~0))
		{
		  next0 = mif->per_interface_next_index;
		  next1 = mif->per_interface_next_index;
		  next2 = mif->per_interface_next_index;
		  next3 = mif->per_interface_next_index;
		}
	      else
		{
		  next0 = next1 = next2 = next3 = next_index;
		  /* redirect if feature path enabled */
		  vnet_feature_start_device_input_x1 (mif->sw_if_index,
						      &next0, b0);
		  vnet_feature_start_device_input_x1 (mif->sw_if_index,
						      &next1, b1);
		  vnet_feature_start_device_input_x1 (mif->sw_if_index,
						      &next2, b2);
		  vnet_feature_start_device_input_x1 (mif->sw_if_index,
						      &next3, b3);
		}
	    }

	  /* trace */
	  if (PREDICT_FALSE (n_trace > 0))
	    {
	      memif_trace_buffer (vm, node, mif, b0, next0, qid, &n_trace);
	      if (PREDICT_FALSE (n_trace > 0))
		memif_trace_buffer (vm, node, mif, b1, next1, qid, &n_trace);
	      if (PREDICT_FALSE (n_trace > 0))
		memif_trace_buffer (vm, node, mif, b2, next2, qid, &n_trace);
	      if (PREDICT_FALSE (n_trace > 0))
		memif_trace_buffer (vm, node, mif, b3, next3, qid, &n_trace);
	    }

	  /* enqueue */
	  vlib_validate_buffer_enqueue_x4 (vm, node, next_index, to_next,
					   n_left_to_next, bi0, bi1, bi2, bi3,
					   next0, next1, next2, next3);

	  /* next */
	  n_from -= 4;
	}
      while (n_from && n_left_to_next)
	{
	  /* enqueue buffer */
	  to_next[0] = bi0 = buffers[0];
	  to_next += 1;
	  n_left_to_next--;
	  buffers += 1;

	  b0 = vlib_get_buffer (vm, bi0);
	  vnet_buffer (b0)->sw_if_index[VLIB_RX] = mif->sw_if_index;
	  vnet_buffer (b0)->sw_if_index[VLIB_TX] = ~0;

	  if (mode == MEMIF_INTERFACE_MODE_IP)
	    {
	      next0 = memif_next_from_ip_hdr (node, b0);
	    }
	  else if (mode == MEMIF_INTERFACE_MODE_ETHERNET)
	    {
	      if (PREDICT_FALSE (mif->per_interface_next_index != ~0))
		next0 = mif->per_interface_next_index;
	      else
		{
		  next0 = next_index;
		  /* redirect if feature path enabled */
		  vnet_feature_start_device_input_x1 (mif->sw_if_index,
						      &next0, b0);
		}
	    }

	  /* trace */
	  if (PREDICT_FALSE (n_trace > 0))
	    memif_trace_buffer (vm, node, mif, b0, next0, qid, &n_trace);

	  /* enqueue */
	  vlib_validate_buffer_enqueue_x1 (vm, node, next_index, to_next,
					   n_left_to_next, bi0, next0);

	  /* next */
	  n_from--;
	}
      vlib_put_next_frame (vm, node, next_index, n_left_to_next);
    }

  vlib_increment_combined_counter (vnm->interface_main.combined_sw_if_counters
				   + VNET_INTERFACE_COUNTER_RX, thread_index,
				   mif->sw_if_index, n_rx_packets,
				   n_rx_bytes);

  /* refill ring with empty buffers */
refill:
  vec_reset_length (ptd->buffers);

  head = ring->head;
  n_slots = ring_size - head + mq->last_tail;
  slot = head & mask;

  n_slots &= ~7;

  if (n_slots < 32)
    goto done;

  memif_desc_t desc_template, *dt = &desc_template;
  clib_memset (dt, 0, sizeof (memif_desc_t));
  dt->length = buffer_length;

  n_alloc = vlib_buffer_alloc_to_ring_from_pool (
    vm, mq->buffers, slot, ring_size, n_slots, mq->buffer_pool_index);
  dt->region = mq->buffer_pool_index + 1;
  offset = (u64) mif->regions[dt->region].shm + start_offset;

  if (PREDICT_FALSE (n_alloc != n_slots))
    vlib_error_count (vm, node->node_index,
		      MEMIF_INPUT_ERROR_BUFFER_ALLOC_FAIL, 1);

  head += n_alloc;

  while (n_alloc)
    {
      memif_desc_t *d = ring->desc + slot;
      u32 *bi = mq->buffers + slot;

      if (PREDICT_FALSE (((slot + 7 > mask) || (n_alloc < 8))))
	goto one_by_one;

      clib_memcpy_fast (d + 0, dt, sizeof (memif_desc_t));
      clib_memcpy_fast (d + 1, dt, sizeof (memif_desc_t));
      clib_memcpy_fast (d + 2, dt, sizeof (memif_desc_t));
      clib_memcpy_fast (d + 3, dt, sizeof (memif_desc_t));
      clib_memcpy_fast (d + 4, dt, sizeof (memif_desc_t));
      clib_memcpy_fast (d + 5, dt, sizeof (memif_desc_t));
      clib_memcpy_fast (d + 6, dt, sizeof (memif_desc_t));
      clib_memcpy_fast (d + 7, dt, sizeof (memif_desc_t));

      d[0].offset = (u64) vlib_get_buffer (vm, bi[0])->data - offset;
      d[1].offset = (u64) vlib_get_buffer (vm, bi[1])->data - offset;
      d[2].offset = (u64) vlib_get_buffer (vm, bi[2])->data - offset;
      d[3].offset = (u64) vlib_get_buffer (vm, bi[3])->data - offset;
      d[4].offset = (u64) vlib_get_buffer (vm, bi[4])->data - offset;
      d[5].offset = (u64) vlib_get_buffer (vm, bi[5])->data - offset;
      d[6].offset = (u64) vlib_get_buffer (vm, bi[6])->data - offset;
      d[7].offset = (u64) vlib_get_buffer (vm, bi[7])->data - offset;

      slot += 8;
      n_alloc -= 8;
      continue;

    one_by_one:
      clib_memcpy_fast (d, dt, sizeof (memif_desc_t));
      d[0].offset = (u64) vlib_get_buffer (vm, bi[0])->data - offset;

      slot = (slot + 1) & mask;
      n_alloc -= 1;
    }

  __atomic_store_n (&ring->head, head, __ATOMIC_RELEASE);

done:
  return n_rx_packets;
}


VLIB_NODE_FN (memif_input_node) (vlib_main_t * vm,
				 vlib_node_runtime_t * node,
				 vlib_frame_t * frame)
{
  u32 n_rx = 0;
  memif_main_t *mm = &memif_main;
  memif_interface_mode_t mode_ip = MEMIF_INTERFACE_MODE_IP;
  memif_interface_mode_t mode_eth = MEMIF_INTERFACE_MODE_ETHERNET;

  vnet_hw_if_rxq_poll_vector_t *pv;
  pv = vnet_hw_if_get_rxq_poll_vector (vm, node);
  for (int i = 0; i < vec_len (pv); i++)
    {
      memif_if_t *mif;
      u32 qid;
      mif = vec_elt_at_index (mm->interfaces, pv[i].dev_instance);
      qid = pv[i].queue_id;
      if ((mif->flags & MEMIF_IF_FLAG_ADMIN_UP) &&
	  (mif->flags & MEMIF_IF_FLAG_CONNECTED))
	{
	  if (mif->flags & MEMIF_IF_FLAG_ZERO_COPY)
	    {
	      if (mif->mode == MEMIF_INTERFACE_MODE_IP)
		n_rx +=
		  memif_device_input_zc_inline (vm, node, mif, qid, mode_ip);
	      else
		n_rx +=
		  memif_device_input_zc_inline (vm, node, mif, qid, mode_eth);
	    }
	  else if (mif->flags & MEMIF_IF_FLAG_IS_SLAVE)
	    {
	      if (mif->mode == MEMIF_INTERFACE_MODE_IP)
		n_rx += memif_device_input_inline (
		  vm, node, mif, MEMIF_RING_M2S, qid, mode_ip);
	      else
		n_rx += memif_device_input_inline (
		  vm, node, mif, MEMIF_RING_M2S, qid, mode_eth);
	    }
	  else
	    {
	      if (mif->mode == MEMIF_INTERFACE_MODE_IP)
		n_rx += memif_device_input_inline (
		  vm, node, mif, MEMIF_RING_S2M, qid, mode_ip);
	      else
		n_rx += memif_device_input_inline (
		  vm, node, mif, MEMIF_RING_S2M, qid, mode_eth);
	    }
	}
    }

  return n_rx;
}

/* *INDENT-OFF* */
VLIB_REGISTER_NODE (memif_input_node) = {
  .name = "memif-input",
  .flags = VLIB_NODE_FLAG_TRACE_SUPPORTED,
  .sibling_of = "device-input",
  .format_trace = format_memif_input_trace,
  .type = VLIB_NODE_TYPE_INPUT,
  .state = VLIB_NODE_STATE_INTERRUPT,
  .n_errors = MEMIF_INPUT_N_ERROR,
  .error_counters = memif_input_error_counters,
};

/* *INDENT-ON* */


/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
