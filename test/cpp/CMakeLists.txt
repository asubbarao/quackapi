cmake_minimum_required(VERSION 3.16)
project(quackapi_http_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

get_filename_component(QUACKAPI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.."
                       ABSOLUTE)
set(QUACKAPI_BUILD_ROOT
    "${QUACKAPI_ROOT}/build/release"
    CACHE PATH "Existing DuckDB release build")
set(QUACKAPI_EXTENSION
    "${QUACKAPI_BUILD_ROOT}/extension/quackapi/quackapi.duckdb_extension"
    CACHE FILEPATH "Exact QuackAPI loadable artifact")
set(JSON_EXTENSION
    "${QUACKAPI_BUILD_ROOT}/extension/json/json.duckdb_extension"
    CACHE FILEPATH "Exact JSON loadable artifact QuackAPI requires")
set(POST_PROVIDER_EXTENSION
    ""
    CACHE FILEPATH "Exact HTTP provider loadable artifact")
set(POST_PROVIDER_NAME
    ""
    CACHE STRING "Expected HTTP provider extension name")
set(QUACKAPI_VERSION
    ""
    CACHE STRING "Expected QuackAPI extension_version")
set(POST_PROVIDER_VERSION
    ""
    CACHE STRING "Expected HTTP provider extension_version")

if(NOT EXISTS "${QUACKAPI_EXTENSION}"
   OR NOT EXISTS "${JSON_EXTENSION}"
   OR NOT EXISTS "${POST_PROVIDER_EXTENSION}"
   OR QUACKAPI_VERSION STREQUAL ""
   OR POST_PROVIDER_NAME STREQUAL ""
   OR POST_PROVIDER_VERSION STREQUAL "")
  message(
    FATAL_ERROR
      "Pass the two existing artifact paths and their expected extension versions"
  )
endif()

find_library(
  DUCKDB_LIBRARY duckdb
  PATHS "${QUACKAPI_BUILD_ROOT}/src"
  NO_DEFAULT_PATH REQUIRED)
add_executable(quackapi_post_http_test quackapi_post_http_test.cpp)
find_package(OpenSSL REQUIRED)
target_include_directories(
  quackapi_post_http_test SYSTEM PRIVATE "${QUACKAPI_ROOT}/duckdb/src/include"
                                         "${QUACKAPI_ROOT}/third_party/httplib")
target_compile_definitions(quackapi_post_http_test
                           PRIVATE CPPHTTPLIB_LISTEN_BACKLOG=SOMAXCONN)
target_link_libraries(quackapi_post_http_test PRIVATE "${DUCKDB_LIBRARY}")
add_executable(quackapi_post_tls_test quackapi_post_tls_test.cpp)
target_include_directories(
  quackapi_post_tls_test SYSTEM PRIVATE "${QUACKAPI_ROOT}/duckdb/src/include"
                                        "${QUACKAPI_ROOT}/third_party/httplib")
target_compile_definitions(quackapi_post_tls_test
                           PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
target_link_libraries(quackapi_post_tls_test
                      PRIVATE "${DUCKDB_LIBRARY}" OpenSSL::SSL OpenSSL::Crypto)
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(quackapi_post_http_test PRIVATE Threads::Threads)
  target_link_libraries(quackapi_post_tls_test PRIVATE Threads::Threads)
endif()

add_test(
  NAME post_http_contract
  COMMAND
    quackapi_post_http_test "${QUACKAPI_EXTENSION}" "${QUACKAPI_VERSION}"
    "${POST_PROVIDER_EXTENSION}" "${POST_PROVIDER_NAME}"
    "${POST_PROVIDER_VERSION}")
set_tests_properties(
  post_http_contract
  PROPERTIES TIMEOUT 30 ENVIRONMENT
             "QUACKAPI_TEST_JSON_EXTENSION=${JSON_EXTENSION}")
add_test(
  NAME post_tls_contract
  COMMAND
    quackapi_post_tls_test "${QUACKAPI_EXTENSION}" "${QUACKAPI_VERSION}"
    "${POST_PROVIDER_EXTENSION}" "${POST_PROVIDER_NAME}"
    "${POST_PROVIDER_VERSION}"
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/loopback.crt"
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/loopback.key")
set_tests_properties(
  post_tls_contract PROPERTIES TIMEOUT 20 ENVIRONMENT
                               "QUACKAPI_TEST_JSON_EXTENSION=${JSON_EXTENSION}")

option(ENABLE_PROVIDER_REPLACEMENT "Build the two-provider lifetime contract"
       OFF)
if(ENABLE_PROVIDER_REPLACEMENT)
  if(NOT EXISTS "${CURL_HTTPFS_EXTENSION}"
     OR NOT EXISTS "${HTTPFS_EXTENSION}"
     OR CURL_HTTPFS_VERSION STREQUAL ""
     OR HTTPFS_VERSION STREQUAL "")
    message(
      FATAL_ERROR
        "Provider replacement requires exact CURL_HTTPFS_EXTENSION/VERSION and HTTPFS_EXTENSION/VERSION"
    )
  endif()
  add_executable(quackapi_provider_replacement_test
                 quackapi_provider_replacement_test.cpp)
  target_include_directories(
    quackapi_provider_replacement_test SYSTEM
    PRIVATE "${QUACKAPI_ROOT}/duckdb/src/include"
            "${QUACKAPI_ROOT}/third_party/httplib")
  target_link_libraries(quackapi_provider_replacement_test
                        PRIVATE "${DUCKDB_LIBRARY}")
  if(NOT WIN32)
    target_link_libraries(quackapi_provider_replacement_test
                          PRIVATE Threads::Threads)
  endif()
  add_test(
    NAME provider_replacement_contract
    COMMAND
      quackapi_provider_replacement_test "${QUACKAPI_EXTENSION}"
      "${QUACKAPI_VERSION}" "${CURL_HTTPFS_EXTENSION}" "${CURL_HTTPFS_VERSION}"
      "${HTTPFS_EXTENSION}" "${HTTPFS_VERSION}")
  set_tests_properties(provider_replacement_contract PROPERTIES TIMEOUT 20)
endif()
