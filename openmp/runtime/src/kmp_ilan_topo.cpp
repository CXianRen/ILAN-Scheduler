#include "kmp_ilan_topo.h"


#include "kmp.h"

#include "kmp_debug.h"
#include <algorithm>

///////////////////////////////////////////////
///               Topology section          ///
///////////////////////////////////////////////

static ILANTopology global_ilan_topology;
static bool is_topology_initialized = false;

ILANTopology::ILANTopology(kmp_topology_t* global_toppology) {

  // read full topology
  init_read_full_topology(); 

  kmp_topology_ = global_toppology;
  num_sockets_= global_toppology->get_count(global_toppology->get_level(KMP_HW_SOCKET)),
  num_numa_   = global_toppology->get_count(global_toppology->get_level(KMP_HW_NUMA));
  num_cores_  = global_toppology->get_count(global_toppology->get_level(KMP_HW_CORE));

  numa_size_  = num_numa_ == 0 ? 0 : static_cast<kmp_uint8>(num_cores_ / num_numa_);
  
  // KA_TRACE(1, ("ILANTopology: Register topology with %d sockets, %d cores, %d numa nodes\n",
  //               num_sockets_, num_cores_, num_numa_));

  KMP_DEBUG_ASSERT(num_sockets_ > 0);
  KMP_DEBUG_ASSERT(num_numa_ > 0);
  KMP_DEBUG_ASSERT(num_cores_ > 0);
  KMP_DEBUG_ASSERT(numa_size_ > 0);
  
  ilan_numa_set_ = 0;
  ilan_cpu_set_ = 0;

  for(int i = 0; i < num_cores_; i++) {
    const kmp_hw_thread_t& hw_htread = global_toppology->at(i);
    // get os_id
    auto os_id = hw_htread.os_id;
    
    // KA_TRACE(1, ("ILANTopology: PU(thread) index %2d -> OS ID %2d\n",
    //               i, os_id));

    // now only consider bitmask up to 64 CPUs
    KMP_DEBUG_ASSERT(os_id<64 && os_id>=0);

    // get numa_id from map
    auto numa_id_iter = os_id_to_numa_id_.find(os_id);
    KMP_DEBUG_ASSERT(numa_id_iter != os_id_to_numa_id_.end());
    auto numa_id = numa_id_iter->second;

    // now only consider bitmask up to 64 NUMA nodes
    KMP_DEBUG_ASSERT(numa_id < 64); 
    
    // update numa_id_to_os_ids_ map
    auto numa_os_ids_iter = numa_id_to_os_ids_.find(numa_id);
    if (numa_os_ids_iter != numa_id_to_os_ids_.end()) {
      numa_os_ids_iter->second.push_back(os_id);
    } else {
      numa_id_to_os_ids_[numa_id] = 
        std::vector<kmp_uint32>{static_cast<kmp_uint32>(os_id)};
    }
    // update numa mask
    ilan_numa_set_ |= (1ULL << numa_id);
    // update cpu mask
    ilan_cpu_set_ |= (1ULL << os_id);
  }

  // sort the os_ids in each numa node
  // so the first os_id is the master os_id in that numa node
  for (auto& entry : numa_id_to_os_ids_) {
    std::vector<kmp_uint32>& os_ids = entry.second;
    std::sort(os_ids.begin(), os_ids.end());
  }

  showTopo();
}


