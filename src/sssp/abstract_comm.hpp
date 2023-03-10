/*
 * abstract_comm.hpp
 *
 *  Created on: 2014/05/17
 *      Author: ueno
 */

#ifndef ABSTRACT_COMM_HPP_
#define ABSTRACT_COMM_HPP_

#include <limits.h>
#if USE_PROPER_HASHMAP
#include <unordered_map>
#endif
#include "utils.hpp"
#include "fiber.hpp"
#include "graph.hpp"
#include "sssp_state.hpp"
#include "omp.h"

#define debug(...) debug_print(ABSCO, __VA_ARGS__)
class AlltoallBufferHandler {
public:
	virtual ~AlltoallBufferHandler() { }
	virtual void* get_buffer() = 0;
	virtual void add(void* buffer, void* data, int offset, int length) = 0;
	virtual void* clear_buffers() = 0;
	virtual void* second_buffer() = 0;
	virtual int max_size() = 0;
	virtual int buffer_length() = 0;
	virtual MPI_Datatype data_type() = 0;
	virtual int element_size() = 0;
	virtual void received(void* buf, int offset, int length, int from, bool is_ptr) = 0;
	virtual void finish() = 0;
};

class AsyncAlltoallManager {
	struct Buffer {
		void* ptr;
		int length;
	};

	struct PointerData {
	   int64_t ptr;
		int64_t header;
		float dist;
		int length;
	};

	struct CommTarget {
		CommTarget()
			: reserved_size_(0)
			, filled_size_(0) {
			cur_buf.ptr = NULL;
			cur_buf.length = 0;
#ifdef USE_PTR_LOCKS_OMP
			omp_init_lock(&send_mutex);
#else
         pthread_mutex_init(&send_mutex, NULL);
#endif
		}
		~CommTarget() {
#ifdef USE_PTR_LOCKS_OMP
		   omp_destroy_lock(&send_mutex);
#else
		   pthread_mutex_destroy(&send_mutex);
#endif
		}

#ifdef USE_PTR_LOCKS_OMP
		omp_lock_t send_mutex;
#else
		pthread_mutex_t send_mutex;
#endif
		// monitor : send_mutex
		volatile int reserved_size_;
		volatile int filled_size_;
		Buffer cur_buf;
		std::vector<Buffer> send_data;
		std::vector<PointerData> send_ptr;
	};
public:
	AsyncAlltoallManager(MPI_Comm comm_, AlltoallBufferHandler* buffer_provider_)
		: comm_(comm_)
		, buffer_provider_(buffer_provider_)
		, scatter_(comm_)
	{
		CTRACER(AsyncA2A_construtor);
		MPI_Comm_size(comm_, &comm_size_);
		node_ = new CommTarget[comm_size_]();
		d_ = new DynamicDataSet();
		pthread_mutex_init(&d_->thread_sync_, NULL);
		buffer_size_ = buffer_provider_->buffer_length();
	}
	virtual ~AsyncAlltoallManager() {
		delete [] node_; node_ = NULL;
	}

	void prepare() {
		CTRACER(prepare);
		debug("prepare idx=%d", sub_comm);
		for(int i = 0; i < comm_size_; ++i) {
			node_[i].reserved_size_ = node_[i].filled_size_ = buffer_size_;
		}
	}

	/**
	 * Asynchronous send.
	 * When the communicator receive data, it will call fold_received(FoldCommBuffer*) function.
	 * To reduce the memory consumption, when the communicator detects stacked jobs,
	 * it also process the tasks in the fiber_man_ except the tasks that have the lowest priority (0).
	 * This feature realize the fixed memory consumption.
	 */
	void put(void* ptr, int length, int target)
	{
		CTRACER(comm_send);
		if(length == 0) {
			assert(length > 0);
			return ;
		}
		CommTarget& node = node_[target];

//#if ASYNC_COMM_LOCK_FREE
		do {
			int offset = __sync_fetch_and_add(&node.reserved_size_, length);
			if(offset > buffer_size_) {
				// wait
				while(node.reserved_size_ > buffer_size_) ;
				continue ;
			}
			else if(offset + length > buffer_size_) {
				// swap buffer
				assert (offset > 0);
				while(offset != node.filled_size_) ;
				flush(node);
				node.cur_buf.ptr = get_send_buffer(); // Maybe, this takes much time.
				// This order is important.
				offset = node.filled_size_ = 0;
				__sync_synchronize(); // membar
				node.reserved_size_ = length;
			}
			buffer_provider_->add(node.cur_buf.ptr, ptr, offset, length);
			__sync_fetch_and_add(&node.filled_size_, length);
			break;
		} while(true);
// #endif
	}

	void put_ptr(int64_t ptr, int length, int64_t header, float dist, int target) {
		CommTarget& node = node_[target];
		PointerData data = { ptr, header, dist, length };

#ifdef USE_PTR_LOCKS_OMP
		omp_set_lock(&node.send_mutex);
      node.send_ptr.push_back(data);
      omp_unset_lock(&node.send_mutex);
#else
		pthread_mutex_lock(&node.send_mutex);
		node.send_ptr.push_back(data);
		pthread_mutex_unlock(&node.send_mutex);
#endif
	}

private:

	// Returns sentinel value
	static inline
	uint32_t get_sentinel() {
	   return castFloatToUInt32(-1.0f);
	}

   // remove duplicates that are to be sent.
   // Returns new length. NOTE: also cleans vertices_pos
   static inline
   int remove_sentinels_buffer(const Graph2DCSR& graph, int read_start, int write_start, int length, uint32_t* restrict stream, int32_t* restrict vertices_pos)
   {
      assert(read_start >= 0 && length >= 0);
      assert(read_start >= write_start);
      const LocalVertex lmask = (LocalVertex(1) << graph.local_bits_) - 1;
      const uint32_t sentinel = get_sentinel();
      int write_pos = write_start;
      const int read_end = read_start + length;

      for( int red_pos = read_start; red_pos < read_end; red_pos += 2 ) {
         uint32_t v = stream[red_pos];
         if( v & 0x80000000u ) {
            // no edges after last predecessor?
            if( write_pos != write_start && (stream[write_pos - 2] & 0x80000000u) ) {
               write_pos -= 2;
            }

            stream[write_pos++] = stream[red_pos];
            stream[write_pos++] = stream[red_pos + 1];
            red_pos += 2;
            v = stream[red_pos];
            assert(!(v & 0x80000000u));
         }

         if( stream[red_pos + 1] == sentinel )
            continue;

         stream[write_pos++] = stream[red_pos];
         stream[write_pos++] = stream[red_pos + 1];
#if !USE_PROPER_HASHMAP
         const LocalVertex tgt_local = v & lmask;
         assert(0 <= tgt_local && tgt_local < graph.num_local_verts_);
         vertices_pos[tgt_local] = -1;
#endif
      }

#ifndef NDEBUG
      for( int64_t i = 0; i < graph.num_local_verts_; i++ )
         assert(vertices_pos[i] == -1);
#endif

      const int length_reduced = write_pos - write_start;
      assert(0 <= length_reduced && length_reduced <= length);
      return length_reduced;
   }


