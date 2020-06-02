#ifndef __FPGA_CACHE_H__
#define __FPGA_CACHE_H__


#include "graph_fpga.h"



typedef  struct
{
    burst_raw data;
    uint_raw addr;
} cache_line;


typedef  struct
{
    uint_raw idx;
    uint_raw size;
} cache_command;

#define URAM_DEPTH              (4096)
#define URAM_PER_EDGE           (4)
#define CACHE_SIZE              (URAM_DEPTH * URAM_PER_EDGE * 2)

#define CACHE_ADDRESS_MASK      (URAM_DEPTH * 8 - 1)

typedef struct
{
    burst_raw tuples;
    burst_raw score;
} edgeBlock;


template <typename T>
void streamMerge(
    hls::stream<T>              &edge,
    hls::stream<T>              &score,
    hls::stream<edgeBlock>      &edgeBlockStream
)
{
    while (true)
    {
        edgeBlock tempEdgeBlock;
        T         tempEdge;
        T         tempScore;
        read_from_stream(edge , tempEdge);
        read_from_stream(score, tempScore);
        tempEdgeBlock.tuples = tempEdge;
        tempEdgeBlock.score = tempScore;
        write_to_stream(edgeBlockStream, tempEdgeBlock);
        if (tempEdge.range(31, 0) == ENDFLAG)
        {
            break;
        }
    }
    {
        edgeBlock end;
        end.tuples = 0x0;
        end.score = 0x0;
        write_to_stream(edgeBlockStream, end);
    }
    empty_stream(edge);
    empty_stream(score);

}


void writeTuples( hls::stream<edge_tuples_t>  &edgeTuplesBuffer, edge_tuples_t (&tuples)[2])
{
#pragma HLS INLINE OFF
    for (int unit_cycle = 0; unit_cycle < 2; unit_cycle ++ )
    {
#pragma HLS UNROLL

        write_to_stream(edgeTuplesBuffer, tuples[unit_cycle]);

    }
}
//uint_raw address = (((uint_raw)(outer_idx) << (LOG_CACHEUPDATEBURST + LOG2_SIZE_BY_INT)) & CACHE_ADDRESS_MASK) >> 3
// the preload size is calculated by
inline uint_raw cacheUpdateByAddr(
    cache_line                      &cache_data,
    uint_uram                       vertexScoreCache[EDGE_NUM][URAM_PER_EDGE][URAM_DEPTH])
{
#pragma HLS INLINE
    {
        uint_raw uram_addr = (cache_data.addr & CACHE_ADDRESS_MASK) >> 3;

        for (int j = 0 ; j < 2 ; j ++)
        {
            for (int k = 0; k < EDGE_NUM; k++)
            {
#pragma HLS UNROLL
                vertexScoreCache[k][0][uram_addr + j] = cache_data.data.range(63 +  (j << 8) + 64 * 0, 0 + (j << 8) + 64 * 0);
                vertexScoreCache[k][1][uram_addr + j] = cache_data.data.range(63 +  (j << 8) + 64 * 1, 0 + (j << 8) + 64 * 1);
                vertexScoreCache[k][2][uram_addr + j] = cache_data.data.range(63 +  (j << 8) + 64 * 2, 0 + (j << 8) + 64 * 2);
                vertexScoreCache[k][3][uram_addr + j] = cache_data.data.range(63 +  (j << 8) + 64 * 3, 0 + (j << 8) + 64 * 3);
            }
        }
        return cache_data.addr;
    }
}



template <typename T>
void  duplicateStreamForCache(hls::stream<T>       &input,
                              hls::stream<T>       &output1,
                              hls::stream<T>       &output2)
{
#pragma HLS function_instantiate variable=input
    T lastUnit = 0;
    while (true)
    {
#pragma HLS PIPELINE II=1
        T  unit;
        read_from_stream(input, unit);
        write_to_stream(output1, unit);
        //if (unit != lastUnit)
        {
            write_to_stream(output2, unit);
        }
        lastUnit = unit;
        if (unit.range(31, 0) == ENDFLAG)
        {
            break;
        }
    }
}

#define CACHE_UPDATE_BURST  (BURSTBUFFERSIZE)
#define LOG_CACHEUPDATEBURST (LOG_BURSTBUFFERSIZE)

