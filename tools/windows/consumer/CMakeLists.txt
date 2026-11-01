# Minimal downstream project: checks that an installed SDK can be found and linked.
cmake_minimum_required(VERSION 3.22)
project(armonik_sdk_consumer CXX)

find_package(ArmoniK.SDK.Client CONFIG REQUIRED)

add_executable(armonik_sdk_consumer main.cpp)
target_link_libraries(armonik_sdk_consumer PRIVATE ArmoniK.SDK.Client)
