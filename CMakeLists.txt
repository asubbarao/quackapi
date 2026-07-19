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
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# The HTTP server uses DuckDB's bundled httplib (third_party/httplib) — no
# external dependencies.
target_include_directories(${EXTENSION_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/httplib)
target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/httplib)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
