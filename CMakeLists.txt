cmake_minimum_required(VERSION 3.28)

project(mog
  VERSION 0.1.0
  DESCRIPTION "Deterministic L3 limit-order-book backtesting engine"
  LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

set(MOG_PROFILES portable frontier lab)
set(MOG_PROFILE "frontier" CACHE STRING "Build profile: portable | frontier | lab")
set(MOG_CXX_STANDARD "26" CACHE STRING "Language standard: 26 (default) or 23 (consumer fallback)")
set(MOG_SANITIZE "" CACHE STRING "Sanitizers: e.g. address;undefined")

if(NOT MOG_PROFILE IN_LIST MOG_PROFILES)
  message(FATAL_ERROR "MOG_PROFILE must be one of: ${MOG_PROFILES}")
endif()
if(NOT MOG_CXX_STANDARD MATCHES "^(26|23)$")
  message(FATAL_ERROR "MOG_CXX_STANDARD must be 26 or 23")
endif()
if(MOG_CXX_STANDARD EQUAL 23 AND NOT MOG_PROFILE STREQUAL "portable")
  message(FATAL_ERROR "frontier/lab profiles require C++26; use portable for the C++23 fallback")
endif()

set(CMAKE_CXX_STANDARD ${MOG_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MOG_CXX_STANDARD EQUAL 26)
  set(MOG_MIN_GNU_VERSION 14.0)
  set(MOG_MIN_CLANG_VERSION 19.0)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
     AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS MOG_MIN_GNU_VERSION)
    message(FATAL_ERROR
      "GCC ${MOG_MIN_GNU_VERSION}+ required for the C++26 dialect; found "
      "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}. "
      "Use -DMOG_PROFILE=portable -DMOG_CXX_STANDARD=23.")
  endif()
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang"
     AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS MOG_MIN_CLANG_VERSION)
    message(FATAL_ERROR
      "Clang ${MOG_MIN_CLANG_VERSION}+ required for the C++26 dialect; found "
      "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}. "
      "Use -DMOG_PROFILE=portable -DMOG_CXX_STANDARD=23.")
  endif()
endif()

include(cmake/Features.cmake)

enable_testing()

string(TOUPPER "${MOG_PROFILE}" MOG_PROFILE_UPPER)
add_library(mog_flags INTERFACE)
target_compile_definitions(mog_flags INTERFACE
  MOG_VERSION="${PROJECT_VERSION}"
  MOG_PROFILE_${MOG_PROFILE_UPPER}=1
  MOG_CXX_STANDARD=${MOG_CXX_STANDARD})
target_compile_options(mog_flags INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -fno-exceptions -fno-rtti)
if(MOG_PROFILE MATCHES "^(frontier|lab)$")
  # MOG_ARCH overrides -march=native for environments that need reproducible
  # codegen independent of the build host's CPU (asm-parity snapshots). The
  # native march remains the default for performance runs.
  if(MOG_ARCH)
    target_compile_options(mog_flags INTERFACE -march=${MOG_ARCH} -Werror)
  else()
    target_compile_options(mog_flags INTERFACE -march=native -Werror)
  endif()
  # On hosts exposing AVX10.1, Clang's -march=native prints an informational
  # note that the 256-bit encoding will be promoted to 512-bit
  # (-Winvalid-feature-combination), which -Werror then escalates. The
  # promotion is exactly what we want from native tuning, so silence that
  # one diagnostic for Clang only; GCC has no such warning.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(mog_flags INTERFACE -Wno-invalid-feature-combination)
  endif()
endif()
if(MOG_HAS_NATIVE_CONTRACTS)
  target_compile_options(mog_flags INTERFACE ${MOG_NATIVE_CONTRACTS_FLAGS})
endif()
if(MOG_SANITIZE)
  foreach(san ${MOG_SANITIZE})
    if(san STREQUAL "address")
      target_compile_options(mog_flags INTERFACE -fsanitize=address -fno-omit-frame-pointer)
      target_link_options(mog_flags INTERFACE -fsanitize=address)
    elseif(san STREQUAL "undefined")
      target_compile_options(mog_flags INTERFACE -fsanitize=undefined -fno-sanitize-recover=all)
      target_link_options(mog_flags INTERFACE -fsanitize=undefined)
    else()
      message(FATAL_ERROR "Unknown sanitizer '${san}'")
    endif()
  endforeach()
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/generated)

add_library(mog_alloc STATIC src/AllocGuard.cpp)
# PIC so the static lib can also link into the nanobind shared module.
set_target_properties(mog_alloc PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(mog_alloc PRIVATE mog_flags)

add_executable(mog src/main.cpp)
target_link_libraries(mog PRIVATE mog_flags mog_alloc)

add_subdirectory(tools/probe)
add_subdirectory(tools/gen-sample)
add_subdirectory(tests)
add_subdirectory(bench)

option(MOG_BUILD_EXAMPLES "Build the example programs under examples/" OFF)
if(MOG_BUILD_EXAMPLES)
  add_subdirectory(examples/cpp)
endif()

option(MOG_BUILD_FUZZ "Build libFuzzer targets under tests/fuzz/ (Clang only)" OFF)
if(MOG_BUILD_FUZZ)
  add_subdirectory(tests/fuzz)
endif()

option(MOG_BUILD_PY "Build the nanobind Python extension module" OFF)
if(MOG_BUILD_PY)
  add_subdirectory(python)
endif()
