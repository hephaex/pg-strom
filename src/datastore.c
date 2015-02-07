/*
 * datastore.c
 *
 * Routines to manage data store; row-store, column-store, toast-buffer,
 * and param-buffer.
 * ----
 * Copyright 2011-2014 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014 (C) The PG-Strom Development Team
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
#include "catalog/pg_type.h"
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
#include "device_numeric.h"
#include <sys/mman.h>

/*
 * GUC variables
 */
static int		pgstrom_chunk_size_kb;
static char	   *pgstrom_temp_tablespace;

/*
 * pgstrom_chunk_size - configured chunk size
 */
Size
pgstrom_chunk_size(void)
{
	return ((Size)pgstrom_chunk_size_kb) << 10;
}

/*
 * pgstrom_create_param_buffer
 *
 * It construct a param-buffer on the shared memory segment, according to
 * the supplied Const/Param list. Its initial reference counter is 1, so
 * this buffer can be released using pgstrom_put_param_buffer().
 */
kern_parambuf *
pgstrom_create_kern_parambuf(List *used_params,
							 ExprContext *econtext)
{
	StringInfoData	str;
	kern_parambuf  *kpbuf;
	char		padding[STROMALIGN_LEN];
	ListCell   *cell;
	Size		offset;
	int			index = 0;
	int			nparams = list_length(used_params);

	/* seek to the head of variable length field */
	offset = STROMALIGN(offsetof(kern_parambuf, poffset[nparams]));
	initStringInfo(&str);
	enlargeStringInfo(&str, offset);
	memset(str.data, 0, offset);
	str.len = offset;
	/* walks on the Para/Const list */
	foreach (cell, used_params)
	{
		Node   *node = lfirst(cell);

		if (IsA(node, Const))
		{
			Const  *con = (Const *) node;

			kpbuf = (kern_parambuf *)str.data;
			if (con->constisnull)
				kpbuf->poffset[index] = 0;	/* null */
			else
			{
				kpbuf->poffset[index] = str.len;
				if (con->constlen > 0)
					appendBinaryStringInfo(&str,
										   (char *)&con->constvalue,
										   con->constlen);
				else
					appendBinaryStringInfo(&str,
										   DatumGetPointer(con->constvalue),
										   VARSIZE(con->constvalue));
			}
		}
		else if (IsA(node, Param))
		{
			ParamListInfo param_info = econtext->ecxt_param_list_info;
			Param  *param = (Param *) node;

			if (param_info &&
				param->paramid > 0 && param->paramid <= param_info->numParams)
			{
				ParamExternData	*prm = &param_info->params[param->paramid - 1];

				/* give hook a chance in case parameter is dynamic */
				if (!OidIsValid(prm->ptype) && param_info->paramFetch != NULL)
					(*param_info->paramFetch) (param_info, param->paramid);

				kpbuf = (kern_parambuf *)str.data;
				if (!OidIsValid(prm->ptype))
				{
					elog(INFO, "debug: Param has no particular data type");
					kpbuf->poffset[index++] = 0;	/* null */
					continue;
				}
				/* safety check in case hook did something unexpected */
				if (prm->ptype != param->paramtype)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("type of parameter %d (%s) does not match that when preparing the plan (%s)",
									param->paramid,
									format_type_be(prm->ptype),
									format_type_be(param->paramtype))));
				if (prm->isnull)
					kpbuf->poffset[index] = 0;	/* null */
				else
				{
					int		typlen = get_typlen(prm->ptype);

					if (typlen == 0)
						elog(ERROR, "cache lookup failed for type %u",
							 prm->ptype);
					if (typlen > 0)
						appendBinaryStringInfo(&str,
											   (char *)&prm->value,
											   typlen);
					else
						appendBinaryStringInfo(&str,
											   DatumGetPointer(prm->value),
											   VARSIZE(prm->value));
				}
			}
		}
		else
			elog(ERROR, "unexpected node: %s", nodeToString(node));

		/* alignment */
		if (STROMALIGN(str.len) != str.len)
			appendBinaryStringInfo(&str, padding,
								   STROMALIGN(str.len) - str.len);
		index++;
	}
	Assert(STROMALIGN(str.len) == str.len);
	kpbuf = (kern_parambuf *)str.data;
	kpbuf->length = str.len;
	kpbuf->nparams = nparams;

	return kpbuf;
}

