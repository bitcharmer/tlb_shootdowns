cmake_minimum_required(VERSION 3.5)
project(tlb_shootdowns C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_GNU_SOURCE -fno-omit-frame-pointer")

add_executable(tlb_shootdowns src/main.c)
target_link_libraries(${PROJECT_NAME} numa rt pthread "/usr/local/lib/libhdr_histogram.so")
