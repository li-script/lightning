cmake_minimum_required(VERSION 3.12.0)
set (CMAKE_CXX_STANDARD 20)
project(Lightning C CXX)

file(GLOB_RECURSE SOURCES  src/*.cpp src/*.hpp)
file(GLOB_RECURSE INCLUDES include/*)

# Will convert to Library once API is stable.
#
add_executable(${PROJECT_NAME} ${SOURCES} ${INCLUDES})
target_include_directories(${PROJECT_NAME} PUBLIC include)