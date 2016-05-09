/*
 * cuda_gpusort.h
 *
 * GPU implementation of GPU bitonic sorting
 * --
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
#ifndef CUDA_GPUSORT_H
#define CUDA_GPUSORT_H

/*
 * GPU Accelerated Sorting
 *
 * It packs kern_parambu, status field, and kern_row_map structure
 * within a continuous memory area, to translate this chunk with
 * a single DMA call.
 */
typedef struct
{
	kern_errorbuf	kerror;
	cl_uint			segid;		/* segment id to be loaded */
	cl_uint			n_loaded;	/* number of items already loaded */
	kern_parambuf	kparams;
	/* input chunk shall be located just after the kparams*/
} kern_gpusort;

#define KERN_GPUSORT_PARAMBUF(kgpusort)			(&(kgpusort)->kparams)
#define KERN_GPUSORT_PARAMBUF_LENGTH(kgpusort)	((kgpusort)->kparams.length)
#define KERN_GPUSORT_KDS_IN(kgpusort)								\
	((kern_data_store *)((char *)KERN_GPUSORT_PARAMBUF(kgpusort) +	\
						 KERN_GPUSORT_PARAMBUF_LENGTH(kgpusort)))
#define KERN_GPUSORT_DMASEND_LENGTH(kgpusort)		\
	(offsetof(kern_gpusort, kparams) +				\
	 KERN_GPUSORT_PARAMBUF_LENGTH(kgpusort))
#define KERN_GPUSORT_DMARECV_LENGTH(kgpusort)						\
	offsetof(kern_gpusort, kparams)

/*
 * NOTE: Persistent segment - GpuSort have two persistent data structure
 * with longer duration than individual GpuSort tasks.
 * One is kern_resultbuf, the other is kern_data_store that keeps sorting
 * key and identifier of the original records.
 * The KDS can keep variable length fields using extra area, during the
 * sorting stage. Once bitonic sorting gets completed, extra area shall
 * be reused to store the identifier of the original records.
 * Thus, extra area has to have (sizeof(cl_ulong) * kds->nitems) at least.
 * If KDS tries to growth more than the threshold, it should be canceled.
 */

#ifdef __CUDACC__
/*
 * Sorting key comparison function - to be generated by PG-Strom
 * on the fly.
 */
STATIC_FUNCTION(cl_int)
gpusort_keycomp(kern_context *kcxt,
				kern_data_store *kds_slot,
				size_t x_index,
				size_t y_index);

/*
 * gpusort_projection
 *
 * It loads all the rows in the supplied chunk. If no space left on the
 * persistent segment, it tells the host code to switch new segment.
 */
