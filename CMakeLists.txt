cmake_minimum_required(VERSION 3.16)

project(MemoryPool LANGUAGES CXX)

find_package(Threads REQUIRED)

add_library(memory_pool INTERFACE)
target_include_directories(memory_pool INTERFACE ${PROJECT_SOURCE_DIR}/include)
target_compile_features(memory_pool INTERFACE cxx_std_17)
target_link_libraries(memory_pool INTERFACE Threads::Threads)

add_executable(memory_pool_demo src/main.cpp)
target_link_libraries(memory_pool_demo PRIVATE memory_pool)
target_compile_options(memory_pool_demo PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()

add_executable(memory_pool_smoke_test tests/memory_pool_smoke_test.cpp)
target_link_libraries(memory_pool_smoke_test PRIVATE memory_pool)
target_compile_options(memory_pool_smoke_test PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME memory_pool_smoke_test COMMAND memory_pool_smoke_test)
