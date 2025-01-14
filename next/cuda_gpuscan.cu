/*
 * cuda_gpuscan.cu
 *
 * Device implementation of GpuScan
 * ----
 * Copyright 2011-2022 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014-2022 (C) PG-Strom Developers Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the PostgreSQL License.
 */
#include "cuda_common.h"

/* ----------------------------------------------------------------
 *
 * execGpuScanLoadSource and related
 *
 * ----------------------------------------------------------------
 */
STATIC_FUNCTION(int)
__gpuscan_load_source_row(kern_context *kcxt,
						  kern_warp_context *wp,
						  kern_data_store *kds_src,
						  kern_expression *kexp_load_vars,
						  kern_expression *kexp_scan_quals,
						  void **kvars_addr,
						  int   *kvars_len,
						  uint32_t *p_smx_row_count)
{
	uint32_t	count;
	uint32_t	index;
	uint32_t	mask;
	uint32_t	wr_pos;
	kern_tupitem *tupitem = NULL;

	/* fetch next warpSize tuples */
	if (LaneId() == 0)
		count = atomicAdd(p_smx_row_count, 1);
	count = __shfl_sync(__activemask(), count, 0);
	index = (get_num_groups() * count + get_group_id()) * warpSize;
	if (index >= kds_src->nitems)
	{
		if (LaneId() == 0)
			wp->scan_done = 1;
		__syncwarp();
		return 1;
	}
	index += LaneId();

	if (index < kds_src->nitems)
	{
		uint32_t	offset = KDS_GET_ROWINDEX(kds_src)[index];

		assert(offset <= kds_src->usage);
		tupitem = (kern_tupitem *)((char *)kds_src +
								   kds_src->length -
								   __kds_unpack(offset));
		assert((char *)tupitem >= (char *)kds_src &&
			   (char *)tupitem <  (char *)kds_src + kds_src->length);
		kcxt->kvars_addr = (void **) alloca(sizeof(void *) * kcxt->kvars_nslots);
		kcxt->kvars_len  = (int *) alloca(sizeof(int) * kcxt->kvars_nslots);
		if (!ExecLoadVarsOuterRow(kcxt,
								  kexp_load_vars,
								  kexp_scan_quals,
								  kds_src,
								  &tupitem->htup))
			tupitem = NULL;
	}
	/* error checks */
	if (__any_sync(__activemask(), kcxt->errcode != ERRCODE_STROM_SUCCESS))
		return -1;
	/*
	 * save the private kvars slot on the combination buffer (depth=0)
	 */
	mask = __ballot_sync(__activemask(), tupitem != NULL);
	if (LaneId() == 0)
	{
		wr_pos = WARP_WRITE_POS(wp,0);
		WARP_WRITE_POS(wp,0) += __popc(mask);
	}
	wr_pos = __shfl_sync(__activemask(), wr_pos, 0);
	mask &= ((1U << LaneId()) - 1);
	wr_pos += __popc(mask);
	if (tupitem != NULL)
	{
		index = (wr_pos % UNIT_TUPLES_PER_DEPTH) * kcxt->kvars_nslots;
		memcpy(kvars_addr + index,
			   kcxt->kvars_addr,
			   sizeof(void *) * kcxt->kvars_nslots);
		memcpy(kvars_len + index,
			   kcxt->kvars_len,
			   sizeof(int) * kcxt->kvars_nslots);
	}
	kcxt->kvars_addr = NULL;
	kcxt->kvars_len = NULL;
	__syncwarp();
	/* move to the next depth if more than 32 htuples were fetched */
	return (WARP_WRITE_POS(wp,0) >= WARP_READ_POS(wp,0) + warpSize ? 1 : 0);
}

/*
 * __gpuscan_load_source_block
 */
