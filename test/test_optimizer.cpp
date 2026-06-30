#include "../include/Optimizer.hpp"

#include <cassert>
#include <cstdio>

int main() {
  using namespace DesktopWebview::Optimizer;

  Optimizer &opt = Optimizer::instance();
  opt.reset();

  // ---- first run executes the work ----------------------------------------
  {
    int runs = 0;
    OptimizeResult r = opt.optimize("task-a", [&runs]() {
      ++runs;
      return true;
    });
    assert(runs == 1);
    assert(r.ok);
    assert(!r.recalled);
    assert(r.name == "task-a");
    std::printf("PASS first run executes work (%.4f ms)\n", r.milliseconds);
  }

  // ---- second run is recalled, work NOT re-run ----------------------------
  {
    int runs = 0;
    OptimizeResult r = opt.optimize("task-a", [&runs]() {
      ++runs;
      return true;
    });
    assert(runs == 0); // recalled: the lambda must not have run
    assert(r.recalled);
    assert(r.ok);
    std::printf("PASS recall skips work (%.4f ms)\n", r.milliseconds);
  }

  // ---- forget makes the next call re-run ----------------------------------
  {
    opt.forget("task-a");
    int runs = 0;
    OptimizeResult r = opt.optimize("task-a", [&runs]() {
      ++runs;
      return true;
    });
    assert(runs == 1);
    assert(!r.recalled);
    std::printf("PASS forget forces re-run\n");
  }

  // ---- a failing pass is recorded as not-ok -------------------------------
  {
    OptimizeResult r = opt.optimize("task-fail", []() { return false; });
    assert(!r.ok);
    assert(!r.recalled);
    std::printf("PASS failing pass recorded\n");
  }

  // ---- history holds one entry per distinct name --------------------------
  {
    const auto &h = opt.history();
    assert(h.size() == 2); // task-a, task-fail
    std::printf("PASS history has %zu passes\n", h.size());
  }

  // ---- optimizeAll drives the real OpenCL logic and times it --------------
  {
    opt.reset();
    auto results = optimizeAll();
    assert(results.size() == 2); // opencl-context, base64-gpu
    for (const auto &r : results) {
      assert(r.milliseconds >= 0.0);
    }
    std::printf("PASS optimizeAll ran %zu passes (GPU: %s)\n", results.size(),
                opt.openCLAvailable() ? "yes" : "no");
  }

  std::printf("\nAll Optimizer tests passed.\n");
  return 0;
}