   // remove duplicates that are to be sent. Returns new length.
   // NOTE: also cleans hash-array
   static inline
   int remove_sentinels_ptr(const Graph2DCSR& graph, int length, uint32_t* restrict stream, int32_t* restrict vertices_pos)
   {
      const uint32_t sentinel = get_sentinel();
      const LocalVertex lmask = (LocalVertex(1) << graph.local_bits_) - 1;
      int length_new = 0;
      assert(length >= 0);

      for( int i = 0; i < length; i++ ) {
         assert(!(stream[i + 2] & 0x80000000u));

         const int length_i = stream[i + 2];
         const int row_start = i + 3;
         const int row_end = row_start + length_i;
         assert(length_i >= 0 && length_i % 2 == 0);

         stream[length_new++] = stream[i];
         stream[length_new++] = stream[i + 1];
         length_new++; // for the length
         assert(length_new <= row_start);

         const int length_new_prev = length_new;
         for( int c = row_start; c < row_end; c += 2 ) {
            if( stream[c + 1] == sentinel ) {
               continue;
            }

            const LocalVertex tgt_local = (stream[c] & lmask);
            assert(0 <= tgt_local && tgt_local < graph.num_local_verts_);

            vertices_pos[tgt_local] = -1;

            stream[length_new++] = stream[c];
            stream[length_new++] = stream[c + 1];
         }
         const int n_new = length_new - length_new_prev;

         // no non-duplicate targets?
         if( n_new == 0 )
            length_new -= 3;
         else
            stream[length_new_prev - 1] = n_new;

         i += 2 + length_i;
      }

      assert(length_new <= length);
      return length_new;
   }

   // Returns (overestimate of) send length for given node.
   static
   int get_node_send_length_buffer(const CommTarget& node, const SsspState& sssp_state, const Graph2DCSR& graph)
   {
      const int n_buffers = (int)node.send_data.size();
      int node_send_length = 0;

      for( int b = 0; b < n_buffers; ++b )
         node_send_length += node.send_data[b].length;

      return node_send_length;
   }

   // Returns (overestimate of) send length for given node.
   static
   int get_node_send_length_ptr(const CommTarget& node, const SsspState& sssp_state, const Graph2DCSR& graph)
   {
      const int n_ptrs = (int)node.send_ptr.size();
      int node_send_length = 0;

      if( 0 == n_ptrs )
         return node_send_length;

      const BitmapType* const vertices_is_settled = sssp_state.vertices_is_settled_;
      const int64_t* const restrict edge_array = graph.edge_array_;
      const int r_bits = graph.r_bits_;
      const int lgl = graph.local_bits_;
      const int64_t L = graph.num_local_verts_;
      const bool with_settled = sssp_state.with_settled_;
      const bool is_bellman_ford = sssp_state.is_bellman_ford_;
      const bool is_light_phase = sssp_state.is_light_phase_;
#if NODE_SEND_COUNT_TYPE == 1
      const float* const restrict edge_weight_array = graph.edge_weight_array_;
      const float bucket_upper = sssp_state.bucket_upper;
#endif

      for( int b = 0; b < n_ptrs; ++b ) {
        const PointerData& buffer = node.send_ptr[b];
        const int buffer_length = buffer.length;
        assert(buffer_length >= 0);
        if( buffer_length == 0 )
           continue;

        // add size for source information and length
        node_send_length += 3;

        if( is_bellman_ford ) {
           const int64_t pos_offset = buffer.ptr;
           assert(pos_offset >= 0);
           assert(with_settled);

           for( int i = 0; i < buffer_length; ++i ) {
              const int64_t pos = pos_offset + i;
              if( SsspState::target_is_settled(vertices_is_settled, edge_array[pos], r_bits, lgl, L) )
                 continue;
              node_send_length += 2; // one for the vertex, one for the weight
           }
        }
        else if( is_light_phase ) {
           assert((buffer.header & int64_t(1) << 63) == 0);
#if NODE_SEND_COUNT_TYPE == 1
           const float buffer_dist = buffer.dist;
           const int64_t pos_offset = buffer.ptr;
           for( int i = 0; i < buffer_length; ++i ) {
              const int64_t pos = pos_offset + i;
              if( with_settled && SsspState::target_is_settled(vertices_is_settled, edge_array[pos], r_bits, lgl, L) )
                 continue;
              if( edge_weight_array[pos] + buffer_dist >= bucket_upper )
                 continue;
              node_send_length += 2; // one for the vertex, one for the weight
           }
#else
           node_send_length += 2 * buffer_length;
#endif
        }
        else
        {
#if NODE_SEND_COUNT_TYPE == 1
           const bool buffer_is_heavy = ((buffer.header & int64_t(1) << 63) != 0);
#endif
           const int64_t pos_offset = buffer.ptr;
           assert(pos_offset >= 0);
           for( int i = 0; i < buffer_length; ++i ) {
              const int64_t pos = pos_offset + i;
              if( with_settled && SsspState::target_is_settled(vertices_is_settled, edge_array[pos], r_bits, lgl, L) )
                 continue;
#if NODE_SEND_COUNT_TYPE == 1
              assert(!buffer_is_heavy || !comp::isLT(edge_weight_array[pos] + buffer.dist, bucket_upper));
              if( !buffer_is_heavy && comp::isLT(edge_weight_array[pos] + buffer.dist, bucket_upper) )
                 continue;
#else
              node_send_length += 2; // one for the vertex, one for the weight
#endif
            }
         }
      }

      return node_send_length;
   }