void stream2Command(hls::stream<burst_raw>          &mapStream,
                    hls::stream<cache_command>      &cmdStream)
{
    uint_raw last_index = ENDFLAG - 1;
    ap_uint<1> update_flag = 0;
    while (true)
    {
        burst_raw map;
        cache_command cmd;
        read_from_stream(mapStream, map);
        if (map.range(31, 0) == ENDFLAG)
        {
            break;
        }

        uint_raw min_index = (map.range(31, 0) >> (LOG_CACHEUPDATEBURST + LOG2_SIZE_BY_INT));
        uint_raw max_index = ((map.range(511, 480) + 64) >> (LOG_CACHEUPDATEBURST + LOG2_SIZE_BY_INT));
        if ((last_index == (ENDFLAG - 1)) || (min_index > last_index) || (max_index > last_index))
        {
            update_flag = 1;
        }
        else
        {
            update_flag = 0;
        }
        if (update_flag)
        {
            uint_raw min_bound;
            if ((last_index == (ENDFLAG - 1) ) || (min_index > last_index))
            {
                min_bound = min_index;
            }
            else
            {
                min_bound = last_index + 1;
            }
            cmd.idx = min_bound;
            cmd.size =  max_index + 2 - min_bound;
            write_to_stream(cmdStream, cmd);
            last_index = max_index + 1;
        }
    }
    cache_command end_cmd;
    end_cmd.idx = ENDFLAG;
    end_cmd.size =  0;
    write_to_stream(cmdStream, end_cmd);

}


#define DELAY_BUFFER       (512)
#define LOG_DELAY_BUFFER   (9)
#define POOL_SIZE          (4096)

void streamDelayScheme1(hls::stream<burst_raw>  &in, hls::stream<burst_raw>   &out)
{
    burst_raw  buffer[POOL_SIZE];
#pragma HLS RESOURCE variable=buffer core=XPM_MEMORY uram
    uint_raw   counter = 0;
    while (true)
    {
        burst_raw in_data;
        burst_raw out_data;

        read_from_stream(in, in_data);
        buffer[counter.range(LOG_DELAY_BUFFER - 1, 0)] = in_data;
        if (in_data.range(31, 0) == ENDFLAG)
        {
            break;
        }

        if (counter >= (DELAY_BUFFER - 1))
        {
            out_data = buffer[(counter.range(LOG_DELAY_BUFFER - 1, 0) + 1) & (DELAY_BUFFER - 1)];
            write_to_stream(out, out_data);
        }
        counter ++;
    }
    for (int i = 0; i < DELAY_BUFFER; i ++)
    {
        burst_raw end_data;
        end_data = buffer[(counter.range(LOG_DELAY_BUFFER - 1, 0) + 1 + i) & (DELAY_BUFFER - 1)];
        write_to_stream(out, end_data);
    }

}


void streamDelayScheme2(hls::stream<burst_raw>  &in, hls::stream<burst_raw>   &out)
{
    burst_raw  buffer[POOL_SIZE];
#pragma HLS RESOURCE variable=buffer core=XPM_MEMORY uram
#pragma HLS DEPENDENCE variable=buffer inter false
#pragma HLS DEPENDENCE variable=buffer intra false

    uint_raw   gr_counter = 0;
    uint_raw   gw_counter = 0;
    while (true)
    {

        burst_raw out_data;
        ap_uint<1> end_flag;
        uint_raw  r_counter = gr_counter;
        uint_raw  w_counter = gw_counter;
//#pragma HLS DEPENDENCE variable=w_counter intra false
//#pragma HLS DEPENDENCE variable=r_counter intra false

        out_data = buffer[gw_counter & (POOL_SIZE - 1)];

        if ((w_counter + DELAY_BUFFER - 1 < r_counter ))
        {
            if (!out.full())
            {
                write_to_stream(out, out_data);
                gw_counter ++;
            }
        }

        if ((w_counter + POOL_SIZE - 2) > r_counter)
        {
            if (!in.empty())
            {
                burst_raw in_data;
                read_from_stream(in, in_data);
                buffer[r_counter & (POOL_SIZE - 1)] = in_data;
                gr_counter ++;
                if (in_data.range(31, 0) == ENDFLAG)
                {
                    end_flag = 1;
                }
                else {
                    end_flag = 0;
                }
            }
        }
        /* magic */
        if (end_flag && r_counter > 16)
        {
            break;
        }
    }
    for (int i = gw_counter; i < gr_counter; i ++)
    {
        burst_raw end_data;
        end_data = buffer[i & (POOL_SIZE - 1)];
        write_to_stream(out, end_data);
    }

}