Datum
pgstrom_fixup_kernel_numeric(Datum datum)
{
	cl_ulong	numeric_value = (cl_ulong) datum;
	bool		sign = PG_NUMERIC_SIGN(numeric_value);
	int			expo = PG_NUMERIC_EXPONENT(numeric_value);
	cl_ulong	mantissa = PG_NUMERIC_MANTISSA(numeric_value);
	char		temp[100];

	/* more effective implementation in the future :-) */
	snprintf(temp, sizeof(temp), "%c%lue%d",
			 sign ? '-' : '+', mantissa, expo);
	//elog(INFO, "numeric %016lx -> %s", numeric_value, temp);
	return DirectFunctionCall3(numeric_in,
							   CStringGetDatum(temp),
							   Int32GetDatum(0),
							   Int32GetDatum(-1));
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

		ExecClearTuple(slot);
		slot->tts_values = tts_values;
		slot->tts_isnull = tts_isnull;
		ExecStoreVirtualTuple(slot);

		/*
		 * MEMO: Is it really needed to take memcpy() here? If we can 
		 * do same job with pointer opertion, it makes more sense from
		 * performance standpoint.
		 * (NOTE: hash-join materialization is a hot point)
		 */
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
	return kern_fetch_data_store(slot, pds->kds, row_index, tuple);
}

void
pgstrom_release_data_store(pgstrom_data_store *pds)
{
	/* release relevant toast store, if any */
	if (pds->ktoast)
		pgstrom_release_data_store(pds->ktoast);

	/* detach from the GpuContext */
	dlist_delete(&pds->chain);

	/* release the data store body */
	if (pds->kds_fname)
	{
		pgstrom_file_unmap_data_store(pds);
		/* unlink the backend file also */
		if (unlink(pds->kds_fname) != 0)
			elog(WARNING, "failed on unlink(\"%s\") : %m", pds->kds_fname);
		pfree(pds->kds_fname);
	}
	else
	{
		pfree(pds->kds);
	}
	pfree(pds);
}

static void
init_kern_data_store(kern_data_store *kds,
					 TupleDesc tupdesc,
					 Size length,
					 int format,
					 uint nrooms,
					 bool internal_format)
{
	int		i, attcacheoff;

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

	attcacheoff = offsetof(HeapTupleHeaderData, t_bits);
	if (tupdesc->tdhasoid)
		attcacheoff += sizeof(Oid);
	attcacheoff = MAXALIGN(attcacheoff);

	for (i=0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute attr = tupdesc->attrs[i];
		bool	attbyval = attr->attbyval;
		int		attalign = typealign_get_width(attr->attalign);
		int		attlen   = attr->attlen;
		int		attnum   = attr->attnum;

		/*
		 * If variable is expected to have special internal format
		 * different from the host representation, we need to fixup
		 * colmeta catalog also. Right now, only NUMERIC can have
		 * special internal format.
		 */
		if (internal_format)
		{
			if (attr->atttypid == NUMERICOID)
			{
				attbyval = true;
				attalign = sizeof(cl_ulong);
				attlen   = sizeof(cl_ulong);
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
		kds->colmeta[i].attnum = attnum;
		kds->colmeta[i].attcacheoff = attcacheoff;
		if (attcacheoff >= 0)
			attcacheoff += attlen;
	}
}

pgstrom_data_store *
pgstrom_create_data_store_row(GpuContext *gcontext,
							  TupleDesc tupdesc, Size length,
							  bool file_mapped)
{
	pgstrom_data_store *pds;
	MemoryContext	gmcxt = gcontext->memcxt;
	const char	   *kds_fname;
	int				kds_fdesc;
	Size			kds_length;

	/* Is file-mapped? */
	kds_fname = (file_mapped ? get_pgstrom_temp_filename() : NULL);

	/* allocation of pds */
	pds = MemoryContextAllocZero(gmcxt, sizeof(pgstrom_data_store));
	dlist_push_tail(&gcontext->pds_list, &pds->chain);

	/* allocation of kds */
	pds->kds_length = (STROMALIGN(offsetof(kern_data_store,
										   colmeta[tupdesc->natts])) +
					   STROMALIGN(length));
	pds->kds_offset = 0;

	if (!kds_fname)
		pds->kds = MemoryContextAlloc(gmcxt, pds->kds_length);
	else
	{
		pds->kds_fname = MemoryContextStrdup(gmcxt, kds_fname);

		kds_fdesc = OpenTransientFile(kds_fname,
									  O_RDWR | O_CREAT | O_TRUNC | PG_BINARY,
									  0600);
		if (kds_fdesc < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create file-mapped data store \"%s\"",
							kds_fname)));
		

		if (ftruncate(kds_fdesc, pds->kds_length) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not truncate file \"%s\" to %zu: %m",
							kds_fname, kds_length)));

		pds->kds = mmap(NULL, kds_length,
						PROT_READ | PROT_WRITE,
						MAP_SHARED | MAP_POPULATE,
						kds_fdesc, 0);
		if (pds->kds == MAP_FAILED)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not mmap \"%s\" with length=%zu: %m",
							kds_fname, kds_length)));

		/* kds is still mapped - not to manage file desc by ourself */
		CloseTransientFile(kds_fdesc);
	}
	pds->ktoast = NULL;	/* never used */

	/*
	 * initialize common part of kds. Note that row-format cannot
	 * determine 'nrooms' preliminary, so INT_MAX instead.
	 */
	init_kern_data_store(pds->kds, tupdesc, pds->kds_length,
						 KDS_FORMAT_ROW, INT_MAX, false);
	return pds;
}

