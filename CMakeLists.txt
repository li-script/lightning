cmake_minimum_required(VERSION 3.12.0)
set (CMAKE_CXX_STANDARD 20)
project(LibLi C CXX)

if(EMSCRIPTEN)
	set(CMAKE_BUILD_TYPE Debug)
	add_compile_definitions(LI_DEBUG)
	add_definitions(-O3)
endif()

file(GLOB_RECURSE SOURCES  src/*.cpp src/*.hpp)
file(GLOB_RECURSE INCLUDES include/*)

# Will convert to Library once API is stable.
#
add_executable(${PROJECT_NAME} ${SOURCES} ${INCLUDES})
if(EMSCRIPTEN)
	target_link_options(${PROJECT_NAME} PRIVATE 
		-sEXPORTED_RUNTIME_METHODS=ccall,cwrap
		-sEXTRA_EXPORTED_RUNTIME_METHODS=ccall,cwrap
		-sEXPORTED_FUNCTIONS=_main,_runscript
	)
endif()
target_include_directories(${PROJECT_NAME} PUBLIC include)