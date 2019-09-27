/*
 * Copyright (c) 2018-2019, NVIDIA CORPORATION.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "adjgraph/runner.h"
#include "common/cumlHandle.hpp"
#include "common/device_buffer.hpp"
#include "common/nvtx.hpp"
#include "cuda_utils.h"
#include "label/classlabels.h"
#include "sparse/csr.h"
#include "vertexdeg/runner.h"

namespace Dbscan {

using namespace MLCommon;

static const int TPB = 256;

/**
 * Adjust labels from weak_cc primitive to match sklearn:
 * 1. Turn any labels matching MAX_LABEL into -1
 * 2. Subtract 1 from all other labels.
 */
template <typename Index_ = long>
__global__ void relabelForSkl(Index_* labels, Index_ N, Index_ MAX_LABEL) {
  Index_ tid = threadIdx.x + blockDim.x * blockIdx.x;
  if (labels[tid] == MAX_LABEL)
    labels[tid] = -1;
  else if (tid < N)
    --labels[tid];
}

/**
 * Turn the non-monotonic labels from weak_cc primitive into
 * an array of labels drawn from a monotonically increasing set.
 */
template <typename Index_ = long>
void final_relabel(Index_* db_cluster, Index_ N, cudaStream_t stream) {
  Index_ MAX_LABEL = std::numeric_limits<Index_>::max();
  MLCommon::Label::make_monotonic(
    db_cluster, db_cluster, N, stream,
    [MAX_LABEL] __device__(Index_ val) { return val == MAX_LABEL; });
}

/* @param N number of points
 * @param D dimensionality of the points
 * @param eps epsilon neighborhood criterion
 * @param minPts core points criterion
 * @param labels the output labels (should be of size N)
 * @param ....
 * @param temp temporary global memory buffer used to store intermediate computations
 *             If this is a null pointer, then this function will return the workspace size needed.
 *             It is the responsibility of the user to cudaMalloc and cudaFree this buffer!
 * @param stream the cudaStream where to launch the kernels
 * @return in case the temp buffer is null, this returns the size needed.
 */
template <typename Type, typename Type_f, typename Index_ = long>
size_t run(const ML::cumlHandle_impl& handle, Type_f* x, Index_ N, Index_ D,
           Type_f eps, Type minPts, Index_* labels, int algoVd, int algoAdj,
           int algoCcl, void* workspace, Index_ nBatches, cudaStream_t stream,
           bool verbose = false) {
  const size_t align = 256;
  Index_ batchSize = ceildiv(N, nBatches);
  size_t adjSize = alignTo<size_t>(sizeof(bool) * N * batchSize, align);
  size_t corePtsSize = alignTo<size_t>(sizeof(bool) * batchSize, align);
  size_t xaSize = alignTo<size_t>(sizeof(bool) * N, align);
  size_t mSize = alignTo<size_t>(sizeof(bool), align);
  size_t vdSize = alignTo<size_t>(sizeof(Index_) * (batchSize + 1), align);
  size_t exScanSize = alignTo<size_t>(sizeof(Index_) * batchSize, align);

  if (workspace == NULL) {
    auto size =
      adjSize + corePtsSize + 2 * xaSize + mSize + vdSize + exScanSize;
    return size;
  }

  // partition the temporary workspace needed for different stages of dbscan
  Index_ adjlen = 0;
  Index_ curradjlen = 0;
  char* temp = (char*)workspace;
  bool* adj = (bool*)temp;
  temp += adjSize;
  bool* core_pts = (bool*)temp;
  temp += corePtsSize;
  bool* xa = (bool*)temp;
  temp += xaSize;
  bool* fa = (bool*)temp;
  temp += xaSize;
  bool* m = (bool*)temp;
  temp += mSize;
  Index_* vd = (Index_*)temp;
  temp += vdSize;
  Index_* ex_scan = (Index_*)temp;
  temp += exScanSize;

  // Running VertexDeg
  MLCommon::Sparse::WeakCCState<Index_> state(xa, fa, m);

  for (int i = 0; i < nBatches; i++) {
    ML::PUSH_RANGE("Trace::Dbscan::VertexDeg");
    MLCommon::device_buffer<Index_> adj_graph(handle.getDeviceAllocator(),
                                              stream);

    Index_ startVertexId = i * batchSize;
    Index_ nPoints = min(N - startVertexId, batchSize);
    if (nPoints <= 0) continue;

    if (verbose)
      std::cout << "- Iteration " << i + 1 << "  " << nBatches
                << ". Batch size is " << nPoints << " samples." << std::endl;

    if (verbose) std::cout << "Computing vertex degrees" << std::endl;
    VertexDeg::run<Type_f, Index_>(handle, adj, vd, x, eps, N, D, algoVd,
                                   startVertexId, nPoints, stream);
    MLCommon::updateHost(&curradjlen, vd + nPoints, 1, stream);
    CUDA_CHECK(cudaStreamSynchronize(stream));
    ML::POP_RANGE();

    if (verbose)
      std::cout << "Computing adjacency graph of size " << curradjlen
                << std::endl;
    // Running AdjGraph
    ML::PUSH_RANGE("Trace::Dbscan::AdjGraph");
    if (curradjlen > adjlen || adj_graph.data() == NULL) {
      adjlen = curradjlen;
      adj_graph.resize(adjlen, stream);
    }

    AdjGraph::run<Type, Index_>(handle, adj, vd, adj_graph.data(), adjlen,
                                ex_scan, N, minPts, core_pts, algoAdj, nPoints,
                                stream);

    ML::POP_RANGE();

    ML::PUSH_RANGE("Trace::Dbscan::WeakCC");

    if (verbose) std::cout << "Computing connected components" << std::endl;

    MLCommon::Sparse::weak_cc_batched<Index_, TPB>(
      labels, ex_scan, adj_graph.data(), adjlen, N, startVertexId, nPoints,
      &state, stream,
      [core_pts] __device__(Index_ tid) { return core_pts[tid]; });
    ML::POP_RANGE();
  }

  ML::PUSH_RANGE("Trace::Dbscan::FinalRelabel");
  if (algoCcl == 2) final_relabel(labels, N, stream);
  Index_ MAX_LABEL = std::numeric_limits<Index_>::max();
  size_t nblks = ceildiv<size_t>(N, TPB);
  relabelForSkl<Index_><<<nblks, TPB, 0, stream>>>(labels, N, MAX_LABEL);
  CUDA_CHECK(cudaPeekAtLastError());
  ML::POP_RANGE();
  return (size_t)0;
}
}  // namespace Dbscan