    // Copies the vertices to send to given compute to to array stream. Marks duplicates by setting sentinel value.
    static inline
    int collect_targets_ptr(const CommTarget& node, const SsspState& sssp_state, const Graph2DCSR& graph,
          uint32_t* restrict stream, int32_t* restrict vertices_pos)
    {
       const BitmapType* const vertices_is_settled = sssp_state.vertices_is_settled_;
       const int64_t* const restrict edge_array = graph.edge_array_;
       const float* const restrict edge_weight_array = graph.edge_weight_array_;
       const LocalVertex lmask = (LocalVertex(1) << graph.local_bits_) - 1;
       const int n_ptrs = (int)node.send_ptr.size();
       const int r_bits = graph.r_bits_;
       const int lgl = graph.local_bits_;
       const int64_t L = graph.num_local_verts_;
       int node_send_pos = 0;
       const bool with_settled = sssp_state.with_settled_;
       const bool is_bellman_ford = sssp_state.is_bellman_ford_;
       const bool is_light_phase = sssp_state.is_light_phase_;
       const float bucket_upper = sssp_state.bucket_upper;
       const uint32_t sentinel = get_sentinel();

       for( int b = 0; b < n_ptrs; ++b ) {
          const PointerData& buffer = node.send_ptr[b];
          const int buffer_length = buffer.length;
          if( buffer_length == 0 )
             continue;

          const int64_t pos_offset = buffer.ptr;
          const float buffer_dist = buffer.dist;
          stream[node_send_pos++] = (buffer.header >> 32);
          stream[node_send_pos++] = (uint32_t)buffer.header;
          node_send_pos++; // save space for the length

          const int node_send_pos_org = node_send_pos;

          if( is_bellman_ford ) {
             for( int i = 0; i < buffer_length; ++i ) {
                const int64_t pos = pos_offset + i;
                assert(with_settled);

                if( SsspState::target_is_settled(vertices_is_settled, edge_array[pos], r_bits, lgl, L) )
                   continue;

                const float dist_new = buffer_dist + edge_weight_array[pos];
                // todo use MACRO inline does not work
                const LocalVertex tgt_local = (edge_array[pos] & lmask);
                if( vertices_pos[tgt_local] < 0 ) {
                   vertices_pos[tgt_local] = node_send_pos;
                   stream[node_send_pos++] = tgt_local;
                   stream[node_send_pos++] = castFloatToUInt32(dist_new);
                   continue;
                }
                const int twin_pos = vertices_pos[tgt_local];
                assert(twin_pos < node_send_pos && tgt_local == stream[twin_pos]);
                if( dist_new < castUInt32ToFloat(stream[twin_pos + 1]) ) {
                   vertices_pos[tgt_local] = node_send_pos;
                   stream[twin_pos + 1] = sentinel;
                   stream[node_send_pos++] = tgt_local;
                   stream[node_send_pos++] = castFloatToUInt32(dist_new);
                }
             }
          }
          else {
             if( is_light_phase ) {
                for( int i = 0; i < buffer_length; ++i ) {
                   const int64_t pos = pos_offset + i;

                   if( with_settled && SsspState::target_is_settled(vertices_is_settled, edge_array[pos], r_bits, lgl, L) )
                      continue;

                   const float dist_new = edge_weight_array[pos] + buffer_dist;
                   if( dist_new >= bucket_upper )
                      continue;
                   // todo use MACRO inline does not work
                   const LocalVertex tgt_local = (edge_array[pos] & lmask);
                   if( vertices_pos[tgt_local] < 0 ) {
                      vertices_pos[tgt_local] = node_send_pos;
                      stream[node_send_pos++] = tgt_local;
                      stream[node_send_pos++] = castFloatToUInt32(dist_new);
                      continue;
                   }
                   const int twin_pos = vertices_pos[tgt_local];
                   assert(twin_pos < node_send_pos && tgt_local == stream[twin_pos]);
                   if( dist_new < castUInt32ToFloat(stream[twin_pos + 1]) ) {
                      vertices_pos[tgt_local] = node_send_pos;
                      stream[twin_pos + 1] = sentinel;
                      stream[node_send_pos++] = tgt_local;
                      stream[node_send_pos++] = castFloatToUInt32(dist_new);
                   }
                }
             }
             else {
                const bool buffer_is_heavy = ((buffer.header & int64_t(1) << 63) != 0);
                for( int i = 0; i < buffer_length; ++i ) {
                   const int64_t pos = pos_offset + i;

                   if( with_settled && SsspState::target_is_settled(vertices_is_settled, edge_array[pos], r_bits, lgl, L) )
                      continue;

                   const float dist_new = edge_weight_array[pos] + buffer_dist;
                   assert(!buffer_is_heavy || !comp::isLT(dist_new, bucket_upper));

                   if( !buffer_is_heavy && comp::isLT(dist_new, bucket_upper) ) {
                      continue;
                   }
                   // todo use MACRO inline does not work
                   const LocalVertex tgt_local = (edge_array[pos] & lmask);
                   if( vertices_pos[tgt_local] < 0 ) {
                      vertices_pos[tgt_local] = node_send_pos;
                      stream[node_send_pos++] = tgt_local;
                      stream[node_send_pos++] = castFloatToUInt32(dist_new);
                      continue;
                   }
                   const int twin_pos = vertices_pos[tgt_local];
                   assert(twin_pos < node_send_pos && tgt_local == stream[twin_pos]);
                   if( dist_new < castUInt32ToFloat(stream[twin_pos + 1]) ) {
                      vertices_pos[tgt_local] = node_send_pos;
                      stream[twin_pos + 1] = sentinel;
                      stream[node_send_pos++] = tgt_local;
                      stream[node_send_pos++] = castFloatToUInt32(dist_new);
                   }
                }
             }
          } // if !is_bellman_ford

          const int buffer_length_filtered = node_send_pos - node_send_pos_org;
          assert(buffer_length_filtered % 2 == 0);
          stream[node_send_pos_org - 1] = buffer_length_filtered;
       }

       return node_send_pos;
    }

    // remove duplicates that are to be sent. Returns new length
    static inline
    int collect_targets_buffer(const CommTarget& node, const Graph2DCSR& graph, const SsspState& sssp_state, int stream_offset, uint32_t* restrict stream,
 #if USE_PROPER_HASHMAP
        std::unordered_map<LocalVertex, int>& tgt_map )
 #else
        int32_t* restrict vertices_pos)
 #endif
    {
       const LocalVertex lmask = (LocalVertex(1) << graph.local_bits_) - 1;
       const uint32_t sentinel = get_sentinel();
       const bool is_presolving = sssp_state.is_presolving_mode_;

       int offset = stream_offset;
       const int n_buffers = (int)node.send_data.size();
       for( int b = 0; b < n_buffers; ++b ) {
          Buffer buffer = node.send_data[b];
          const int buffer_length = buffer.length;
          if( buffer_length == 0 )
             continue;
          assert(buffer_length > 0);
          memcpy((uint8_t*)(stream + offset), (uint8_t*)buffer.ptr, buffer_length * sizeof(*stream));
          offset += buffer_length;
       }
       const int stream_end = offset;
       const int length = stream_end - stream_offset;
       assert(length % 2 == 0);

       for( int j = stream_offset; j < stream_end; j += 2 ) {
          if( stream[j] & 0x80000000u ) {
             j += 2;
             assert(!(stream[j] & 0x80000000u));
             assert(j < stream_end - 1);
          }

          stream[j] &= lmask;
          const LocalVertex tgt_local = stream[j];
          assert(stream[j + 1] != sentinel);

#if SKIP_FILTERING
          if( is_presolving )
             continue;
#endif

 #if USE_PROPER_HASHMAP
          auto element = tgt_map.find(tgt_local);
          if( element == tgt_map.end() ) {
             tgt_map[tgt_local] = j;
             continue;
          }
          const int twin_pos = element->second;
 #else
          if( vertices_pos[tgt_local] < 0 ) {
             vertices_pos[tgt_local] = j;
             continue;
          }
          const int twin_pos = vertices_pos[tgt_local];
 #endif

          assert(twin_pos < j);
          assert(stream[j] == stream[twin_pos]);

          const float weight = castUInt32ToFloat(stream[j + 1]);
          const float twin_weight = castUInt32ToFloat(stream[twin_pos + 1]);
          if( weight < twin_weight ) {
 #if USE_PROPER_HASHMAP
             tgt_map[tgt_local] = j;
 #else
             vertices_pos[tgt_local] = j;
 #endif
             stream[twin_pos + 1] = sentinel;
          }
          else {
             stream[j + 1] = sentinel;
          }
       }


 #if USE_PROPER_HASHMAP
       tgt_map.clear();
 #endif

       return length;
    }


