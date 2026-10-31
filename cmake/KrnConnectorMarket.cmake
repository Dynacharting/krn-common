# krn_connector_market: connectors/market, imported verbatim from krn at
# KRN_COMMON_KRN_COMMIT (purity SPEC D8, D17). Path mapping: identity, so
# connectors/market/** here is krn's connectors/market/** at that commit.
# The lists mirror krn's KRN_MARKET_SOURCES / KRN_MARKET_HEADERS there.
# Included from the root CMakeLists.txt after KrnEngine.cmake.

set(_kcm_root "${PROJECT_SOURCE_DIR}/connectors/market")

set(KRN_COMMON_MARKET_SOURCES
  src/MarketConfig.cpp
  src/MarketConnector.cpp
  src/MarketIngress.cpp
  src/MarketTA.cpp
  src/book/OrderBook.cpp
  src/book/OrderBookManager.cpp
  src/feed/IFeedAdapter.cpp
  src/feed/ItchAdapter.cpp
  src/ob/ObKey.cpp
  src/ob/ObMaterializer.cpp
  src/ob/ObProvider.cpp
  src/server/FeedServer.cpp
  src/ws/WsFeedClient.cpp
)
list(TRANSFORM KRN_COMMON_MARKET_SOURCES PREPEND "${_kcm_root}/")

set(KRN_COMMON_MARKET_HEADERS
  include/krn/book/DepthTypes.hpp
  include/krn/book/OrderBook.hpp
  include/krn/book/OrderBookManager.hpp
  include/krn/feed/FeedEvent.hpp
  include/krn/feed/IFeedAdapter.hpp
  include/krn/feed/ItchAdapter.hpp
  include/krn/market/AtomicFunctions.hpp
  include/krn/market/FeedServer.hpp
  include/krn/market/MarketConfig.hpp
  include/krn/market/MarketConnector.hpp
  include/krn/market/MarketFieldMap.hpp
  include/krn/market/MarketIngress.hpp
  include/krn/market/MarketTA.hpp
  include/krn/market/SymbolHistory.hpp
  include/krn/market/WsFeedClient.hpp
  include/krn/ob/FunctionalSnapshotSource.hpp
  include/krn/ob/ObKey.hpp
  include/krn/ob/ObMaterializer.hpp
  include/krn/ob/ObProvider.hpp
  include/krn/ob/ObSnapshot.hpp
  include/krn/ta/Indicators.hpp
)
list(TRANSFORM KRN_COMMON_MARKET_HEADERS PREPEND "${_kcm_root}/")

add_library(krn_connector_market STATIC
  ${KRN_COMMON_MARKET_SOURCES} ${KRN_COMMON_MARKET_HEADERS})
target_include_directories(krn_connector_market PUBLIC "${_kcm_root}/include")
target_link_libraries(krn_connector_market PUBLIC krn::krn_engine)

# Every file under connectors/market is in exactly one of the two lists, so an
# imported file the build skips is a configure error, not a silent gap.
file(GLOB_RECURSE _kcm_on_disk LIST_DIRECTORIES false "${_kcm_root}/*")
set(_kcm_listed ${KRN_COMMON_MARKET_SOURCES} ${KRN_COMMON_MARKET_HEADERS})
list(SORT _kcm_on_disk)
list(SORT _kcm_listed)
if (NOT _kcm_on_disk STREQUAL _kcm_listed)
  message(FATAL_ERROR
    "krn-common: connectors/market on disk [${_kcm_on_disk}] differs from "
    "krn_connector_market's lists [${_kcm_listed}]")
endif()
list(LENGTH _kcm_listed _kcm_n)
message(STATUS "krn-common: krn_connector_market: ${_kcm_n} files, all listed")
