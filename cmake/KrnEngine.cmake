# Fetch the engine at KRN_COMMON_KRN_COMMIT and assert what was fetched
# (purity SPEC D5). Included from the root CMakeLists.txt, which sets
# KRN_COMMON_KRN_REPOSITORY, KRN_COMMON_KRN_COMMIT and KRN_COMMON_KRN_TARGETS.
#
# Unmerged engine work: -DFETCHCONTENT_SOURCE_DIR_KRN=<krn checkout> builds
# against that tree instead of the pin. Nothing is written into it.

if (NOT KRN_COMMON_KRN_COMMIT MATCHES "^[0-9a-f]{40}$")
  message(FATAL_ERROR
    "krn-common: KRN_COMMON_KRN_COMMIT must be a full 40-character commit, "
    "got '${KRN_COMMON_KRN_COMMIT}'")
endif()

# D5: a FetchContent base dir outside this build tree is shared state between
# worktrees, and a green build against another ticket's engine.
if (DEFINED FETCHCONTENT_BASE_DIR)
  file(REAL_PATH "${FETCHCONTENT_BASE_DIR}" _kc_base)
  file(REAL_PATH "${CMAKE_BINARY_DIR}" _kc_bin)
  string(FIND "${_kc_base}/" "${_kc_bin}/" _kc_pos)
  if (NOT _kc_pos EQUAL 0)
    message(FATAL_ERROR
      "krn-common: FETCHCONTENT_BASE_DIR=${FETCHCONTENT_BASE_DIR} is outside "
      "this build tree. Never share it across worktrees (SPEC D5).")
  endif()
endif()

include(FetchContent)
FetchContent_Declare(krn
  GIT_REPOSITORY "${KRN_COMMON_KRN_REPOSITORY}"
  GIT_TAG        "${KRN_COMMON_KRN_COMMIT}"
)
FetchContent_MakeAvailable(krn)

file(REAL_PATH "${krn_SOURCE_DIR}" _kc_src)
execute_process(COMMAND git -C "${_kc_src}" rev-parse HEAD
  OUTPUT_VARIABLE _kc_head OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE _kc_rc ERROR_QUIET)

if (FETCHCONTENT_SOURCE_DIR_KRN)
  # The pin is not in force; say so, and say what is.
  execute_process(COMMAND git -C "${_kc_src}" status --porcelain
    OUTPUT_VARIABLE _kc_dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if (_kc_dirty)
    set(_kc_state "with uncommitted changes")
  else()
    set(_kc_state "clean")
  endif()
  message(WARNING
    "krn-common: engine OVERRIDDEN by FETCHCONTENT_SOURCE_DIR_KRN=${_kc_src} "
    "(HEAD '${_kc_head}', ${_kc_state}); the pin ${KRN_COMMON_KRN_COMMIT} is "
    "NOT what this build compiles.")
else()
  if (NOT _kc_rc EQUAL 0 OR NOT _kc_head STREQUAL KRN_COMMON_KRN_COMMIT)
    message(FATAL_ERROR
      "krn-common: fetched engine at ${_kc_src} is at '${_kc_head}', "
      "not the pin ${KRN_COMMON_KRN_COMMIT}")
  endif()
  message(STATUS "krn-common: engine pinned at ${KRN_COMMON_KRN_COMMIT} "
    "(fetched HEAD ${_kc_head})")
endif()

# ---- The engine defines exactly its library targets (krn's consumed-mode
# guard, ENC-1553), checked from here as the engine's own consumer smoke does.
set(_kc_dirs "${krn_SOURCE_DIR}")
set(_kc_all "")
while (_kc_dirs)
  list(POP_FRONT _kc_dirs _kc_d)
  get_directory_property(_kc_t DIRECTORY "${_kc_d}" BUILDSYSTEM_TARGETS)
  get_directory_property(_kc_sub DIRECTORY "${_kc_d}" SUBDIRECTORIES)
  list(APPEND _kc_all ${_kc_t})
  list(APPEND _kc_dirs ${_kc_sub})
endwhile()
list(SORT _kc_all)
set(_kc_expected ${KRN_COMMON_KRN_TARGETS})
list(SORT _kc_expected)
if (NOT _kc_all STREQUAL _kc_expected)
  message(FATAL_ERROR
    "krn-common: the engine defines [${_kc_all}], expected exactly "
    "[${_kc_expected}] (KRN_COMMON_KRN_TARGETS)")
endif()
foreach (_kc_t IN ITEMS krn_tests krn_server krn_connector_market krn
                        krn_data krn_ingest krn_data_tests krn_benchmarks
                        krn_isolation_control)
  if (TARGET ${_kc_t})
    message(FATAL_ERROR "krn-common: the engine defined ${_kc_t} in consumed mode")
  endif()
endforeach()
message(STATUS "krn-common: engine targets are exactly [${_kc_all}]")

# ---- Capture assertion (SPEC D5 shape iii): every engine SOURCE sits under
# the fetched tree, never under this repo's src/ or include/.
foreach (_kc_lib IN LISTS KRN_COMMON_KRN_TARGETS)
  get_target_property(_kc_srcs ${_kc_lib} SOURCES)
  list(LENGTH _kc_srcs _kc_n)
  if (_kc_n EQUAL 0)
    message(FATAL_ERROR "krn-common: ${_kc_lib} has no SOURCES")
  endif()
  set(_kc_outside "")
  foreach (_kc_s IN LISTS _kc_srcs)
    file(REAL_PATH "${_kc_s}" _kc_rs BASE_DIRECTORY "${_kc_src}")
    string(FIND "${_kc_rs}" "${_kc_src}/" _kc_p)
    if (NOT _kc_p EQUAL 0)
      list(APPEND _kc_outside "${_kc_rs}")
    endif()
  endforeach()
  if (_kc_outside)
    message(FATAL_ERROR
      "krn-common: ${_kc_lib} SOURCES outside the engine tree ${_kc_src}: "
      "[${_kc_outside}]")
  endif()
  message(STATUS "krn-common: ${_kc_lib}: ${_kc_n} SOURCES, all under ${_kc_src}")
endforeach()
