# ---------------------------------------------------------------
# Programmer(s): Cody J. Balos @ LLNL
# ---------------------------------------------------------------
# SUNDIALS Copyright Start
# Copyright (c) 2002-2022, Lawrence Livermore National Security
# and Southern Methodist University.
# All rights reserved.
#
# See the top-level LICENSE and NOTICE files for details.
#
# SPDX-License-Identifier: BSD-3-Clause
# SUNDIALS Copyright End
# ---------------------------------------------------------------
# CMakeLists.txt file for the F2003 SUNDIALS object library
# ---------------------------------------------------------------

set(sundials_SOURCES
  fsundials_futils_mod.f90
  fsundials_futils_mod.c
  fsundials_logger_mod.f90
  fsundials_logger_mod.c
  fsundials_linearsolver_mod.f90
  fsundials_linearsolver_mod.c
  fsundials_matrix_mod.f90
  fsundials_matrix_mod.c
  fsundials_nonlinearsolver_mod.f90
  fsundials_nonlinearsolver_mod.c
  fsundials_nvector_mod.f90
  fsundials_nvector_mod.c
  fsundials_types_mod.f90
  fsundials_types_mod.c
  fsundials_context_mod.f90
  fsundials_context_mod.c)

if(SUNDIALS_BUILD_WITH_PROFILING)
  list(APPEND sundials_SOURCES
    fsundials_profiler_mod.f90
    fsundials_profiler_mod.c)
endif()

sundials_add_f2003_library(sundials_fgeneric_mod
  SOURCES
    ${sundials_SOURCES}
  OBJECT_LIB_ONLY
)
