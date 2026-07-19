cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME quackapi)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

set(EXTENSION_SOURCES
    src/quackapi_extension.cpp
    src/quackapi_registry.cpp
    src/quackapi_server.cpp
    src/quackapi_ddl.cpp
    src/quackapi_auth.cpp
    src/quackapi_http_fetch.cpp
    src/quackapi_table_api.cpp
    src/quackapi_openapi.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# The HTTP *server* uses DuckDB's bundled httplib (third_party/httplib).
# Outbound HTTP uses DuckDB's HTTPUtil (core) and is upgraded at runtime by
# LOAD curl_httpfs / LOAD httpfs — quackapi does NOT link libcurl.
target_include_directories(${EXTENSION_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/httplib)
target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/httplib)

# Auth crypto uses DuckDB's bundled mbedtls (third_party/mbedtls), the SAME
# static library httpfs and parquet link as `duckdb_mbedtls`.
#
# Citations (verified in this tree / upstream):
#   duckdb/third_party/mbedtls/CMakeLists.txt
#     → add_library(duckdb_mbedtls STATIC … sha256.cpp md.cpp base64.cpp …)
#   duckdb/extension/parquet/CMakeLists.txt
#     → target_link_libraries(parquet_loadable_extension duckdb_mbedtls …)
#   duckdb-httpfs CMakeLists.txt (GIT_URL in duckdb/.github/config/extensions/httpfs.cmake)
#     → target_link_libraries(httpfs_loadable_extension duckdb_mbedtls …)
#     → target_link_libraries(httpfs_extension duckdb_mbedtls …)
#   duckdb/CMakeLists.txt
#     → include_directories(third_party/mbedtls/include)
#     → add_third_party(mbedtls)
#
# Headers: mbedtls/sha256.h, mbedtls/md.h, mbedtls/base64.h, mbedtls/constant_time.h
target_include_directories(${EXTENSION_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/mbedtls/include)
target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/mbedtls/include)

# Link the bundled target into BOTH the static and loadable extension objects,
# matching httpfs (links both) rather than only the loadable (parquet).
target_link_libraries(${EXTENSION_NAME} duckdb_mbedtls)
target_link_libraries(${LOADABLE_EXTENSION_NAME} duckdb_mbedtls)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
