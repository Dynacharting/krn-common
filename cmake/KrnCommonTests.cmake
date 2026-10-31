# krn_common_tests: krn's krn_market_tests, imported verbatim from krn at
# KRN_COMMON_KRN_COMMIT (purity SPEC D13, D17). Path mapping: identity, so
# tests/{book,feed,ob,market}/** here is krn's at that commit. The list
# mirrors krn's KRN_MARKET_TEST_SOURCES there.
# Included from the root CMakeLists.txt when KRN_COMMON_BUILD_TESTS is ON,
# after KrnEngine.cmake, KrnConnectorMarket.cmake and googletest.
#
# Not imported (D13): the corpus, tests/support/CorpusPath.hpp and
# tests/support/ClockCoherentHarness.hpp. They are read from the fetched
# engine tree: the imported files include "../support/<name>.hpp", which
# resolves through ${krn_SOURCE_DIR}/tests/market, and CorpusPath.hpp finds
# the corpus through KRN_TEST_SOURCE_DIR=${krn_SOURCE_DIR}.

set(KRN_COMMON_TEST_SOURCES
  tests/book/CrossedBookTest.cpp
  tests/book/OrderBookTest.cpp
  tests/feed/FeedEventDispatchTest.cpp
  tests/feed/ItchAdapterTest.cpp
  tests/feed/ItchSemanticTest.cpp
  tests/ob/ObKeyTest.cpp
  tests/ob/ObMaterializerTest.cpp
  tests/ob/ObProviderTest.cpp
  tests/market/AtomicFunctionsTest.cpp
  tests/market/AtomicKeyNamespaceMarketTest.cpp
  tests/market/BootstrapIdempotenceTest.cpp
  tests/market/ClockCoherentCorpusSweepTest.cpp
  tests/market/ConnectorLifecycleRealTest.cpp
  tests/market/GapFixesTest.cpp
  tests/market/IndicatorsTest.cpp
  tests/market/IntegrationTest.cpp
  tests/market/MarketFieldMapTest.cpp
  tests/market/MarketTANumericTest.cpp
  tests/market/test_bootstrap.cpp
)
list(TRANSFORM KRN_COMMON_TEST_SOURCES PREPEND "${PROJECT_SOURCE_DIR}/")

# Every file under tests/ is listed, so an imported file the build skips is a
# configure error, not a silent gap.
file(GLOB_RECURSE _kct_on_disk LIST_DIRECTORIES false "${PROJECT_SOURCE_DIR}/tests/*")
set(_kct_listed ${KRN_COMMON_TEST_SOURCES})
list(SORT _kct_on_disk)
list(SORT _kct_listed)
if (NOT _kct_on_disk STREQUAL _kct_listed)
  message(FATAL_ERROR
    "krn-common: tests/ on disk [${_kct_on_disk}] differs from "
    "krn_common_tests' list [${_kct_listed}]")
endif()

# The three engine-tree files, resolved from the fetched engine only.
file(REAL_PATH "${krn_SOURCE_DIR}" _kct_krn)
set(_kct_include_root "${_kct_krn}/tests/market")
foreach (_kct_f IN ITEMS
    tests/support/CorpusPath.hpp
    tests/support/ClockCoherentHarness.hpp
    tests/treebuilder/corpus_requests.json)
  if (EXISTS "${PROJECT_SOURCE_DIR}/${_kct_f}")
    message(FATAL_ERROR "krn-common: ${_kct_f} must not exist here; it is read "
      "from the engine tree (SPEC D13)")
  endif()
  if (NOT EXISTS "${_kct_krn}/${_kct_f}")
    message(FATAL_ERROR "krn-common: ${_kct_krn}/${_kct_f} not found in the engine tree")
  endif()
  message(STATUS "krn-common: krn_common_tests reads ${_kct_krn}/${_kct_f}")
endforeach()

add_executable(krn_common_tests ${KRN_COMMON_TEST_SOURCES})
target_include_directories(krn_common_tests PRIVATE "${_kct_include_root}")
target_compile_definitions(krn_common_tests PRIVATE
  KRN_TEST_SOURCE_DIR="${_kct_krn}")
target_link_libraries(krn_common_tests PRIVATE
  krn_connector_market krn::krn_engine krn_plugin_reference GTest::gtest_main)
list(LENGTH _kct_listed _kct_n)
message(STATUS "krn-common: krn_common_tests: ${_kct_n} files, all listed")
add_test(NAME krn_common_tests
  COMMAND krn_common_tests
  WORKING_DIRECTORY "$<TARGET_FILE_DIR:krn_common_tests>")
