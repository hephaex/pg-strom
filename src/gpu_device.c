/*
 * gpu_device.c
 *
 * Routines to collect GPU device information.
 * ----
 * Copyright 2011-2018 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014-2018 (C) The PG-Strom Development Team
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
#include "pg_strom.h"

/* variable declarations */
DevAttributes	   *devAttrs = NULL;
cl_int				numDevAttrs = 0;
cl_ulong			devComputeCapability = UINT_MAX;
cl_uint				devBaselineMaxThreadsPerBlock = UINT_MAX;

/* catalog of device attributes */
typedef enum {
	DEVATTRKIND__INT,
	DEVATTRKIND__BYTES,
	DEVATTRKIND__KB,
	DEVATTRKIND__KHZ,
	DEVATTRKIND__COMPUTEMODE,
	DEVATTRKIND__BOOL,
	DEVATTRKIND__BITS,
} DevAttrKind;

static struct {
	CUdevice_attribute	attr_id;
	DevAttrKind	attr_kind;
	size_t		attr_offset;
	const char *attr_desc;
} DevAttrCatalog[] = {
#define DEV_ATTR(LABEL,KIND,a,DESC)				\
	{ CU_DEVICE_ATTRIBUTE_##LABEL,				\
	  DEVATTRKIND__##KIND,						\
	  offsetof(struct DevAttributes, LABEL),	\
	  DESC },
#include "device_attrs.h"
#undef DEV_ATTR
};

/* declaration */
Datum pgstrom_device_info(PG_FUNCTION_ARGS);
Datum pgstrom_gpu_device_name(PG_FUNCTION_ARGS);
Datum pgstrom_gpu_global_memsize(PG_FUNCTION_ARGS);
Datum pgstrom_gpu_max_blocksize(PG_FUNCTION_ARGS);
Datum pgstrom_gpu_warp_size(PG_FUNCTION_ARGS);
Datum pgstrom_gpu_max_shared_memory_perblock(PG_FUNCTION_ARGS);
Datum pgstrom_gpu_num_registers_perblock(PG_FUNCTION_ARGS);
Datum pgstrom_gpu_num_multiptocessors(PG_FUNCTION_ARGS);
Datum pgstrom_gpu_num_cuda_cores(PG_FUNCTION_ARGS);
Datum pgstrom_gpu_cc_major(PG_FUNCTION_ARGS);
Datum pgstrom_gpu_cc_minor(PG_FUNCTION_ARGS);
Datum pgstrom_gpu_pci_id(PG_FUNCTION_ARGS);

/*
 * pgstrom_collect_gpu_device
 */