public:

    // use both buffers and pointers
    void run_with_both(const Graph2DCSR& graph, const SsspState& sssp_state, int32_t* restrict vertices_pos) {
       PROF(profiling::TimeKeeper tk_all);
       const int es = buffer_provider_->element_size();
       assert(sizeof(uint32_t) == es);
       const int max_size_per_node = buffer_provider_->max_size() / (es * comm_size_);
       VERBOSE(last_send_size_ = 0);
       VERBOSE(last_recv_size_ = 0);
       int node_send_lengths_ptr[comm_size_];
       int node_send_lengths_buffer[comm_size_];
       int comm_rank;
       MPI_Comm_rank(comm_, &comm_rank);
       assert(0 <= comm_rank && comm_rank < comm_size_);

 #pragma omp parallel for schedule(static)
       for(int i = 0; i < comm_size_; ++i) {
          CommTarget& node = node_[i];
          flush(node);

          node_send_lengths_buffer[i] = get_node_send_length_buffer(node, sssp_state, graph);
          node_send_lengths_ptr[i] = get_node_send_length_ptr(node, sssp_state, graph);
       }

       for( int loop = 0; true; ++loop ) {
          USER_START(a2a_merge);

 #pragma omp parallel
          {
             int* counts = scatter_.get_counts();
             bool thread_has_ptr = false;
 #pragma omp for schedule(static)
             for(int c = 0; c < comm_size_; ++c) {
                // NOTE: we make the shift so that the receiver compute nodes are more evenly used.
                const int i = (c + comm_rank) % comm_size_;
                assert(0 == counts[i]);

                if( 0 == node_send_lengths_ptr[i] && 0 == node_send_lengths_buffer[i] )
                   continue;
                counts[i] = 1; // for storing the size
                if( node_send_lengths_buffer[i] > 0 ) {
                   assert(loop == 0);
                   counts[i] += node_send_lengths_buffer[i];
                }

                if( node_send_lengths_ptr[i] == 0 ) {
                   if( node_send_lengths_buffer[i] == 0 ) {
                      assert(counts[i] == 1);
                      counts[i] = 0;
                   }
                   continue;
                }

                // is the ptr size too large? AND has this thread already stored a pointer or are we in first loop?
                if( node_send_lengths_buffer[i] + node_send_lengths_ptr[i] > max_size_per_node &&
                     (thread_has_ptr || loop == 0) ) {
                   if( node_send_lengths_buffer[i] == 0 ) {
                      assert(counts[i] == 1);
                      counts[i] = 0;
                   }
                   //std::cout << mpi.rank_2d << "X terminate early" << '\n';
                   continue;
                }
                thread_has_ptr = true;
                counts[i] += node_send_lengths_ptr[i];
             } // #pragma omp for schedule(static)
          } // #pragma omp parallel

          scatter_.sum();

          // todo maybe catch this somehow and rerun upper loop?
          if( scatter_.get_send_count() > (buffer_provider_->max_size() / es) ) {
             std::cerr << "memory issue for node send: " << scatter_.get_send_count() << " > " << (buffer_provider_->max_size() / es) << "\n";
             std::cout << "memory issue for node send: " << scatter_.get_send_count() << " > " << (buffer_provider_->max_size() / es) << "\n";
             MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
          }

          if( loop > 0 ) {
             int has_data = (scatter_.get_send_count() > 0);
             MPI_Allreduce(MPI_IN_PLACE, &has_data, 1, MPI_INT, MPI_LOR, comm_);

             if( mpi.isMaster() && has_data )
                std::cout << "re-running allgather, count: " << loop << '\n';

             if(has_data == 0) break;
          }

          int* const send_lengths = scatter_.get_send_lengths();

#pragma omp parallel
          {
             int* offsets = scatter_.get_offsets();
             int* counts = scatter_.get_counts_org();
             uint32_t* stream = (uint32_t*)buffer_provider_->second_buffer();

             const int64_t pos_offset = omp_get_thread_num() * graph.num_local_verts_;
#pragma omp for schedule(static)
             for( int c = 0; c < comm_size_; ++c ) {
                const int i = (c + comm_rank) % comm_size_;
                if( counts[i] == 0 ) {
                   assert(send_lengths[i] == 0);
                   continue;
                }

                CommTarget& node = node_[i];
                const bool use_buffer = (node_send_lengths_buffer[i] != 0);
                const bool use_ptr = (counts[i] > node_send_lengths_buffer[i] + 1);

                assert(use_ptr || use_buffer);
                int offset = offsets[i];
                const int offset_org = offset;
                int length_ptr = 0;
                int length_ptr_reduced = 0;
                int length_buffer = 0;
                stream[offset++] = 0; // the "number of pointers" entry
                const int offset_targets = offset;

                if( use_ptr ) {
                   length_ptr = collect_targets_ptr(node, sssp_state, graph, stream + offset_targets, vertices_pos + pos_offset);
                   assert(length_ptr <= counts[i]);
                   assert(i + 1 == comm_size_ || offset + length_ptr <= offsets[i + 1]);
                   offset += length_ptr;
                }
                if( use_buffer ) {
                   const int stream_offset = length_ptr;
                   length_buffer = collect_targets_buffer(node, graph, sssp_state, stream_offset, stream + offset_targets, vertices_pos + pos_offset);
                }

                offset = offset_targets;
                send_lengths[i] = 1; // to store the offset
                if( use_ptr ) {
                   length_ptr_reduced = remove_sentinels_ptr(graph, length_ptr, stream + offset_targets, vertices_pos + pos_offset);
                   assert(length_ptr_reduced <= length_ptr);
                   assert(stream[offset_org] == 0);

                   stream[offset_org] = length_ptr_reduced; // here we store the ptr length
                   send_lengths[i] += length_ptr_reduced;
                   offset += length_ptr_reduced;
                   node.send_ptr.clear();
                   node_send_lengths_ptr[i] = 0;
                }
                if( use_buffer ) {
                   const int read_start = length_ptr;
                   const int write_start = length_ptr_reduced;
                   const int length_buffer_reduced = remove_sentinels_buffer(graph, read_start, write_start, length_buffer, stream + offset_targets, vertices_pos + pos_offset);
                   assert(length_buffer_reduced <= length_buffer);

                   send_lengths[i] += length_buffer_reduced;
                   node.send_data.clear();
                   node_send_lengths_buffer[i] = 0;
                }
                assert(1 <= send_lengths[i] && send_lengths[i] <= counts[i]);

                // nothing new added?
                if( send_lengths[i] == 1 )
                   send_lengths[i] = 0;

             } // #pragma omp for schedule(static)
          } // #pragma omp parallel
          USER_END(a2a_merge);

          void* sendbuf = buffer_provider_->second_buffer();
          void* recvbuf = buffer_provider_->clear_buffers();
          MPI_Datatype type = buffer_provider_->data_type();
          const int recvbufsize = buffer_provider_->max_size() / es;

          PROF(merge_time_ += tk_all);
          USER_START(a2a_comm);
          VERBOSE(if(loop > 0 && mpi.isMaster()) print_with_prefix("Alltoall with pointer (Again)"));
          scatter_.alltoallv(sendbuf, recvbuf, type, recvbufsize);
          PROF(comm_time_ += tk_all);
          USER_END(a2a_comm);

          VERBOSE(last_send_size_ += scatter_.get_send_count() * es);
          VERBOSE(last_recv_size_ += scatter_.get_recv_count() * es);

          int* recv_offsets = scatter_.get_recv_offsets();

 #pragma omp parallel for
          for(int i = 0; i < comm_size_; ++i) {
             int offset = recv_offsets[i];
             if( recv_offsets[i + 1] == recv_offsets[i] )
                continue;

             const int length_ptr = ((uint32_t*)recvbuf)[offset];
             offset++;

             // store the received distances (method lives in sssp.hpp)
             buffer_provider_->received(recvbuf, offset, length_ptr, i, true);
             offset += length_ptr;
             assert(offset <= recv_offsets[i+1] );

             const int length_buf = recv_offsets[i + 1] - offset;
             assert(loop == 0 || length_buf == 0);
             buffer_provider_->received(recvbuf, offset, length_buf, i, false);
          }
          PROF(recv_proc_time_ += tk_all);

          buffer_provider_->finish();
          PROF(recv_proc_large_time_ += tk_all);
       }

 #ifndef NDEBUG
       for(int i = 0; i < comm_size_; ++i)
          assert(node_[i].send_ptr.size() == 0);
 #endif
    }

	void run_ptr(const Graph2DCSR& graph, const SsspState& sssp_state, int32_t* restrict vertices_pos) {
		PROF(profiling::TimeKeeper tk_all);
		const int n_threads = omp_get_max_threads();
		const int es = buffer_provider_->element_size();
		assert(sizeof(uint32_t) == es);
      const int MINIMUM_POINTER_SPACE = 40;
		const int max_size_per_node = buffer_provider_->max_size() / (es * comm_size_);
      const int max_size_per_thread =  buffer_provider_->max_size() / (es * n_threads);
		VERBOSE(last_send_size_ = 0);
		VERBOSE(last_recv_size_ = 0);
		int node_send_lengths[comm_size_];
		int comm_rank;
		MPI_Comm_rank(comm_, &comm_rank);
		assert(0 <= comm_rank && comm_rank < comm_size_);

#pragma omp parallel for schedule(static)
      for(int i = 0; i < comm_size_; ++i) {
         CommTarget& node = node_[i];
         //flush(node); // todo: problem?
         node_send_lengths[i] = 0;

         if( 0 == node.send_ptr.size() )
            continue;
         node_send_lengths[i] = get_node_send_length_buffer(node, sssp_state, graph);
      }

		for(int loop = 0; ; ++loop) {
			USER_START(a2a_merge);

#pragma omp parallel
			{
				int* counts = scatter_.get_counts();
				int size_thread = 0;
#pragma omp for schedule(static)
				for(int c = 0; c < comm_size_; ++c) {
				   const int i = (c + comm_rank) % comm_size_;
					CommTarget& node = node_[i];
               assert(0 == counts[i]);

               if( 0 == node.send_ptr.size() )
                  continue;

               const int spare_size = max_size_per_thread - size_thread;
               if( spare_size < MINIMUM_POINTER_SPACE ) {
                  assert(size_thread != 0);
                  continue;
               }
               assert(size_thread < max_size_per_thread);

               const int node_send_length = node_send_lengths[i];
               counts[i] = node_send_length;

               // is the size too large and do we have something already?
               if( node_send_length > max_size_per_node && size_thread > 0 ) {
                  counts[i] = 0;
                  //std::cout << mpi.rank_2d << "X terminate early" << '\n';
                  continue;
               }

               if( size_thread + node_send_length > max_size_per_thread) {
                  counts[i] = 0;

              //    std::cout << mpi.rank_2d << " terminate early" << '\n';

                  if( size_thread == 0 ) {
                     std::cerr << "memory issue for node send:" << node_send_length << " > " << max_size_per_thread << "\n";
                     MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
                  }
               }
               size_thread += node_send_length;
				} // #pragma omp for schedule(static)
			} // #pragma omp parallel

			scatter_.sum();

			if(loop > 0) {
				int has_data = (scatter_.get_send_count() > 0);
				MPI_Allreduce(MPI_IN_PLACE, &has_data, 1, MPI_INT, MPI_LOR, comm_);

				if( mpi.isMaster() && has_data )
				   std::cout << "has_data: " << has_data << '\n';

				if(has_data == 0) break;
			}

			int* const send_lengths = scatter_.get_send_lengths();

#pragma omp parallel
			{
				int* offsets = scatter_.get_offsets();
				int* counts = scatter_.get_counts_org();
	         uint32_t* stream = (uint32_t*)buffer_provider_->second_buffer();

            const int64_t pos_offset = omp_get_thread_num() * graph.num_local_verts_;
#pragma omp for schedule(static)
				for( int c = 0; c < comm_size_; ++c ) {
				   const int i = (c + comm_rank) % comm_size_;
				   if( counts[i] == 0 ) {
				      assert(send_lengths[i] == 0);
				      continue;
				   }

					CommTarget& node = node_[i];
					if( 0 == node.send_ptr.size() ) {
					   assert(send_lengths[i] == 0);
					   continue;
					}

					const int length_ptr = collect_targets_ptr(node, sssp_state, graph, stream + offsets[i], vertices_pos + pos_offset);
               const int length_reduced = remove_sentinels_ptr(graph, length_ptr, stream + offsets[i], vertices_pos + pos_offset);

					assert(length_reduced <= length_ptr && length_ptr <= counts[i]);
					assert(i + 1 == comm_size_ || offsets[i] + length_ptr <= offsets[i + 1]);
					send_lengths[i] = length_reduced;

					node.send_ptr.clear();
				} // #pragma omp for schedule(static)
			} // #pragma omp parallel
			USER_END(a2a_merge);

			void* sendbuf = buffer_provider_->second_buffer();
			void* recvbuf = buffer_provider_->clear_buffers();
			MPI_Datatype type = buffer_provider_->data_type();
			const int recvbufsize = buffer_provider_->max_size() / es;

			PROF(merge_time_ += tk_all);
			USER_START(a2a_comm);
			VERBOSE(if(loop > 0 && mpi.isMaster()) print_with_prefix("Alltoall with pointer (Again)"));
			scatter_.alltoallv(sendbuf, recvbuf, type, recvbufsize);
			PROF(comm_time_ += tk_all);
			USER_END(a2a_comm);

			VERBOSE(last_send_size_ += scatter_.get_send_count() * es);
			VERBOSE(last_recv_size_ += scatter_.get_recv_count() * es);

			int* recv_offsets = scatter_.get_recv_offsets();

#pragma omp parallel for
			for(int i = 0; i < comm_size_; ++i) {
				const int offset = recv_offsets[i];
				const int length = recv_offsets[i+1] - offset;

				// store the received distances (method lives in sssp.hpp)
				buffer_provider_->received(recvbuf, offset, length, i, true);
			}
			PROF(recv_proc_time_ += tk_all);

			buffer_provider_->finish();
			PROF(recv_proc_large_time_ += tk_all);
		}

#ifndef NDEBUG
		for(int i = 0; i < comm_size_; ++i)
		   assert(node_[i].send_ptr.size() == 0);
#endif
	}

	void run() {
	   assert(0 && "currently bottom-up is not supported..");
	}


	void run_buffer(const Graph2DCSR& graph, const SsspState& sssp_state, int32_t* restrict vertices_pos) {
		// merge
		PROF(profiling::TimeKeeper tk_all);
		const int es = buffer_provider_->element_size();
		const int64_t num_local_verts = graph.num_local_verts_;

		assert(es == sizeof(uint32_t)); // todo just for top-down!
		VERBOSE(last_send_size_ = 0);
		VERBOSE(last_recv_size_ = 0);
		USER_START(a2a_merge);

#ifndef NDEBUG
#if !USE_PROPER_HASHMAP
      for( int64_t i = 0; i < num_local_verts * omp_get_max_threads(); ++i )
         assert(vertices_pos[i] == -1);
#endif
#endif

#pragma omp parallel
		{
			int* counts = scatter_.get_counts();
#pragma omp for schedule(static)
			for(int i = 0; i < comm_size_; ++i) {
				CommTarget& node = node_[i];
				flush(node);
				for(int b = 0; b < (int)node.send_data.size(); ++b) {
					counts[i] += node.send_data[b].length;
				}
			} // #pragma omp for schedule(static)
		}

		scatter_.sum();

		int* const send_lengths = scatter_.get_send_lengths();

#pragma omp parallel
		{
			int* offsets = scatter_.get_offsets();
			uint8_t* dst = (uint8_t*)buffer_provider_->second_buffer();

#if USE_PROPER_HASHMAP
			std::unordered_map<LocalVertex, int> tgt_map;
#endif

#pragma omp for schedule(static)
			for(int i = 0; i < comm_size_; ++i) {
				CommTarget& node = node_[i];
            uint32_t* const stream = (uint32_t*) (dst + offsets[i] * es);
            const int pos_offset = omp_get_thread_num() * num_local_verts;
				const int length_buffer = collect_targets_buffer(node, graph, sssp_state, 0, stream,
#if USE_PROPER_HASHMAP
				      tgt_map);
#else
				      vertices_pos + pos_offset);
#endif
			   const int length_reduced = remove_sentinels_buffer(graph, 0, 0, length_buffer, stream, vertices_pos + pos_offset);
		      assert(send_lengths[i] >= length_reduced);
		      send_lengths[i] = length_reduced;

				node.send_data.clear();
			} // #pragma omp for schedule(static)
		} // #pragma omp parallel
		USER_END(a2a_merge);

		void* sendbuf = buffer_provider_->second_buffer();
		void* recvbuf = buffer_provider_->clear_buffers();
		MPI_Datatype type = buffer_provider_->data_type();

		const int recvbufsize = buffer_provider_->max_size() / sizeof(uint32_t);
		PROF(merge_time_ += tk_all);
		USER_START(a2a_comm);
		scatter_.alltoallv(sendbuf, recvbuf, type, recvbufsize);
		PROF(comm_time_ += tk_all);
		USER_END(a2a_comm);

		VERBOSE(last_send_size_ = scatter_.get_send_count() * es);
		VERBOSE(last_recv_size_ = scatter_.get_recv_count() * es);

		int* recv_offsets = scatter_.get_recv_offsets();

