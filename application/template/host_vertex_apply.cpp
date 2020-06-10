#include "host_graph_sw.h"

#include "fpga_application.h"

#include "host_graph_sw_verification.h"


void setApplyKernel(cl_kernel &kernel_apply, int partId, int vertexNum)
{
#if HAVE_APPLY
    int argvi = 0;
    int base_score = float2int((1.0f - kDamp) / vertexNum);
    partitionDescriptor *p_partition = getPartition(partId * SUB_PARTITION_NUM);

    volatile unsigned int partitionVertexNum = ((p_partition->dstVertexEnd - p_partition->dstVertexStart)
            / (ALIGN_SIZE ) + 1) * (ALIGN_SIZE );
    int sink_end = partitionVertexNum;
    int offset = p_partition->dstVertexStart;

    clSetKernelArg(kernel_apply, argvi++, sizeof(cl_mem), get_cl_mem_pointer(getGatherScatter(2)->prop.id));
    clSetKernelArg(kernel_apply, argvi++, sizeof(cl_mem), get_cl_mem_pointer(getPartition(partId * SUB_PARTITION_NUM + 2)->tmpProp.id));
    clSetKernelArg(kernel_apply, argvi++, sizeof(cl_mem), get_cl_mem_pointer(getPartition(partId * SUB_PARTITION_NUM + 1)->tmpProp.id));
    clSetKernelArg(kernel_apply, argvi++, sizeof(cl_mem), get_cl_mem_pointer(getPartition(partId * SUB_PARTITION_NUM + 0)->tmpProp.id));
    clSetKernelArg(kernel_apply, argvi++, sizeof(cl_mem), get_cl_mem_pointer(getPartition(partId * SUB_PARTITION_NUM + 3)->tmpProp.id));
    clSetKernelArg(kernel_apply, argvi++, sizeof(cl_mem), get_cl_mem_pointer(getGatherScatter(2)->propUpdate.id));
    clSetKernelArg(kernel_apply, argvi++, sizeof(cl_mem), get_cl_mem_pointer(getGatherScatter(1)->propUpdate.id));
    clSetKernelArg(kernel_apply, argvi++, sizeof(cl_mem), get_cl_mem_pointer(getGatherScatter(0)->propUpdate.id));
    clSetKernelArg(kernel_apply, argvi++, sizeof(cl_mem), get_cl_mem_pointer(getGatherScatter(3)->propUpdate.id));
    clSetKernelArg(kernel_apply, argvi++, sizeof(cl_mem), get_cl_mem_pointer(MEM_ID_OUT_DEG));
    clSetKernelArg(kernel_apply, argvi++, sizeof(cl_mem), get_cl_mem_pointer(MEM_ID_ERROR));

    clSetKernelArg(kernel_apply, argvi++, sizeof(int),    &sink_end);
    clSetKernelArg(kernel_apply, argvi++, sizeof(int),    &offset);
    clSetKernelArg(kernel_apply, argvi++, sizeof(int),    &base_score);
#endif
}


prop_t  applyVerfication(prop_t tProp,prop_t source, unsigned int outDeg, void * arg)
{
    prop_t updateVerify = 0;
    unsigned int baseScore = *((unsigned int *)arg);
    int newScore = baseScore  + ((kDampFixPoint * tProp) >> 7);
    if (outDeg != 0)
    {
        updateVerify = newScore / (int)outDeg;
    }
    return updateVerify;
}


void partitionApplyCModel(
    cl_context              &context,
    cl_device_id            &device,
    int                     partId,
    unsigned int            baseScore
)
{
    DEBUG_PRINTF("partId %d\n", partId);
    for (int i  = 0; i < SUB_PARTITION_NUM; i++)
    {
        transfer_data_from_pl(context, device, getPartition(partId * SUB_PARTITION_NUM + i)->tmpProp.id);

    }
    prop_t * pCuData[SUB_PARTITION_NUM];
    prop_t * updateVerify = (prop_t*)get_host_mem_pointer(MEM_ID_VERTEX_PROP_VERIFY);
    prop_t * outDeg       = (prop_t*)get_host_mem_pointer(MEM_ID_OUT_DEG);
    prop_t * vertexProp   = (prop_t*)get_host_mem_pointer(MEM_ID_VERTEX_SCORE_CACHED);

    partitionDescriptor  *p_partition = getPartition(partId * SUB_PARTITION_NUM);

    for (int i = 0; i < SUB_PARTITION_NUM; i++)
    {
        pCuData[i] = (prop_t*)get_host_mem_pointer(getPartition(partId * SUB_PARTITION_NUM + i)->tmpProp.id);
    }

    volatile unsigned int partitionVertexNum = ((p_partition->dstVertexEnd - p_partition->dstVertexStart)
            / (ALIGN_SIZE) + 1 ) * (ALIGN_SIZE);
    DEBUG_PRINTF("[DUMP] partitionVertexNum %d\n", partitionVertexNum);

    int offset = getPartition(partId * SUB_PARTITION_NUM)->dstVertexStart;

    for (int i = 0; i < VERTEX_MAX; i++)
    {
        prop_t mergeData = 0;
        for (int j = 0 ; j < SUB_PARTITION_NUM; j++)
        {
            prop_t* pLocal = pCuData[j];

            mergeData = PROP_COMPUTE_STAGE4(mergeData, pLocal[i]);
            if (DATA_DUMP)
            {
                DEBUG_PRINTF("[DUMP-0] %d 0x%08x 0x%08x \n", i,
                             pLocal[i], mergeData);
            }
        }

        prop_t tProp = mergeData;
        updateVerify[i] = applyVerfication(tProp,vertexProp[i + offset],outDeg[i + offset],(void *)&baseScore);
    }

    int error_count = 0;
    transfer_data_from_pl(context, device, getGatherScatter(0)->propUpdate.id);
    prop_t* hwUpdate = (prop_t *)get_host_mem_pointer(getGatherScatter(0)->propUpdate.id);
    for (unsigned int i = 0; i < p_partition->dstVertexEnd - p_partition->dstVertexStart + 1; i++)
    {
        if (updateVerify[i] !=  hwUpdate[i + offset])
        {
            error_count ++;
            if (error_count < 50)
            {

                DEBUG_PRINTF("apply error tmp  %d 0x%08x 0x%08x  diff 0x%08x !!!!\n", i,
                             updateVerify[i],
                             hwUpdate[i + offset],
                             updateVerify[i] - hwUpdate[i + offset]);
            }
        }
        if (DATA_DUMP)
        {
            DEBUG_PRINTF("[DUMP] %d 0x%08x 0x%08x  diff 0x%08x \n", i,
                         updateVerify[i],
                         hwUpdate[i + offset],
                         updateVerify[i] - hwUpdate[i + offset]);
        }
    }
    DEBUG_PRINTF("[RES] apply error_count %d \n", error_count);
}