static void
pgstrom_collect_gpu_device(void)
{
	StringInfoData str;
	char	   *cmdline;
	char		linebuf[2048];
	FILE	   *filp;
	char	   *tok_attr;
	char	   *tok_val;
	char	   *pos;
	char	   *cuda_runtime_version = NULL;
	char	   *nvidia_driver_version = NULL;
	int			num_devices = -1;	/* total num of GPUs; incl legacy models */
	int			i, j;

	initStringInfo(&str);

	cmdline = psprintf("%s -md", CMD_GPUINFO_PATH);
	filp = OpenPipeStream(cmdline, PG_BINARY_R);

	while (fgets(linebuf, sizeof(linebuf), filp) != NULL)
	{
		/* trim '\n' on the tail */
		pos = linebuf + strlen(linebuf);
		while (pos > linebuf && isspace(*--pos))
			*pos = '\0';
		/* empty line? */
		if (linebuf[0] == '\0')
			continue;

		tok_attr = strchr(linebuf, ':');
		if (!tok_attr)
			elog(ERROR, "unexpected gpuinfo -md format");
		*tok_attr++ = '\0';

		tok_val = strchr(tok_attr, '=');
		if (!tok_val)
			elog(ERROR, "incorrect gpuinfo -md format");
		*tok_val++ = '\0';

		if (strcmp(linebuf, "PLATFORM") == 0)
		{
			if (strcmp(tok_attr, "CUDA_RUNTIME_VERSION") == 0)
				cuda_runtime_version = pstrdup(tok_val);
			else if (strcmp(tok_attr, "NVIDIA_DRIVER_VERSION") == 0)
				nvidia_driver_version = pstrdup(tok_val);
			else if (strcmp(tok_attr, "NUMBER_OF_DEVICES") == 0)
			{
				num_devices = atoi(tok_val);
				if (num_devices < 0)
					elog(ERROR, "NUMBER_OF_DEVICES is not correct");
			}
			else
				elog(ERROR, "unknown PLATFORM attribute");
		}
		else if (strncmp(linebuf, "DEVICE", 6) == 0)
		{
			int		dindex = atoi(linebuf + 6);

			if (!devAttrs)
			{
				if (!cuda_runtime_version ||
					!nvidia_driver_version ||
					num_devices < 0)
					elog(ERROR, "incorrect gpuinfo -md format");
				Assert(num_devices > 0);
				devAttrs = MemoryContextAllocZero(TopMemoryContext,
												  sizeof(DevAttributes) *
												  num_devices);
			}

			if (dindex < 0 || dindex >= num_devices)
				elog(ERROR, "device index out of range");

#define DEV_ATTR(LABEL,a,b,c)						\
			else if (strcmp(tok_attr, #LABEL) == 0)	\
				devAttrs[dindex].LABEL = atoi(tok_val);

			if (strcmp(tok_attr, "DEVICE_ID") == 0)
			{
				if (dindex != atoi(tok_val))
					elog(ERROR, "incorrect gpuinfo -md format");
				devAttrs[dindex].DEV_ID = dindex;
			}
			else if (strcmp(tok_attr, "DEVICE_NAME") == 0)
			{
				strncpy(devAttrs[dindex].DEV_NAME, tok_val,
						sizeof(devAttrs[dindex].DEV_NAME));
			}
			else if (strcmp(tok_attr, "GLOBAL_MEMORY_SIZE") == 0)
				devAttrs[dindex].DEV_TOTAL_MEMSZ = atol(tok_val);
#include "device_attrs.h"
			else
				elog(ERROR, "incorrect gpuinfo -md format");
#undef DEV_ATTR
		}
		else
			elog(ERROR, "unexpected gpuinfo -md input:\n%s", linebuf);
	}
	ClosePipeStream(filp);

	for (i=0, j=0; i < num_devices; i++)
	{
		DevAttributes  *dattrs = &devAttrs[i];
		int				compute_capability;

		/* Recommend to use Pascal or later */
		if (dattrs->COMPUTE_CAPABILITY_MAJOR < 6)
		{
			elog(LOG, "PG-Strom: GPU%d %s - CC %d.%d is not supported",
				 dattrs->DEV_ID,
				 dattrs->DEV_NAME,
				 dattrs->COMPUTE_CAPABILITY_MAJOR,
				 dattrs->COMPUTE_CAPABILITY_MINOR);
			continue;
		}

		/* Update the baseline device capability */
		compute_capability = (dattrs->COMPUTE_CAPABILITY_MAJOR * 10 +
							  dattrs->COMPUTE_CAPABILITY_MINOR);
		devComputeCapability = Min(devComputeCapability,
								   compute_capability);
		devBaselineMaxThreadsPerBlock = Min(devBaselineMaxThreadsPerBlock,
											dattrs->MAX_THREADS_PER_BLOCK);

		/* Determine CORES_PER_MPU by CC */
		if (dattrs->COMPUTE_CAPABILITY_MAJOR == 1)
			dattrs->CORES_PER_MPU = 8;
		else if (dattrs->COMPUTE_CAPABILITY_MAJOR == 2)
		{
			if (dattrs->COMPUTE_CAPABILITY_MINOR == 0)
				dattrs->CORES_PER_MPU = 32;
			else if (dattrs->COMPUTE_CAPABILITY_MINOR == 1)
				dattrs->CORES_PER_MPU = 48;
			else
				dattrs->CORES_PER_MPU = -1;
		}
		else if (dattrs->COMPUTE_CAPABILITY_MAJOR == 3)
			dattrs->CORES_PER_MPU = 192;
		else if (dattrs->COMPUTE_CAPABILITY_MAJOR == 5)
			dattrs->CORES_PER_MPU = 128;
		else if (dattrs->COMPUTE_CAPABILITY_MAJOR == 6)
		{
			if (dattrs->COMPUTE_CAPABILITY_MINOR == 0)
				dattrs->CORES_PER_MPU = 64;
			else
				dattrs->CORES_PER_MPU = 128;
		}
		else if (dattrs->COMPUTE_CAPABILITY_MAJOR == 7)
			dattrs->CORES_PER_MPU = 64;
		else
			dattrs->CORES_PER_MPU = 0;	/* unknown */

		/* Log brief CUDA device properties */
		resetStringInfo(&str);
		appendStringInfo(&str, "GPU%d %s (",
						 dattrs->DEV_ID, dattrs->DEV_NAME);
		if (dattrs->CORES_PER_MPU > 0)
			appendStringInfo(&str, "%d CUDA cores",
							 dattrs->CORES_PER_MPU *
							 dattrs->MULTIPROCESSOR_COUNT);
		else
			appendStringInfo(&str, "%d SMs",
							 dattrs->MULTIPROCESSOR_COUNT);
		appendStringInfo(&str, "; %dMHz, L2 %dkB)",
						 dattrs->CLOCK_RATE / 1000,
						 dattrs->L2_CACHE_SIZE >> 10);
		if (dattrs->DEV_TOTAL_MEMSZ > (4UL << 30))
			appendStringInfo(&str, ", RAM %.2fGB",
							 ((double)dattrs->DEV_TOTAL_MEMSZ /
							  (double)(1UL << 30)));
		else
			appendStringInfo(&str, ", RAM %zuMB",
							 dattrs->DEV_TOTAL_MEMSZ >> 20);
		if (dattrs->MEMORY_CLOCK_RATE > (1UL << 20))
			appendStringInfo(&str, " (%dbits, %.2fGHz)",
							 dattrs->GLOBAL_MEMORY_BUS_WIDTH,
							 ((double)dattrs->MEMORY_CLOCK_RATE /
							  (double)(1UL << 20)));
		else
			appendStringInfo(&str, " (%dbits, %dMHz)",
							 dattrs->GLOBAL_MEMORY_BUS_WIDTH,
							 dattrs->MEMORY_CLOCK_RATE >> 10);
		appendStringInfo(&str, ", CC %d.%d",
						 dattrs->COMPUTE_CAPABILITY_MAJOR,
						 dattrs->COMPUTE_CAPABILITY_MINOR);
		elog(LOG, "PG-Strom: %s", str.data);

		if (i != j)
			memcpy(&devAttrs[j], &devAttrs[i], sizeof(DevAttributes));

		j++;
	}
	Assert(j <= num_devices);
	numDevAttrs = j;
	if (numDevAttrs == 0)
		elog(ERROR, "PG-Strom: no supported GPU devices found");
}

/*
 * pgstrom_init_gpu_device
 */
void
pgstrom_init_gpu_device(void)
{
	static char	   *cuda_visible_devices = NULL;

	/*
	 * Set CUDA_VISIBLE_DEVICES environment variable prior to CUDA
	 * initialization
	 */
	DefineCustomStringVariable("pg_strom.cuda_visible_devices",
							   "CUDA_VISIBLE_DEVICES environment variables",
							   NULL,
							   &cuda_visible_devices,
							   NULL,
							   PGC_POSTMASTER,
							   GUC_NOT_IN_SAMPLE,
							   NULL, NULL, NULL);
	if (cuda_visible_devices)
	{
		if (setenv("CUDA_VISIBLE_DEVICES", cuda_visible_devices, 1) != 0)
			elog(ERROR, "failed to set CUDA_VISIBLE_DEVICES");
	}
	/* collect device properties by gpuinfo command */
	pgstrom_collect_gpu_device();
}

/*
 * optimal_workgroup_size - calculates the optimal block size
 * according to the function and device attributes
 */
static __thread size_t __dynamic_shmem_per_block;
static __thread size_t __dynamic_shmem_per_thread;

static size_t
blocksize_to_shmemsize_helper(int blocksize)
{
	return (__dynamic_shmem_per_block +
			__dynamic_shmem_per_thread * (size_t)blocksize);
}

void
optimal_workgroup_size(size_t *p_grid_size,
					   size_t *p_block_size,
					   CUfunction function,
					   CUdevice device,
					   size_t nitems,
					   size_t dynamic_shmem_per_block,
					   size_t dynamic_shmem_per_thread)
{
	cl_int		min_grid_sz;
	cl_int		max_block_sz;
	cl_int		warpSize;
	CUresult	rc;

	rc = cuDeviceGetAttribute(&warpSize,
							  CU_DEVICE_ATTRIBUTE_WARP_SIZE,
							  device);
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "failed on cuDeviceGetAttribute: %s", errorText(rc));

	__dynamic_shmem_per_block = dynamic_shmem_per_block;
	__dynamic_shmem_per_thread = dynamic_shmem_per_thread;
	rc = cuOccupancyMaxPotentialBlockSize(&min_grid_sz,
										  &max_block_sz,
										  function,
										  blocksize_to_shmemsize_helper,
										  0,
										  Min((size_t)nitems,
											  (size_t)INT_MAX));
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "failed on cuOccupancyMaxPotentialBlockSize: %s",
			 errorText(rc));

	if ((size_t)max_block_sz * (size_t)INT_MAX < nitems)
		elog(ERROR, "to large nitems (%zu) to launch kernel (blockSz=%d)",
			 nitems, max_block_sz);

	*p_block_size = (size_t)max_block_sz;
	*p_grid_size  = (nitems + (size_t)max_block_sz - 1) / (size_t)max_block_sz;
}

