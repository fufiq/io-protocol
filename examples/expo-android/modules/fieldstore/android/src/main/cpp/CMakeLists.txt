cmake_minimum_required(VERSION 3.22)
project(fieldstore CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(fieldstore SHARED fieldstore.cpp)
target_include_directories(fieldstore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# The emulator this was developed against is a 16 KB-page image, and so are
# recent physical devices. Without this every LOAD segment is aligned to 4096
# and the loader reads the dynamic section at the wrong offset - the symptom is
# not a clean error but a misparsed dynamic section and a SIGSEGV.
target_link_options(fieldstore PRIVATE "-Wl,-z,max-page-size=16384")
