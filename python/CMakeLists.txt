# nanobind shim over the portable-profile mog core. Enabled from the root
# build with -DMOG_BUILD_PY=ON; scikit-build-core sets the same defines when
# building wheels (see pyproject.toml).
# The unified 'Python' package is required: nanobind consumes
# Python_INCLUDE_DIRS / Python::Module (not the legacy Python3 variants).
find_package(Python COMPONENTS Interpreter Development.Module REQUIRED)

# Prefer a pip-installed nanobind (ships a CMake package); fall back to a
# pinned FetchContent so bare checkouts still configure.
find_package(nanobind QUIET CONFIG)
if(NOT nanobind_FOUND)
  include(FetchContent)
  FetchContent_Declare(nanobind
    GIT_REPOSITORY https://github.com/wjakob/nanobind.git
    GIT_TAG v2.8.0)
  FetchContent_MakeAvailable(nanobind)
endif()

nanobind_add_module(_core mog/_core.cpp)
target_link_libraries(_core PRIVATE mog_flags mog_alloc)
target_include_directories(_core PRIVATE ${PROJECT_SOURCE_DIR}/tests)
# mog_flags turns RTTI/exceptions off repo-wide; nanobind's machinery needs
# both. INTERFACE flags from linked targets land after target-private ones,
# so re-enable at source level, which sorts last on the command line.
set_source_files_properties(mog/_core.cpp PROPERTIES
  COMPILE_OPTIONS "-fexceptions;-frtti")
set_target_properties(_core PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/pylib/mog)

# Self-contained dev/CI layout: PYTHONPATH=<build>/pylib python -m unittest ...
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/pylib/mog)
# Copy at build time (not configure time) so edits to package files are
# picked up by plain rebuilds.
add_custom_command(TARGET _core POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/mog/__init__.py"
    "${CMAKE_BINARY_DIR}/pylib/mog/__init__.py")

add_test(NAME mog-py
  COMMAND ${CMAKE_COMMAND} -E env
    "PYTHONPATH=${CMAKE_BINARY_DIR}/pylib"
    "${Python_EXECUTABLE}" -m unittest discover -v
    -s "${PROJECT_SOURCE_DIR}/python/tests" -t "${PROJECT_SOURCE_DIR}/python/tests")

if(SKBUILD)
  # scikit-build-core maps DESTINATION relative to site-packages; the module
  # must land inside the mog/ package directory next to __init__.py.
  install(TARGETS _core
    LIBRARY DESTINATION mog
    RUNTIME DESTINATION mog)
endif()