pgstrom_data_store *
pgstrom_create_data_store_slot(GpuContext *gcontext,
							   TupleDesc tupdesc, cl_uint nrooms,
							   bool internal_format,
							   pgstrom_data_store *ktoast)
{
	pgstrom_data_store *pds;
	MemoryContext	gmcxt = gcontext->memcxt;
	Size			kds_offset;
	int				kds_fdesc;

	/* allocation of pds */
	pds = MemoryContextAllocZero(gmcxt, sizeof(pgstrom_data_store));
	dlist_push_tail(&gcontext->pds_list, &pds->chain);

	/* allocation of kds */
	pds->kds_length = (STROMALIGN(offsetof(kern_data_store,
										   colmeta[tupdesc->natts])) +
					   (LONGALIGN(sizeof(bool) * tupdesc->natts) +
						LONGALIGN(sizeof(Datum) * tupdesc->natts)) * nrooms);

	if (!ktoast || !ktoast->kds_fname)
		pds->kds = MemoryContextAlloc(gmcxt, kds_length);
	else
	{
		/* append KDS after the ktoast file */
		kds_offset = TYPEALIGN(BLCKSZ, ktoast->kds_length);

		pds->kds_fname = MemoryContextStrdup(gmcxt, ktoast->kds_fname);
		kds_fdesc = OpenTransientFile(pds->kds_fname,
									  O_RDWR | PG_BINARY, 0600);
		if (kds_fdesc < 0)
			ereport(ERROR,
                    (errcode_for_file_access(),
					 errmsg("could not open file-mapped data store \"%s\"",
							pds->kds_fname)));

		if (ftruncate(kds_fdesc, kds_offset + pds->kds_length) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not truncate file \"%s\" to %zu: %m",
							pds->kds_fname, kds_offset + pds->kds_length)));

		pds->kds = mmap(NULL, kds_length,
						PROT_READ | PROT_WRITE,
						MAP_SHARED | MAP_POPULATE,
						kds_fdesc, kds_offset);
		if (pds->kds == MAP_FAILED)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not mmap \"%s\" with len/ofs=%zu/%zu: %m",
							pds->kds_fname, pds->kds_length, kds_offset)));

		/* kds is still mapped - not to manage file desc by ourself */
		CloseTransientFile(kds_fdesc);
	}
	pds->ktoast = ktoast;

	init_kern_data_store(pds->kds, tupdesc, kds_length,
						 KDS_FORMAT_SLOT, nrooms, internal_format);
	return pds;
}

/*
 * pgstrom_file_mmap_data_store
 *
 * This routine assume that dynamic background worker maps shared-file
 * to process it. So, here is no GpuContext.
 */
pgstrom_data_store *
pgstrom_file_mmap_data_store(const char *kds_fname,
                             Size kds_offset, Size kds_length)
{
	pgstrom_data_store *pds;
	int		kds_fdesc;

	Assert(kds_offset == TYPE_ALIGN(BLCKSZ, kds_offset));

	pds = palloc0(sizeof(pgstrom_data_store));
	pds->kds_fname = pstrdup(kds_fname);
	pds->kds_offset = kds_offset;
	pds->kds_length = kds_length;

	kds_fdesc = OpenTransientFile(pds->kds_fname,
								  O_RDWR | PG_BINARY, 0600);
	if (kds_fdesc < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file-mapped data store \"%s\"",
						pds->kds_fname)));

	if (ftruncate(kds_fdesc, kds_offset + kds_length) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not truncate file \"%s\" to %zu: %m",
						pds->kds_fname, kds_offset + kds_length)));

	pds->kds = mmap(NULL, kds_length,
					PROT_READ | PROT_WRITE,
					MAP_SHARED | MAP_POPULATE,
					kds_fdesc, kds_offset);
	if (pds->kds == MAP_FAILED)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not mmap \"%s\" with len/ofs=%zu/%zu: %m",
						pds->kds_fname, pds->kds_length, kds_offset)));

	/* kds is still mapped - not to manage file desc by ourself */
	CloseTransientFile(kds_fdesc);

	return pds;
}