#pragma omp parallel for schedule(dynamic,1)
		for(int i = 0; i < comm_size_; ++i) {
			const int offset = recv_offsets[i];
			const int length = recv_offsets[i+1] - offset;

			buffer_provider_->received(recvbuf, offset, length, i, false);
		}

		PROF(recv_proc_time_ += tk_all);
	}
#if PROFILING_MODE
	void submit_prof_info(int level, bool with_ptr) {
		merge_time_.submit("merge a2a data", level);
		comm_time_.submit("a2a comm", level);
		recv_proc_time_.submit("proc recv data", level);
		if(with_ptr) {
			recv_proc_large_time_.submit("proc recv large data", level);
		}
		VERBOSE(profiling::g_pis.submitCounter(last_send_size_, "a2a send data", level);)
		VERBOSE(profiling::g_pis.submitCounter(last_recv_size_, "a2a recv data", level);)
	}
#endif
#if VERBOSE_MODE
	int get_last_send_size() { return last_send_size_; }
#endif
private:

	struct DynamicDataSet {
		// lock topology
		// FoldNode::send_mutex -> thread_sync_
		pthread_mutex_t thread_sync_;
	} *d_;

	MPI_Comm comm_;

	int buffer_size_;
	int comm_size_;

	int node_list_length_;
	CommTarget* node_;
	AlltoallBufferHandler* buffer_provider_;
	ScatterContext scatter_;

	PROF(profiling::TimeSpan merge_time_);
	PROF(profiling::TimeSpan comm_time_);
	PROF(profiling::TimeSpan recv_proc_time_);
	PROF(profiling::TimeSpan recv_proc_large_time_);
	VERBOSE(int last_send_size_);
	VERBOSE(int last_recv_size_);

	void flush(CommTarget& node) {
		if(node.cur_buf.ptr != NULL) {
			node.cur_buf.length = node.filled_size_;
			node.send_data.push_back(node.cur_buf);
			node.cur_buf.ptr = NULL;
		}
	}

	void* get_send_buffer() {
		CTRACER(get_send_buffer);
		pthread_mutex_lock(&d_->thread_sync_);
		void* ret = buffer_provider_->get_buffer();
		pthread_mutex_unlock(&d_->thread_sync_);
		return ret;
	}
};