/*
 * largest_workgroup_size - calculate the block size maximum available
 */
void
largest_workgroup_size(size_t *p_grid_size,
					   size_t *p_block_size,
					   CUfunction function,
					   CUdevice device,
					   size_t nitems,
					   size_t dynamic_shmem_per_block,
					   size_t dynamic_shmem_per_thread)
{
	cl_int		warpSize;
	cl_int		maxBlockSize;
	cl_int		staticShmemSize;
	cl_int		maxShmemSize;
	cl_int		shmemSizeTotal;
	CUresult	rc;

	/* get max number of thread per block on this kernel function */
	rc = cuFuncGetAttribute(&maxBlockSize,
							CU_FUNC_ATTRIBUTE_MAX_THREADS_PER_BLOCK,
							function);
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "failed on cuFuncGetAttribute: %s", errorText(rc));

	/* get statically allocated shared memory */
	rc = cuFuncGetAttribute(&staticShmemSize,
							CU_FUNC_ATTRIBUTE_SHARED_SIZE_BYTES,
							function);
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "failed on cuFuncGetAttribute: %s", errorText(rc));

	/* get device warp size */
	rc = cuDeviceGetAttribute(&warpSize,
							  CU_DEVICE_ATTRIBUTE_WARP_SIZE,
							  device);
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "failed on cuDeviceGetAttribute: %s", errorText(rc));

	/* get device limit of thread/block ratio */
	rc = cuDeviceGetAttribute(&maxShmemSize,
							  CU_DEVICE_ATTRIBUTE_SHARED_MEMORY_PER_BLOCK,
							  device);
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "failed on cuDeviceGetAttribute: %s", errorText(rc));

	/* only shared memory consumption is what we have to control */
	shmemSizeTotal = (staticShmemSize +
					  dynamic_shmem_per_block +
					  dynamic_shmem_per_thread * maxBlockSize);
	if (shmemSizeTotal > maxShmemSize)
	{
		if (dynamic_shmem_per_thread > 0 &&
			staticShmemSize +
			dynamic_shmem_per_block +
			dynamic_shmem_per_thread * warpSize <= maxShmemSize)
		{
			maxBlockSize = (maxShmemSize -
							staticShmemSize -
							dynamic_shmem_per_block)/dynamic_shmem_per_thread;
			maxBlockSize = (maxBlockSize / warpSize) * warpSize;
		}
		else
			elog(ERROR,
				 "too large fixed amount of shared memory consumption: "
				 "static: %d, dynamic-per-block: %zu, dynamic-per-thread: %zu",
				 staticShmemSize,
				 dynamic_shmem_per_block,
				 dynamic_shmem_per_thread);
	}

	if ((size_t)maxBlockSize * (size_t)INT_MAX < nitems)
		elog(ERROR, "to large nitems (%zu) to launch kernel (blockSz=%d)",
			 nitems, maxBlockSize);

	*p_block_size = (size_t)maxBlockSize;
	*p_grid_size = (nitems + maxBlockSize - 1) / maxBlockSize;
}