void ILANTopology::init_read_full_topology() {

  hwloc_topology_t topology = nullptr;

  if (hwloc_topology_init(&topology) == -1) {
    KMP_FATAL(MsgExiting, "Hardware topology: init failed");
  }
  if (hwloc_topology_load(topology) == -1) {
    hwloc_topology_destroy(topology);
    KMP_FATAL(MsgExiting, "Hardware topology: load failed");
  }

  total_num_sockets_ = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_PACKAGE);
  total_num_numa_ = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_NUMANODE);
  total_num_cores_ = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_CORE);
  total_num_pus_ = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_PU);

  KMP_ASSERT(total_num_sockets_ > 0);
  KMP_ASSERT(total_num_numa_ > 0);
  KMP_ASSERT(total_num_cores_ > 0);
  KMP_ASSERT(total_num_pus_ > 0);

  KA_TRACE(1, ("ILANTopology: System has %d sockets, %d NUMA nodes, %d cores, %d PUs\n",
               total_num_sockets_, total_num_numa_, total_num_cores_, total_num_pus_));

  // Iterate through all NUMA nodes and collect their PUs
  for (int numa_idx = 0; numa_idx < total_num_numa_; numa_idx++) {
    hwloc_obj_t numa_obj = hwloc_get_obj_by_type(topology, HWLOC_OBJ_NUMANODE, numa_idx);
    
    KMP_ASSERT(numa_obj != nullptr);
    
    int numa_id = numa_obj->logical_index;
    
    // Iterate through all bits in the cpuset (each bit represents a PU)
    int pu_os_id = -1;
    hwloc_bitmap_foreach_begin(pu_os_id, numa_obj->cpuset) {
      os_id_to_numa_id_[pu_os_id] = numa_id;
      
      // KA_TRACE(1, ("ILANTopology: PU OS ID %3d -> NUMA %d\n", pu_os_id, numa_id));
    } hwloc_bitmap_foreach_end();
  }
  
  // Verify all PUs are mapped
  int unmapped_count = 0;
  for (int pu_idx = 0; pu_idx < total_num_pus_; pu_idx++) {
    hwloc_obj_t pu_obj = hwloc_get_obj_by_type(topology, HWLOC_OBJ_PU, pu_idx);
    if (pu_obj) {
      int pu_os_id = pu_obj->os_index;
      if (os_id_to_numa_id_.find(pu_os_id) == os_id_to_numa_id_.end()) {
        KA_TRACE(1, ("ILANTopology: Warning - PU OS ID %d not mapped to any NUMA node\n",
                     pu_os_id));
        KMP_ASSERT(0); // should not happen
      }
    }
  }
  
  hwloc_topology_destroy(topology);
}


void ILANTopology::showTopo() const {
  KA_TRACE(1, ("ILANTopology:\n"
               "    - Number of sockets: %d\n"
               "    - Number of NUMA nodes: %d\n"
               "    - Number of cores: %d\n"
               "    - Total number of socket: %d\n"
               "    - Total number of NUMA nodes: %d\n"
               "    - Total number of cores: %d\n",
              num_sockets_, num_numa_, num_cores_,
               total_num_sockets_, total_num_numa_, total_num_cores_));
  // print bitmask
  KA_TRACE(1, ("    - ILAN NUMA mask: 0x%02llx\n", ilan_numa_set_));
  KA_TRACE(1, ("    - ILAN  CPU mask: 0x%016llx\n", ilan_cpu_set_));
  // print numa to os ids map
  for (const auto& entry : numa_id_to_os_ids_) {
    kmp_uint32 numa_id = entry.first;
    const std::vector<kmp_uint32>& os_ids = entry.second;
    KA_TRACE(1, ("    - NUMA ID %2d: OS IDs :", numa_id));
    for (kmp_uint32 os_id : os_ids) {
      KA_TRACE(1, ("%2d ", os_id));
    }
    KA_TRACE(1, ("\n"));
  }

  // print full mapping from numa_id to os_ids
  KA_TRACE(1, ("ILANTopology: Full mapping from NUMA ID to OS IDs:\n"));

  std::unordered_map<kmp_uint32, std::vector<kmp_uint32>> full_numa_to_os_ids;
  for (const auto& entry : os_id_to_numa_id_) {
    kmp_uint32 os_id = entry.first;
    kmp_uint32 numa_id = entry.second;
    if (full_numa_to_os_ids.find(numa_id) != full_numa_to_os_ids.end()) {
      full_numa_to_os_ids[numa_id].push_back(os_id);
    } else {
      full_numa_to_os_ids[numa_id] = 
        std::vector<kmp_uint32>{static_cast<kmp_uint32>(os_id)};
    }
  }
  // print full mapping
  for (const auto& entry : full_numa_to_os_ids) {
    kmp_uint32 numa_id = entry.first;
    const std::vector<kmp_uint32>& os_ids = entry.second;
    KA_TRACE(1, ("    - NUMA ID %2d: OS IDs :", numa_id));
    for (kmp_uint32 os_id : os_ids) {
      KA_TRACE(1, ("%2d ", os_id)); 
    }
    KA_TRACE(1, ("\n"));
  } 
  KA_TRACE(1, ("\n"));

  
}



