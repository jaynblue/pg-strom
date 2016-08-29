/*
 * datastore.c
 *
 * Routines to manage data store; row-store, column-store, toast-buffer,
 * and param-buffer.
 * ----
 * Copyright 2011-2016 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014-2016 (C) The PG-Strom Development Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include "postgres.h"
#include "access/relscan.h"
#include "catalog/catalog.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_type.h"
#include "optimizer/cost.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/predicate.h"
#include "utils/builtins.h"
#include "utils/bytea.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/tqual.h"
#include "pg_strom.h"
#include "cuda_numeric.h"
#include <float.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

/*
 * static variables
 */
static int		pgstrom_chunk_size_kb;
static int		pgstrom_chunk_limit_kb = INT_MAX;
static long		sysconf_pagesize;		/* _SC_PAGESIZE */
static long		sysconf_phys_pages;		/* _SC_PHYS_PAGES */

/*
 * pgstrom_chunk_size - configured chunk size
 */
Size
pgstrom_chunk_size(void)
{
	return ((Size)pgstrom_chunk_size_kb) << 10;
}

static bool
check_guc_chunk_size(int *newval, void **extra, GucSource source)
{
	if (*newval > pgstrom_chunk_limit_kb)
	{
		GUC_check_errdetail("pg_strom.chunk_size = %d, is larger than "
							"pg_strom.chunk_limit = %d",
							*newval, pgstrom_chunk_limit_kb);
		return false;
	}
	return true;
}

/*
 * pgstrom_chunk_size_limit
 */
Size
pgstrom_chunk_size_limit(void)
{
	return ((Size)pgstrom_chunk_limit_kb) << 10;
}

static bool
check_guc_chunk_limit(int *newval, void **extra, GucSource source)
{
	if (*newval < pgstrom_chunk_size_kb)
	{
		GUC_check_errdetail("pg_strom.chunk_limit = %d, is less than "
							"pg_strom.chunk_size = %d",
							*newval, pgstrom_chunk_size_kb);
	}
	return true;
}

/*
 * pgstrom_bulk_exec_supported - returns true, if supplied planstate
 * supports bulk execution mode.
 */
bool
pgstrom_bulk_exec_supported(const PlanState *planstate)
{
	if (pgstrom_plan_is_gpuscan(planstate->plan)) // ||
//        pgstrom_plan_is_gpujoin(planstate->plan) ||
		//      pgstrom_plan_is_gpupreagg(planstate->plan) ||
//        pgstrom_plan_is_gpusort(planstate->plan))
	{
		GpuTaskState   *gts = (GpuTaskState *) planstate;

		if (gts->cb_bulk_exec != NULL)
			return true;
	}
	return false;
}

/*
 * estimate_num_chunks
 *
 * it estimates number of chunks to be fetched from the supplied Path
 */
cl_uint
estimate_num_chunks(Path *pathnode)
{
	RelOptInfo *rel = pathnode->parent;
	int			ncols = list_length(rel->reltarget->exprs);
    Size        htup_size;
	cl_uint		num_chunks;

	htup_size = MAXALIGN(offsetof(HeapTupleHeaderData,
								  t_bits[BITMAPLEN(ncols)]));
	if (rel->reloptkind != RELOPT_BASEREL)
		htup_size += MAXALIGN(rel->reltarget->width);
	else
	{
		double      heap_size = (double)
			(BLCKSZ - SizeOfPageHeaderData) * rel->pages;

		htup_size += MAXALIGN(heap_size / Max(rel->tuples, 1.0) -
							  sizeof(ItemIdData) - SizeofHeapTupleHeader);
	}
	num_chunks = (cl_uint)
		((double)(htup_size + sizeof(cl_int)) * pathnode->rows /
		 (double)(pgstrom_chunk_size() -
				  STROMALIGN(offsetof(kern_data_store, colmeta[ncols]))));
	num_chunks = Max(num_chunks, 1);

	return num_chunks;
}

/*
 * BulkExecProcNode
 *
 * It runs the underlying sub-plan managed by PG-Strom in bulk-execution
 * mode. Caller can expect the data-store shall be filled up by the rows
 * read from the sub-plan.
 */
pgstrom_data_store *
BulkExecProcNode(GpuTaskState *gts, size_t chunk_size)
{
	PlanState		   *plannode = &gts->css.ss.ps;
	pgstrom_data_store *pds;

	CHECK_FOR_INTERRUPTS();

	if (plannode->chgParam != NULL)			/* If something changed, */
		ExecReScan(&gts->css.ss.ps);		/* let ReScan handle this */

	Assert(IsA(gts, CustomScanState));		/* rough checks */
	if (gts->cb_bulk_exec)
	{
		/* must provide our own instrumentation support */
		if (plannode->instrument)
			InstrStartNode(plannode->instrument);
		/* execution per chunk */
		pds = gts->cb_bulk_exec(gts, chunk_size);

		/* must provide our own instrumentation support */
		if (plannode->instrument)
			InstrStopNode(plannode->instrument,
						  !pds ? 0.0 : (double)pds->kds.nitems);
		Assert(!pds || pds->kds.nitems > 0);
		return pds;
	}
	elog(ERROR, "Bug? exec_chunk callback was not implemented");
}