/*
 * gpuOptimalBlockSize - a simple wrapper of cuOccupancyMaxPotentialBlockSize
 */
CUresult
gpuOptimalBlockSize(size_t *p_min_grid_sz,
					size_t *p_max_block_sz,
					CUfunction kern_function,
					size_t dynamic_shmem_per_block,
					size_t dynamic_shmem_per_thread)
{
	int		min_grid_sz;
	int		max_block_sz;
	CUresult rc;

	__dynamic_shmem_per_block = dynamic_shmem_per_block;
	__dynamic_shmem_per_thread = dynamic_shmem_per_thread;
	rc = cuOccupancyMaxPotentialBlockSize(&min_grid_sz,
										  &max_block_sz,
										  kern_function,
										  blocksize_to_shmemsize_helper,
										  0,
										  0);
	if (rc == CUDA_SUCCESS)
	{
		if (p_min_grid_sz)
			*p_min_grid_sz = min_grid_sz;
		if (p_max_block_sz)
			*p_max_block_sz = max_block_sz;
	}
	return rc;
}

/*
 * pgstrom_device_info - SQL function to dump device info
 */
Datum
pgstrom_device_info(PG_FUNCTION_ARGS)
{
	FuncCallContext *fncxt;
	DevAttributes  *dattrs;
	int				dindex;
	int				aindex;
	const char	   *att_name;
	const char	   *att_value;
	Datum			values[3];
	bool			isnull[3];
	HeapTuple		tuple;

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc		tupdesc;
		MemoryContext	oldcxt;

		fncxt = SRF_FIRSTCALL_INIT();
		oldcxt = MemoryContextSwitchTo(fncxt->multi_call_memory_ctx);

		tupdesc = CreateTemplateTupleDesc(3, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "id",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "attribute",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "value",
						   TEXTOID, -1, 0);
		fncxt->tuple_desc = BlessTupleDesc(tupdesc);

		fncxt->user_fctx = 0;

		MemoryContextSwitchTo(oldcxt);
	}
	fncxt = SRF_PERCALL_SETUP();

	dindex = fncxt->call_cntr / (lengthof(DevAttrCatalog) + 3);
	aindex = fncxt->call_cntr % (lengthof(DevAttrCatalog) + 3);

	if (dindex >= numDevAttrs)
		SRF_RETURN_DONE(fncxt);
	dattrs = &devAttrs[dindex];

	if (aindex == 0)
	{
		att_name = "GPU Device ID";
		att_value = psprintf("%d", dattrs->DEV_ID);
	}
	else if (aindex == 1)
	{
		att_name = "GPU Device Name";
		att_value = dattrs->DEV_NAME;
	}
	else if (aindex == 2)
	{
		att_name = "GPU Total RAM Size";
		att_value = format_bytesz(dattrs->DEV_TOTAL_MEMSZ);
	}
	else
	{
		int		i = aindex - 3;
		int		value = *((int *)((char *)dattrs +
								  DevAttrCatalog[i].attr_offset));

		att_name = DevAttrCatalog[i].attr_desc;
		switch (DevAttrCatalog[i].attr_kind)
		{
			case DEVATTRKIND__INT:
				att_value = psprintf("%d", value);
				break;
			case DEVATTRKIND__BYTES:
				att_value = format_bytesz((size_t)value);
				break;
			case DEVATTRKIND__KB:
				att_value = format_bytesz((size_t)value * 1024);
				break;
			case DEVATTRKIND__KHZ:
				if (value > 4000000)
					att_value = psprintf("%.2f GHz", (double)value/1000000.0);
				else if (value > 4000)
					att_value = psprintf("%d MHz", value / 1000);
				else
					att_value = psprintf("%d kHz", value);
				break;
			case DEVATTRKIND__COMPUTEMODE:
				switch (value)
				{
					case CU_COMPUTEMODE_DEFAULT:
						att_value = "Default";
						break;
#if CUDA_VERSION < 8000
					case CU_COMPUTEMODE_EXCLUSIVE:
						att_value = "Exclusive";
						break;
#endif
					case CU_COMPUTEMODE_PROHIBITED:
						att_value = "Prohibited";
						break;
					case CU_COMPUTEMODE_EXCLUSIVE_PROCESS:
						att_value = "Exclusive Process";
						break;
					default:
						att_value = "Unknown";
						break;
				}
				break;
			case DEVATTRKIND__BOOL:
				att_value = psprintf("%s", value != 0 ? "True" : "False");
				break;
			case DEVATTRKIND__BITS:
				att_value = psprintf("%dbits", value);
				break;
			default:
				elog(ERROR, "Bug? unknown DevAttrKind: %d",
					 (int)DevAttrCatalog[i].attr_kind);
		}
	}
	memset(isnull, 0, sizeof(isnull));
	values[0] = Int32GetDatum(dindex);
	values[1] = CStringGetTextDatum(att_name);
	values[2] = CStringGetTextDatum(att_value);

	tuple = heap_form_tuple(fncxt->tuple_desc, values, isnull);

	SRF_RETURN_NEXT(fncxt, HeapTupleGetDatum(tuple));
}
PG_FUNCTION_INFO_V1(pgstrom_device_info);