// Allgather

class MpiCompletionHandler {
public:
	virtual ~MpiCompletionHandler() { }
	virtual void complete(MPI_Status* status) = 0;
};

class MpiRequestManager {
public:
	MpiRequestManager(int MAX_REQUESTS)
		: MAX_REQUESTS(MAX_REQUESTS)
		, finish_count(0)
		, reqs(new MPI_Request[MAX_REQUESTS])
		, handlers(new MpiCompletionHandler*[MAX_REQUESTS])
	{
		for(int i = 0; i < MAX_REQUESTS; ++i) {
			reqs[i] = MPI_REQUEST_NULL;
			empty_list.push_back(i);
		}
	}
	~MpiRequestManager() {
		delete [] reqs; reqs = NULL;
		delete [] handlers; handlers = NULL;
	}
	MPI_Request* submit_handler(MpiCompletionHandler* handler) {
		if(empty_list.size() == 0) {
			fprintf(IMD_OUT, "No more empty MPI requests...\n");
			throw "No more empty MPI requests...";
		}
		int empty = empty_list.back();
		empty_list.pop_back();
		handlers[empty] = handler;
		return &reqs[empty];
	}
	void finished() {
		--finish_count;
	}
	void run(int finish_count__) {
		finish_count += finish_count__;

		while(finish_count > 0) {
			assert(MAX_REQUESTS >= 0);
			if(empty_list.size() == static_cast<unsigned>(MAX_REQUESTS)) {
				fprintf(IMD_OUT, "Error: No active request\n");
				throw "Error: No active request";
			}
			int index;
			MPI_Status status;
			MPI_Waitany(MAX_REQUESTS, reqs, &index, &status);
			if(index == MPI_UNDEFINED) {
				fprintf(IMD_OUT, "MPI_Waitany returns MPI_UNDEFINED ...\n");
				throw "MPI_Waitany returns MPI_UNDEFINED ...";
			}
			MpiCompletionHandler* handler = handlers[index];
			reqs[index] = MPI_REQUEST_NULL;
			empty_list.push_back(index);

			handler->complete(&status);
		}
	}

private:
	int MAX_REQUESTS;
	int finish_count;
	MPI_Request *reqs;
	MpiCompletionHandler** handlers;
	std::vector<int> empty_list;
};

