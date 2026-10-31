cmake_minimum_required(VERSION 3.20)
project(KRN_COMMON LANGUAGES CXX)

# krn-common's own options. Never spelled KRN_*: those belong to the engine,
# and a consumer's cache must not carry values the engine could read.
option(KRN_COMMON_BUILD_TESTS "Build krn_common_tests and register it with ctest" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- The engine pin (purity SPEC D5) ----
# A full 40-character commit of Dynacharting/krn master. Bumping it is a
# one-line diff here; KRN_COMMON_KRN_TARGETS is what the engine defines in
# consumed mode at that commit, and changes in the same diff when it changes.
set(KRN_COMMON_KRN_REPOSITORY "https://github.com/Dynacharting/krn.git")
set(KRN_COMMON_KRN_COMMIT     "ca544ec5fff80e4a7cf0c4bf5c2feefacad596b3")
set(KRN_COMMON_KRN_TARGETS    krn_engine krn_host krn_plugin_reference)

include("${PROJECT_SOURCE_DIR}/cmake/KrnEngine.cmake")

# connectors/market is imported verbatim from krn at KRN_COMMON_KRN_COMMIT
# (SPEC D17); its build glue is cmake/KrnConnectorMarket.cmake.
include("${PROJECT_SOURCE_DIR}/cmake/KrnConnectorMarket.cmake")

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
  # krn_common_tests: krn's krn_market_tests, imported verbatim (SPEC D13).
  include("${PROJECT_SOURCE_DIR}/cmake/KrnCommonTests.cmake")
else()
  message(STATUS "krn-common: KRN_COMMON_BUILD_TESTS=OFF, no tests registered")
endif()