/*
 * SQL functions for GPU attributes
 */
Datum
pgstrom_gpu_device_name(PG_FUNCTION_ARGS)
{
	DevAttributes *dattr;
	int		cuda_dindex = PG_GETARG_INT32(0);

	if (cuda_dindex < 0 || cuda_dindex >= numDevAttrs)
		elog(ERROR, "invalid GPU device index: %d", cuda_dindex);
	dattr = &devAttrs[cuda_dindex];

	PG_RETURN_TEXT_P(cstring_to_text(dattr->DEV_NAME));
}
PG_FUNCTION_INFO_V1(pgstrom_gpu_device_name);

Datum
pgstrom_gpu_global_memsize(PG_FUNCTION_ARGS)
{
	DevAttributes *dattr;
	int		cuda_dindex = PG_GETARG_INT32(0);

	if (cuda_dindex < 0 || cuda_dindex >= numDevAttrs)
		elog(ERROR, "invalid GPU device index: %d", cuda_dindex);
	dattr = &devAttrs[cuda_dindex];

	PG_RETURN_INT64(dattr->DEV_TOTAL_MEMSZ);
}
PG_FUNCTION_INFO_V1(pgstrom_gpu_global_memsize);

Datum
pgstrom_gpu_max_blocksize(PG_FUNCTION_ARGS)
{
	DevAttributes *dattr;
	int		cuda_dindex = PG_GETARG_INT32(0);

	if (cuda_dindex < 0 || cuda_dindex >= numDevAttrs)
		elog(ERROR, "invalid GPU device index: %d", cuda_dindex);
	dattr = &devAttrs[cuda_dindex];

	PG_RETURN_INT32(dattr->MAX_THREADS_PER_BLOCK);
}
PG_FUNCTION_INFO_V1(pgstrom_gpu_max_blocksize);