void updateVertexCache(uint16                          *input,
                       hls::stream<cache_command>      &cmdStream,
                       hls::stream<cache_line>         &cacheStream)
{

    burst_raw read_buffer[CACHE_UPDATE_BURST];
    while (true)
    {
        cache_command cmd;
        read_from_stream(cmdStream, cmd);
        if (cmd.idx == ENDFLAG)
        {
            break;
        }

        C_PRINTF("updating %d  %d  from %d \n", (int)min_index, (int)max_index, (int)last_index)
        for (uint_raw i = 0; i < cmd.size ; i ++)
        {
            uint_raw outer_idx = (i + cmd.idx) & (((2 * 1024 * 1024 * 1024) >> LOG_CACHEUPDATEBURST) - 1);
            for (int inner_idx = 0 ; inner_idx < CACHE_UPDATE_BURST; inner_idx ++) {
#pragma HLS PIPELINE II=1
                read_buffer[inner_idx] = input[((uint_raw)(outer_idx) << LOG_CACHEUPDATEBURST) + inner_idx];
            }
            uint_raw address = ((uint_raw)(outer_idx) << (LOG_CACHEUPDATEBURST + LOG2_SIZE_BY_INT));
            for (int inner_idx = 0 ; inner_idx < CACHE_UPDATE_BURST; inner_idx ++)
            {
                cache_line  cache_data;
                cache_data.addr = address + (inner_idx << 4);
                cache_data.data = read_buffer[inner_idx];
                write_to_stream(cacheStream, cache_data);
#if 0
                for (int j = 0 ; j < 4 ; j ++)
                {
                    for (int k = 0; k < EDGE_NUM; k++)
                    {
#pragma HLS UNROLL
                        vertexScoreCache[k][0][address + (inner_idx << 2) + j] = read_buffer[inner_idx].range(63 +  (j << 7), 0 + (j << 7));
                        vertexScoreCache[k][1][address + (inner_idx << 2) + j] = read_buffer[inner_idx].range(63 +  (j << 7) + 64, 0 + (j << 7) + 64);
                    }
                }
#endif
            }
        }
    }
    {
        cache_line  end;
        end.addr = (ENDFLAG - 15);
        end.data = 0x0;
        write_to_stream(cacheStream, end);
    }
    clear_stream(cmdStream);
}


