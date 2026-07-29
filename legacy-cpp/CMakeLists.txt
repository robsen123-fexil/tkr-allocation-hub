cmake_minimum_required(VERSION 3.16)
project(tkr-allocation-hub VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_SOURCE_DIR}/include)

file(GLOB_RECURSE TKR_SOURCES CONFIGURE_DEPENDS src/*.cc)

add_library(tkr STATIC ${TKR_SOURCES})
target_include_directories(tkr PUBLIC ${CMAKE_SOURCE_DIR}/include)

add_executable(tkrctl tools/tkrctl.cc)
target_link_libraries(tkrctl PRIVATE tkr)

add_executable(allocation_test tests/integration/allocation_test.cc)
target_link_libraries(allocation_test PRIVATE tkr)

set(FUZZERS batch_fuzzer router_fuzzer session_fuzzer fix_fuzzer swift_fuzzer)
foreach(FUZZER ${FUZZERS})
  add_executable(${FUZZER} fuzz/${FUZZER}.cc)
  target_link_libraries(${FUZZER} PRIVATE tkr)
  target_compile_options(${FUZZER} PRIVATE -fsanitize=fuzzer,address)
  target_link_options(${FUZZER} PRIVATE -fsanitize=fuzzer,address)
endforeach()

enable_testing()
add_test(NAME allocation_test COMMAND allocation_test)
