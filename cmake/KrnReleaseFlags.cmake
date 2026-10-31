# The engine's build flags on krn-common's own targets (ENC-1589), mirroring
# krn's CMakeLists.txt at KRN_COMMON_KRN_COMMIT target for target:
#   krn_connector_market  warnings, Release opt + arch flags, LTO
#   krn_server            warnings, LTO (krn gives it no arch flags)
#   krn_common_tests      warnings (krn's krn_market_tests)
#   bench_*               warnings
# Nothing is spelled here. The opt/arch flags are read from krn_engine's
# COMPILE_OPTIONS and LTO from its INTERPROCEDURAL_OPTIMIZATION, so they follow
# KRN_ARCH_BASELINE / KRN_NATIVE_ARCH and the build type exactly as the engine
# does. Included last from the root CMakeLists.txt, after every target exists.

if (NOT COMMAND krn_apply_warnings)
  message(FATAL_ERROR "krn-common: the engine no longer defines krn_apply_warnings()")
endif()
foreach (_krf_t IN ITEMS krn_connector_market krn_server krn_common_tests bench_atomic_functions)
  if (TARGET ${_krf_t})
    krn_apply_warnings(${_krf_t})
  endif()
endforeach()

# The engine's Release block adds exactly -O3 and its arch flags (or MSVC's
# /O2 /Ob2 /GL) to krn_engine; everything else there is warnings or
# sanitizers, which are not copied.
get_target_property(_krf_engine_opts krn_engine COMPILE_OPTIONS)
set(_krf_opts "")
foreach (_krf_o IN LISTS _krf_engine_opts)
  if (_krf_o MATCHES "^(-O[0-9s]|-march=.+|/O2|/Ob2|/GL)$")
    list(APPEND _krf_opts "${_krf_o}")
  endif()
endforeach()
if (_krf_opts)
  target_compile_options(krn_connector_market PRIVATE ${_krf_opts})
endif()

get_target_property(_krf_ipo krn_engine INTERPROCEDURAL_OPTIMIZATION)
if (_krf_ipo)
  set_property(TARGET krn_connector_market krn_server
    PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
else()
  set(_krf_ipo FALSE)
endif()
message(STATUS "krn-common: from krn_engine: opt/arch [${_krf_opts}] on "
  "krn_connector_market; LTO ${_krf_ipo} on krn_connector_market + krn_server")