Datum
pgstrom_gpu_warp_size(PG_FUNCTION_ARGS)
{
	DevAttributes *dattr;
	int		cuda_dindex = PG_GETARG_INT32(0);

	if (cuda_dindex < 0 || cuda_dindex >= numDevAttrs)
		elog(ERROR, "invalid GPU device index: %d", cuda_dindex);
	dattr = &devAttrs[cuda_dindex];

	PG_RETURN_INT32(dattr->WARP_SIZE);
}
PG_FUNCTION_INFO_V1(pgstrom_gpu_warp_size);

Datum
pgstrom_gpu_max_shared_memory_perblock(PG_FUNCTION_ARGS)
{
	DevAttributes *dattr;
	int		cuda_dindex = PG_GETARG_INT32(0);

	if (cuda_dindex < 0 || cuda_dindex >= numDevAttrs)
		elog(ERROR, "invalid GPU device index: %d", cuda_dindex);
	dattr = &devAttrs[cuda_dindex];

	PG_RETURN_INT32(dattr->MAX_SHARED_MEMORY_PER_BLOCK);
}
PG_FUNCTION_INFO_V1(pgstrom_gpu_max_shared_memory_perblock);

Datum
pgstrom_gpu_num_registers_perblock(PG_FUNCTION_ARGS)
{
	DevAttributes *dattr;
	int		cuda_dindex = PG_GETARG_INT32(0);

	if (cuda_dindex < 0 || cuda_dindex >= numDevAttrs)
		elog(ERROR, "invalid GPU device index: %d", cuda_dindex);
	dattr = &devAttrs[cuda_dindex];

	PG_RETURN_INT32(dattr->MAX_REGISTERS_PER_BLOCK);
}
PG_FUNCTION_INFO_V1(pgstrom_gpu_num_registers_perblock);