KERNEL_FUNCTION(void)
gpusort_projection(kern_gpusort *kgpusort,
				   kern_resultbuf *kresults,
				   kern_data_store *kds_slot,
				   kern_data_store *kds_in)
{
	kern_parambuf  *kparams = KERN_GPUSORT_PARAMBUF(kgpusort);
	kern_context	kcxt;
	kern_tupitem   *tupitem = NULL;
	cl_uint		   *row_index;
	cl_bool			tup_isnull[GPUSORT_DEVICE_PROJECTION_NFIELDS];
	Datum			tup_values[GPUSORT_DEVICE_PROJECTION_NFIELDS];
	cl_bool		   *dest_isnull;
	Datum		   *dest_values;
	cl_uint			extra_len = 0;
	cl_uint			extra_ofs;
	cl_uint			extra_sum;
	char		   *extra_buf;
	char		   *extra_pos;
	cl_uint			nrows_sum;
	cl_uint			nrows_ofs;
	cl_uint			kds_index;
	cl_uint			i, ncols;
	__shared__ cl_uint extra_base;
	__shared__ cl_uint nrows_base;
	__shared__ cl_uint kresults_base;

	INIT_KERNEL_CONTEXT(&kcxt, gpusort_projection, kparams);

	/*
	 * Extract the sorting keys and put record identifier here.
	 * If the least bit of row_index[] (that is usually aligned to 8-bytes)
	 * is set, it means this record is already loaded to another sorting
	 * segment, so we shall ignore them
	 */
	row_index = KERN_DATA_STORE_ROWINDEX(kds_in);

	if (get_global_id() < kds_in->nitems &&
		(row_index[get_global_id()] & 0x01) == 0)
	{
		tupitem = (kern_tupitem *)
			((char *)kds_in + row_index[get_global_id()]);

		extra_len = deform_kern_heaptuple(&kcxt,
										  kds_in,
										  tupitem,
										  kds_slot->ncols,
										  false,	/* as device pointer */
										  tup_values,
										  tup_isnull);
		assert(extra_len == MAXALIGN(extra_len));
	}

	/* count resource consumption by this block */
	nrows_ofs = arithmetic_stairlike_add(tupitem != NULL ? 1 : 0, &nrows_sum);
	extra_ofs = arithmetic_stairlike_add(extra_len, &extra_sum);

	/*
	 * Quick bailout if we have no hope for buffer allocation on the current
	 * sorting segment, prior to atomic operations.
	 */
	if (KERN_DATA_STORE_SLOT_LENGTH(kds_slot,
									kds_slot->nitems + nrows_sum)
		+ kds_slot->usage + extra_sum > kds_slot->length)
	{
		STROM_SET_ERROR(&kcxt.e, StromError_DataStoreNoSpace);
		goto out;
	}

	/* buffer allocation on the current sorting block, by atomic operations */
	if (get_local_id() == 0)
	{
		extra_base = atomicAdd(&kds_slot->usage, extra_sum);
		nrows_base = atomicAdd(&kds_slot->nitems, nrows_sum);
	}
	__syncthreads();

	/* confirmation of buffer usage */
	if (KERN_DATA_STORE_SLOT_LENGTH(kds_slot,
									nrows_base + nrows_sum) +
		+ (extra_base + extra_sum) > kds_slot->length)
	{
		STROM_SET_ERROR(&kcxt.e, StromError_DataStoreNoSpace);
		goto out;
	}

	/* OK, we could get buffer space successfully */
	kds_index = nrows_base + nrows_ofs;
	extra_buf = ((char *)kds_slot +
				 kds_slot->length - (extra_base + extra_sum) + extra_ofs);
	__syncthreads();

	/*
	 * Get space on kresults
	 */
	if (get_local_id() == 0)
		kresults_base = atomicAdd(&kresults->nitems, nrows_sum);
	__syncthreads();
	if (kresults_base + nrows_sum > kresults->nrooms)
	{
		STROM_SET_ERROR(&kcxt.e, StromError_DataStoreNoSpace);
		goto out;
	}
	kresults->results[kresults_base + nrows_ofs] = kds_index;
	__syncthreads();

	/* Copy the values/isnull to the sorting segment */
	if (tupitem != NULL)
	{
		ncols = kds_slot->ncols;
		dest_isnull = KERN_DATA_STORE_ISNULL(kds_slot, kds_index);
		dest_values = KERN_DATA_STORE_VALUES(kds_slot, kds_index);
		extra_pos = extra_buf;

		for (i=0; i < ncols; i++)
		{
			kern_colmeta	cmeta = kds_slot->colmeta[i];

			if (tup_isnull[i])
			{
				dest_isnull[i] = true;
				dest_values[i] = (Datum) 0;
			}
			else
			{
				dest_isnull[i] = false;

				if (cmeta.attbyval)
				{
					/* fixed length inline variables */
					dest_values[i] = tup_values[i];
				}
				else if (cmeta.attlen > 0)
				{
					/* fixed length indirect variables */
					extra_pos = (char *)TYPEALIGN(cmeta.attlen, extra_pos);
					assert(extra_pos + cmeta.attlen <= extra_buf + extra_len);
					memcpy(extra_pos,
						   DatumGetPointer(tup_values[i]),
						   cmeta.attlen);
					dest_values[i] = PointerGetDatum(extra_pos);
					extra_pos += cmeta.attlen;
				}
				else
				{
					/* varlena datum */
					cl_uint		vl_len = VARSIZE_ANY(tup_values[i]);
					extra_pos = (char *)TYPEALIGN(cmeta.attlen, extra_pos);
					assert(extra_pos + vl_len <= extra_buf + extra_len);
					hoge;
					memcpy(extra_pos,
						   DatumGetPointer(tup_values[i]),
						   vl_len);
					dest_values[i] = PointerGetDatum(extra_pos);
#if 0
					extra_pos += vl_len;
#else
					dest_isnull[i] = true;
#endif
				}
			}
		}

		/*
		 * Invalidate the row_index, to inform we could successfully move
		 * this record to kds_slot.
		 */
		row_index[get_global_id()] |= 0x00000001U;
	}
	/* inform host-side the number of rows actually moved */
	if (get_local_id() == 0)
		atomicAdd(&kgpusort->n_loaded, nrows_sum);
	__syncthreads();

out:
	kern_writeback_error_status(&kgpusort->kerror, kcxt.e);
}

