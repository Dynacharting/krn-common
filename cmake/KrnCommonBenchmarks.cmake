# bench_atomic_functions: benchmarks/AtomicFunctionsBench.cpp, imported verbatim
# from krn at KRN_COMMON_KRN_COMMIT (purity SPEC D17); path mapping: identity.
# Google Benchmark at the engine's own tag (krn CMakeLists.txt).
# Included from the root CMakeLists.txt when KRN_COMMON_BUILD_BENCHMARKS is ON.

include(FetchContent)
FetchContent_Declare(
  googlebenchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG        v1.8.3
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googlebenchmark)

add_executable(bench_atomic_functions
  "${PROJECT_SOURCE_DIR}/benchmarks/AtomicFunctionsBench.cpp")
target_link_libraries(bench_atomic_functions PRIVATE
  krn_connector_market krn::krn_engine benchmark::benchmark)

file(GLOB_RECURSE _kcb_on_disk LIST_DIRECTORIES false "${PROJECT_SOURCE_DIR}/benchmarks/*")
if (NOT _kcb_on_disk STREQUAL "${PROJECT_SOURCE_DIR}/benchmarks/AtomicFunctionsBench.cpp")
  message(FATAL_ERROR
    "krn-common: benchmarks/ on disk [${_kcb_on_disk}] differs from "
    "[benchmarks/AtomicFunctionsBench.cpp]")
endif()
message(STATUS "krn-common: bench_atomic_functions from benchmarks/AtomicFunctionsBench.cpp")
