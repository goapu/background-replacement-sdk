cmake_minimum_required(VERSION 3.21)
project(magic_portal_consumer LANGUAGES C CXX)

find_package(magic_portal 0.1 REQUIRED CONFIG)

# A shared build exports only the C interface; the C++ classes are for static builds.
get_target_property(_mp_type magic_portal::magic_portal TYPE)
if(_mp_type STREQUAL "STATIC_LIBRARY")
    add_executable(consumer_cpp main.cpp)
    target_link_libraries(consumer_cpp PRIVATE magic_portal::magic_portal)
endif()

add_executable(consumer_c main.c)
target_link_libraries(consumer_c PRIVATE magic_portal::magic_portal)
set_target_properties(consumer_c PROPERTIES LINKER_LANGUAGE CXX)
