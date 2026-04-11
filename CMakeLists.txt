cmake_minimum_required(VERSION 3.15)

project(SlugHorn LANGUAGES C CXX)

# =============================================================================
# Compile options
# =============================================================================

if(WIN32)
	set(SLUGHORN_COMPILE_OPTIONS
		"/W4"
		"/WX"
		"/wd4100"
		"/analyze"
	)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
	set(SLUGHORN_COMPILE_OPTIONS
		"-W"
		"-Wall"
		"-Wshadow"
		"-Wconversion"
		"-Wpedantic"
		"-Werror"
		"-Wno-unused-parameter"
	)
else()
	set(SLUGHORN_COMPILE_OPTIONS
		"-W"
		"-Wall"
		"-Wshadow"
		"-Wsign-conversion"
		"-Wpedantic"
		"-Werror"
		"-Wno-unused-parameter"
	)
endif()

# =============================================================================
# Core library target
# =============================================================================

option(SLUGHORN_SERIAL "Include serialization support (requires nlohmann)" OFF)
option(SLUGHORN_NANOSVG "Include NanoSVG support (requires nanosvg)" OFF)

add_library(slughorn STATIC slughorn.cpp)

target_include_directories(slughorn
	PUBLIC
		# When used as a submodule, consumers get our headers via this path
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
		$<INSTALL_INTERFACE:include>
)

target_compile_features(slughorn PUBLIC cxx_std_20)
target_compile_options(slughorn PRIVATE ${SLUGHORN_COMPILE_OPTIONS})
set_target_properties(slughorn PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(SLUGHORN_SERIAL)
	if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ext/json")
		message(FATAL_ERROR
			"ext/json not found. Run:\n"
			"  git submodule update --init --recursive"
		)
	endif()

	add_subdirectory("ext/json")

	target_compile_definitions(slughorn PRIVATE SLUGHORN_SERIAL_IMPLEMENTATION)
	#target_include_directories(slughorn PRIVATE
	#	"${CMAKE_CURRENT_SOURCE_DIR}/ext/json"
	#)
	target_link_libraries(slughorn PRIVATE nlohmann_json::nlohmann_json)
endif()

if(SLUGHORN_NANOSVG)
	if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ext/nanosvg")
		message(FATAL_ERROR
			"ext/nanosvg not found. Run:\n"
			"  git submodule update --init --recursive"
		)
	endif()

	target_compile_definitions(slughorn PRIVATE SLUGHORN_NANOSVG_IMPLEMENTATION)
	#target_include_directories(slughorn PRIVATE
	#	"${CMAKE_CURRENT_SOURCE_DIR}/ext/nanosvg/src"
	#)
	target_include_directories(slughorn
		PUBLIC
			$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
			$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/ext/nanosvg/src>
			$<INSTALL_INTERFACE:include>
	)
endif()

# =============================================================================
# Python bindings (optional - only when building slughorn standalone)
# =============================================================================

# When slughorn is included as a submodule, the parent project controls
# whether Python bindings are built. We only auto-configure them when
# slughorn is the top-level project.

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
	find_package(Python3 COMPONENTS Interpreter Development REQUIRED)

	if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ext/pybind11/CMakeLists.txt")
		message(FATAL_ERROR
			"pybind11 submodule is missing. Run:\n"
			"  git submodule update --init --recursive"
		)
	endif()

	add_subdirectory(ext/pybind11)

	pybind11_add_module(slughorn_python "ext/slughorn-python.cpp")

	set_target_properties(slughorn_python PROPERTIES OUTPUT_NAME "slughorn")

	target_link_libraries(slughorn_python PRIVATE slughorn pybind11::module)
	target_compile_features(slughorn_python PUBLIC cxx_std_20)
	target_compile_options(slughorn_python PRIVATE ${SLUGHORN_COMPILE_OPTIONS})
	set_target_properties(slughorn_python PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
