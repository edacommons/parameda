cmake_minimum_required(VERSION 3.20)

project(parameda
    VERSION 0.1.0
    DESCRIPTION "Hierarchical configuration system for EDA tooling"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Default build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

option(PARAMEDA_BUILD_TESTS "Build C++ tests" ON)

# Some transitive deps (tl::expected via rawast, doctest) declare very old
# cmake_minimum_required values that modern CMake refuses outright.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

include(FetchContent)

# ---------------------------------------------------------------------------
# rawast — value model, parsing, and bidirectional serialization.
#
# Pinned to a tag for reproducible builds (see docs/SPEC.md §8). For local
# co-development against a working tree, configure with
#   -DFETCHCONTENT_SOURCE_DIR_RAWAST=/path/to/rawast
#
# We hide SKBUILD from rawast while configuring it: under scikit-build-core,
# rawast's own CMake would otherwise also build *its* nanobind module (a second
# `_native` target + a duplicate nanobind fetch). We only want its static lib;
# PIC is forced on below so it links cleanly into our Python extension.
# ---------------------------------------------------------------------------
FetchContent_Declare(rawast
    GIT_REPOSITORY https://github.com/edacommons/rawast.git
    GIT_TAG v0.1.8
    GIT_SHALLOW ON
)
set(RAWAST_BUILD_TESTS OFF CACHE BOOL "" FORCE)

set(_parameda_skbuild_saved "${SKBUILD}")
set(SKBUILD OFF)
FetchContent_MakeAvailable(rawast)
set(SKBUILD "${_parameda_skbuild_saved}")
set_property(TARGET rawast PROPERTY POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# parameda core library.
# ---------------------------------------------------------------------------
add_library(parameda STATIC
    src/version.cpp
    src/core.cpp
)
add_library(parameda::parameda ALIAS parameda)

target_include_directories(parameda
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(parameda PUBLIC rawast::rawast)
target_compile_features(parameda PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(parameda PRIVATE /W4)
else()
    target_compile_options(parameda PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(PARAMEDA_BUILD_TESTS AND NOT SKBUILD)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Python binding (built only when invoked via scikit-build-core).
# ---------------------------------------------------------------------------
if(SKBUILD)
    set_property(TARGET parameda PROPERTY POSITION_INDEPENDENT_CODE ON)

    find_package(Python REQUIRED COMPONENTS Interpreter Development.Module)

    FetchContent_Declare(nanobind
        GIT_REPOSITORY https://github.com/wjakob/nanobind.git
        GIT_TAG v2.4.0
        GIT_SHALLOW ON
    )
    FetchContent_MakeAvailable(nanobind)

    nanobind_add_module(_native NB_STATIC python/src/native.cc)
    target_link_libraries(_native PRIVATE parameda::parameda)
    target_compile_features(_native PRIVATE cxx_std_17)

    install(TARGETS _native LIBRARY DESTINATION parameda)
endif()
