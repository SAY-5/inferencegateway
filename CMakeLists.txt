cmake_minimum_required(VERSION 3.20)
project(inferencegateway LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Strict warnings.
add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

# Header-only ig:: library.
add_library(ig_headers INTERFACE)
target_include_directories(ig_headers INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ig_headers INTERFACE pthread)

# Binaries.
add_executable(inferenceg src/main.cpp)
target_link_libraries(inferenceg PRIVATE ig_headers)

add_executable(fakebackend src/fakebackend.cpp)
target_link_libraries(fakebackend PRIVATE ig_headers)

add_executable(loadgen src/loadgen.cpp)
target_link_libraries(loadgen PRIVATE ig_headers)

# Tests.
enable_testing()
add_executable(test_router tests/test_router.cpp)
target_link_libraries(test_router PRIVATE ig_headers)
add_test(NAME router COMMAND test_router)

add_executable(test_scheduler tests/test_scheduler.cpp)
target_link_libraries(test_scheduler PRIVATE ig_headers)
add_test(NAME scheduler COMMAND test_scheduler)

add_executable(test_stream tests/test_stream.cpp)
target_link_libraries(test_stream PRIVATE ig_headers)
add_test(NAME stream COMMAND test_stream)
