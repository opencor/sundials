# ---------------------------------------------------------------
# Programmer(s): Steven Smith @ LLNL
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
# CMakeLists.txt file for hypre nvector examples
# ---------------------------------------------------------------

# Example lists are tuples "name\;args\;nodes\;tasks\;type" where the type is
# develop for examples excluded from 'make test' in releases

# Examples using SUNDIALS parallel hypre nvector
set(nvector_parhyp_examples
    "test_nvector_parhyp\;1000 0\;1\;1\;" # run sequentially
    "test_nvector_parhyp\;1000 0\;1\;4\;" # run parallel on 4 procs
)

# Dependencies for nvector examples
set(nvector_examples_dependencies test_nvector test_mpinvector)

# Add source directory to include directories
include_directories(. ..)
include_directories(${HYPRE_INC_PATH})
include_directories(${HYPRE_INC_DIR})
include_directories(${HYPRE_LIB_DIR})

# Specify libraries to link against
set(NVECS_LIB sundials_nvecparhyp)

# Set-up linker flags and link libraries
set(SUNDIALS_LIBS ${NVECS_LIB} ${EXE_EXTRA_LINK_LIBS})

list(APPEND SUNDIALS_LIBS)

if(MPI_C_COMPILER)
  # use MPI wrapper as the compiler
  set(CMAKE_C_COMPILER ${MPI_C_COMPILER})
else()
  # add MPI_INCLUDE_PATH to include directories
  include_directories(${MPI_INCLUDE_PATH})
endif()

# Add the build and install targets for each example
foreach(example_tuple ${nvector_parhyp_examples})

  # parse the example tuple
  list(GET example_tuple 0 example)
  list(GET example_tuple 1 example_args)
  list(GET example_tuple 2 number_of_nodes)
  list(GET example_tuple 3 number_of_tasks)
  list(GET example_tuple 4 example_type)

  # check if this example has already been added, only need to add example
  # source files once for testing with different inputs
  if(NOT TARGET ${example})
    # example source files
    sundials_add_executable(${example} ${example}.c)

    # link vector test utilities
    target_link_libraries(${example} PRIVATE test_nvector_obj
                                             test_nvectormpi_obj)

    # folder to organize targets in an IDE
    set_target_properties(${example} PROPERTIES FOLDER "Examples")

    # libraries to link against
    target_link_libraries(${example} PRIVATE ${SUNDIALS_LIBS})

    if(NOT MPI_C_COMPILER)
      target_link_libraries(${example} PRIVATE ${MPI_LIBRARIES})
    endif()
  endif()

  # check if example args are provided and set the test name
  if("${example_args}" STREQUAL "")
    if("${number_of_tasks}" STREQUAL "")
      set(test_name ${example})
    else()
      string(REGEX REPLACE " " "_" test_name ${example}_${number_of_tasks})
    endif()
  else()
    if("${number_of_tasks}" STREQUAL "")
      string(REGEX REPLACE " " "_" test_name ${example}_${example_args})
    else()
      string(REGEX REPLACE " " "_" test_name
                           ${example}_${number_of_tasks}_${example_args})
    endif()
  endif()

  # add example to regression tests
  sundials_add_test(
    ${test_name} ${example}
    TEST_ARGS ${example_args}
    MPI_NPROCS ${number_of_tasks}
    EXAMPLE_TYPE ${example_type}
    NODIFF)

  # install example source files
  if(EXAMPLES_INSTALL)
    install(FILES ${example}.c ../test_nvector.c ../test_nvector.h
                  ../test_mpinvector.c
            DESTINATION ${EXAMPLES_INSTALL_PATH}/nvector/parhyp)
  endif()

endforeach(example_tuple ${nvector_parhyp_examples})

if(EXAMPLES_INSTALL)

  # Install the README file
  install(FILES DESTINATION ${EXAMPLES_INSTALL_PATH}/nvector/parhyp)

  # Prepare substitution variables for Makefile and/or CMakeLists templates
  set(SOLVER_LIB "sundials_nvecparhyp")

  examples2string(nvector_parhyp_examples EXAMPLES)
  examples2string(nvector_examples_dependencies EXAMPLES_DEPENDENCIES)

  # Regardless of the platform we're on, we will generate and install
  # CMakeLists.txt file for building the examples. This file  can then be used
  # as a template for the user's own programs.

  # generate CMakelists.txt in the binary directory
  configure_file(
    ${PROJECT_SOURCE_DIR}/examples/templates/cmakelists_parhyp_C_ex.in
    ${PROJECT_BINARY_DIR}/test/unit_tests/nvector/parhyp/CMakeLists.txt @ONLY)

  # install CMakelists.txt
  install(
    FILES ${PROJECT_BINARY_DIR}/test/unit_tests/nvector/parhyp/CMakeLists.txt
    DESTINATION ${EXAMPLES_INSTALL_PATH}/nvector/parhyp)

  # On UNIX-type platforms, we also  generate and install a makefile for
  # building the examples. This makefile can then be used as a template for the
  # user's own programs.

  if(UNIX)
    # generate Makefile and place it in the binary dir
    configure_file(
      ${PROJECT_SOURCE_DIR}/examples/templates/makefile_parhyp_C_ex.in
      ${PROJECT_BINARY_DIR}/test/unit_tests/nvector/parhyp/Makefile_ex @ONLY)
    # install the configured Makefile_ex as Makefile
    install(
      FILES ${PROJECT_BINARY_DIR}/test/unit_tests/nvector/parhyp/Makefile_ex
      DESTINATION ${EXAMPLES_INSTALL_PATH}/nvector/parhyp
      RENAME Makefile)
  endif()

endif()