void
pgstrom_file_unmap_data_store(pgstrom_data_store *pds)
{
	Assert(pds->kds_fname != NULL);

	if (munmap(pds->kds, pds->kds_length) != 0)
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not unmap file \"%s\" from %p-%p: %m",
						pds->kds_fname,
						(char *)pds->kds,
						(char *)pds->kds + pds->kds_length)));
}



int
pgstrom_data_store_insert_block(pgstrom_data_store *pds,
								Relation rel, BlockNumber blknum,
								Snapshot snapshot, bool page_prune)
{
	kern_data_store	*kds = pds->kds;
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
	Assert(kds->format == KDS_FORMAT_ROW);

	CHECK_FOR_INTERRUPTS();

	/* Load the target buffer */
	buffer = ReadBuffer(rel, blknum);

	/* Just like heapgetpage(), however, jobs we focus on is OLAP
	 * workload, so it's uncertain whether we should vacuum the page
	 * here.
	 */
	if (page_prune)
		heap_page_prune_opt(rel, buffer);

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
	max_consume = (STROMALIGN(offsetof(kern_data_store,
									   colmeta[kds->ncols])) +
				   sizeof(uint) * (kds->nitems + lines) +
				   offsetof(kern_tupitem, htup) * lines + BLCKSZ +
				   kds->usage);
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
	tup_index = (uint *)KERN_DATA_STORE_BODY(kds) + kds->nitems;
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
		kds->usage += LONGALIGN(tup.t_len);
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
 * pgstrom_data_store_insert_tuple
 *
 * It inserts a tuple on the data store. Unlike block read mode, we can use
 * this interface for both of row and column data store.
 */
bool
pgstrom_data_store_insert_tuple(pgstrom_data_store *pds,
								TupleTableSlot *slot)
{
	kern_data_store	   *kds = pds->kds;
	Size				consume;
	HeapTuple			tuple;
	uint			   *tup_index;
	kern_tupitem	   *tup_item;

	/* No room to store a new kern_rowitem? */
	if (kds->nitems >= kds->nrooms)
		return false;
	Assert(kds->ncols == slot->tts_tupleDescriptor->natts);

	if (kds->format != KDS_FORMAT_ROW)
		elog(ERROR, "Bug? unexpected data-store format: %d", kds->format);

	/* OK, put a record */
	tup_index = (uint *)KERN_DATA_STORE_BODY(kds);

	/* reference a HeapTuple in TupleTableSlot */
	tuple = ExecFetchSlotTuple(slot);

	/* check whether we have room for this tuple */
	consume = ((uintptr_t)(tup_index + kds->nitems + 1) -
			   (uintptr_t)(kds) +			/* from the head */
			   kds->usage +					/* from the tail */
			   LONGALIGN(offsetof(kern_tupitem, htup) +
						 tuple->t_len));	/* newly added */
	if (consume > kds->length)
		return false;

	kds->usage += LONGALIGN(offsetof(kern_tupitem, htup) + tuple->t_len);
	tup_item = (kern_tupitem *)((char *)kds + kds->length - kds->usage);
	tup_item->t_len = tuple->t_len;
	tup_item->t_self = tuple->t_self;
	memcpy(&tup_item->htup, tuple->t_data, tuple->t_len);
	tup_index[kds->nitems++] = (uintptr_t)tup_item - (uintptr_t)kds;

	return true;
}

/*
 * pgstrom_dump_data_store
 *
 * A utility routine that dumps properties of data store
 */
static inline void
__dump_datum(StringInfo buf, kern_colmeta *cmeta, char *datum)
{
	int		i;

	switch (cmeta->attlen)
	{
		case sizeof(char):
			appendStringInfo(buf, "%02x", *((char *)datum));
			break;
		case sizeof(short):
			appendStringInfo(buf, "%04x", *((short *)datum));
			break;
		case sizeof(int):
			appendStringInfo(buf, "%08x", *((int *)datum));
			break;
		case sizeof(long):
			appendStringInfo(buf, "%016lx", *((long *)datum));
			break;
		default:
			if (cmeta->attlen >= 0)
			{
				for (i=0; i < cmeta->attlen; i++)
					appendStringInfo(buf, "%02x", datum[i]);
			}
			else
			{
				Datum	vl_txt = DirectFunctionCall1(byteaout,
													 PointerGetDatum(datum));
				appendStringInfo(buf, "%s", DatumGetCString(vl_txt));
			}
	}
}

void
pgstrom_dump_data_store(pgstrom_data_store *pds)
{
	kern_data_store	   *kds = pds->kds;
	StringInfoData		buf;
	int					i, j;

	elog(INFO,
		 "pds {kds_fname=%s kds_fdesc=%d kds_length=%zu kds=%p ktoast=%p}",
		 pds->kds_fname, pds->kds_fdesc, pds->kds_length,
		 pds->kds, pds->ktoast);
	elog(INFO,
		 "kds {hostptr=%lu length=%u usage=%u ncols=%u nitems=%u nrooms=%u"
		 " format=%s tdhasoid=%s tdtypeid=%u tdtypmod=%d}",
		 kds->hostptr,
		 kds->length, kds->usage, kds->ncols, kds->nitems, kds->nrooms,
		 kds->format == KDS_FORMAT_ROW ? "row" :
		 kds->format == KDS_FORMAT_SLOT ? "slot" : "unknown",
		 kds->tdhasoid ? "true" : "false", kds->tdtypeid, kds->tdtypmod);
	for (i=0; i < kds->ncols; i++)
	{
		elog(INFO, "column[%d] "
			 "{attbyval=%d attalign=%d attlen=%d attnum=%d attcacheoff=%d}",
			 i,
			 kds->colmeta[i].attbyval,
			 kds->colmeta[i].attalign,
			 kds->colmeta[i].attlen,
			 kds->colmeta[i].attnum,
			 kds->colmeta[i].attcacheoff);
	}

	if (kds->format == KDS_FORMAT_ROW)
	{
		initStringInfo(&buf);
		for (i=0; i < kds->nitems; i++)
		{
			kern_tupitem *tup_item = KERN_DATA_STORE_TUPITEM(kds, i);
			HeapTupleHeaderData *htup = &tup_item->htup;
			size_t		offset = (uintptr_t)tup_item - (uintptr_t)kds;
			cl_int		natts = (htup->t_infomask2 & HEAP_NATTS_MASK);
			cl_int		curr = htup->t_hoff;
			char	   *datum;

			resetStringInfo(&buf);
			appendStringInfo(&buf, "htup[%d] @%zu natts=%u {",
							 i, offset, natts);
			for (j=0; j < kds->ncols; j++)
			{
				if (j > 0)
					appendStringInfo(&buf, ", ");
				if ((htup->t_infomask & HEAP_HASNULL) != 0 &&
					att_isnull(j, htup->t_bits))
					appendStringInfo(&buf, "null");
				else if (kds->colmeta[j].attlen > 0)
				{
					curr = TYPEALIGN(kds->colmeta[j].attalign, curr);
					datum = (char *)htup + curr;

					__dump_datum(&buf, &kds->colmeta[j], datum);
				}
				else
				{
					if (!VARATT_NOT_PAD_BYTE((char *)htup + curr))
						curr = TYPEALIGN(kds->colmeta[j].attalign, curr);
					datum = (char *)htup + curr;
					__dump_datum(&buf, &kds->colmeta[j], datum);
					curr += VARSIZE_ANY(datum);
				}
			}
			appendStringInfo(&buf, "}");
			elog(INFO, "%s", buf.data);
		}
		pfree(buf.data);
	}
}

void
pgstrom_init_datastore(void)
{
	DefineCustomIntVariable("pg_strom.chunk_size",
							"default size of pgstrom_data_store",
							NULL,
							&pgstrom_chunk_size_kb,
							15872,
							4096,
							MAX_KILOBYTES,
							PGC_USERSET,
							GUC_NOT_IN_SAMPLE | GUC_UNIT_KB,
							NULL, NULL, NULL);
	DefineCustomStringVariable("pg_strom.temp_tablespace",
							   "tablespace of file mapped data store",
							   NULL,
							   &pgstrom_temp_tablespace,
							   NULL,
							   PGC_USERSET,
							   GUC_NOT_IN_SAMPLE,
							   NULL, NULL, NULL);
}
