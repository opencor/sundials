# ---------------------------------------------------------------
# Programmer(s): David J. Gardner, Slaven Peles, and
#                Cody Balos @ LLNL
# ---------------------------------------------------------------
# SUNDIALS Copyright Start
# Copyright (c) 2002-2025, Lawrence Livermore National Security
# and Southern Methodist University.
# All rights reserved.
#
# See the top-level LICENSE and NOTICE files for details.
#
# SPDX-License-Identifier: BSD-3-Clause
# SUNDIALS Copyright End
# ---------------------------------------------------------------
# examples/ida level CMakeLists.txt for SUNDIALS
# ---------------------------------------------------------------

# C examples
if(EXAMPLES_ENABLE_C)
  add_subdirectory(serial)
  if(ENABLE_OPENMP AND OPENMP_FOUND)
    add_subdirectory(C_openmp)
  endif()
  if(ENABLE_MPI AND MPI_C_FOUND)
    add_subdirectory(parallel)
  endif()
  if(ENABLE_PETSC AND PETSC_FOUND)
    add_subdirectory(petsc)
  endif()
endif()

# C++ examples
if(EXAMPLES_ENABLE_CXX)
  if(ENABLE_RAJA AND RAJA_FOUND)
    add_subdirectory(raja)
    if(ENABLE_MPI
       AND MPI_C_FOUND
       AND (SUNDIALS_RAJA_BACKENDS MATCHES "CUDA"))
      add_subdirectory(mpiraja)
    endif()
  endif()
  if(ENABLE_TRILINOS AND Trilinos_FOUND)
    add_subdirectory(trilinos)
  endif()
endif()

# Fortran examples
if(BUILD_FORTRAN_MODULE_INTERFACE AND EXAMPLES_ENABLE_F2003)
  add_subdirectory(F2003_serial)
  if(ENABLE_MPI)
    add_subdirectory(F2003_parallel)
  endif()
  if(ENABLE_OPENMP AND OPENMP_FOUND)
    add_subdirectory(F2003_openmp)
  endif()
endif()

# cuda examples
if(EXAMPLES_ENABLE_CUDA)

  if(ENABLE_CUDA AND CMAKE_CUDA_COMPILER)
    add_subdirectory(cuda)
    if(ENABLE_MPI AND MPI_C_FOUND)
      add_subdirectory(mpicuda)
    endif()
  endif()

endif()