/*
 * gpusort_bitonic_local
 *
 * It tries to apply each steps of bitonic-sorting until its unitsize
 * reaches the workgroup-size (that is expected to power of 2).
 */
KERNEL_FUNCTION_MAXTHREADS(void)
gpusort_bitonic_local(kern_gpusort *kgpusort,
					  kern_resultbuf *kresults,
					  kern_data_store *kds_slot)
{
	kern_parambuf  *kparams = KERN_GPUSORT_PARAMBUF(kgpusort);
	kern_context	kcxt;
	cl_uint		   *localIdx = SHARED_WORKMEM(cl_uint);
	cl_uint			nitems = kresults->nitems;
	size_t			part_id = get_global_id() / get_local_size();
	size_t			part_size = 2 * get_local_size();	/* Partition Size */
	size_t			part_base = part_id * part_size;	/* Base of partition */
	size_t			blockSize;
	size_t			unitSize;
	size_t			i;

	INIT_KERNEL_CONTEXT(&kcxt, gpusort_bitonic_local, kparams);

	/* Load index to localIdx[] */
	if (part_base + part_size > nitems)
		part_size = nitems - part_base;
	for (i = get_local_id(); i < part_size; i += get_local_size())
		localIdx[i] = kresults->results[part_base + i];
	__syncthreads();

	/* bitonic sorting */
	for (blockSize = 2; blockSize <= part_size; blockSize *= 2)
	{
		for (unitSize = blockSize; unitSize >= 2; unitSize /= 2)
        {
			size_t	unitMask		= unitSize - 1;
			size_t	halfUnitSize	= unitSize / 2;
			bool	reversing  = (unitSize == blockSize ? true : false);
			size_t	idx0 = ((get_local_id() / halfUnitSize) * unitSize
							+ get_local_id() % halfUnitSize);
            size_t	idx1 = ((reversing == true)
							? ((idx0 & ~unitMask) | (~idx0 & unitMask))
							: (halfUnitSize + idx0));

            if(idx1 < part_size)
			{
				cl_uint		pos0 = localIdx[idx0];
				cl_uint		pos1 = localIdx[idx1];

				if (gpusort_keycomp(&kcxt, kds_slot, pos0, pos1) > 0)
				{
					/* swap them */
					localIdx[idx0] = pos1;
					localIdx[idx1] = pos0;
				}
			}
			__syncthreads();
		}
	}
	/* write back local sorted result */
	for (i = get_local_id(); i < part_size; i += get_local_size())
		kresults->results[part_base + i] = localIdx[i];
	__syncthreads();

	/* any error during run-time? */
	kern_writeback_error_status(&kresults->kerror, kcxt.e);
}

/*
 * gpusort_bitonic_step
 *
 * It tries to apply individual steps of bitonic-sorting for each step,
 * but does not have restriction of workgroup size. The host code has to
 * control synchronization of each step not to overrun.
 */
KERNEL_FUNCTION_MAXTHREADS(void)
gpusort_bitonic_step(kern_gpusort *kgpusort,
					 kern_resultbuf *kresults,
					 kern_data_store *kds_slot,
					 size_t unitsz,
					 cl_bool reversing)
{
	kern_parambuf  *kparams = KERN_GPUSORT_PARAMBUF(kgpusort);
	kern_context	kcxt;
	cl_uint			nitems = kresults->nitems;
	size_t			halfUnitSize = unitsz / 2;
	size_t			unitMask = unitsz - 1;
	cl_int			idx0, idx1;
	cl_int			pos0, pos1;

	INIT_KERNEL_CONTEXT(&kcxt, gpusort_bitonic_step, kparams);

	idx0 = ((get_global_id() / halfUnitSize) * unitsz
			+ get_global_id() % halfUnitSize);
	idx1 = (reversing
			? ((idx0 & ~unitMask) | (~idx0 & unitMask))
			: (idx0 + halfUnitSize));
	if (idx1 >= nitems)
		goto out;

	pos0 = kresults->results[idx0];
	pos1 = kresults->results[idx1];
	if (gpusort_keycomp(&kcxt, kds_slot, pos0, pos1) > 0)
	{
		/* swap them */
		kresults->results[idx0] = pos1;
		kresults->results[idx1] = pos0;
	}
out:
	kern_writeback_error_status(&kresults->kerror, kcxt.e);
}

