cmake_minimum_required(VERSION 3.10)
project(MemoryPool)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

add_executable(MemoryPool src/main.cpp)

if(MSVC)
    target_compile_options(MemoryPool PRIVATE /W4)
else()
    target_compile_options(MemoryPool PRIVATE -Wall -Wextra -pthread)
endif()
