// include for ThrustGPUNode method implementations
#include "Kokkos_ThrustGPUNode.cuh"

// includes for all ops
#include "Kokkos_DefaultSparseSolveKernelOps.hpp"

#define INSTANTIATE_SPARSESOLVE_ORDINAL_SCALAR(ORDINAL, SCALAR) \
  template void Kokkos::ThrustGPUNode::parallel_for< Kokkos::DefaultSparseSolveOp1            <SCALAR, ORDINAL, SCALAR, SCALAR> >(int, int, Kokkos::DefaultSparseSolveOp1            <SCALAR, ORDINAL, SCALAR, SCALAR> ); \
  template void Kokkos::ThrustGPUNode::parallel_for< Kokkos::DefaultSparseSolveOp2            <SCALAR, ORDINAL, SCALAR, SCALAR> >(int, int, Kokkos::DefaultSparseSolveOp2            <SCALAR, ORDINAL, SCALAR, SCALAR> ); \
  template void Kokkos::ThrustGPUNode::parallel_for< Kokkos::DefaultSparseTransposeSolveOp1   <SCALAR, ORDINAL, SCALAR, SCALAR> >(int, int, Kokkos::DefaultSparseTransposeSolveOp1   <SCALAR, ORDINAL, SCALAR, SCALAR> ); \
  template void Kokkos::ThrustGPUNode::parallel_for< Kokkos::DefaultSparseTransposeSolveOp2   <SCALAR, ORDINAL, SCALAR, SCALAR> >(int, int, Kokkos::DefaultSparseTransposeSolveOp2   <SCALAR, ORDINAL, SCALAR, SCALAR> );

#ifdef HAVE_KOKKOS_CUDA_FLOAT
INSTANTIATE_SPARSESOLVE_ORDINAL_SCALAR(int,float)
typedef short int ShortInt; INSTANTIATE_SPARSESOLVE_ORDINAL_SCALAR(ShortInt,float)
#endif

#ifdef HAVE_KOKKOS_CUDA_DOUBLE
INSTANTIATE_SPARSESOLVE_ORDINAL_SCALAR(int,double)
typedef short int ShortInt; INSTANTIATE_SPARSESOLVE_ORDINAL_SCALAR(ShortInt,double)
#endif

INSTANTIATE_SPARSESOLVE_ORDINAL_SCALAR(int,int)
typedef short int ShortInt; INSTANTIATE_SPARSESOLVE_ORDINAL_SCALAR(ShortInt,int)
