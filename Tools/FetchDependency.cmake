# Fetch the exact locked commit with Git's configured TLS trust. Avoid downloading
# entire histories and do not disable certificate verification for CMake binaries
# that were distributed without a CA bundle.
cmake_minimum_required(VERSION 3.24)
find_package(Git REQUIRED)
foreach(required DEPENDENCY_REPOSITORY DEPENDENCY_REVISION DEPENDENCY_DIRECTORY)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "Missing ${required}")
  endif()
endforeach()
file(MAKE_DIRECTORY "${DEPENDENCY_DIRECTORY}")
execute_process(COMMAND "${GIT_EXECUTABLE}" init "${DEPENDENCY_DIRECTORY}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${DEPENDENCY_DIRECTORY}" diff --quiet HEAD RESULT_VARIABLE modified OUTPUT_QUIET ERROR_QUIET)
if(modified EQUAL 1)
  message(FATAL_ERROR "Refusing to replace modified dependency sources")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${DEPENDENCY_DIRECTORY}" -c http.sslVerify=true
  fetch --depth 1 --no-tags "https://github.com/${DEPENDENCY_REPOSITORY}.git" "${DEPENDENCY_REVISION}"
  COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${DEPENDENCY_DIRECTORY}" checkout --detach FETCH_HEAD COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${DEPENDENCY_DIRECTORY}" rev-parse HEAD
  OUTPUT_VARIABLE actual OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT actual STREQUAL DEPENDENCY_REVISION)
  message(FATAL_ERROR "Dependency commit does not match lock")
endif()