/*
 * gpusort_bitonic_merge
 *
 * It handles the merging step of bitonic-sorting if unitsize becomes less
 * than or equal to the workgroup size.
 */
KERNEL_FUNCTION_MAXTHREADS(void)
gpusort_bitonic_merge(kern_gpusort *kgpusort,
					  kern_resultbuf *kresults,
					  kern_data_store *kds_slot)
{
	kern_parambuf  *kparams = KERN_GPUSORT_PARAMBUF(kgpusort);
	kern_context	kcxt;
	cl_int		   *localIdx = SHARED_WORKMEM(cl_int);
	cl_uint			nitems = kresults->nitems;
	size_t			part_id = get_global_id() / get_local_size();
	size_t			part_size = 2 * get_local_size();	/* partition Size */
	size_t			part_base = part_id * part_size;	/* partition Base */
	size_t			blockSize = part_size;
	size_t			unitSize = part_size;
	size_t			i, temp;

	INIT_KERNEL_CONTEXT(&kcxt, gpusort_bitonic_merge, kparams);

	/* Load index to localIdx[] */
	if (part_base + part_size > nitems)
		part_size = nitems - part_base;
	for (i = get_local_id(); i < part_size && i < 2048; i += get_local_size())
		localIdx[i] = kresults->results[part_base + i];
	__syncthreads();

	/* merge two sorted blocks */
	for (unitSize = blockSize; unitSize >= 2; unitSize /= 2)
	{
		size_t	halfUnitSize = unitSize / 2;
		size_t	idx0, idx1;

		idx0 = (get_local_id() / halfUnitSize * unitSize
				+ get_local_id() % halfUnitSize);
		idx1 = halfUnitSize + idx0;

        if (idx1 < part_size)
		{
			size_t	pos0 = localIdx[idx0];
			size_t	pos1 = localIdx[idx1];

			if (gpusort_keycomp(&kcxt, kds_slot, pos0, pos1) > 0)
			{
				/* swap them */
				localIdx[idx0] = pos1;
                localIdx[idx1] = pos0;
			}
		}
		__syncthreads();
	}
	/* Save index to kresults[] */
	for (i = get_local_id(); i < part_size; i += get_local_size())
		kresults->results[part_base + i] = localIdx[i];
	__syncthreads();

	kern_writeback_error_status(&kresults->kerror, kcxt.e);
}

KERNEL_FUNCTION(void)
gpusort_fixup_pointers(kern_gpusort *kgpusort,
					   kern_resultbuf *kresults,
					   kern_data_store *kds_slot)
{
	kern_parambuf  *kparams = KERN_GPUSORT_PARAMBUF(kgpusort);
	kern_context	kcxt;
	cl_uint			kds_index;
	Datum		   *tup_values;
	cl_bool		   *tup_isnull;
	cl_int			i;

	INIT_KERNEL_CONTEXT(&kcxt, gpusort_fixup_pointers, kparams);

	if (get_global_id() < kresults->nitems)
	{
		kds_index = kresults->results[get_global_id()];
		assert(kds_index < kds_slot->nitems);

		tup_values = KERN_DATA_STORE_VALUES(kds_slot, kds_index);
		tup_isnull = KERN_DATA_STORE_ISNULL(kds_slot, kds_index);

		for (i=0; i < kds_slot->ncols; i++)
		{
			kern_colmeta	cmeta = kds_slot->colmeta[i];

			if (cmeta.attbyval)
				continue;
			if (tup_isnull[i])
				continue;
			tup_values[i] = ((hostptr_t)tup_values[i] -
							 (hostptr_t)&kds_slot->hostptr +
							 kds_slot->hostptr);
		}
	}
out:
	kern_writeback_error_status(&kgpusort->kerror, kcxt.e);
}

