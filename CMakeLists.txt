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
option(SLUGHORN_FREETYPE "Include FreeType support (requires freetype2)" OFF)
# option(SLUGHORN_GLFW "Include GLFW support (requires glfw3)" OFF)

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
	target_compile_definitions(slughorn PUBLIC  SLUGHORN_HAS_SERIAL)
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
	target_compile_definitions(slughorn PUBLIC  SLUGHORN_HAS_NANOSVG)
	target_include_directories(slughorn
		PUBLIC
			$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
			$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/ext/nanosvg/src>
			$<INSTALL_INTERFACE:include>
	)
endif()

if(SLUGHORN_FREETYPE)
	# if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ext/nanosvg")
	# 	message(FATAL_ERROR
	# 		"ext/nanosvg not found. Run:\n"
	# 		"  git submodule update --init --recursive"
	# 	)
	# endif()

	find_package(Freetype REQUIRED)

	target_compile_definitions(slughorn PRIVATE SLUGHORN_FREETYPE_IMPLEMENTATION)
	target_compile_definitions(slughorn PUBLIC  SLUGHORN_HAS_FREETYPE)
	# target_include_directories(slughorn
	# 	PUBLIC
	# 		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
	# 		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/ext/nanosvg/src>
	# 		$<INSTALL_INTERFACE:include>
	# )
	target_link_libraries(slughorn PUBLIC Freetype::Freetype)
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

# =============================================================================
# Tests (optional, enable with -DSLUGHORN_BUILD_TESTS=ON)
# =============================================================================
option(SLUGHORN_BUILD_TESTS "Compile testing files" OFF)

if(SLUGHORN_BUILD_TESTS)
	enable_testing()

	find_package(PkgConfig REQUIRED)

	# Cairo (optional)
	pkg_check_modules(CAIRO IMPORTED_TARGET cairo)

	if(CAIRO_FOUND)
		add_executable(slughorn-test-cairo "test/slughorn-test-cairo.cpp")
		target_link_libraries(slughorn-test-cairo PRIVATE slughorn PkgConfig::CAIRO)
		add_test(NAME cairo COMMAND slughorn-test-cairo)
		message(STATUS "slughorn tests: Cairo found, building slughorn-test-cairo")
	else()
		message(STATUS "slughorn tests: Cairo not found, skipping slughorn-test-cairo")
	endif()

	# NanoSVG (optional - requires SLUGHORN_NANOSVG=ON)
	if(SLUGHORN_NANOSVG)
		add_executable(slughorn-test-nanosvg "test/slughorn-test-nanosvg.cpp")

		target_include_directories(slughorn-test-nanosvg PRIVATE
			"${CMAKE_CURRENT_SOURCE_DIR}/ext/nanosvg/src"
		)

		target_link_libraries(slughorn-test-nanosvg PRIVATE slughorn)
		add_test(NAME nanosvg COMMAND slughorn-test-nanosvg)
		message(STATUS "slughorn tests: NanoSVG enabled, building slughorn-test-nanosvg")
	else()
		message(STATUS "slughorn tests: SLUGHORN_NANOSVG=OFF, skipping slughorn-test-nanosvg")
	endif()

	if(SLUGHORN_SERIAL)
		add_executable(slughorn-test-canvas "test/slughorn-test-canvas.cpp")
		target_link_libraries(slughorn-test-canvas PRIVATE slughorn)
		add_test(NAME canvas COMMAND slughorn-test-canvas)
		message(STATUS "slughorn tests: serial found, building slughorn-test-canvas")
	else()
		message(STATUS "slughorn tests: SLUGHORN_SERIAL=OFF, skipping slughorn-test-canvas")
	endif()

	# if(SLUGHORN_GLFW)
	# 	find_package(glfw3 REQUIRED)
	# 	add_executable(slughorn-test-glfw "test/slughorn-test-glfw.cpp")
	# 	target_link_libraries(slughorn-test-glfw PRIVATE slughorn glfw glad)
	# 	message(STATUS "slughorn tests: GLFW found, building slughorn-test-glfw")
	# else()
	# 	message(STATUS "slughorn tests: SLUGHORN_GLFW=OFF, skipping slughorn-test-glfw")
	# endif()

	# add_executable(slughorn-test-mvt "test/slughorn-test-mvt.cpp")
	# target_link_libraries(slughorn-test-mvt PRIVATE slughorn)
	# target_compile_options(slughorn-test-mvt PRIVATE ${SLUGHORN_COMPILE_OPTIONS})
	# target_include_directories(slughorn-test-mvt PRIVATE
	# 	"${CMAKE_CURRENT_SOURCE_DIR}/../protozero/include"
	# 	"${CMAKE_CURRENT_SOURCE_DIR}/../vtzero/include"
	# )

	# add_executable(slughorn-test-gdal "test/slughorn-test-gdal.cpp")
	# target_link_libraries(slughorn-test-gdal PRIVATE slughorn gdal)
	# target_compile_options(slughorn-test-gdal PRIVATE ${SLUGHORN_COMPILE_OPTIONS})
	# target_include_directories(slughorn-test-gdal PRIVATE "/usr/include/gdal")
endif()

# Let's be honest... I mostly care about the frontends I care about! So, these
# will all likely be AI-generated things (for now).
option(SLUGHORN_BUILD_EXAMPLES "Build example programs" OFF)

if(SLUGHORN_BUILD_EXAMPLES)
	add_subdirectory("example")
endif()
