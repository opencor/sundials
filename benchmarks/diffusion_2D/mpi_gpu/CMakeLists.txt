# ------------------------------------------------------------------------------
# Programmer(s): David J. Gardner @ LLNL
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

# list of tests
set(tests)

if(BUILD_ARKODE)
  if(BUILD_NVECTOR_CUDA)
    list(APPEND tests "arkode\;BENCHMARK_ODE\;USE_CUDA")
  endif()
  if(BUILD_NVECTOR_HIP)
    list(APPEND tests "arkode\;BENCHMARK_ODE\;USE_HIP")
  endif()
endif()

if(BUILD_CVODE)
  if(BUILD_NVECTOR_CUDA)
    list(APPEND tests "cvode\;BENCHMARK_ODE\;USE_CUDA")
  endif()
  if(BUILD_NVECTOR_HIP)
    list(APPEND tests "cvode\;BENCHMARK_ODE\;USE_HIP")
  endif()
endif()

if(BUILD_IDA)
  if(BUILD_NVECTOR_CUDA)
    list(APPEND tests "ida\;BENCHMARK_DAE\;USE_CUDA")
  endif()
  if(BUILD_NVECTOR_HIP)
    list(APPEND tests "ida\;BENCHMARK_DAE\;USE_HIP")
  endif()
endif()

# create executables
foreach(test_tuple ${tests})

  # parse the test tuple
  list(GET test_tuple 0 package)
  list(GET test_tuple 1 problem_type)
  list(GET test_tuple 2 backend)

  set(sources ${benchmark_prefix}/main_${package}.cpp ${shared_sources}
              buffers.cpp diffusion.cpp solution.cpp utils.cpp)

  if("${backend}" STREQUAL "USE_CUDA")

    foreach(file ${sources})
      set_source_files_properties(${file} PROPERTIES LANGUAGE CUDA)
    endforeach()

    set(target ${package}_diffusion_2D_mpicuda)

  else()

    set(target ${package}_diffusion_2D_mpihip)

  endif()

  sundials_add_executable(${target} ${sources})

  # if("${backend}" STREQUAL "USE_CUDA") sundials_add_benchmark(${target}
  # ${target} diffusion_2D ENABLE_GPU NUM_CORES ${SUNDIALS_BENCHMARK_NUM_GPUS} )
  # endif()

  if("${backend}" STREQUAL "USE_HIP")
    sundials_add_benchmark(${target} ${target} diffusion_2D ENABLE_GPU
                           NUM_CORES ${SUNDIALS_BENCHMARK_NUM_GPUS})

  endif()

  set_target_properties(${target} PROPERTIES FOLDER "Benchmarks")

  target_compile_definitions(${target} PRIVATE ${problem_type})

  if("${backend}" STREQUAL "USE_CUDA")

    target_compile_definitions(${target} PRIVATE USE_CUDA)

    target_include_directories(${target} PRIVATE ${benchmark_prefix})

    target_link_libraries(
      ${target} PRIVATE sundials_${package} sundials_nvecmpiplusx
                        sundials_nveccuda)

  else()

    target_compile_definitions(${target} PRIVATE USE_HIP)

    target_include_directories(${target} PRIVATE ${benchmark_prefix})

    target_link_libraries(
      ${target} PRIVATE sundials_${package} sundials_nvecmpiplusx
                        sundials_nvechip hip::device)

  endif()

  # Workaround bug in CMake < 3.17.3 when using MPI::MPI_CXX
  target_include_directories(${target} PRIVATE ${MPI_CXX_INCLUDE_DIRS})
  target_link_libraries(${target} PRIVATE ${MPI_CXX_LIBRARIES})

  install(TARGETS ${target}
          DESTINATION "${BENCHMARKS_INSTALL_PATH}/diffusion_2D")

endforeach()