STATIC_FUNCTION(int)
__gpuscan_load_source_block(kern_context *kcxt,
							kern_warp_context *wp,
							kern_data_store *kds_src,
							kern_expression *kexp_load_vars,
							kern_expression *kexp_scan_quals,
							void **kvars_addr,
							int *kvars_len,
							uint32_t *p_smx_row_count)
{
	uint32_t	block_id = __shfl_sync(__activemask(), wp->block_id, 0);
	uint32_t	wr_pos = __shfl_sync(__activemask(), wp->lp_wr_pos, 0);
	uint32_t	rd_pos = __shfl_sync(__activemask(), wp->lp_rd_pos, 0);
	uint32_t	count;
	uint32_t	mask;

	assert(wr_pos >= rd_pos);
	if (block_id > kds_src->nitems || wr_pos >= rd_pos + warpSize)
	{
		HeapTupleHeaderData *htup = NULL;
		uint32_t	off;
		int			index;

		kcxt_reset(kcxt);
		rd_pos += LaneId();
		if (rd_pos < wr_pos)
		{
			off = wp->lp_items[rd_pos % UNIT_TUPLES_PER_DEPTH];
			htup = (HeapTupleHeaderData *)((char *)kds_src + __kds_unpack(off));
			kcxt->kvars_addr = (void **) alloca(sizeof(void *) * kcxt->kvars_nslots);
			kcxt->kvars_len  = (int *) alloca(sizeof(int) * kcxt->kvars_nslots);
			if (!ExecLoadVarsOuterRow(kcxt,
									  kexp_load_vars,
									  kexp_scan_quals,
									  kds_src, htup))
				htup = NULL;
		}
		/* error checks */
		if (__any_sync(__activemask(), kcxt->errcode != ERRCODE_STROM_SUCCESS))
			return -1;
		if (LaneId() == 0)
			wp->lp_rd_pos = Min(wp->lp_wr_pos,
								wp->lp_rd_pos + warpSize);
		/*
		 * save the private kvars on the warp-buffer
		 */
		mask = __ballot_sync(__activemask(), htup != NULL);
		if (LaneId() == 0)
		{
			wr_pos = WARP_WRITE_POS(wp,0);
			WARP_WRITE_POS(wp,0) += __popc(mask);
		}
		wr_pos = __shfl_sync(__activemask(), wr_pos, 0);
		mask &= ((1U << LaneId()) - 1);
		wr_pos += __popc(mask);
		if (htup != NULL)
		{
			index = (wr_pos % UNIT_TUPLES_PER_DEPTH) * kcxt->kvars_nslots;
			memcpy(kvars_addr + index,
				   kcxt->kvars_addr,
				   sizeof(void *) * kcxt->kvars_nslots);
			memcpy(kvars_len + index,
				   kcxt->kvars_len,
				   sizeof(int) * kcxt->kvars_nslots);
		}
		kcxt->kvars_addr = NULL;
		kcxt->kvars_len = NULL;
		__syncwarp();
		/* end-of-scan checks */
		if (block_id > kds_src->nitems &&	/* no more blocks to fetch */
			wp->lp_rd_pos >= wp->lp_wr_pos)	/* no more pending tuples  */
		{
			if (LaneId() == 0)
				wp->scan_done = 1;
			return 1;
		}
		/* move to the next depth if more than 32 htuples were fetched */
		return (WARP_WRITE_POS(wp,0) >= WARP_READ_POS(wp,0) + warpSize ? 1 : 0);
	}

	/*
	 * Here, number of pending tuples (which is saved in the lp_items[]) is
	 * not enough to run ScanQuals checks. So, we move to the next bunch of
	 * line-items or next block.
	 * The pending tuples just passed the MVCC visivility checks, but
	 * ScanQuals check is not applied yet. We try to run ScanQuals checks
	 * with 32 threads simultaneously.
	 */
	if (block_id == 0)
	{
		/*
		 * block_id == 0 means this warp is not associated with particular
		 * block-page, so we try to fetch the next page.
		 */
		if (LaneId() == 0)
			count = atomicAdd(p_smx_row_count, 1);
		count = __shfl_sync(__activemask(), count, 0);
		block_id = (get_num_groups() * count + get_group_id()) + 1;
		if (LaneId() == 0)
			wp->block_id = block_id;
	}
	if (block_id <= kds_src->nitems)
	{
		PageHeaderData *pg_page = KDS_BLOCK_PGPAGE(kds_src, block_id-1);
		HeapTupleHeaderData *htup = NULL;

		count = __shfl_sync(__activemask(), wp->lp_count, 0);
		if (count < PageGetMaxOffsetNumber(pg_page))
		{
			count += LaneId();
			if (count < PageGetMaxOffsetNumber(pg_page))
			{
				ItemIdData *lpp = &pg_page->pd_linp[count];

				assert((char *)lpp < (char *)pg_page + BLCKSZ);
				if (ItemIdIsNormal(lpp))
					htup = (HeapTupleHeaderData *)PageGetItem(pg_page, lpp);
				else
					htup = NULL;
			}
			/* put visible tuples on the lp_items[] array */
			mask = __ballot_sync(__activemask(), htup != NULL);
			if (LaneId() == 0)
			{
				wr_pos = wp->lp_wr_pos;
				wp->lp_wr_pos += __popc(mask);
			}
			wr_pos = __shfl_sync(__activemask(), wr_pos, 0);
			mask &= ((1U << LaneId()) - 1);
			wr_pos += __popc(mask);
			if (htup != NULL)
			{
				wp->lp_items[wr_pos % UNIT_TUPLES_PER_DEPTH]
					= __kds_packed((char *)htup - (char *)kds_src);
			}
			if (LaneId() == 0)
				wp->lp_count += warpSize;
		}
		else
		{
			/* no more tuples to fetch from the current page */
			if (LaneId() == 0)
			{
				wp->block_id = 0;
				wp->lp_count = 0;
			}
			__syncwarp();
		}
	}
	return 0;	/* stay depth-0 */
}