static void print_affinity(char* prefix, kmp_affinity_t *affin) {
  // level 10 : KMP_HW_CORE
  KA_TRACE(1, ("%s Affinity:\n" 
    "\tAffinity type: %d\n"
    "\tGrain level: %d\n"
    "\tNum masks: %d\n"
    "\tNum os id masks: %d\n"
    "\tCompact: %d\n"
    "\toffset: %d\n"
    "\tenv var: %s\n",
    prefix,
    affin->type, 
    affin->gran,          
    affin->num_masks, 
    affin->num_os_id_masks,
    affin->compact,
    affin->offset,
    affin->env_var
  ));

  for (unsigned i = 0; i < affin->num_masks; i++) {
    char buf[KMP_AFFIN_MASK_PRINT_LEN];
    __kmp_affinity_print_mask(buf, KMP_AFFIN_MASK_PRINT_LEN,
                              KMP_CPU_INDEX(affin->masks, i));
    KA_TRACE(1, ("\tMask %d: %s\n", i, buf));
  }
}


/*
  register kmp_global_topology
   read the topology information from the kmp inner structure
   to support cases like MPI + OpenMP or mannual cpu allocation

   call chain:
   kmp_affinity.cpp :__kmp_affinity_initialize
    -> __kmp_ilan_sync_affinity_topology
*/
static void ilan_register_topology(kmp_topology_t* global_toppology) {
  if (!is_topology_initialized) {
    global_ilan_topology = ILANTopology(global_toppology);
    is_topology_initialized = true;
  }
  // register kmp_global_topology
}


void ILAN::__kmp_ilan_sync_affinity_topology(kmp_affinity_t *global_affin, kmp_topology_t* global_toppology) {

  // register topology to ILAN
  ilan_register_topology(global_toppology);

  // print current affinity settings
  auto ncpus = global_toppology->get_num_hw_threads();
  KA_TRACE(1, ("num __kmp_topology->get_num_hw_threads(): %d\n",
               ncpus));

  print_affinity("Before", global_affin);

  if(global_affin->type != affinity_compact) {
    KA_TRACE(1, ("Affinity type is 'none', ILAN Scheduler will set it to:\n"
                 "\t'explicit'\n"
                 "\tgranularity: 'core'\n"));

    // To make bind_place do something
    global_affin->type = affinity_explicit;
    global_affin->num_masks = ncpus;

    KMP_CPU_ALLOC_ARRAY(global_affin->masks, ncpus);

    kmp_affin_mask_t *mask;
    for (int i = 0; i < ncpus; i++) {
      mask = KMP_CPU_INDEX(global_affin->masks, i);
      KMP_CPU_ZERO(mask);

      // get the OS id of the logical processor
      // OS id is the actual id used by  logical id mOS to set affinity
      // why we need this? because the available cores might not 
      // start from 0 and might not be continuous
      // e.g. only cores 2,4,6,8 are available in the system
      int os_id = global_toppology->at(i).os_id;
      KMP_CPU_SET(os_id, mask);
      KA_TRACE(1, ("Setting affinity mask for logical cpu %d (os id %d)\n",
                   i, os_id));
    }
    
    print_affinity("After", global_affin);
  }
}


