// krn_common_tests carries no test cases yet; the imported market tests
// (ENC-1566) arrive here. This file keeps the binary honest meanwhile: it
// compiles an engine header and, through an external-linkage function the
// linker must resolve, links krn_engine. It defines no TEST, so it adds
// nothing to any case count.
#include <krn/rt/ThreadPool.hpp>

int krn_common_scaffold_link_probe() {
  krn::rt::ThreadPool pool(1);
  pool.drain();
  pool.shutdown();
  return 0;
}