/*
 * __gpuscan_load_source_arrow
 */
STATIC_FUNCTION(int)
__gpuscan_load_source_arrow(kern_context *kcxt,
							kern_warp_context *wp,
							kern_data_store *kds_src,
							kern_expression *kexp_load_vars,
							kern_expression *kexp_scan_quals,
							void **kvars_addr,
							int *kvars_len,
							uint32_t *p_smx_row_count)
{
	uint32_t	kds_index;
	uint32_t	count;
	uint32_t	mask;
	uint32_t	wr_pos;
	bool		is_valid = false;

	/* fetch next warpSize tuples */
	if (LaneId() == 0)
		count = atomicAdd(p_smx_row_count, 1);
	count = __shfl_sync(__activemask(), count, 0);
	kds_index = (get_num_groups() * count + get_group_id()) * warpSize;
	if (kds_index >= kds_src->nitems)
	{
		wp->scan_done = 1;
		__syncwarp(__activemask());
		return 1;
	}
	kds_index += LaneId();

	if (kds_index < kds_src->nitems)
	{
		kcxt->kvars_addr = (void **) alloca(sizeof(void *) * kcxt->kvars_nslots);
		kcxt->kvars_len  = (int *) alloca(sizeof(int) * kcxt->kvars_nslots);
		if (ExecLoadVarsOuterArrow(kcxt,
								   kexp_load_vars,
								   kexp_scan_quals,
								   kds_src,
								   kds_index))
			is_valid = true;
	}
	/* error checks */
	if (__any_sync(__activemask(), kcxt->errcode != 0))
		return -1;
	/*
	 * save the htuple on the local combination buffer (depth=0)
	 */
	mask = __ballot_sync(__activemask(), is_valid);
	if (LaneId() == 0)
	{
		wr_pos = WARP_WRITE_POS(wp,0);
		WARP_WRITE_POS(wp,0) += __popc(mask);
	}
	wr_pos = __shfl_sync(__activemask(), wr_pos, 0);
	mask &= ((1U << LaneId()) - 1);
	wr_pos += __popc(mask);
	if (is_valid)
	{
		int		index = (wr_pos % UNIT_TUPLES_PER_DEPTH) * kcxt->kvars_nslots;
		memcpy(kvars_addr + index,
			   kcxt->kvars_addr,
			   sizeof(void *) * kcxt->kvars_nslots);
		memcpy(kvars_len + index,
			   kcxt->kvars_len,
			   sizeof(int) * kcxt->kvars_nslots);
	}
	kcxt->kvars_addr = NULL;
	kcxt->kvars_len  = NULL;
	/* move to the next depth if more than 32 htuples were fetched */
	return (WARP_WRITE_POS(wp,0) >= WARP_READ_POS(wp,0) + warpSize ? 1 : 0);
}

/*
 * __gpuscan_load_source_column
 */
INLINE_FUNCTION(int)
__gpuscan_load_source_column(kern_context *kcxt,
							 kern_warp_context *wp,
							 kern_data_store *kds_src,
							 kern_data_extra *kds_extra,
							 kern_expression *kexp_load_vars,
							 kern_expression *kern_scan_quals,
							 void **kvars_addr,
							 int *kvars_len,
							 uint32_t *p_smx_row_count)
{
	STROM_ELOG(kcxt, "KDS_FORMAT_COLUMN not implemented");
	return -1;
}