void  readEdgesStage(
    hls::stream<edge_tuples_t>      &edgeTuplesBuffer,
    hls::stream<edgeBlock>          &edgeBlockStream,
    hls::stream<cache_line>         &cacheStream,
    uint_uram                       vertexScoreCache[EDGE_NUM][URAM_PER_EDGE][URAM_DEPTH]
)
{
#pragma HLS dependence variable=vertexScoreCache inter false
#pragma HLS DEPENDENCE variable=vertexScoreCache intra false

    C_PRINTF("%s \n", "start readedges");
    uint_raw end_value = 0;
    ap_uint<1>  break_flag = 0;
    uint_raw caching_value = 0;
    uint_raw processing_value = 0;
    uint_raw min_processing_value = 0;
    cache_line  cache_data[2];
    uint_raw    caching_counter = 0;
    edgeBlock   tmpBlock[2];
    uint_raw    processing_counter = 0;

    while (true)
    {
#pragma HLS PIPELINE II=2
        if (break_flag == 1)
        {
            break;
        }
        if (!cacheStream.empty() && (min_processing_value + CACHE_SIZE - 64) > caching_value)
        {
            read_from_stream(cacheStream, cache_data[0]);
            caching_value = (cache_data[0].addr);
            if (caching_counter > 0)
            {
                cacheUpdateByAddr(cache_data[1], vertexScoreCache);
            }
            cache_data[1] = cache_data[0];
            caching_counter ++;
        }
        //else
        if ((!edgeBlockStream.empty() && ((processing_value) < caching_value  ) ) || (processing_value == ENDFLAG))
        {
            read_from_stream(edgeBlockStream, tmpBlock[0]);
            processing_value = tmpBlock[0].score.range(511, 511 - 31);
            min_processing_value = tmpBlock[0].score.range(31, 0);
            if (processing_counter > 0)
            {
#pragma HLS latency min=4 max=10
                edge_tuples_t tuples[2];
readCache: for (int unit_cycle = 0; unit_cycle < 2; unit_cycle ++)
                {
#pragma HLS UNROLL
readCacheInner: for (int k = 0; k < EDGE_NUM; k ++) {
#pragma HLS UNROLL
#define  range_start  (( k ) << INT_WIDTH_SHIFT)

                        tuples[unit_cycle].data[k].x =
                            tmpBlock[1].tuples.range((range_start) + 31 + unit_cycle * 256, range_start + unit_cycle * 256);
                        unsigned int vertex_index =
                            tmpBlock[1].score.range((range_start) + 31 + unit_cycle * 256, range_start + unit_cycle * 256);
                        //tuples[0].data[k].y = get_cached_value(vertex_index, vertexScoreCache);

                        unsigned int address = (vertex_index & CACHE_ADDRESS_MASK) >> 3;
                        unsigned int bit =  ((vertex_index & CACHE_ADDRESS_MASK) >> 1) & (URAM_PER_EDGE - 1);

                        uint_uram tmp;
                        {
#pragma HLS latency min=1 max=3
                            tmp = vertexScoreCache[k][bit][address];
                        }

                        if (vertex_index & 0x01)
                            tuples[unit_cycle].data[k].y = tmp.range(63, 32);
                        else
                            tuples[unit_cycle].data[k].y = tmp.range(31,  0);
#if CAHCE_FETCH_DEBUG
                        if (tuples[unit_cycle].data[k].y != vertex_index)
                        {
                            C_PRINTF("[FETCH] error %d %d\n", tuples[unit_cycle].data[k].y, vertex_index);
                        }
                        else
                        {
                            C_PRINTF("[FETCH] dump %d %d\n", tuples[unit_cycle].data[k].y, vertex_index);
                        }
#endif
                    }
                }
                writeTuples(edgeTuplesBuffer, tuples);
                end_value = tmpBlock[1].tuples.range(31, 0);
            }
            tmpBlock[1] = tmpBlock[0];
            processing_counter ++;
        }
        if (end_value == ENDFLAG)
        {
            break_flag = 1;
        }
        else
        {
            break_flag = 0;
        }
    }

    C_PRINTF("%s\n", "end");
    empty_stream(edgeBlockStream);
    empty_stream(cacheStream);
    C_PRINTF("%s\n", "end2");
    return;

}
void cacheProcess( uint16                           *vertexScore,
                   hls::stream<burst_raw>           &edgeBurstStream,
                   hls::stream<burst_raw>           &mapStream,
                   hls::stream<edge_tuples_t>       &edgeTuplesBuffer
                 )
{
#pragma HLS DATAFLOW
    uint_uram vertexScoreCache[EDGE_NUM][URAM_PER_EDGE][URAM_DEPTH];
#pragma HLS ARRAY_PARTITION variable=vertexScoreCache dim=1 complete
#pragma HLS ARRAY_PARTITION variable=vertexScoreCache dim=2 complete
#pragma HLS RESOURCE variable=vertexScoreCache core=XPM_MEMORY uram

#pragma HLS DEPENDENCE variable=vertexScoreCache inter false
#pragma HLS DEPENDENCE variable=vertexScoreCache intra false

    hls::stream<cache_line>    cacheUpdateStream;
#pragma HLS stream variable=cacheUpdateStream  depth=512

    hls::stream<cache_command>    cmdStream;
#pragma HLS stream variable=cmdStream  depth=512

    hls::stream<burst_raw>      delayingMapStream;
#pragma HLS stream variable=delayingMapStream depth=2

    hls::stream<burst_raw>      delayedMapStream;
#pragma HLS stream variable=delayedMapStream depth=2

    hls::stream<burst_raw>      map4CacheStream;
#pragma HLS stream variable=map4CacheStream depth=2

    hls::stream<edgeBlock>      edgeBlockStream;
#pragma HLS stream variable=edgeBlockStream depth=2

    duplicateStreamForCache(mapStream, delayingMapStream, map4CacheStream);

    streamDelayScheme2(delayingMapStream, delayedMapStream);

    streamMerge(edgeBurstStream, delayedMapStream, edgeBlockStream);

    stream2Command(map4CacheStream, cmdStream);

    updateVertexCache(vertexScore , cmdStream, cacheUpdateStream);

    readEdgesStage(edgeTuplesBuffer, edgeBlockStream, cacheUpdateStream, vertexScoreCache);
}

#endif /* __FPGA_CACHE_H__ */