KERNEL_FUNCTION(void)
gpusort_main(kern_gpusort *kgpusort,
			 kern_resultbuf *kresults,
			 kern_data_store *kds_slot)
{
	kern_parambuf  *kparams = KERN_GPUSORT_PARAMBUF(kgpusort);
	kern_context	kcxt;
	const void	   *kern_funcs[3];
	void		  **kern_args;
	cl_uint			nitems = kresults->nitems;
	cl_uint			nhalf;
	cl_uint			__block_sz = UINT_MAX;
	dim3			grid_sz;
	dim3			block_sz;
	cl_uint			i, j;
	cudaError_t		status = cudaSuccess;


	INIT_KERNEL_CONTEXT(&kcxt, gpusort_main, kparams);

	// MEMO: error code shall be put on kresults, not kgpusort
	// because NoSpace error should not prevent sort the segment

	/*
	 * NOTE: Because of the bitonic sorting algorithm characteristics,
	 * block size has to be 2^N value and common in the three kernel
	 * functions below, thus we pick up the least one. Usually it shall
	 * be hardware's maximum available block size because kernel functions
	 * are declared with KERNEL_FUNCTION_MAXTHREADS.
	 */
	kern_funcs[0] = (const void *)gpusort_bitonic_local;
	kern_funcs[1] = (const void *)gpusort_bitonic_step;
	kern_funcs[2] = (const void *)gpusort_bitonic_merge;
	for (i=0; i < 3; i++)
	{
		status = pgstrom_largest_workgroup_size(&grid_sz,
												&block_sz,
												kern_funcs[i],
												(nitems + 1) / 2,
												2 * sizeof(cl_uint));
		if (status != cudaSuccess)
		{
			STROM_SET_RUNTIME_ERROR(&kcxt.e, status);
			goto out;
		}
		__block_sz = Min(__block_sz, 1 << (get_next_log2(block_sz.x + 1) - 1));
	}
	assert((__block_sz & (__block_sz - 1)) == 0);	/* to be 2^N */
	block_sz.x = __block_sz;
	block_sz.y = 1;
	block_sz.z = 1;

	/* nhalf is the least power of two value that is larger than or
	 * equal to half of the nitems. */
	nhalf = 1UL << (get_next_log2(nitems + 1) - 1);

	/*
	 * KERNEL_FUNCTION_MAXTHREADS(void)
	 * gpusort_bitonic_local(kern_gpusort *kgpusort,
	 *                       kern_resultbuf *kresults,
	 *                       kern_data_store *kds_slot)
	 */
	kern_args = (void **)
		cudaGetParameterBuffer(sizeof(void *),
							   sizeof(void *) * 3);
	if (!kern_args)
	{
		STROM_SET_ERROR(&kcxt.e, StromError_OutOfKernelArgs);
		goto out;
	}
	kern_args[0] = kgpusort;
	kern_args[1] = kresults;
	kern_args[2] = kds_slot;

	grid_sz.x = ((nitems + 1) / 2 + block_sz.x - 1) / block_sz.x;
	grid_sz.y = 1;
	grid_sz.z = 1;
	status = cudaLaunchDevice((void *)gpusort_bitonic_local,
							  kern_args, grid_sz, block_sz,
							  2 * sizeof(cl_uint) * block_sz.x,
							  NULL);
	if (status != cudaSuccess)
	{
		STROM_SET_RUNTIME_ERROR(&kcxt.e, status);
		goto out;
	}

	status = cudaDeviceSynchronize();
	if (status != cudaSuccess)
	{
		STROM_SET_RUNTIME_ERROR(&kcxt.e, status);
		goto out;
	}

	/* Inter blocks bitonic sorting */
	for (i = block_sz.x; i < nhalf; i *= 2)
	{
		for (j = 2 * i; j > block_sz.x; j /= 2)
		{
			/*
			 * KERNEL_FUNCTION_MAXTHREADS(void)
			 * gpusort_bitonic_step(kern_gpusort *kgpusort,
			 *                       kern_resultbuf *kresults,
			 *                       kern_data_store *kds_slot)
			 *                       size_t unitsz,
			 *                       cl_bool reversing)
			 */
			size_t		unitsz = 2 * j;
			cl_bool		reversing = ((j == 2 * i) ? true : false);
			size_t		work_size;

			kern_args = (void **)
				cudaGetParameterBuffer(sizeof(void *),
									   sizeof(void *) * 5);
			if (!kern_args)
			{
				STROM_SET_ERROR(&kcxt.e, StromError_OutOfKernelArgs);
				goto out;
			}
			kern_args[0] = kgpusort;
			kern_args[1] = kresults;
			kern_args[2] = kds_slot;
			kern_args[3] = (void *)unitsz;
			kern_args[4] = (void *)reversing;

			work_size = (((nitems + unitsz - 1) / unitsz) * unitsz / 2);
			grid_sz.x = (work_size + block_sz.x - 1) / block_sz.x;
			grid_sz.y = 1;
			grid_sz.z = 1;
			status = cudaLaunchDevice((void *)gpusort_bitonic_step,
									  kern_args, grid_sz, block_sz,
									  2 * sizeof(cl_uint) * block_sz.x,
									  NULL);
			if (status != cudaSuccess)
			{
				STROM_SET_RUNTIME_ERROR(&kcxt.e, status);
				goto out;
			}

			status = cudaDeviceSynchronize();
			if (status != cudaSuccess)
			{
				STROM_SET_RUNTIME_ERROR(&kcxt.e, status);
				goto out;
			}
		}
		/*
		 * KERNEL_FUNCTION_MAXTHREADS(void)
		 * gpusort_bitonic_merge(kern_gpusort *kgpusort,
		 *                       kern_resultbuf *kresults,
		 *                       kern_data_store *kds_slot)
		 */
		kern_args = (void **)
			cudaGetParameterBuffer(sizeof(void *),
								   sizeof(void *) * 3);
		if (!kern_args)
		{
			STROM_SET_ERROR(&kcxt.e, StromError_OutOfKernelArgs);
			goto out;
		}
		kern_args[0] = kgpusort;
		kern_args[1] = kresults;
		kern_args[2] = kds_slot;

		grid_sz.x = ((nitems + 1) / 2 + block_sz.x - 1) / block_sz.x;
		grid_sz.y = 1;
		grid_sz.z = 1;
		status = cudaLaunchDevice((void *)gpusort_bitonic_merge,
								  kern_args, grid_sz, block_sz,
								  2 * sizeof(cl_uint) * block_sz.x,
								  NULL);
		if (status != cudaSuccess)
		{
			STROM_SET_RUNTIME_ERROR(&kcxt.e, status);
			goto out;
		}

		status = cudaDeviceSynchronize();
		if (status != cudaSuccess)
		{
			STROM_SET_RUNTIME_ERROR(&kcxt.e, status);
			goto out;
		}
	}

	/*
	 * OK, kresults shall hold a sorted indexes at this timing.
	 * For receive DMA, we will make an array of record-id.
	 *
	 * KERNEL_FUNCTION(void)
	 * gpusort_fixup_pointers(kern_gpusort *kgpusort,
	 *                        kern_resultbuf *kresults,
	 *                        kern_data_store *kds_slot)
	 */
	kern_args = (void **)
		cudaGetParameterBuffer(sizeof(void *),
							   sizeof(void *) * 3);
	if (!kern_args)
	{
		STROM_SET_ERROR(&kcxt.e, StromError_OutOfKernelArgs);
		goto out;
	}
	kern_args[0] = kgpusort;
	kern_args[1] = kresults;
	kern_args[2] = kds_slot;

	status = pgstrom_optimal_workgroup_size(&grid_sz,
											&block_sz,
											(const void *)
											gpusort_fixup_pointers,
											kresults->nitems,
											sizeof(cl_uint));
	if (status != cudaSuccess)
	{
		STROM_SET_RUNTIME_ERROR(&kcxt.e, status);
		goto out;
	}

	status = cudaLaunchDevice((void *)gpusort_fixup_pointers,
							  kern_args, grid_sz, block_sz,
							  sizeof(cl_uint) * block_sz.x,
							  NULL);
	if (status != cudaSuccess)
	{
		STROM_SET_RUNTIME_ERROR(&kcxt.e, status);
		goto out;
	}

	status = cudaDeviceSynchronize();
	if (status != cudaSuccess)
	{
		STROM_SET_RUNTIME_ERROR(&kcxt.e, status);
		goto out;
	}
out:
	kern_writeback_error_status(&kresults->kerror, kcxt.e);
}

#endif	/* __CUDACC__ */
#endif	/* CUDA_GPUSORT_H */