template <typename T>
class AllgatherHandler : public MpiCompletionHandler {
public:
	AllgatherHandler() { }
	virtual ~AllgatherHandler() { }

	void start(MpiRequestManager* req_man_, T *buffer_, int* count_, int* offset_, MPI_Comm comm_,
			int rank_, int size_, int left_, int right_, int tag_)
	{
		req_man = req_man_;
		buffer = buffer_;
		count = count_;
		offset = offset_;
		comm = comm_;
		rank = rank_;
		size = size_;
		left = left_;
		right = right_;
		tag = tag_;

		current = 1;
		l_sendidx = rank;
		l_recvidx = (rank + size + 1) % size;
		r_sendidx = rank;
		r_recvidx = (rank + size - 1) % size;

		next();
	}

	virtual void complete(MPI_Status* status) {
		if(++complete_count == 4) {
			next();
		}
	}

private:
	MpiRequestManager* req_man;

	T *buffer;
	int *count;
	int *offset;
	MPI_Comm comm;
	int rank;
	int size;
	int left;
	int right;
	int tag;

	int current;
	int l_sendidx;
	int l_recvidx;
	int r_sendidx;
	int r_recvidx;
	int complete_count;

	void next() {
		if(current >= size) {
			req_man->finished();
			return ;
		}

		if(l_sendidx >= size) l_sendidx -= size;
		if(l_recvidx >= size) l_recvidx -= size;
		if(r_sendidx < 0) r_sendidx += size;
		if(r_recvidx < 0) r_recvidx += size;

		int l_send_off = offset[l_sendidx];
		int l_send_cnt = count[l_sendidx] / 2;
		int l_recv_off = offset[l_recvidx];
		int l_recv_cnt = count[l_recvidx] / 2;

		int r_send_off = offset[r_sendidx] + count[r_sendidx] / 2;
		int r_send_cnt = count[r_sendidx] - count[r_sendidx] / 2;
		int r_recv_off = offset[r_recvidx] + count[r_recvidx] / 2;
		int r_recv_cnt = count[r_recvidx] - count[r_recvidx] / 2;

		MPI_Irecv(&buffer[l_recv_off], l_recv_cnt, MpiTypeOf<T>::type,
				right, tag, comm, req_man->submit_handler(this));
		MPI_Irecv(&buffer[r_recv_off], r_recv_cnt, MpiTypeOf<T>::type,
				left, tag, comm, req_man->submit_handler(this));
		MPI_Isend(&buffer[l_send_off], l_send_cnt, MpiTypeOf<T>::type,
				left, tag, comm, req_man->submit_handler(this));
		MPI_Isend(&buffer[r_send_off], r_send_cnt, MpiTypeOf<T>::type,
				right, tag, comm, req_man->submit_handler(this));

		++current;
		++l_sendidx;
		++l_recvidx;
		--r_sendidx;
		--r_recvidx;

		complete_count = 0;
	}
};

template <typename T>
class AllgatherStep1Handler : public MpiCompletionHandler {
public:
	AllgatherStep1Handler() { }
	virtual ~AllgatherStep1Handler() { }

	void start(MpiRequestManager* req_man_, T *buffer_, int* count_, int* offset_,
			COMM_2D comm_, int unit_x_, int unit_y_, int steps_, int tag_)
	{
		req_man = req_man_;
		buffer = buffer_;
		count = count_;
		offset = offset_;
		comm = comm_;
		unit_x = unit_x_;
		unit_y = unit_y_;
		steps = steps_;
		tag = tag_;

		current = 1;

		send_to = get_rank(-1);
		recv_from = get_rank(1);

		next();
	}

	virtual void complete(MPI_Status* status) {
		if(++complete_count == 2) {
			next();
		}
	}

private:
	MpiRequestManager* req_man;

	T *buffer;
	int *count;
	int *offset;
	COMM_2D comm;
	int unit_x;
	int unit_y;
	int steps;
	int tag;

	int send_to;
	int recv_from;

	int current;
	int complete_count;

	int get_rank(int diff) {
		int pos_x = (comm.rank_x + unit_x * diff + comm.size_x) % comm.size_x;
		int pos_y = (comm.rank_y + unit_y * diff + comm.size_y) % comm.size_y;
		return comm.rank_map[pos_x + pos_y * comm.size_x];
	}

