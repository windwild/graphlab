/**  
 * Copyright (c) 2009 Carnegie Mellon University. 
 *     All rights reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an "AS
 *  IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 *  express or implied.  See the License for the specific language
 *  governing permissions and limitations under the License.
 *
 * For more about this software visit:
 *
 *      http://www.graphlab.ml.cmu.edu
 *
 */

#ifndef GRAPHLAB_DISTRIBUTED_INGRESS_BASE_HPP
#define GRAPHLAB_DISTRIBUTED_INGRESS_BASE_HPP

#include <boost/functional/hash.hpp>

#include <graphlab/rpc/buffered_exchange.hpp>
#include <graphlab/graph/graph_basic_types.hpp>
#include <graphlab/graph/ingress/idistributed_ingress.hpp>
#include <graphlab/graph/ingress/ingress_edge_decision.hpp>
#include <graphlab/graph/distributed_graph.hpp>
#include <google/malloc_extension.h>

#include <graphlab/macros_def.hpp>
namespace graphlab {
  template<typename VertexData, typename EdgeData>
  class distributed_graph;

  template<typename VertexData, typename EdgeData>
  class distributed_ingress_base : 
    public idistributed_ingress<VertexData, EdgeData> {
  public:
    typedef distributed_graph<VertexData, EdgeData> graph_type;
    /// The type of the vertex data stored in the graph 
    typedef VertexData vertex_data_type;
    /// The type of the edge data stored in the graph 
    typedef EdgeData   edge_data_type;
    /// The type of a vertex is a simple size_t
    typedef graphlab::vertex_id_type vertex_id_type;
    /// Vertex record
    typedef typename graph_type::lvid_type  lvid_type;
    typedef typename graph_type::vertex_record vertex_record;
    typedef typename graph_type::mirror_type mirror_type;


    /// The rpc interface for this object
    dc_dist_object<distributed_ingress_base> rpc;
    /// The underlying distributed graph object that is being loaded
    graph_type& graph;

    /// Temporar buffers used to store vertex data on ingress
    struct vertex_buffer_record {
      vertex_id_type vid;
      vertex_data_type vdata;
      vertex_buffer_record(vertex_id_type vid = -1,
                           vertex_data_type vdata = vertex_data_type()) :
        vid(vid), vdata(vdata) { }
      void load(iarchive& arc) { arc >> vid >> vdata; }
      void save(oarchive& arc) const { arc << vid << vdata; }
    }; 
    buffered_exchange<vertex_buffer_record> vertex_exchange;

    /// Temporar buffers used to store edge data on ingress
    struct edge_buffer_record {
      vertex_id_type source, target;
      edge_data_type edata;
      edge_buffer_record(const vertex_id_type& source = vertex_id_type(-1), 
                         const vertex_id_type& target = vertex_id_type(-1), 
                         const edge_data_type& edata = edge_data_type()) :
        source(source), target(target), edata(edata) { }
      void load(iarchive& arc) { arc >> source >> target >> edata; }
      void save(oarchive& arc) const { arc << source << target << edata; }
    };
    buffered_exchange<edge_buffer_record> edge_exchange;

   
    struct shuffle_record : public graphlab::IS_POD_TYPE {
      vertex_id_type vid, num_in_edges, num_out_edges;
      shuffle_record(vertex_id_type vid = 0, vertex_id_type num_in_edges = 0,
                     vertex_id_type num_out_edges = 0) : 
        vid(vid), num_in_edges(num_in_edges), num_out_edges(num_out_edges) { }     
    }; // end of shuffle_record


    struct vertex_negotiator_record {
      vertex_id_type vid;
      vertex_id_type num_in_edges, num_out_edges;
      procid_t owner;
      mirror_type mirrors;
      vertex_data_type vdata;
      vertex_negotiator_record() : 
        vid(-1), num_in_edges(0), num_out_edges(0), owner(-1) { }
      void load(iarchive& arc) { 
        arc >> vid >> num_in_edges >> num_out_edges >> owner >> mirrors >> vdata;
      }
      void save(oarchive& arc) const { 
        arc << vid << num_in_edges << num_out_edges << owner << mirrors << vdata;
      }
    };

    ingress_edge_decision<VertexData, EdgeData> edge_decision;

    public:
    distributed_ingress_base(distributed_control& dc, graph_type& graph) :
      rpc(dc, this), graph(graph), vertex_exchange(dc), edge_exchange(dc),
     edge_decision(dc) {
      rpc.barrier();
    } // end of constructor

    ~distributed_ingress_base() { }

virtual void add_edge(vertex_id_type source, vertex_id_type target,
                  const EdgeData& edata) {
      const procid_t owning_proc = edge_decision.edge_to_proc_random(source, target, rpc.numprocs());
      const edge_buffer_record record(source, target, edata);
      edge_exchange.send(owning_proc, record);
    } // end of add edge

virtual void add_vertex(vertex_id_type vid, const VertexData& vdata)  { 
      const procid_t owning_proc = vertex_to_proc(vid);
      const vertex_buffer_record record(vid, vdata);
      vertex_exchange.send(owning_proc, record);
    } // end of add vertex


virtual void finalize() {
      edge_exchange.flush(); vertex_exchange.flush();
      // Add all the edges to the local graph --------------------------------
      logstream(LOG_INFO) << "Graph Finalize: constructing local graph" << std::endl;
      size_t nedges = edge_exchange.size()+1;
      graph.local_graph.reserve_edge_space(nedges + 1);
      {
        typedef typename buffered_exchange<edge_buffer_record>::buffer_type 
          edge_buffer_type;
        edge_buffer_type edge_buffer;
        procid_t proc;
        while(edge_exchange.recv(proc, edge_buffer)) {
          foreach(const edge_buffer_record& rec, edge_buffer) {
            // Get the source_vlid;
            lvid_type source_lvid(-1);
            if(graph.vid2lvid.find(rec.source) == graph.vid2lvid.end()) {
              source_lvid = graph.vid2lvid.size();
              graph.vid2lvid[rec.source] = source_lvid;
              // graph.local_graph.resize(source_lvid + 1);
            } else source_lvid = graph.vid2lvid[rec.source];
            // Get the target_lvid;
            lvid_type target_lvid(-1);
            if(graph.vid2lvid.find(rec.target) == graph.vid2lvid.end()) {
              target_lvid = graph.vid2lvid.size();
              graph.vid2lvid[rec.target] = target_lvid;
              // graph.local_graph.resize(target_lvid + 1);
            } else target_lvid = graph.vid2lvid[rec.target];

            // Add the edge data to the graph
            if (source_lvid >= graph.local_graph.num_vertices() ||
                target_lvid >= graph.local_graph.num_vertices())
              graph.local_graph.resize(std::max(source_lvid, target_lvid) + 1);

            graph.local_graph.add_edge(source_lvid, target_lvid, rec.edata);          
          } // end of loop over add edges
        } // end for loop over buffers
      }
      edge_exchange.clear();

      // Finalize local graph
      logstream(LOG_INFO) << "Graph Finalize: finalizing local graph" << std::endl;
      graph.local_graph.finalize();
      logstream(LOG_INFO) << "Local graph info: " << std::endl
                          << "\t nverts: " << graph.local_graph.num_vertices()
                          << std::endl
                          << "\t nedges: " << graph.local_graph.num_edges()
                          << std::endl;


      // Initialize vertex records
      graph.lvid2record.resize(graph.vid2lvid.size());
      typedef typename boost::unordered_map<vertex_id_type, lvid_type>::value_type 
        vid2lvid_pair_type;
      foreach(const vid2lvid_pair_type& pair, graph.vid2lvid) 
        graph.lvid2record[pair.second].gvid = pair.first;      
      // Check conditions on graph
      ASSERT_EQ(graph.local_graph.num_vertices(), graph.lvid2record.size());   
   

      // Begin the shuffle phase for all the vertices that this
      // processor has seen determine the "negotiator" and send the
      // negotiator the edge information for that vertex.
      typedef std::vector< std::vector<shuffle_record> > proc2vids_type;
      proc2vids_type proc2vids(rpc.numprocs());
      foreach(const vid2lvid_pair_type& pair, graph.vid2lvid) {
        const vertex_id_type vid = pair.first;
        const vertex_id_type lvid = pair.second;
        const procid_t negotiator = vertex_to_proc(vid);
        const shuffle_record rec(vid, graph.local_graph.num_in_edges(lvid),
                                 graph.local_graph.num_out_edges(lvid));
        proc2vids[negotiator].push_back(rec);
      }

      // The returned local vertices are the vertices from each
      // machine for which this machine is a negotiator.
      logstream(LOG_INFO) 
        << "Graph Finalize: Exchanging shuffle records" << std::endl;
      mpi_tools::all2all(proc2vids, proc2vids);

      // Receive any vertex data sent by other machines
      typedef boost::unordered_map<vertex_id_type, vertex_negotiator_record>
        vrec_map_type;
      vrec_map_type vrec_map;
      {
        typedef typename buffered_exchange<vertex_buffer_record>::buffer_type 
          vertex_buffer_type;
        vertex_buffer_type vertex_buffer;
        procid_t proc;
        while(vertex_exchange.recv(proc, vertex_buffer)) {
          foreach(const vertex_buffer_record& rec, vertex_buffer) {
            vertex_negotiator_record& negotiator_rec = vrec_map[rec.vid];
            negotiator_rec.vdata = rec.vdata;
          }
        }
      } // end of loop to populate vrecmap

   
      // Update the mirror information for all vertices negotiated by
      // this machine
      logstream(LOG_INFO) 
        << "Graph Finalize: Accumulating mirror set for each vertex" << std::endl;
      for(procid_t proc = 0; proc < rpc.numprocs(); ++proc) {
        foreach(const shuffle_record& shuffle_rec, proc2vids[proc]) {
          vertex_negotiator_record& negotiator_rec = vrec_map[shuffle_rec.vid];
          negotiator_rec.num_in_edges += shuffle_rec.num_in_edges;
          negotiator_rec.num_out_edges += shuffle_rec.num_out_edges;
          negotiator_rec.mirrors.set_bit(proc);
        }
      }


      // Construct the vertex owner assignments and send assignment
      // along with vdata to all the mirrors for each vertex
      logstream(LOG_INFO) << "Graph Finalize: Constructing and sending vertex assignments" 
                          << std::endl;
      std::vector<size_t> counts(rpc.numprocs());      
      typedef typename vrec_map_type::value_type vrec_pair_type;
      buffered_exchange<vertex_negotiator_record> negotiator_exchange(rpc.dc());
      // Loop over all vertices and the vertex buffer
      foreach(vrec_pair_type& pair, vrec_map) {
        const vertex_id_type vid = pair.first;
        vertex_negotiator_record& negotiator_rec = pair.second;
        negotiator_rec.vid = vid; // update the vid if it has not been set

        // The branch here is because singleton edge doesn't participate in
        // the shuffle phase, so there is no mirror assigned. 
        if (negotiator_rec.mirrors.popcount() > 0) {
          // Find the best (least loaded) processor to assign the vertex.
          uint32_t first_mirror = 0; 
          ASSERT_TRUE(negotiator_rec.mirrors.first_bit(first_mirror));
          std::pair<size_t, uint32_t> 
             best_asg(counts[first_mirror], first_mirror);
          foreach(uint32_t proc, negotiator_rec.mirrors) {
              best_asg = std::min(best_asg, std::make_pair(counts[proc], proc));
          }
          negotiator_rec.owner = best_asg.second;
          counts[negotiator_rec.owner]++;
        } else {
          // random assign a singleton vertex to a proc
          size_t proc = negotiator_rec.vid % rpc.numprocs();
          negotiator_rec.mirrors.set_bit(proc);
          negotiator_rec.owner = proc;
        }
        // Notify all machines of the new assignment
        foreach(uint32_t proc, negotiator_rec.mirrors) {
            negotiator_exchange.send(proc, negotiator_rec);
        }
      } // end of loop over vertex records
      negotiator_exchange.flush();

      logstream(LOG_INFO) << "Graph Finalize: Recieving vertex assignments." << std::endl;
      {
        typedef typename buffered_exchange<vertex_negotiator_record>::buffer_type 
          buffer_type;
        buffer_type negotiator_buffer;
        procid_t proc;
        while(negotiator_exchange.recv(proc, negotiator_buffer)) {
          foreach(const vertex_negotiator_record& negotiator_rec, negotiator_buffer) {
            // ASSERT_TRUE(graph.vid2lvid.find(negotiator_rec.vid) != 
            //             graph.vid2lvid.end());
            
            // The assertion above is disabled because the receiver could 
            // receive a singleton edge which it has never seen.
            lvid_type lvid;
            if(graph.vid2lvid.find(negotiator_rec.vid) == graph.vid2lvid.end()) {
              lvid = graph.vid2lvid.size();
              graph.vid2lvid[negotiator_rec.vid] = lvid;
              graph.local_graph.add_vertex(lvid, negotiator_rec.vdata);
              graph.lvid2record.resize(graph.vid2lvid.size());
              graph.lvid2record[lvid].gvid = negotiator_rec.vid;
            } else {
              lvid = graph.vid2lvid[negotiator_rec.vid];
              ASSERT_LT(lvid, graph.local_graph.num_vertices());
              graph.local_graph.vertex_data(lvid) = negotiator_rec.vdata;
            }

            ASSERT_LT(lvid, graph.lvid2record.size());
            vertex_record& local_record = graph.lvid2record[lvid];
            local_record.owner = negotiator_rec.owner;
            ASSERT_EQ(local_record.num_in_edges, 0); // this should have not been set
            local_record.num_in_edges = negotiator_rec.num_in_edges;
            ASSERT_EQ(local_record.num_out_edges, 0); // this should have not been set
            local_record.num_out_edges = negotiator_rec.num_out_edges;
            ASSERT_TRUE(negotiator_rec.mirrors.begin() != negotiator_rec.mirrors.end());
            local_record._mirrors = negotiator_rec.mirrors;
            local_record._mirrors.clear_bit(negotiator_rec.owner);
          }
        }
      }

      ASSERT_EQ(graph.vid2lvid.size(), graph.local_graph.num_vertices());
      ASSERT_EQ(graph.lvid2record.size(), graph.local_graph.num_vertices());
 
      // Count the number of vertices owned locally
      graph.local_own_nverts = 0;
      foreach(const vertex_record& record, graph.lvid2record)
        if(record.owner == rpc.procid()) ++graph.local_own_nverts;

      // Finalize global graph statistics. 
      logstream(LOG_INFO)
        << "Graph Finalize: exchange global statistics " << std::endl;

      // Compute edge counts
      std::vector<size_t> swap_counts(rpc.numprocs(), graph.num_local_edges());
      mpi_tools::all2all(swap_counts, swap_counts);
      graph.nedges = 0;
      foreach(size_t count, swap_counts) graph.nedges += count;

      // compute begin edge id
      graph.begin_eid = 0;
      for(size_t i = 0; i < rpc.procid(); ++i) graph.begin_eid += swap_counts[i];

      // compute vertex count
      swap_counts.assign(rpc.numprocs(), graph.num_local_own_vertices());
      mpi_tools::all2all(swap_counts, swap_counts);
      graph.nverts = 0;
      foreach(size_t count, swap_counts) graph.nverts += count;

      // compute replicas
      swap_counts.assign(rpc.numprocs(), graph.num_local_vertices());
      mpi_tools::all2all(swap_counts, swap_counts);
      graph.nreplicas = 0;
      foreach(size_t count, swap_counts) graph.nreplicas += count;
    } // end of finalize


  protected:
    // HELPER ROUTINES =======================================================>    
    procid_t vertex_to_proc(const vertex_id_type vid) const { 
      return vid % rpc.numprocs();
    }        
  }; // end of distributed_ingress_base

}; // end of namespace graphlab
#include <graphlab/macros_undef.hpp>


#endif
