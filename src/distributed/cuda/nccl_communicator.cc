/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

/*!
 * \file src/distributed/nccl_communicator.cc
 * \brief NCCL Communicator.
 */

#include "raf/nccl_communicator.h"

namespace raf {
namespace distributed {
namespace communicator {

NCCLCommunicatorObj::~NCCLCommunicatorObj() {
  NCCL_CALL(ncclCommDestroy(nccl_comm));
}

NCCLCommunicator NCCLCommunicator::make(Value rank_list) {
  auto mpi = Communicator::Get("mpi");  // Must init MPI first
  auto obj = make_object<NCCLCommunicatorObj>();

  ncclUniqueId nccl_id;
  NCCL_CALL(ncclGetUniqueId(&nccl_id));

  if (!rank_list.defined()) {
    // Create Global Communicator
    obj->local_size = mpi->local_size;
    obj->local_rank = mpi->local_rank;
    obj->size = mpi->size;
    obj->rank = mpi->rank;
    obj->world_size = mpi->world_size;
    obj->world_rank = mpi->world_rank;
    obj->root_rank = mpi->root_rank;
    obj->group_id = -1;
    obj->group_size = 0;
    obj->host_ids = mpi->host_ids;
    obj->parent_comm = mpi;
    cudaSetDevice(obj->local_rank);
    MPI_CALL(MPI_Bcast(reinterpret_cast<void*>(&nccl_id), sizeof(nccl_id), MPI_BYTE, obj->root_rank,
                       MPI_COMM_WORLD));
    NCCL_CALL(ncclCommInitRank(&obj->nccl_comm, obj->size, nccl_id, obj->rank));
  } else {
    // Create Sub-communicator
    InitSubCommunicator(obj.get(), rank_list, mpi);
    obj->parent_comm = mpi;

    std::vector<ncclUniqueId> nccl_ids(obj->group_size);
    std::vector<int> counts(obj->world_size, 0);
    std::vector<int> displacements(obj->world_size);

    int offset = 0;

    for (auto group : Downcast<TupleValue>(rank_list)->fields) {
      auto root_rank = Downcast<TupleValue>(group)->fields[0];
      auto root_rank_ = Downcast<IntValue>(root_rank)->value;
      counts[root_rank_] = sizeof(nccl_id);
    }

    for (int i = 0; i < obj->world_size; ++i) {
      displacements[i] = offset;
      if (counts[i] > 0) offset += sizeof(nccl_id);
    }

    MPI_CALL(MPI_Allgatherv(reinterpret_cast<void*>(&nccl_id), counts[obj->world_rank], MPI_BYTE,
                            reinterpret_cast<void*>(&nccl_ids[0]),
                            reinterpret_cast<int*>(&counts[0]),
                            reinterpret_cast<int*>(&displacements[0]), MPI_BYTE, MPI_COMM_WORLD));

    auto& root_nccl_id = (obj->group_id == -1) ? nccl_id : nccl_ids[obj->group_id];
    NCCL_CALL(ncclCommInitRank(&obj->nccl_comm, obj->size, root_nccl_id, obj->rank));
  }

  return NCCLCommunicator(obj);
}

RAF_REGISTER_GLOBAL("raf.distributed.communicator._make.nccl")
    .set_body_typed(NCCLCommunicator::make);

RAF_REGISTER_OBJECT_REFLECT(NCCLCommunicatorObj);

}  // namespace communicator
}  // namespace distributed
}  // namespace raf