bool
kern_fetch_data_store(TupleTableSlot *slot,
					  kern_data_store *kds,
					  size_t row_index,
					  HeapTuple tuple)
{
	if (row_index >= kds->nitems)
		return false;	/* out of range */

	/* in case of KDS_FORMAT_ROW */
	if (kds->format == KDS_FORMAT_ROW)
	{
		kern_tupitem   *tup_item = KERN_DATA_STORE_TUPITEM(kds, row_index);

		ExecClearTuple(slot);
		tuple->t_len = tup_item->t_len;
		tuple->t_self = tup_item->t_self;
		//tuple->t_tableOid = InvalidOid;
		tuple->t_data = &tup_item->htup;

		ExecStoreTuple(tuple, slot, InvalidBuffer, false);

		return true;
	}
	/* in case of KDS_FORMAT_SLOT */
	if (kds->format == KDS_FORMAT_SLOT)
	{
		Datum  *tts_values = (Datum *)KERN_DATA_STORE_VALUES(kds, row_index);
		bool   *tts_isnull = (bool *)KERN_DATA_STORE_ISNULL(kds, row_index);
		int		natts = slot->tts_tupleDescriptor->natts;

		memcpy(slot->tts_values, tts_values, sizeof(Datum) * natts);
		memcpy(slot->tts_isnull, tts_isnull, sizeof(bool) * natts);
#ifdef NOT_USED
		/*
		 * XXX - pointer reference is better than memcpy from performance
		 * perspectives, however, we need to ensure tts_values/tts_isnull
		 * shall be restored when pgstrom-data-store is released.
		 * It will be cause of complicated / invisible bugs.
		 */
		slot->tts_values = tts_values;
		slot->tts_isnull = tts_isnull;
#endif
		ExecStoreVirtualTuple(slot);
		return true;
	}
	elog(ERROR, "Bug? unexpected data-store format: %d", kds->format);
	return false;
}

bool
pgstrom_fetch_data_store(TupleTableSlot *slot,
						 pgstrom_data_store *pds,
						 size_t row_index,
						 HeapTuple tuple)
{
	return kern_fetch_data_store(slot, &pds->kds, row_index, tuple);
}

pgstrom_data_store *
PDS_retain(pgstrom_data_store *pds)
{
	Assert(pds->refcnt > 0);

	pds->refcnt++;

	return pds;
}

void
PDS_release(pgstrom_data_store *pds)
{
	Assert(pds->refcnt > 0);
	if (--pds->refcnt == 0)
		dmaBufferFree(pds);
}

void
init_kernel_data_store(kern_data_store *kds,
					   TupleDesc tupdesc,
					   Size length,
					   int format,
					   uint nrooms,
					   bool use_internal)
{
	int		i, attcacheoff;

	memset(kds, 0, offsetof(kern_data_store, colmeta));
	kds->hostptr = (hostptr_t) &kds->hostptr;
	kds->length = length;
	kds->usage = 0;
	kds->ncols = tupdesc->natts;
	kds->nitems = 0;
	kds->nrooms = nrooms;
	kds->format = format;
	kds->tdhasoid = tupdesc->tdhasoid;
	kds->tdtypeid = tupdesc->tdtypeid;
	kds->tdtypmod = tupdesc->tdtypmod;
	kds->table_oid = InvalidOid;	/* caller shall set */
	kds->nslots = 0;				/* caller shall set, if any */
	kds->hash_min = 0;
	kds->hash_max = UINT_MAX;
	kds->nblocks_uncached = 0;
	kds->nrows_per_block = 0;

	attcacheoff = offsetof(HeapTupleHeaderData, t_bits);
	if (tupdesc->tdhasoid)
		attcacheoff += sizeof(Oid);
	attcacheoff = MAXALIGN(attcacheoff);

	for (i=0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute attr = tupdesc->attrs[i];
		int		attalign = typealign_get_width(attr->attalign);
		bool	attbyval = attr->attbyval;
		int		attlen = attr->attlen;

		if (!attr->attbyval)
			kds->has_notbyval = true;
		if (attr->atttypid == NUMERICOID)
		{
			kds->has_numeric = true;
			if (use_internal)
			{
				attbyval = true;
				attlen = sizeof(cl_long);
			}
		}

		if (attcacheoff > 0)
		{
			if (attlen > 0)
				attcacheoff = TYPEALIGN(attalign, attcacheoff);
			else
				attcacheoff = -1;	/* no more shortcut any more */
		}
		kds->colmeta[i].attbyval = attbyval;
		kds->colmeta[i].attalign = attalign;
		kds->colmeta[i].attlen = attlen;
		kds->colmeta[i].attnum = attr->attnum;
		kds->colmeta[i].attcacheoff = attcacheoff;
		kds->colmeta[i].atttypid = (cl_uint)attr->atttypid;
		kds->colmeta[i].atttypmod = (cl_int)attr->atttypmod;
		if (attcacheoff >= 0)
			attcacheoff += attr->attlen;
		/*
		 * !!don't forget to update pl_cuda.c if kern_colmeta layout would
		 * be updated !!
		 */
	}
}

