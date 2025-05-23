# ------------------------------------------------------------------------------
# Programmer(s): Cody J. Balos @ LLNL
# ------------------------------------------------------------------------------
# SUNDIALS Copyright Start
# Copyright (c) 2002-2025, Lawrence Livermore National Security
# and Southern Methodist University.
# All rights reserved.
#
# See the top-level LICENSE and NOTICE files for details.
#
# SPDX-License-Identifier: BSD-3-Clause
# SUNDIALS Copyright End
# ------------------------------------------------------------------------------
# vector level CMakeLists.txt for SUNDIALS
# ------------------------------------------------------------------------------

# required modules
add_subdirectory(serial)

if(BUILD_NVECTOR_MANYVECTOR OR BUILD_NVECTOR_MPIMANYVECTOR)
  add_subdirectory(manyvector)
endif()

if(BUILD_NVECTOR_PARALLEL)
  add_subdirectory(parallel)
endif()

if(BUILD_NVECTOR_MPIPLUSX)
  add_subdirectory(mpiplusx)
endif()

if(BUILD_NVECTOR_PARHYP)
  add_subdirectory(parhyp)
endif()

if(BUILD_NVECTOR_OPENMP)
  add_subdirectory(openmp)
endif()

if(BUILD_NVECTOR_OPENMPDEV)
  add_subdirectory(openmpdev)
endif()

if(BUILD_NVECTOR_PTHREADS)
  add_subdirectory(pthreads)
endif()

if(BUILD_NVECTOR_PETSC)
  add_subdirectory(petsc)
endif()

if(BUILD_NVECTOR_CUDA)
  add_subdirectory(cuda)
endif()

if(BUILD_NVECTOR_HIP)
  add_subdirectory(hip)
endif()

if(BUILD_NVECTOR_SYCL)
  add_subdirectory(sycl)
endif()

if(BUILD_NVECTOR_RAJA)
  add_subdirectory(raja)
endif()

if(BUILD_NVECTOR_TRILINOS)
  add_subdirectory(trilinos)
endif()

if(BUILD_NVECTOR_KOKKOS)
  install(CODE "MESSAGE(\"\nInstall NVECTOR_KOKKOS\n\")")
  message(STATUS "Added BUILD_NVECTOR_KOKKOS module")
  add_library(sundials_nveckokkos INTERFACE)
  target_link_libraries(sundials_nveckokkos INTERFACE sundials_core)
  if(ENABLE_HIP)
    target_link_libraries(sundials_nveckokkos INTERFACE Kokkos::kokkos
                                                        hip::device)
  else()
    target_link_libraries(sundials_nveckokkos INTERFACE Kokkos::kokkos)
  endif()
  target_include_directories(
    sundials_nveckokkos
    INTERFACE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
              $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
              $<BUILD_INTERFACE:${SUNDIALS_SOURCE_DIR}/src/sundials>
              $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  install(FILES ${PROJECT_SOURCE_DIR}/include/nvector/nvector_kokkos.hpp
          DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/nvector")
  set(_SUNDIALS_INSTALLED_COMPONENTS
      "nveckokkos;${_SUNDIALS_INSTALLED_COMPONENTS}"
      CACHE INTERNAL "" FORCE)
endif()
