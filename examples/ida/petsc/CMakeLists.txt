# ---------------------------------------------------------------
# Programmer(s): Slaven Peles, Eddy Banks and Cody J. Balos @ LLNL
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
# CMakeLists.txt file for IDA PETSc examples
# ---------------------------------------------------------------

# Example lists are tuples "name\;args;\nodes\;tasks\;type" where the type is
# develop for examples excluded from 'make test' in releases

# Examples using SUNDIALS linear solvers
set(IDA_examples
    "idaHeat2D_petsc_spgmr\;\;1\;4\;develop"
    "idaHeat2D_petsc_snes\;\;1\;4\;develop"
    "idaHeat2D_petsc_snes\;-pre\;1\;4\;develop")

# Previous tests with PETSc v3.17.1 installed with Spack v0.18.1 had identical
# results with 32 and 64-bit indexing. Now with PETSc v3.21.4 installed with
# Spack v0.23.0.dev0 (73fc86cbc3ae6dea5f69e3a0516e1a7691864fa3) the results are
# slightly different.
if(SUNDIALS_INDEX_SIZE STREQUAL "32")
  list(APPEND IDA_examples "idaHeat2D_petsc_snes\;-jac\;1\;4\;develop"
       "idaHeat2D_petsc_snes\;-jac -pre\;1\;4\;develop")
endif()

if(MPI_C_COMPILER)
  # use MPI wrapper as the compiler
  set(CMAKE_C_COMPILER ${MPI_C_COMPILER})
else()
  # add MPI_INCLUDE_PATH to include directories
  include_directories(${MPI_INCLUDE_PATH})
endif()

# Specify libraries to link against
set(IDA_LIB sundials_ida)
set(NVECP_LIB sundials_nvecpetsc)
set(SUNNLS_LIB sundials_sunnonlinsolpetscsnes)

# Set-up linker flags and link libraries
set(SUNDIALS_LIBS ${IDA_LIB} ${NVECP_LIB} ${SUNNLS_LIB} ${EXE_EXTRA_LINK_LIBS})

# Add the build and install targets for each example
foreach(example_tuple ${IDA_examples})

  list(GET example_tuple 0 example)
  list(GET example_tuple 1 example_args)
  list(GET example_tuple 2 number_of_nodes)
  list(GET example_tuple 3 number_of_tasks)
  list(GET example_tuple 4 example_type)

  # check if this example has already been added, only need to add example
  # source files once for testing with different inputs
  if(NOT TARGET ${example})
    sundials_add_executable(${example} ${example}.c)

    # group examples in IDE
    set_target_properties(${example} PROPERTIES FOLDER "Examples")

    # libraries to link against
    target_link_libraries(${example} ${SUNDIALS_LIBS})

    if(NOT MPI_C_COMPILER)
      target_link_libraries(${example} ${MPI_LIBRARY} ${MPI_EXTRA_LIBRARIES})
    endif()
  endif()

  # check if example args are provided and set the test name
  if("${example_args}" STREQUAL "")
    set(test_name ${example})
  else()
    string(REGEX REPLACE " " "_" test_name ${example}_${example_args})
  endif()

  # add example to regression tests
  sundials_add_test(
    ${test_name} ${example}
    TEST_ARGS ${example_args}
    MPI_NPROCS ${number_of_tasks}
    ANSWER_DIR ${CMAKE_CURRENT_SOURCE_DIR}
    ANSWER_FILE ${test_name}.out
    EXAMPLE_TYPE ${example_type})

  # find all .out files for this example
  file(GLOB example_out ${example}*.out)

  # install example
  if(EXAMPLES_INSTALL)
    install(FILES ${example}.c ${example_out}
            DESTINATION ${EXAMPLES_INSTALL_PATH}/ida/petsc)
  endif()

endforeach(example_tuple ${IDA_examples})

if(EXAMPLES_INSTALL)

  # Install the README file
  install(FILES README DESTINATION ${EXAMPLES_INSTALL_PATH}/ida/petsc)

  # Prepare substitution variables for Makefile and/or CMakeLists templates
  set(SOLVER "IDA")
  set(SOLVER_LIB "sundials_ida")

  examples2string(IDA_examples EXAMPLES)

  # Regardless of the platform we're on, we will generate and install
  # CMakeLists.txt file for building the examples. This file  can then be used
  # as a template for the user's own programs.

  # generate CMakelists.txt in the binary directory
  configure_file(
    ${PROJECT_SOURCE_DIR}/examples/templates/cmakelists_petsc_C_ex.in
    ${PROJECT_BINARY_DIR}/examples/ida/petsc/CMakeLists.txt @ONLY)

  # install CMakelists.txt
  install(FILES ${PROJECT_BINARY_DIR}/examples/ida/petsc/CMakeLists.txt
          DESTINATION ${EXAMPLES_INSTALL_PATH}/ida/petsc)

  # On UNIX-type platforms, we also  generate and install a makefile for
  # building the examples. This makefile can then be used as a template for the
  # user's own programs.

  if(UNIX)
    # generate Makefile and place it in the binary dir
    configure_file(
      ${PROJECT_SOURCE_DIR}/examples/templates/makefile_petsc_C_ex.in
      ${PROJECT_BINARY_DIR}/examples/ida/petsc/Makefile_ex @ONLY)
    # install the configured Makefile_ex as Makefile
    install(
      FILES ${PROJECT_BINARY_DIR}/examples/ida/petsc/Makefile_ex
      DESTINATION ${EXAMPLES_INSTALL_PATH}/ida/petsc
      RENAME Makefile)
  endif()

  # add test_install target
  sundials_add_test_install(ida petsc)

endif()