PUBLIC_FUNCTION(int)
execGpuScanLoadSource(kern_context *kcxt,
					  kern_warp_context *wp,
					  kern_data_store *kds_src,
					  kern_data_extra *kds_extra,
					  kern_expression *kexp_load_vars,
					  kern_expression *kexp_scan_quals,
					  void **kvars_addr,
					  int   *kvars_len,
					  uint32_t *p_smx_row_count)
{
	/*
	 * Move to the next depth (or projection), if combination buffer (depth=0)
	 * may overflow on the next action, or we already reached to the KDS tail.
	 */
	if (wp->scan_done || WARP_WRITE_POS(wp,0) >= WARP_READ_POS(wp,0) + warpSize)
		return 1;

	switch (kds_src->format)
	{
		case KDS_FORMAT_ROW:
			return __gpuscan_load_source_row(kcxt, wp,
											 kds_src,
											 kexp_load_vars,
											 kexp_scan_quals,
											 kvars_addr,
											 kvars_len,
											 p_smx_row_count);
		case KDS_FORMAT_BLOCK:
			return __gpuscan_load_source_block(kcxt, wp,
											   kds_src,
											   kexp_load_vars,
											   kexp_scan_quals,
											   kvars_addr,
											   kvars_len,
											   p_smx_row_count);
		case KDS_FORMAT_ARROW:
			return __gpuscan_load_source_arrow(kcxt, wp,
											   kds_src,
											   kexp_load_vars,
											   kexp_scan_quals,
											   kvars_addr,
											   kvars_len,
											   p_smx_row_count);
		case KDS_FORMAT_COLUMN:
			return __gpuscan_load_source_column(kcxt, wp,
												kds_src,
												kds_extra,
												kexp_load_vars,
												kexp_scan_quals,
												kvars_addr,
												kvars_len,
												p_smx_row_count);
		default:
			STROM_ELOG(kcxt, "Bug? Unknown KDS format");
			break;
	}
	return -1;
}

/*
 * kern_gpuscan_main
 */
KERNEL_FUNCTION(void)
kern_gpuscan_main(kern_session_info *session,
				  kern_gputask *kgtask,
				  kern_multirels *__kmrels,		/* should be NULL */
				  kern_data_store *kds_src,
				  kern_data_extra *kds_extra,
				  kern_data_store *kds_dst)
{
	kern_context	   *kcxt;
	kern_warp_context  *wp, *wp_saved;
	uint32_t			wp_unitsz;
	void			  **kvars_addr;		/* only depth-0 */
	int				   *kvars_len;		/* only depth-0 */
	int					depth;
	int					status;
	__shared__ uint32_t	smx_row_count;

	assert(kgtask->nslots == session->kvars_slot_width &&
		   kgtask->n_rels == 0 &&
		   __kmrels == NULL);
	/* setup execution context */
	INIT_KERNEL_CONTEXT(kcxt, session, kds_src, NULL, kds_dst);
	wp_unitsz = __KERN_WARP_CONTEXT_UNITSZ_BASE(0);
	wp = (kern_warp_context *)SHARED_WORKMEM(wp_unitsz, get_local_id() / warpSize);
	wp_saved = KERN_GPUTASK_WARP_CONTEXT(kgtask);
	if (kgtask->resume_context)
	{
		/* resume warp-context from the previous execution */
		if (LaneId() == 0)
			memcpy(wp, wp_saved, wp_unitsz);
		if (get_local_id() == 0)
			smx_row_count = wp->smx_row_count;
		depth = __shfl_sync(__activemask(), wp->depth, 0);
	}
	else
	{
		/* zero clear the wp */
		if (LaneId() == 0)
			memset(wp, 0, wp_unitsz);
		if (get_local_id() == 0)
			smx_row_count = 0;
		depth = 0;
	}
	kvars_addr = (void **)((char *)wp_saved + wp_unitsz);
	kvars_len  = (int *)(kvars_addr + UNIT_TUPLES_PER_DEPTH * kcxt->kvars_nslots);
	__syncthreads();

	while (depth >= 0)
	{
		if (depth == 0)
		{
			/* LOAD FROM THE SOURCE */
			depth = execGpuScanLoadSource(kcxt, wp,
										  kds_src,
										  kds_extra,
										  SESSION_KEXP_SCAN_LOAD_VARS(session),
										  SESSION_KEXP_SCAN_QUALS(session),
										  kvars_addr,
										  kvars_len,
										  &smx_row_count);
		}
		else
		{
			/* PROJECTION */
			assert(depth == 1);
			status = execGpuJoinProjection(kcxt, wp,
										   0,	/* no inner relations */
										   kds_dst,
										   SESSION_KEXP_PROJECTION(session),
										   kvars_addr,
										   kvars_len);
			if (status == -2)
			{
				if (LaneId() == 0)
					atomicAdd(&kgtask->suspend_count, 1);
				break;
			}
			depth = status;
		}
		__syncwarp();
	}
	__syncthreads();

	if (LaneId() == 0)
	{
		wp->depth = depth;
		wp->smx_row_count = smx_row_count;
		memcpy(wp_saved, wp, wp_unitsz);
	}
	STROM_WRITEBACK_ERROR_STATUS(&kgtask->kerror, kcxt);
}