pgstrom_data_store *
PDS_expand_size(GpuContext_v2 *gcontext,
				pgstrom_data_store *pds_old,
				Size kds_length_new)
{
	pgstrom_data_store *pds_new;
	Size		kds_length_old = pds_old->kds.length;
	Size		kds_usage = pds_old->kds.usage;
	cl_uint		i, nitems = pds_old->kds.nitems;

	/* sanity checks */
	Assert(pds_old->kds.format == KDS_FORMAT_ROW ||
		   pds_old->kds.format == KDS_FORMAT_HASH);
	Assert(pds_old->kds.nslots == 0);

	/* no need to expand? */
	kds_length_new = STROMALIGN_DOWN(kds_length_new);
	if (pds_old->kds.length >= kds_length_new)
		return pds_old;

	pds_new = dmaBufferAlloc(gcontext, offsetof(pgstrom_data_store,
												kds) + kds_length_new);
	memcpy(pds_new, pds_old, (offsetof(pgstrom_data_store, kds) +
							  KERN_DATA_STORE_HEAD_LENGTH(&pds_old->kds)));
	pds_new->kds.hostptr = (hostptr_t)&pds_new->kds.hostptr;
	pds_new->kds.length  = kds_length_new;

	/*
	 * Move the contents to new buffer from the old one
	 */
	if (pds_new->kds.format == KDS_FORMAT_ROW ||
		pds_new->kds.format == KDS_FORMAT_HASH)
	{
		cl_uint	   *row_index_old = KERN_DATA_STORE_ROWINDEX(&pds_old->kds);
		cl_uint	   *row_index_new = KERN_DATA_STORE_ROWINDEX(&pds_new->kds);
		size_t		shift = STROMALIGN_DOWN(kds_length_new - kds_length_old);
		size_t		offset = kds_length_old - kds_usage;

		/*
		 * If supplied new nslots is too big, larger than the expanded,
		 * it does not make sense to expand the buffer.
		 */
		if ((pds_new->kds.format == KDS_FORMAT_HASH
			 ? KDS_CALCULATE_HASH_LENGTH(pds_new->kds.ncols,
										 pds_new->kds.nitems,
										 pds_new->kds.usage)
			 : KDS_CALCULATE_ROW_LENGTH(pds_new->kds.ncols,
										pds_new->kds.nitems,
										pds_new->kds.usage)) >= kds_length_new)
			elog(ERROR, "New nslots consumed larger than expanded");

		memcpy((char *)&pds_new->kds + offset + shift,
			   (char *)&pds_old->kds + offset,
			   kds_length_old - offset);
		for (i = 0; i < nitems; i++)
			row_index_new[i] = row_index_old[i] + shift;
	}
	else if (pds_new->kds.format == KDS_FORMAT_SLOT)
	{
		/*
		 * We cannot expand KDS_FORMAT_SLOT with extra area because we don't
		 * know the way to fix pointers that reference the extra area.
		 */
		if (pds_new->kds.usage > 0)
			elog(ERROR, "cannot expand KDS_FORMAT_SLOT with extra area");
		/* copy the values/isnull pair */
		memcpy(KERN_DATA_STORE_BODY(&pds_new->kds),
			   KERN_DATA_STORE_BODY(&pds_old->kds),
			   KERN_DATA_STORE_SLOT_LENGTH(&pds_old->kds,
										   pds_old->kds.nitems));
	}
	else
		elog(ERROR, "unexpected KDS format: %d", pds_new->kds.format);

	/* release the old PDS, and return the new one */
	dmaBufferFree(pds_old);
	return pds_new;
}

