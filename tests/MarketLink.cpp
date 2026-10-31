// Link probe for krn_connector_market (ENC-1565). Like ScaffoldLink.cpp it
// defines no TEST, so it adds nothing to any case count; the imported market
// tests arrive with ENC-1566. Its external-linkage function references the
// connector, the ITCH adapter and the ob/ key parser, so the linker must
// resolve them from krn_connector_market and, through it, krn_engine.
#include "krn/feed/ItchAdapter.hpp"
#include "krn/market/MarketConnector.hpp"
#include "krn/ob/ObKey.hpp"

int krn_common_market_link_probe() {
  krn::market::MarketConnector connector;
  krn::feed::ItchAdapter adapter;
  auto events = adapter.translate("{}");
  auto key = krn::ob::parseObKey("ob.spread");
  return static_cast<int>(events.size()) + (key ? 1 : 0);
}
