cmake_minimum_required(VERSION 3.20)
project(bc CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
add_compile_options(-Wall -Wextra)

option(ENABLE_SANITIZERS "Build tests/fuzzers with ASan+UBSan (kills throughput)" OFF)
option(ENABLE_FUZZERS "Build libFuzzer targets (requires clang)" OFF)

find_library(URING_LIB uring REQUIRED)
find_path(URING_INCLUDE liburing.h REQUIRED)
find_library(SODIUM_LIB sodium REQUIRED)
find_path(SODIUM_INCLUDE sodium.h REQUIRED)

# Header-only components. The dependency direction here is the point of the
# layout, and it is enforced by what each target is allowed to include:
#   net      <- nothing else in this repo
#   storage  <- nothing else in this repo
#   runtime  <- nothing else in this repo (fibers/queues are swappable)
#   replication -> net + storage   (the ONLY component that couples them)
add_library(net INTERFACE)
target_include_directories(net INTERFACE ${CMAKE_SOURCE_DIR}/net ${URING_INCLUDE} ${SODIUM_INCLUDE})
target_link_libraries(net INTERFACE ${URING_LIB} ${SODIUM_LIB})

add_library(storage INTERFACE)
target_include_directories(storage INTERFACE ${CMAKE_SOURCE_DIR}/storage)

add_library(runtime INTERFACE)
target_include_directories(runtime INTERFACE ${CMAKE_SOURCE_DIR}/runtime)

add_library(replication INTERFACE)
target_link_libraries(replication INTERFACE net storage)

# --- benches / demos -------------------------------------------------------
function(add_bench name)
    add_executable(${name} bench/${name}.cpp)
    target_link_libraries(${name} PRIVATE net)
endfunction()

foreach(b
    level1_echo_server level1_client_bench
    level2_handshake_bench level2_udp_bench_server level2_udp_bench_client
    level2_post_handshake_pps_server level2_post_handshake_pps_client
    level3_fragment_pps_server level3_fragment_pps_client
    level4_transport_server level4_transport_client
    level5_anti_dos_server level5_legit_client level5_attack_client
    level6_reliability_server level6_reliability_client level6_reliability_pps_client
    level7_protocol_server level7_protocol_client)
    add_bench(${b})
endforeach()

add_executable(stress_heap bench/stress_heap.cpp)
target_link_libraries(stress_heap PRIVATE storage runtime)
find_package(Threads REQUIRED)
target_link_libraries(stress_heap PRIVATE Threads::Threads)

add_executable(business_demo_heap bench/business_demo_heap.cpp)
target_link_libraries(business_demo_heap PRIVATE storage runtime Threads::Threads)

add_executable(replication_demo bench/replication_demo.cpp)
target_link_libraries(replication_demo PRIVATE replication)

# --- tests -----------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.12)
FetchContent_MakeAvailable(doctest)

file(GLOB TEST_SOURCES ${CMAKE_SOURCE_DIR}/tests/test_*.cpp)
# test_replication.cpp brings its own main() (it drives real endpoint pairs
# and a fiber scheduler, which doesn't fit doctest's per-case model), so it
# builds as its own runner rather than linking into the doctest binary.
list(REMOVE_ITEM TEST_SOURCES ${CMAKE_SOURCE_DIR}/tests/test_replication.cpp)

add_executable(unit_tests ${TEST_SOURCES})
target_link_libraries(unit_tests PRIVATE replication runtime doctest::doctest Threads::Threads)

add_executable(replication_tests tests/test_replication.cpp)
target_link_libraries(replication_tests PRIVATE replication runtime Threads::Threads)

if(ENABLE_SANITIZERS)
    foreach(t unit_tests replication_tests)
        target_compile_options(${t} PRIVATE -fsanitize=address,undefined -g -O1)
        target_link_options(${t} PRIVATE -fsanitize=address,undefined)
    endforeach()
endif()

enable_testing()
add_test(NAME unit_tests COMMAND unit_tests)
add_test(NAME replication_tests COMMAND replication_tests)

# --- fuzzers ---------------------------------------------------------------
# libFuzzer needs clang; these are opt-in so a gcc build stays unaffected.
if(ENABLE_FUZZERS)
    file(GLOB FUZZ_SOURCES ${CMAKE_SOURCE_DIR}/tests/fuzz/fuzz_*.cpp)
    foreach(fsrc ${FUZZ_SOURCES})
        get_filename_component(fname ${fsrc} NAME_WE)
        add_executable(${fname} ${fsrc})
        target_link_libraries(${fname} PRIVATE replication runtime)
        target_compile_options(${fname} PRIVATE -fsanitize=fuzzer,address,undefined -g -O1)
        target_link_options(${fname} PRIVATE -fsanitize=fuzzer,address,undefined)
    endforeach()
endif()
