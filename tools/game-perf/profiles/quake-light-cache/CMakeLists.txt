cmake_minimum_required(VERSION 3.20)
project(pxa_quake_light_cache C CXX)
set(PXA_QUAKE_LIGHTMAP_CACHE ON CACHE BOOL "" FORCE)
include("${CMAKE_CURRENT_LIST_DIR}/../quake-host-prepare/CMakeLists.txt")