	void next() {
		if(current >= steps) {
			req_man->finished();
			return ;
		}

		int sendidx = get_rank(current - 1);
		int recvidx = get_rank(current);

		int send_off = offset[sendidx];
		int send_cnt = count[sendidx];
		int recv_off = offset[recvidx];
		int recv_cnt = count[recvidx];

		MPI_Irecv(&buffer[recv_off], recv_cnt, MpiTypeOf<T>::type,
				recv_from, tag, comm.comm, req_man->submit_handler(this));
		MPI_Isend(&buffer[send_off], send_cnt, MpiTypeOf<T>::type,
				send_to, tag, comm.comm, req_man->submit_handler(this));

		++current;
		complete_count = 0;
	}
};

template <typename T>
class AllgatherStep2Handler : public MpiCompletionHandler {
public:
	AllgatherStep2Handler() { }
	virtual ~AllgatherStep2Handler() { }

	void start(MpiRequestManager* req_man_, T *buffer_, int* count_, int* offset_,
			COMM_2D comm_, int unit_x_, int unit_y_, int steps_, int width_, int tag_)
	{
		req_man = req_man_;
		buffer = buffer_;
		count = count_;
		offset = offset_;
		comm = comm_;
		unit_x = unit_x_;
		unit_y = unit_y_;
		steps = steps_;
		width = width_;
		tag = tag_;

		current = 1;

		send_to = get_rank(-1, 0);
		recv_from = get_rank(1, 0);

		next();
	}

	virtual void complete(MPI_Status* status) {
		if(++complete_count == width*2) {
			next();
		}
	}

private:
	MpiRequestManager* req_man;

	T *buffer;
	int *count;
	int *offset;
	COMM_2D comm;
	int unit_x;
	int unit_y;
	int steps;
	int width;
	int tag;

	int send_to;
	int recv_from;

	int current;
	int complete_count;

	int get_rank(int step_diff, int idx) {
		int pos_x = (comm.rank_x + unit_x * step_diff + (!unit_x * idx) + comm.size_x) % comm.size_x;
		int pos_y = (comm.rank_y + unit_y * step_diff + (!unit_y * idx) + comm.size_y) % comm.size_y;
		return comm.rank_map[pos_x + pos_y * comm.size_x];
	}

	void next() {
		if(current >= steps) {
			req_man->finished();
			return ;
		}

		for(int i = 0; i < width; ++i) {
			int sendidx = get_rank(current - 1, i);
			int recvidx = get_rank(current, i);

			int send_off = offset[sendidx];
			int send_cnt = count[sendidx];
			int recv_off = offset[recvidx];
			int recv_cnt = count[recvidx];

			MPI_Irecv(&buffer[recv_off], recv_cnt, MpiTypeOf<T>::type,
					recv_from, tag, comm.comm, req_man->submit_handler(this));
			MPI_Isend(&buffer[send_off], send_cnt, MpiTypeOf<T>::type,
					send_to, tag, comm.comm, req_man->submit_handler(this));
		}

		++current;
		complete_count = 0;
	}
};

template <typename T>
void my_allgatherv_2d(T *sendbuf, int send_count, T *recvbuf, int* recv_count, int* recv_offset, COMM_2D comm)
{
	// copy own data
	memcpy(&recvbuf[recv_offset[comm.rank]], sendbuf, sizeof(T) * send_count);

	if(mpi.isMultiDimAvailable == false) {
		MpiRequestManager req_man(8);
		AllgatherHandler<T> handler;
		int size; MPI_Comm_size(comm.comm, &size);
		int rank; MPI_Comm_rank(comm.comm, &rank);
		int left = (rank + size - 1) % size;
		int right = (rank + size + 1) % size;
		handler.start(&req_man, recvbuf, recv_count, recv_offset, comm.comm,
				rank, size, left, right, PRM::MY_EXPAND_TAG1);
		req_man.run(1);
		return ;
	}

	//MPI_Allgatherv(sendbuf, send_count, MpiTypeOf<T>::type, recvbuf, recv_count, recv_offset, MpiTypeOf<T>::type, comm.comm);
	//return;

	MpiRequestManager req_man((comm.size_x + comm.size_y)*4);
	int split_count[4][comm.size];
	int split_offset[4][comm.size];

	for(int s = 0; s < 4; ++s) {
		for(int i = 0; i < comm.size; ++i) {
			int max = recv_count[i];
			int split = (max + 3) / 4;
			int start = recv_offset[i] + std::min(max, split * s);
			int end = recv_offset[i] + std::min(max, split * (s+1));
			split_count[s][i] = end - start;
			split_offset[s][i] = start;
		}
	}

	{
		AllgatherStep1Handler<T> handler[4];
		handler[0].start(&req_man, recvbuf, split_count[0], split_offset[0], comm, 1, 0, comm.size_x, PRM::MY_EXPAND_TAG1);
		handler[1].start(&req_man, recvbuf, split_count[1], split_offset[1], comm,-1, 0, comm.size_x, PRM::MY_EXPAND_TAG1);
		handler[2].start(&req_man, recvbuf, split_count[2], split_offset[2], comm, 0, 1, comm.size_y, PRM::MY_EXPAND_TAG2);
		handler[3].start(&req_man, recvbuf, split_count[3], split_offset[3], comm, 0,-1, comm.size_y, PRM::MY_EXPAND_TAG2);
		req_man.run(4);
	}
	{
		AllgatherStep2Handler<T> handler[4];
		handler[0].start(&req_man, recvbuf, split_count[0], split_offset[0], comm, 0, 1, comm.size_y, comm.size_x, PRM::MY_EXPAND_TAG1);
		handler[1].start(&req_man, recvbuf, split_count[1], split_offset[1], comm, 0,-1, comm.size_y, comm.size_x, PRM::MY_EXPAND_TAG1);
		handler[2].start(&req_man, recvbuf, split_count[2], split_offset[2], comm, 1, 0, comm.size_x, comm.size_y, PRM::MY_EXPAND_TAG2);
		handler[3].start(&req_man, recvbuf, split_count[3], split_offset[3], comm,-1, 0, comm.size_x, comm.size_y, PRM::MY_EXPAND_TAG2);
		req_man.run(4);
	}
}

template <typename T>
void my_allgather_2d(T *sendbuf, int count, T *recvbuf, COMM_2D comm)
{
	memcpy(&recvbuf[count * comm.rank], sendbuf, sizeof(T) * count);
	int recv_count[comm.size];
	int recv_offset[comm.size+1];
	recv_offset[0] = 0;
	for(int i = 0; i < comm.size; ++i) {
		recv_count[i] = count;
		recv_offset[i+1] = recv_offset[i] + count;
	}
	my_allgatherv_2d(sendbuf, count, recvbuf, recv_count, recv_offset, comm);
}

#undef debug

#endif /* ABSTRACT_COMM_HPP_ */
