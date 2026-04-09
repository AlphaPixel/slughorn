cmake_minimum_required(VERSION 3.15)

project(SlugHorn LANGUAGES C CXX)

find_package(Python3 COMPONENTS Interpreter Development REQUIRED)
add_subdirectory("ext/pybind11")

if(WIN32)
	set(SLUGHORN_COMPILE_OPTIONS
		"/W4"
		"/WX"
		"/wd4100"
		"/analyze"
		# "/wd4225"
		# "/wd4244"
		# "/wd4668"
		# "/wd4146"
		# "/wd4267"
		# "/wd4996"
		# "/wd4334"
		# "/wd4311"
	)

else(WIN32)
	if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
		set(SLUGHORN_COMPILE_OPTIONS
			"-W"
			"-Wall"
			"-Wshadow"
			"-Wconversion"
			"-Wpedantic"
			"-Werror"
			"-Wno-unused-parameter"
		)
	else(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
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
endif()

pybind11_add_module(slughorn_python
	"ext/slughorn-python.cpp"
	"slughorn.cpp"
)

set_target_properties(slughorn_python PROPERTIES OUTPUT_NAME "slughorn")

target_include_directories(slughorn_python PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(slughorn_python PRIVATE Python3::Python pybind11::module)
target_compile_features(slughorn_python PUBLIC cxx_std_20)
target_compile_options(slughorn_python PRIVATE ${SLUGHORN_COMPILE_OPTIONS})
