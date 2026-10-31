# krn-common

The shared book/event machinery and the live market plugin, built against the
`krn` engine (`Dynacharting/krn`). Design record: the workspace's
`specs/2026-10-15-gma-engine-purity/SPEC.md` (D2, D5, D8, D17). Default branch:
`main`. `LICENSE` is the repo's first commit and is never edited.

## Build and test

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Out of tree works the same (`cmake -S . -B <dir> …`). Always pass
`-DCMAKE_BUILD_TYPE` (an empty one defaults to Release, top-level and
single-config). In Release, `cmake/KrnReleaseFlags.cmake` gives this repo's
targets the engine's flags as krn gives its own, read from `krn_engine`:
`krn_connector_market` its `-O3`/`-march` and LTO, `krn_server` LTO, and all
of them the engine's warnings. `KRN_COMMON_BUILD_TESTS` (default `ON`) builds
`krn_common_tests` and registers `krn_port_conflict` and
`krn_feed_port_override` against `krn_server`; with it `OFF`, ctest finds no
tests and still exits 0. `KRN_COMMON_BUILD_BENCHMARKS` (default `OFF`) fetches
Google Benchmark and builds `bench_atomic_functions`.

`krn_server [wsPort] [conf] [feedPort]` is always built; `conf/krn.conf` is its
default config.

Options are spelled `KRN_COMMON_*`. Never pass the engine's own `KRN_*`
options here: in consumed mode the engine ignores them, and a configure log line
`not used by the project` means a misspelled option, not a setting.

## The engine pin

`CMakeLists.txt` pins the engine by `FetchContent` at a full 40-character commit
of `krn` master (`KRN_COMMON_KRN_COMMIT`). Configure fails unless:

- the fetched tree's `HEAD` equals the pin (the log prints both);
- the engine defines exactly `KRN_COMMON_KRN_TARGETS`, and none of
  `krn_tests`, `krn_server`, `krn_connector_market` (its consumed-mode guard);
- every `SOURCE` of those targets sits under the fetched tree, never under this
  repo (the capture defect in SPEC D5).

**Bumping the pin** is one diff: the new SHA in `KRN_COMMON_KRN_COMMIT`, plus
`KRN_COMMON_KRN_TARGETS` if the engine's consumed target set changed at that
commit. Use a commit on `krn` master, never a branch name or a tag.

**Unmerged engine work:** `-DFETCHCONTENT_SOURCE_DIR_KRN=<krn checkout>` builds
against that tree instead of the pin; configure warns, naming its `HEAD` and
whether it is dirty. It is a cache variable, so drop it with
`-UFETCHCONTENT_SOURCE_DIR_KRN` (or a fresh build dir) before trusting a result.

**Never share `FETCHCONTENT_BASE_DIR` across worktrees** (SPEC D5); configure
refuses one outside the build tree.

## Importing code from the engine (SPEC D17)

Code arrives verbatim from a named `krn` master commit, recorded in the commit
message. The only permitted edits are build glue (this repo's CMake and include
roots), listed in the ticket. No rename, reformat or fix in the import commit,
and no history graft (`git subtree`, `filter-repo`, a merge of the engine's
history): plain files on top of the `LICENSE` root.
