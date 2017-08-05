cmake_minimum_required(VERSION 3.5)
project(cehc)

set(CMAKE_CXX_FLAGS "-g -O0 -Wno-attributes")
#set(CMAKE_CXX_FLAGS "-O3 -Wno-attributes")
set(CMAKE_CXX_STANDARD 11)

add_subdirectory(./src)
