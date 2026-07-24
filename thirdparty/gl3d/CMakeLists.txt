cmake_minimum_required(VERSION 3.5)
project(gl3d)

add_library(gl3d)
target_sources(gl3d PRIVATE "gl3d_amd_compat.cpp")
target_include_directories(gl3d PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/")
target_link_libraries(gl3d PUBLIC glm glad stb_image)
