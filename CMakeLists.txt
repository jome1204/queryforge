cmake_minimum_required(VERSION 3.16)
project(QueryForge VERSION 1.0.0 LANGUAGES C CXX)

option(QUERYFORGE_BUILD_TESTS "Build QueryForge unit tests" ON)
option(QUERYFORGE_BUILD_TOOLS "Build QueryForge command-line tools" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(queryforge
  src/analysis.cc
  src/btree.cc
  src/catalog.cc
  src/database.cc
  src/expression.cc
  src/lexer.cc
  src/page.cc
  src/parser.cc
  src/planner.cc
  src/record.cc
  src/transaction.cc
  src/utilities.cc
  src/value.cc
  src/wal.cc
)
target_include_directories(queryforge PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
if(MSVC)
  target_compile_options(queryforge PRIVATE /W4 /permissive-)
else()
  target_compile_options(queryforge PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(QUERYFORGE_BUILD_TOOLS)
  add_executable(qf_header_probe tools/c/qf_header_probe.c)
  add_executable(queryforge_cli tools/cpp/queryforge_cli.cc)
  target_link_libraries(queryforge_cli PRIVATE queryforge)
endif()

if(QUERYFORGE_BUILD_TESTS)
  enable_testing()
  add_executable(queryforge_tests tests/tests.cc)
  target_link_libraries(queryforge_tests PRIVATE queryforge)
  add_test(NAME queryforge_tests COMMAND queryforge_tests)
endif()