void
PDS_shrink_size(pgstrom_data_store *pds)
{
	kern_data_store	   *kds = &pds->kds;
	size_t				new_length;

	if (kds->format == KDS_FORMAT_ROW ||
		kds->format == KDS_FORMAT_HASH)
	{
		cl_uint	   *hash_slot = KERN_DATA_STORE_HASHSLOT(kds);
		cl_uint	   *row_index = KERN_DATA_STORE_ROWINDEX(kds);
		cl_uint		i, nslots = kds->nslots;
		size_t		shift;
		char	   *baseptr;

		/* small shift has less advantage than CPU cycle consumption */
		shift = kds->length - (kds->format == KDS_FORMAT_HASH
							   ? KDS_CALCULATE_HASH_LENGTH(kds->ncols,
														   kds->nitems,
														   kds->usage)
							   : KDS_CALCULATE_ROW_LENGTH(kds->ncols,
														  kds->nitems,
														  kds->usage));
		shift = STROMALIGN_DOWN(shift);

		if (shift < BLCKSZ || shift < sizeof(Datum) * kds->nitems)
			return;

		/* move the kern_tupitem / kern_hashitem */
		baseptr = (char *)kds + (kds->format == KDS_FORMAT_HASH
								 ? KDS_CALCULATE_HASH_FRONTLEN(kds->ncols,
															   kds->nitems)
								 : KDS_CALCULATE_ROW_FRONTLEN(kds->ncols,
															  kds->nitems));
		memmove(baseptr, baseptr + shift, kds->length - shift);

		/* clear the hash slot once */
		if (nslots > 0)
		{
			Assert(kds->format == KDS_FORMAT_HASH);
			memset(hash_slot, 0, sizeof(cl_uint) * nslots);
		}

		/* adjust row_index and hash_slot */
		for (i=0; i < kds->nitems; i++)
		{
			row_index[i] -= shift;
			if (nslots > 0)
			{
				kern_hashitem  *khitem = KERN_DATA_STORE_HASHITEM(kds, i);
				cl_uint			khindex;

				Assert(khitem->rowid == i);
				khindex = khitem->hash % nslots;
                khitem->next = hash_slot[khindex];
                hash_slot[khindex] = (uintptr_t)khitem - (uintptr_t)kds;
			}
		}
		new_length = kds->length - shift;
	}
	else if (kds->format == KDS_FORMAT_SLOT)
	{
		new_length = KERN_DATA_STORE_SLOT_LENGTH(kds, kds->nitems);

		/*
		 * We cannot know which datum references the extra area with
		 * reasonable cost. So, prohibit it simply. We don't use SLOT
		 * format for data source, so usually no matter.
		 */
		if (kds->usage > 0)
			elog(ERROR, "cannot shirink KDS_SLOT with extra region");
	}
	else
		elog(ERROR, "Bug? unexpected PDS to be shrinked");

	Assert(new_length <= kds->length);
	kds->length = new_length;
}

pgstrom_data_store *
PDS_create_row(GpuContext_v2 *gcontext, TupleDesc tupdesc, Size length)
{
	pgstrom_data_store *pds;
	Size		kds_length = STROMALIGN_DOWN(length);

	pds = dmaBufferAlloc(gcontext, offsetof(pgstrom_data_store,
											kds) + kds_length);
	pds->refcnt = 1;	/* owned by the caller at least */

	/*
	 * initialize common part of KDS. Note that row-format cannot
	 * determine 'nrooms' preliminary, so INT_MAX instead.
	 */
	init_kernel_data_store(&pds->kds, tupdesc, kds_length,
						   KDS_FORMAT_ROW, INT_MAX, false);
	return pds;
}

pgstrom_data_store *
PDS_create_slot(GpuContext_v2 *gcontext,
				TupleDesc tupdesc,
				cl_uint nrooms,
				Size extra_length,
				bool use_internal)
{
	pgstrom_data_store *pds;
	size_t			kds_length;

	kds_length = (STROMALIGN(offsetof(kern_data_store,
									  colmeta[tupdesc->natts])) +
				  STROMALIGN(LONGALIGN((sizeof(Datum) + sizeof(char)) *
									   tupdesc->natts) * nrooms) +
				  STROMALIGN(extra_length));
	pds = dmaBufferAlloc(gcontext, offsetof(pgstrom_data_store,
											kds) + kds_length);
	pds->refcnt = 1;	/* owned by the caller at least */

	init_kernel_data_store(&pds->kds, tupdesc, kds_length,
						   KDS_FORMAT_SLOT, nrooms, use_internal);
	return pds;
}

pgstrom_data_store *
PDS_create_hash(GpuContext_v2 *gcontext,
				TupleDesc tupdesc,
				Size length)
{
	pgstrom_data_store *pds;
	Size		kds_length = STROMALIGN_DOWN(length);

	if (KDS_CALCULATE_HEAD_LENGTH(tupdesc->natts) > kds_length)
		elog(ERROR, "Required length for KDS-Hash is too short");

	pds = dmaBufferAlloc(gcontext, offsetof(pgstrom_data_store,
											kds) + kds_length);
	pds->refcnt = 1;

	init_kernel_data_store(&pds->kds, tupdesc, kds_length,
						   KDS_FORMAT_HASH, INT_MAX, false);
	return pds;
}