Datum
pgstrom_gpu_num_multiptocessors(PG_FUNCTION_ARGS)
{
	DevAttributes *dattr;
	int		cuda_dindex = PG_GETARG_INT32(0);

	if (cuda_dindex < 0 || cuda_dindex >= numDevAttrs)
		elog(ERROR, "invalid GPU device index: %d", cuda_dindex);
	dattr = &devAttrs[cuda_dindex];

	PG_RETURN_INT32(dattr->MULTIPROCESSOR_COUNT);
}
PG_FUNCTION_INFO_V1(pgstrom_gpu_num_multiptocessors);

Datum
pgstrom_gpu_num_cuda_cores(PG_FUNCTION_ARGS)
{
	DevAttributes *dattr;
	int		cuda_dindex = PG_GETARG_INT32(0);

	if (cuda_dindex < 0 || cuda_dindex >= numDevAttrs)
		elog(ERROR, "invalid GPU device index: %d", cuda_dindex);
	dattr = &devAttrs[cuda_dindex];

	PG_RETURN_INT32(dattr->CORES_PER_MPU *
					dattr->MULTIPROCESSOR_COUNT);
}
PG_FUNCTION_INFO_V1(pgstrom_gpu_num_cuda_cores);

Datum
pgstrom_gpu_cc_major(PG_FUNCTION_ARGS)
{
	DevAttributes *dattr;
	int		cuda_dindex = PG_GETARG_INT32(0);

	if (cuda_dindex < 0 || cuda_dindex >= numDevAttrs)
		elog(ERROR, "invalid GPU device index: %d", cuda_dindex);
	dattr = &devAttrs[cuda_dindex];

	PG_RETURN_INT32(dattr->COMPUTE_CAPABILITY_MAJOR);
}
PG_FUNCTION_INFO_V1(pgstrom_gpu_cc_major);

Datum
pgstrom_gpu_cc_minor(PG_FUNCTION_ARGS)
{
	DevAttributes *dattr;
	int		cuda_dindex = PG_GETARG_INT32(0);

	if (cuda_dindex < 0 || cuda_dindex >= numDevAttrs)
		elog(ERROR, "invalid GPU device index: %d", cuda_dindex);
	dattr = &devAttrs[cuda_dindex];

	PG_RETURN_INT32(dattr->COMPUTE_CAPABILITY_MINOR);
}
PG_FUNCTION_INFO_V1(pgstrom_gpu_cc_minor);

Datum
pgstrom_gpu_pci_id(PG_FUNCTION_ARGS)
{
	DevAttributes *dattr;
	int		cuda_dindex = PG_GETARG_INT32(0);
	char	temp[256];

	if (cuda_dindex < 0 || cuda_dindex >= numDevAttrs)
		elog(ERROR, "invalid GPU device index: %d", cuda_dindex);
	dattr = &devAttrs[cuda_dindex];
	snprintf(temp, sizeof(temp), "%04d:%02d:%02d",
			 dattr->PCI_DOMAIN_ID,
			 dattr->PCI_BUS_ID,
			 dattr->PCI_DEVICE_ID);
	PG_RETURN_TEXT_P(cstring_to_text(temp));
}
PG_FUNCTION_INFO_V1(pgstrom_gpu_pci_id);
