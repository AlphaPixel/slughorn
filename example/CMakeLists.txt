# CMakeLists.txt (slughorn-example-glfw target)
#
# Add this to your existing CMakeLists.txt, or use standalone with:
#
# cmake -B build -DCMAKE_BUILD_TYPE=Release
# cmake --build build
#
# Assumes slughorn (the core library) is already defined as a CMake target
# named `slughorn` in the parent build. If you're building standalone, replace
# `target_link_libraries(... slughorn ...)` with whatever brings in slughorn.hpp
# and slughorn.cpp (e.g. add_library(slughorn ...) or just add slughorn.cpp to
# the executable's sources directly).

include(FetchContent)

# ----------------------------------------------------------------------------
# GLAD (GL 3.3 core, no extensions needed)
#
# We use the davidcralph/glad-cmake wrapper which fetches from the official
# GLAD2 generator output. If you already have a glad/ directory vendored in
# your repo, skip this block and just add_subdirectory(glad) instead.
# ----------------------------------------------------------------------------
FetchContent_Declare(
	glad
	GIT_REPOSITORY https://github.com/Dav1dde/glad.git
	GIT_TAG v0.1.36 # last stable v1 release; use v2 branch for GLAD2
	GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(glad)

# ----------------------------------------------------------------------------
# GLFW 3.x
#
# BUILD_SHARED_LIBS=OFF keeps it static (no .dll/.so to ship alongside the demo).
# The GLFW_* options silence docs/tests/examples that we don't need.
# ----------------------------------------------------------------------------
FetchContent_Declare(
	glfw
	GIT_REPOSITORY https://github.com/glfw/glfw.git
	GIT_TAG 3.4
	GIT_SHALLOW TRUE
)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(glfw)

# ----------------------------------------------------------------------------
# slughorn-example-glfw
# ----------------------------------------------------------------------------
add_executable(slughorn-example-glfw
	"slughorn-example-glfw.cpp"
	# If slughorn is not already a library target in your build, add here:
	# ${CMAKE_SOURCE_DIR}/slughorn.cpp
)

target_link_libraries(slughorn-example-glfw PRIVATE
	slughorn # your existing core library target
	glad
	glfw
)

# slughorn-canvas.hpp is header-only; just needs to be on the include path.
# Adjust if your layout differs.
target_include_directories(slughorn-example-glfw PRIVATE
	${CMAKE_SOURCE_DIR} # for slughorn.hpp, slughorn-canvas.hpp
)

# GL 3.3 core; tell GLAD what to generate (only needed for the GLAD v1 cmake target)
target_compile_definitions(slughorn-example-glfw PRIVATE
	GLAD_GLAPI_EXPORT
)

set_target_properties(slughorn-example-glfw PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