pgstrom_data_store *
PDS_create_block(GpuContext_v2 *gcontext,
				 TupleDesc tupdesc,
				 Size length,
				 cl_uint nrows_per_tuple)
{
	pgstrom_data_store *pds;
	Size		kds_length = STROMALIGN_DOWN(length);
	cl_uint		nrooms;

	if (KDS_CALCULATE_HEAD_LENGTH(tupdesc->natts) > kds_length)
		elog(ERROR, "Required length for KDS-Block is too short");

	pds = dmaBufferAlloc(gcontext, offsetof(pgstrom_data_store,
											kds) + kds_length);
	pds->refcnt = 1;

	nrooms = (kds_length - KDS_CALCULATE_HEAD_LENGTH(tupdesc->natts))
		/ (sizeof(BlockNumber) + BLCKSZ);
	while (KDS_CALCULATE_HEAD_LENGTH(tupdesc->natts) +
		   STROMALIGN(sizeof(BlockNumber) * nrooms) +
		   BLCKSZ * nrooms > kds_length)
		nrooms--;
	if (nrooms < 1)
		elog(ERROR, "Required length for KDS-Block is too short");

	init_kernel_data_store(&pds->kds, tupdesc, kds_length,
						   KDS_FORMAT_BLOCK, nrooms, false);
	pds->kds.nrows_per_block = nrows_per_tuple;

	return pds;
}

/*
 * support for bulkload onto ROW/BLOCK format
 */

/* see storage/smgr/md.c */
typedef struct _MdfdVec
{
	File			mdfd_vfd;		/* fd number in fd.c's pool */
	BlockNumber		mdfd_segno;		/* segment number, from 0 */
	struct _MdfdVec *mdfd_chain;	/* next segment, or NULL */
} MdfdVec;

typedef struct PDSScanState
{
	BlockNumber		curr_segno;
	Buffer			curr_vmbuffer;
	struct {
		File		vfd;
		BlockNumber	segno;
	} mdfd[FLEXIBLE_ARRAY_MEMBER];
} PDSScanState;

/*
 * PDS_begin_heapscan
 */
void
PDS_begin_heapscan(GpuTaskState_v2 *gts)
{
	Relation		relation = gts->css.ss.ss_currentRelation;
	EState		   *estate = gts->css.ss.ps.state;
	BlockNumber		nr_blocks;
	BlockNumber		nr_segs;
	MdfdVec		   *vec;
	PDSScanState   *sstate;
	cl_uint			i;

	/*
	 * NOTE: RelationGetNumberOfBlocks() has a significant side-effect.
	 * It opens all the underlying files of MAIN_FORKNUM, then set @rd_smgr
	 * of the relation.
	 * It allows extension to touch file descriptors without invocation of
	 * ReadBuffer().
	 */
	nr_blocks = RelationGetNumberOfBlocks(relation);
	nr_segs = (nr_blocks + (BlockNumber) RELSEG_SIZE - 1) / RELSEG_SIZE;

	pds_sstate = MemoryContextAlloc(state->es_query_cxt,
									offsetof(PDSScanState, mdfd[nr_segs]));
	memset(pds_sstate, -1, offsetof(PDSScanState, mdfd[nr_segs]));
	pds_sstate->curr_segno = InvalidBlockNumber;
	pds_sstate->curr_vmbuffer = InvalidBuffer;

	vec = relation->rd_smgr->md_fd[MAIN_FORKNUM];
	while (vec)
	{
		if (vec->mdfd_vfd < 0 ||
			vec->mdfd_segno >= nr_segs)
			elog(ERROR, "Bug? MdfdVec {vfd=%d segno=%u} is out of range",
				 vec->mdfd_vfd, vec->mdfd_segno);
		pds_sstate->mdfd[vec->mdfd_segno].segno	= vec->mdfd_segno;
		pds_sstate->mdfd[vec->mdfd_segno].vfd	= vec->mdfd_vfd;
		vec = vec->mdfd_chain;
	}

	/* sanity checks */
	for (i=0; i < nr_segs; i++)
	{
		if (pds_sstate->mdfd[i].segno >= nr_segs ||
			pds_sstate->mdfd[i].vfd < 0)
			elog(ERROR, "Bug? Here is a hole segment which was not open");
	}
	gts->pds_sstate = pds_sstate;
}

/*
 * PDS_end_heapscan
 */
void
PDS_end_heapscan(GpuTaskState_v2 *gts)
{
	PDSScanState   *pds_sstate = gts->pds_sstate;

	if (pds_sstate)
	{
		/* release visibility map, if any */
		if (pds_sstate->curr_vmbuffer != InvalidBuffer)
		{
			ReleaseBuffer(pds_sstate->curr_vmbuffer);
			pds_sstate->curr_vmbuffer = InvalidBuffer;
		}
		pfree(pds_sstate);
		gts->pds_sstate = NULL;
	}
}

