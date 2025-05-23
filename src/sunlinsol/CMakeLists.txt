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
# linear solver level CMakeLists.txt for SUNDIALS
# ------------------------------------------------------------------------------

# required native linear solvers
add_subdirectory(band)
add_subdirectory(dense)
add_subdirectory(pcg)
add_subdirectory(spbcgs)
add_subdirectory(spfgmr)
add_subdirectory(spgmr)
add_subdirectory(sptfqmr)

# optional TPL linear solvers
if(BUILD_SUNLINSOL_CUSOLVERSP)
  add_subdirectory(cusolversp)
endif()

if(BUILD_SUNLINSOL_GINKGO)
  install(CODE "MESSAGE(\"\nInstall SUNLINSOL_GINKGO\n\")")
  message(STATUS "Added BUILD_SUNLINSOL_GINKGO module")
  add_library(sundials_sunlinsolginkgo INTERFACE)
  target_link_libraries(sundials_sunlinsolginkgo INTERFACE sundials_core)
  target_include_directories(
    sundials_sunlinsolginkgo
    INTERFACE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
              $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
              $<BUILD_INTERFACE:${SUNDIALS_SOURCE_DIR}/src/sundials>
              $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  install(FILES ${PROJECT_SOURCE_DIR}/include/sunlinsol/sunlinsol_ginkgo.hpp
          DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/sunlinsol")
  set(_SUNDIALS_INSTALLED_COMPONENTS
      "sunlinsolginkgo;${_SUNDIALS_INSTALLED_COMPONENTS}"
      CACHE INTERNAL "" FORCE)
endif()

if(BUILD_SUNLINSOL_KLU)
  add_subdirectory(klu)
endif()

if(BUILD_SUNLINSOL_KOKKOSDENSE)
  install(CODE "MESSAGE(\"\nInstall SUNLINSOL_KOKKOSDENSE\n\")")
  message(STATUS "Added BUILD_SUNLINSOL_KOKKOSDENSE module")
  add_library(sundials_sunlinsolkokkosdense INTERFACE)
  target_link_libraries(
    sundials_sunlinsolkokkosdense INTERFACE sundials_core Kokkos::kokkos
                                            Kokkos::kokkoskernels)
  target_include_directories(
    sundials_sunlinsolkokkosdense
    INTERFACE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
              $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
              $<BUILD_INTERFACE:${SUNDIALS_SOURCE_DIR}/src/sundials>
              $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  install(
    FILES ${PROJECT_SOURCE_DIR}/include/sunlinsol/sunlinsol_kokkosdense.hpp
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/sunlinsol")
  set(_SUNDIALS_INSTALLED_COMPONENTS
      "sunlinsolkokkosdense;${_SUNDIALS_INSTALLED_COMPONENTS}"
      CACHE INTERNAL "" FORCE)
endif()

if(BUILD_SUNLINSOL_LAPACKBAND)
  add_subdirectory(lapackband)
endif()

if(BUILD_SUNLINSOL_LAPACKDENSE)
  add_subdirectory(lapackdense)
endif()

if(BUILD_SUNLINSOL_MAGMADENSE)
  add_subdirectory(magmadense)
endif()

if(BUILD_SUNLINSOL_ONEMKLDENSE)
  add_subdirectory(onemkldense)
endif()

if(BUILD_SUNLINSOL_SUPERLUDIST)
  add_subdirectory(superludist)
endif()

if(BUILD_SUNLINSOL_SUPERLUMT)
  add_subdirectory(superlumt)
endif()
