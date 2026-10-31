# krn_server [wsPort] [conf] [feedPort]: krn_host + the market connector
# (purity SPEC D6). src/main.cpp was imported verbatim from krn at ca544ec
# (D17); path mapping: identity. conf/krn.conf is krn's src/util/krn.conf at
# that commit.
# Included from the root CMakeLists.txt after KrnConnectorMarket.cmake.

add_executable(krn_server "${PROJECT_SOURCE_DIR}/src/main.cpp")
target_link_libraries(krn_server PRIVATE
  krn::krn_host krn_connector_market krn::krn_engine)

# Every file under src/ is built, so an imported file the build skips is a
# configure error, not a silent gap.
file(GLOB_RECURSE _ks_on_disk LIST_DIRECTORIES false "${PROJECT_SOURCE_DIR}/src/*")
if (NOT _ks_on_disk STREQUAL "${PROJECT_SOURCE_DIR}/src/main.cpp")
  message(FATAL_ERROR
    "krn-common: src/ on disk [${_ks_on_disk}] differs from krn_server's [src/main.cpp]")
endif()
if (NOT EXISTS "${PROJECT_SOURCE_DIR}/conf/krn.conf")
  message(FATAL_ERROR "krn-common: conf/krn.conf not found")
endif()
message(STATUS "krn-common: krn_server from src/main.cpp")