/*
 * PDS_exec_heapscan_block - PDS scan for KDS_FORMAT_BLOCK format
 */
static int
PDS_exec_heapscan_block(pgstrom_data_store *pds,
						Relation relation,
						HeapScanDesc hscan,
						PDSScanState *pds_sstate)
{
	BlockNumber		blknum = hscan->rs_cblock;
	Snapshot		snapshot = hscan->rs_snapshot;
	BufferAccessStrategy strategy = hscan->rs_strategy;
	SMgrRelation   *smgr = relation->rd_smgr;
	BufferTag		newTag;
	uint32			newHash;
	LWLock		   *newPartitionLock = NULL;
	int				buf_id;

	/*
	 * Always sync read if NVMe-Strom does not support the relation
	 * (likely, incorrect decision in the caller)
	 */
	if (!RelationCanUseNvmeStrom(relation))
		goto sync_read_buffer;

	/* check whether the target block is all-visible */
	if (!VM_ALL_VISIBLE(relation, blknum,
						&pds_sstate->curr_vmbuffer))
		goto sync_read_buffer;

	/* create a tag so we can lookup the buffer */
	INIT_BUFFERTAG(newTag, smgr->smgr_rnode.node, MAIN_FORKNUM, blknum);
	/* determine its hash code and partition lock ID */
	newHash = BufTableHashCode(&newTag);
	newPartitionLock = BufMappingPartitionLock(newHash);

	/* check whether the block exists on the shared buffer? */
	LWLockAcquire(newPartitionLock, LW_SHARED);
	buf_id = BufTableLookup(&newTag, newHash);
	if (buf_id >= 0)
		goto sync_read_buffer;	/* already loaded */

	// Here is 128 BufMappingPartitionLock() 
	// Each process can hold up to 200 locks
	// Likely, long BufMappingPartitionLock hold prevents concurrency
	// Do we have any other mechanism to lock file write?
	// Linux's mandatory lock - needs to set group's bit
	// Is it possible to overwrite by kernel module?
	// anyway, write lock to page-cache is sufficient


	/*
	 * OK, the source block is not loaded to the buffer and it shall be
	 * all visible.
	 */





	return;



	/*
	 * MEMO: cuMemcpyHtoDAsync() will take higher performance as long as
	 * we can load the entire table blocks onto main memory.
	 * SSD-to-GPU Direct DMA involeves raw i/o operations with less
	 * intermediation by VFS, however, its throughput is less than RAM.
	 * So, we like to avoid SSD-to-GPU Direct DMA for tables that are
	 * enough small to cache.
	 */
	nr_blocks = RelationGetNumberOfBlocks(relation);












	/* create a tag so we can lookup the buffer */
	INIT_BUFFERTAG(newTag, smgr->smgr_rnode.node, MAIN_FORKNUM, blknum);


	// if not all visible, --> sync read
	// or direct DMA is disabled


	// if all visible and direct DMA enabled 




sync_read_buffer:
	if (newPartitionLock)
		LWLockRelease(newPartitionLock);



}

/*
 * PDS_exec_heapscan_row - PDS scan for KDS_FORMAT_ROW format
 */
static int
PDS_exec_heapscan_row(pgstrom_data_store *pds,
					  Relation relation,
					  HeapScanDesc hscan,
					  PDSScanState *pds_sstate)
{



}

/*
 * PDS_exec_heapscan - PDS scan entrypoint
 */
int
PDS_exec_heapscan(pgstrom_data_store *pds, GpuScanState_v2 *gts)
{
	Relation		relation = gts->css.ss.ss_currentRelation;
	HeapScanDesc	hscan = gts->css.ss.ss_currentScanDesc;
	PDSScanState   *pds_sstate = gts->pds_sstate;
	int				retval;

	if (pds->kds.format == KDS_FORMAT_ROW)
		retval = PDS_exec_heapscan_row(pds, relation, hscan, pds_sstate);
	else if (pds->kds.format == KDS_FORMAT_BLOCK)
		retval = PDS_exec_heapscan_block(pds, relatioin, hscan, pds_sstate);
	else
		elog(ERROR, "Bug? unexpected PDS format: %d", pds->kds.format);

	return retval;
}


