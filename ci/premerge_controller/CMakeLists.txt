# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
cmake_minimum_required(VERSION 3.25)
project(HyperCinePremergeController LANGUAGES NONE)
include(CTest)
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
get_filename_component(REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
add_test(NAME premerge-controller-unit
  COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s ci/premerge_controller/tests -v)
add_test(NAME premerge-controller-mutations
  COMMAND "${Python3_EXECUTABLE}" -m ci.premerge_controller.mutation_suite
          --output "${CMAKE_BINARY_DIR}/mutation-results.json")
set_tests_properties(premerge-controller-unit premerge-controller-mutations PROPERTIES
  WORKING_DIRECTORY "${REPOSITORY_ROOT}" TIMEOUT 300
  ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

# Public cost-policy unit/failure/mutation suites, independent of real GitHub.
add_test(NAME compute-policy-unit
  COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s ci/compute_policy/tests -v)
add_test(NAME compute-policy-mutations
  COMMAND "${Python3_EXECUTABLE}" -m ci.compute_policy.mutation_suite
          --output "${CMAKE_BINARY_DIR}/compute-mutation-results.json")
set_tests_properties(compute-policy-unit compute-policy-mutations PROPERTIES
  WORKING_DIRECTORY "${REPOSITORY_ROOT}" TIMEOUT 300
  ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

# Preserve the original routing/capability negative controls alongside Mac scope.
add_test(NAME workflow-routing-contract
  COMMAND "${Python3_EXECUTABLE}" -m unittest ci.test_workflow_event_matrix ci.test_runner_capability -v)
set_tests_properties(workflow-routing-contract PROPERTIES
  WORKING_DIRECTORY "${REPOSITORY_ROOT}" TIMEOUT 300
  ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
