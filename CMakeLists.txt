cmake_minimum_required(VERSION 3.16)
project(MeterMandelbrotDesktop LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Cross-platform build of the command-line engine (`mset`).
#
# The Win32 + WebView2 desktop application is Windows-only; see
# desktop/build.bat and docs/BUILD.md.  Everything under src/ is portable:
# the only platform fork is src/gmp_bridge.cpp (LoadLibrary vs dlopen).
#
#   cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-linux -j"$(nproc)"
#   ./build-linux/mset info
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MB_AVX2       "Build the four-pixel AVX2 kernel (needs an AVX2 CPU)" ON)
option(MB_BUILD_TESTS "Build the b2 verification programs" OFF)

# Exactly the translation-unit list in buildall.bat.  The src/recovered*,
# src/app and most of src/port trees are historical reconstruction material
# and are deliberately NOT compiled.
set(MB_CORE_SOURCES
  src/bigfloat.cpp
  src/gmp_bridge.cpp
  src/dive.cpp
  src/reference.cpp
  src/deep.cpp
  src/render.cpp
  src/palette.cpp
  src/image.cpp
  src/workspace.cpp
  src/port/registry.cpp
)

find_package(Threads REQUIRED)

add_library(mset_core STATIC ${MB_CORE_SOURCES})
target_include_directories(mset_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(mset_core PUBLIC Threads::Threads)

if(MSVC)
  target_compile_options(mset_core PRIVATE /W3 /utf-8 /EHsc)
  if(MB_AVX2)
    target_compile_options(mset_core PRIVATE /arch:AVX2)
  endif()
else()
  target_compile_options(mset_core PRIVATE -O3 -Wall -Wextra)
  if(MB_AVX2)
    target_compile_options(mset_core PRIVATE -mavx2 -mfma)
  endif()
  target_link_libraries(mset_core PUBLIC ${CMAKE_DL_LIBS})
endif()

add_executable(mset src/main.cpp)
target_link_libraries(mset PRIVATE mset_core)

if(MB_BUILD_TESTS)
  # b2.bat builds one test at a time; CMake builds the same programs in bulk.
  file(GLOB MB_B2_TESTS CONFIGURE_DEPENDS "tests/b2/*.cpp")
  foreach(test_src IN LISTS MB_B2_TESTS)
    get_filename_component(test_name "${test_src}" NAME_WE)
    add_executable("b2_${test_name}" "${test_src}")
    target_link_libraries("b2_${test_name}" PRIVATE mset_core)
  endforeach()
endif()

# `make check` equivalent: byte-level accuracy against the exact BigFloat oracle.
enable_testing()
add_test(NAME oracle_1e38 COMMAND mset cmp 38 3000 5)