int
PDS_insert_block(pgstrom_data_store *pds,
				 Relation rel, BlockNumber blknum,
				 Snapshot snapshot,
				 BufferAccessStrategy strategy)
{
	kern_data_store	*kds = &pds->kds;
	Buffer			buffer;
	Page			page;
	int				lines;
	int				ntup;
	OffsetNumber	lineoff;
	ItemId			lpp;
	uint		   *tup_index;
	kern_tupitem   *tup_item;
	bool			all_visible;
	Size			max_consume;

	/* only row-store can block read */
	Assert(kds->format == KDS_FORMAT_ROW && kds->nslots == 0);

	CHECK_FOR_INTERRUPTS();

	/* Load the target buffer */
	//buffer = ReadBuffer(rel, blknum);
	buffer = ReadBufferExtended(rel, MAIN_FORKNUM, blknum,
								RBM_NORMAL, strategy);

#if 1
	/* Just like heapgetpage(), however, jobs we focus on is OLAP
	 * workload, so it's uncertain whether we should vacuum the page
	 * here.
	 */
	heap_page_prune_opt(rel, buffer);
#endif

	/* we will check tuple's visibility under the shared lock */
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = (Page) BufferGetPage(buffer);
	lines = PageGetMaxOffsetNumber(page);
	ntup = 0;

	/*
	 * Check whether we have enough rooms to store expected number of
	 * tuples on the remaining space. If it is hopeless to load all
	 * the items in a block, we inform the caller this block shall be
	 * loaded on the next data store.
	 */
	max_consume = KDS_CALCULATE_HASH_LENGTH(kds->ncols,
											kds->nitems + lines,
											offsetof(kern_tupitem,
													 htup) * lines +
											BLCKSZ + kds->usage);
	if (max_consume > kds->length)
	{
		UnlockReleaseBuffer(buffer);
		return -1;
	}

	/*
	 * Logic is almost same as heapgetpage() doing.
	 */
	all_visible = PageIsAllVisible(page) && !snapshot->takenDuringRecovery;

	/* TODO: make SerializationNeededForRead() an external function
	 * on the core side. It kills necessity of setting up HeapTupleData
	 * when all_visible and non-serialized transaction.
	 */
	tup_index = KERN_DATA_STORE_ROWINDEX(kds) + kds->nitems;
	for (lineoff = FirstOffsetNumber, lpp = PageGetItemId(page, lineoff);
		 lineoff <= lines;
		 lineoff++, lpp++)
	{
		HeapTupleData	tup;
		bool			valid;

		if (!ItemIdIsNormal(lpp))
			continue;

		tup.t_tableOid = RelationGetRelid(rel);
		tup.t_data = (HeapTupleHeader) PageGetItem((Page) page, lpp);
		tup.t_len = ItemIdGetLength(lpp);
		ItemPointerSet(&tup.t_self, blknum, lineoff);

		if (all_visible)
			valid = true;
		else
			valid = HeapTupleSatisfiesVisibility(&tup, snapshot, buffer);

		CheckForSerializableConflictOut(valid, rel, &tup, buffer, snapshot);
		if (!valid)
			continue;

		/* put tuple */
		kds->usage += LONGALIGN(offsetof(kern_tupitem, htup) + tup.t_len);
		tup_item = (kern_tupitem *)((char *)kds + kds->length - kds->usage);
		tup_index[ntup] = (uintptr_t)tup_item - (uintptr_t)kds;
		tup_item->t_len = tup.t_len;
		tup_item->t_self = tup.t_self;
		memcpy(&tup_item->htup, tup.t_data, tup.t_len);

		ntup++;
	}
	UnlockReleaseBuffer(buffer);
	Assert(ntup <= MaxHeapTuplesPerPage);
	Assert(kds->nitems + ntup <= kds->nrooms);
	kds->nitems += ntup;

	return ntup;
}

/*
 * PDS_insert_tuple
 *
 * It inserts a tuple onto the data store. Unlike block read mode, we cannot
 * use this API only for row-format.
 */
bool
PDS_insert_tuple(pgstrom_data_store *pds, TupleTableSlot *slot)
{
	kern_data_store	   *kds = &pds->kds;
	size_t				required;
	HeapTuple			tuple;
	cl_uint			   *tup_index;
	kern_tupitem	   *tup_item;

	/* No room to store a new kern_rowitem? */
	if (kds->nitems >= kds->nrooms)
		return false;
	Assert(kds->ncols == slot->tts_tupleDescriptor->natts);

	if (kds->format != KDS_FORMAT_ROW)
		elog(ERROR, "Bug? unexpected data-store format: %d", kds->format);

	/* OK, put a record */
	tup_index = KERN_DATA_STORE_ROWINDEX(kds);

	/* reference a HeapTuple in TupleTableSlot */
	tuple = ExecFetchSlotTuple(slot);

	/* check whether we have room for this tuple */
	required = LONGALIGN(offsetof(kern_tupitem, htup) + tuple->t_len);
	if (KDS_CALCULATE_ROW_LENGTH(kds->ncols,
								 kds->nitems + 1,
								 required + kds->usage) > kds->length)
		return false;

	kds->usage += required;
	tup_item = (kern_tupitem *)((char *)kds + kds->length - kds->usage);
	tup_item->t_len = tuple->t_len;
	tup_item->t_self = tuple->t_self;
	memcpy(&tup_item->htup, tuple->t_data, tuple->t_len);
	tup_index[kds->nitems++] = (uintptr_t)tup_item - (uintptr_t)kds;

	return true;
}


