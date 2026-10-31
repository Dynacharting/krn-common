cmake_minimum_required(VERSION 3.20)
project(KRN_COMMON LANGUAGES CXX)

# ---- Default build type (ENC-1589, as krn ENC-1548 and krn-data ENC-1586) ----
# An empty CMAKE_BUILD_TYPE on a single-config generator compiles with no
# optimisation flags, and the engine's Release block never runs. Top-level and
# single-config only; a non-empty -DCMAKE_BUILD_TYPE always wins.
get_property(_kc_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if (CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR AND NOT _kc_multi_config AND "${CMAKE_BUILD_TYPE}" STREQUAL "")
  set(CMAKE_BUILD_TYPE Release CACHE STRING
    "Build type (single-config generators). Defaulted to Release when empty." FORCE)
  message(STATUS "krn-common: CMAKE_BUILD_TYPE was empty, defaulted to Release")
endif()

# krn-common's own options. Never spelled KRN_*: those belong to the engine,
# and a consumer's cache must not carry values the engine could read.
option(KRN_COMMON_BUILD_TESTS "Build krn_common_tests and register it with ctest" ON)
option(KRN_COMMON_BUILD_BENCHMARKS "Build bench_atomic_functions (fetches Google Benchmark)" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- The engine pin (purity SPEC D5) ----
# A full 40-character commit of Dynacharting/krn master. Bumping it is a
# one-line diff here; KRN_COMMON_KRN_TARGETS is what the engine defines in
# consumed mode at that commit, and changes in the same diff when it changes.
set(KRN_COMMON_KRN_REPOSITORY "https://github.com/Dynacharting/krn.git")
set(KRN_COMMON_KRN_COMMIT     "c01005bb371c2e39937e691e55f81054d3de0659")
set(KRN_COMMON_KRN_TARGETS    krn_engine krn_host krn_plugin_reference)

include("${PROJECT_SOURCE_DIR}/cmake/KrnEngine.cmake")

# connectors/market is imported verbatim from krn at KRN_COMMON_KRN_COMMIT
# (SPEC D17); its build glue is cmake/KrnConnectorMarket.cmake.
include("${PROJECT_SOURCE_DIR}/cmake/KrnConnectorMarket.cmake")

# krn_server: src/main.cpp, imported verbatim from krn at KRN_COMMON_KRN_COMMIT.
include("${PROJECT_SOURCE_DIR}/cmake/KrnServer.cmake")

if (KRN_COMMON_BUILD_TESTS)
  include(FetchContent)
  # Same googletest as the engine's own tests (krn CMakeLists.txt).
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  # krn_common_tests: krn's krn_market_tests, imported verbatim (SPEC D13),
  # and the feed-port script tests against krn_server (D6).
  include("${PROJECT_SOURCE_DIR}/cmake/KrnCommonTests.cmake")
else()
  message(STATUS "krn-common: KRN_COMMON_BUILD_TESTS=OFF, no tests registered")
endif()

if (KRN_COMMON_BUILD_BENCHMARKS)
  include("${PROJECT_SOURCE_DIR}/cmake/KrnCommonBenchmarks.cmake")
endif()

# The engine's warnings, Release optimisation/arch flags and LTO, applied to
# this repo's targets as krn applies them to its own at the pin.
include("${PROJECT_SOURCE_DIR}/cmake/KrnReleaseFlags.cmake")