ILANTopology& ILAN::__kmp_ilan_topology() {
  KMP_DEBUG_ASSERT(is_topology_initialized);
  return global_ilan_topology;
}


// given a gtid, return its numa id
kmp_uint32 ILANTopology::get_numa_id(kmp_int32 gtid){
  KMP_ASSERT(is_topology_initialized);

  kmp_uint32 numa_id = 0;

  // kmp_info_t *thread = __kmp_threads[gtid];
  // KMP_DEBUG_ASSERT(thread != nullptr);
  // int place = thread->th.th_current_place;
  // KMP_ASSERT(place >=0 && place < 64);
  // const kmp_hw_thread_t& hw_htread = __kmp_topology->at(place);
  // auto os_id = hw_htread.os_id;

  // // get numa_id from map
  // auto numa_id_iter = os_id_to_numa_id_.find(os_id);
  // KMP_DEBUG_ASSERT(numa_id_iter != os_id_to_numa_id_.end());
  // numa_id = numa_id_iter->second;

  // get os_id from gtid
  auto gtid_os_id_iter = gtid_to_os_id_.find(gtid);
  KMP_DEBUG_ASSERT(gtid_os_id_iter != gtid_to_os_id_.end());
  auto os_id = gtid_os_id_iter->second;

  // get numa_id from map
  auto numa_id_iter = os_id_to_numa_id_.find(os_id);
  KMP_DEBUG_ASSERT(numa_id_iter != os_id_to_numa_id_.end());
  numa_id = numa_id_iter->second;

  return numa_id;
}

// given a tid, return its ilan_master_numa_id
kmp_uint32 ILANTopology::get_numa_master_tid(kmp_int32 gtid){
  KMP_ASSERT(is_topology_initialized);

  kmp_uint32 master_tid = 0;

  // get the thread with the given tid
  // auto gtid = __kmp_gtid_from_tid(tid);
  auto numa_id = get_numa_id(gtid);

  // get the first os_id in that numa node
  auto numa_os_ids_iter = numa_id_to_os_ids_.find(numa_id);
  KMP_DEBUG_ASSERT(numa_os_ids_iter != numa_id_to_os_ids_.end());
  const std::vector<kmp_uint32>& os_ids = numa_os_ids_iter->second;
  KMP_DEBUG_ASSERT(!os_ids.empty());
  kmp_uint32 master_os_id = os_ids[0];

  // get one of the gtids mapped to that os_id
  auto os_id_gtids_iter = os_id_to_gtids_.find(master_os_id);
  KMP_DEBUG_ASSERT(os_id_gtids_iter != os_id_to_gtids_.end());
  auto & gtids = os_id_gtids_iter->second;
  // Ensure gtids are processed in order
  // this is important when multiple threads are mapped to the same os_id
  // like over-subscription case
  std::vector<kmp_int32> sorted_gtids(gtids.begin(), gtids.end());
  std::sort(sorted_gtids.begin(), sorted_gtids.end());

  kmp_team_t* my_team = __kmp_threads[gtid]->th.th_team;
  bool found = false;
  for (auto candidate_gtid : sorted_gtids) {
    if (__kmp_threads[candidate_gtid] &&
        __kmp_threads[candidate_gtid]->th.th_team == my_team) {
      master_tid = __kmp_tid_from_gtid(candidate_gtid);
      found = true;
      break;
    }
  }

  KMP_DEBUG_ASSERT(found);

  return master_tid;
}

bool ILANTopology::is_same_numa_node(kmp_int32 gtid1, kmp_int32 gtid2) {
  // kmp_uint32 gtid1 = __kmp_gtid_from_tid(tid1);
  // kmp_uint32 gtid2 = __kmp_gtid_from_tid(tid2);

  kmp_uint32 numa_id1 = get_numa_id(gtid1);
  kmp_uint32 numa_id2 = get_numa_id(gtid2);

  return (numa_id1 == numa_id2);
}