/*
 * PDS_insert_hashitem
 *
 * It inserts a tuple to the data store of hash format.
 */
bool
PDS_insert_hashitem(pgstrom_data_store *pds,
					TupleTableSlot *slot,
					cl_uint hash_value)
{
	kern_data_store	   *kds = &pds->kds;
	cl_uint			   *row_index = KERN_DATA_STORE_ROWINDEX(kds);
	Size				required;
	HeapTuple			tuple;
	kern_hashitem	   *khitem;

	/* No room to store a new kern_hashitem? */
	if (kds->nitems >= kds->nrooms)
		return false;
	Assert(kds->ncols == slot->tts_tupleDescriptor->natts);

	/* KDS has to be KDS_FORMAT_HASH */
	if (kds->format != KDS_FORMAT_HASH)
		elog(ERROR, "Bug? unexpected data-store format: %d", kds->format);

	/* compute required length */
	tuple = ExecFetchSlotTuple(slot);
	required = MAXALIGN(offsetof(kern_hashitem, t.htup) + tuple->t_len);

	Assert(kds->usage == MAXALIGN(kds->usage));
	if (KDS_CALCULATE_HASH_LENGTH(kds->ncols,
								  kds->nitems + 1,
								  required + kds->usage) > pds->kds.length)
		return false;	/* no more space to put */

	/* OK, put a tuple */
	Assert(kds->usage == MAXALIGN(kds->usage));
	khitem = (kern_hashitem *)((char *)kds + kds->length
							   - (kds->usage + required));
	kds->usage += required;
	khitem->hash = hash_value;
	khitem->next = 0x7f7f7f7f;	/* to be set later */
	khitem->rowid = kds->nitems++;
	khitem->t.t_len = tuple->t_len;
	khitem->t.t_self = tuple->t_self;
	memcpy(&khitem->t.htup, tuple->t_data, tuple->t_len);

	row_index[khitem->rowid] = (cl_uint)((uintptr_t)&khitem->t.t_len -
										 (uintptr_t)kds);
	return true;
}

/*
 * PDS_build_hashtable
 *
 * construct hash table according to the current contents
 */
void
PDS_build_hashtable(pgstrom_data_store *pds)
{
	kern_data_store *kds = &pds->kds;
	cl_uint		   *row_index = KERN_DATA_STORE_ROWINDEX(kds);
	cl_uint		   *hash_slot = KERN_DATA_STORE_HASHSLOT(kds);
	cl_uint			i, j, nslots = __KDS_NSLOTS(kds->nitems);

	if (kds->format != KDS_FORMAT_HASH)
		elog(ERROR, "Bug? Only KDS_FORMAT_HASH can build a hash table");
	if (kds->nslots > 0)
		elog(ERROR, "Bug? hash table is already built");

	memset(hash_slot, 0, sizeof(cl_uint) * nslots);
	for (i = 0; i < kds->nitems; i++)
	{
		kern_hashitem  *khitem = (kern_hashitem *)
			((char *)kds + row_index[i] - offsetof(kern_hashitem, t));

		Assert(khitem->rowid == i);
		j = khitem->hash % nslots;
		khitem->next = hash_slot[j];
		hash_slot[j] = (uintptr_t)khitem - (uintptr_t)kds;
	}
	kds->nslots = nslots;
}

void
pgstrom_init_datastore(void)
{
	/* get system configuration */
	sysconf_pagesize = sysconf(_SC_PAGESIZE);
	if (sysconf_pagesize < 0)
		elog(ERROR, "failed on sysconf(_SC_PAGESIZE): %m");
	sysconf_phys_pages = sysconf(_SC_PHYS_PAGES);
	if (sysconf_phys_pages < 0)
		elog(ERROR, "failed on sysconf(_SC_PHYS_PAGES): %m");
	
	/* init GUC variables */
	DefineCustomIntVariable("pg_strom.chunk_size",
							"default size of pgstrom_data_store",
							NULL,
							&pgstrom_chunk_size_kb,
							32768 - (2 * BLCKSZ / 1024),	/* almost 32MB */
							4096,
							MAX_KILOBYTES,
							PGC_INTERNAL,
							GUC_NOT_IN_SAMPLE | GUC_UNIT_KB,
							check_guc_chunk_size, NULL, NULL);
	DefineCustomIntVariable("pg_strom.chunk_limit",
							"limit size of pgstrom_data_store",
							NULL,
							&pgstrom_chunk_limit_kb,
							5 * pgstrom_chunk_size_kb,
							4096,
							MAX_KILOBYTES,
							PGC_INTERNAL,
							GUC_NOT_IN_SAMPLE | GUC_UNIT_KB,
							check_guc_chunk_limit, NULL, NULL);
}
