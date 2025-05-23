# ---------------------------------------------------------------
# Programmer(s): Shelby Lockhart @ LLNL
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
# CMakeLists.txt file for KINSOL C++ parhyp examples
# ---------------------------------------------------------------

# Example lists are tuples "name\;args\;nodes\;tasks\;type" where the type is
# develop for examples excluded from 'make test' in releases

# Examples using SUNDIALS linear solvers
set(KINSOL_examples "kin_bratu2D_hypre_pfmg\;--np 2 2\;1\;4\;develop"
                    "kin_heat2D_nonlin_hypre_pfmg\;--np 2 2\;1\;4\;develop")

set(KINSOL_headers "kin_bratu2D_hypre_pfmg.hpp"
                   "kin_heat2D_nonlin_hypre_pfmg.hpp")

# Specify libraries to link against
set(KINSOL_LIB sundials_kinsol)
set(NVECP_LIB sundials_nvecparallel)

# Set-up linker flags and link libraries
set(SUNDIALS_LIBS ${KINSOL_LIB} ${NVECP_LIB} ${EXE_EXTRA_LINK_LIBS})

# Add the build target for each example
foreach(example_tuple ${KINSOL_examples})

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
    sundials_add_executable(${example} ${example}.cpp)

    # folder to organize targets in an IDE
    set_target_properties(${example} PROPERTIES FOLDER "Examples")

    # libraries to link against
    target_link_libraries(${example} PRIVATE MPI::MPI_CXX ${SUNDIALS_LIBS}
                                             SUNDIALS::HYPRE)
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

  # install example source and out files
  if(EXAMPLES_INSTALL)
    install(FILES ${example}.cpp ${example_out}
            DESTINATION ${EXAMPLES_INSTALL_PATH}/kinsol/CXX_parhyp)
  endif()

endforeach(example_tuple ${KINSOL_examples})

# create Makefile and CMakeLists.txt for examples
if(EXAMPLES_INSTALL)

  # Install the header files
  foreach(extrafile ${KINSOL_headers})
    install(FILES ${extrafile}
            DESTINATION ${EXAMPLES_INSTALL_PATH}/kinsol/CXX_parhyp)
  endforeach()

  # Install the README file
  install(FILES README DESTINATION ${EXAMPLES_INSTALL_PATH}/kinsol/CXX_parhyp)

  # Prepare substitution variables for Makefile and/or CMakeLists templates
  set(SOLVER "KINSOL")
  set(SOLVER_LIB "sundials_kinsol")

  examples2string(KINSOL_examples EXAMPLES)

  # Regardless of the platform we're on, we will generate and install
  # CMakeLists.txt file for building the examples. This file  can then be used
  # as a template for the user's own programs.

  # generate CMakelists.txt in the binary directory
  configure_file(
    ${PROJECT_SOURCE_DIR}/examples/templates/cmakelists_parhyp_CXX_ex.in
    ${PROJECT_BINARY_DIR}/examples/kinsol/CXX_parhyp/CMakeLists.txt @ONLY)

  # install CMakelists.txt
  install(FILES ${PROJECT_BINARY_DIR}/examples/kinsol/CXX_parhyp/CMakeLists.txt
          DESTINATION ${EXAMPLES_INSTALL_PATH}/kinsol/CXX_parhyp)

  # On UNIX-type platforms, we also  generate and install a makefile for
  # building the examples. This makefile can then be used as a template for the
  # user's own programs.

  if(UNIX)
    # generate Makefile and place it in the binary dir
    configure_file(
      ${PROJECT_SOURCE_DIR}/examples/templates/makefile_parhyp_CXX_ex.in
      ${PROJECT_BINARY_DIR}/examples/kinsol/CXX_parhyp/Makefile_ex @ONLY)
    # install the configured Makefile_ex as Makefile
    install(
      FILES ${PROJECT_BINARY_DIR}/examples/kinsol/CXX_parhyp/Makefile_ex
      DESTINATION ${EXAMPLES_INSTALL_PATH}/kinsol/CXX_parhyp
      RENAME Makefile)
  endif()

  # add test_install target
  sundials_add_test_install(kinsol CXX_parhyp)

endif()
