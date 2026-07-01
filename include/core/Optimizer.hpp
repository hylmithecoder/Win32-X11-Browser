#ifndef OPTIMIZER_HPP
#define OPTIMIZER_HPP

#include <functional>
#include <string>
#include <vector>

namespace DesktopWebview {
namespace Optimizer {

// The outcome of a single optimization pass. One of these is produced (and
// printed) every time `optimize` runs, whether the work executed or was
// recalled from cache.
struct OptimizeResult {
  std::string name;          // task identifier (the cache key)
  bool ok = false;           // did the work succeed?
  bool usedGpu = false;      // true if the OpenCL/GPU path was active
  bool recalled = false;     // true if served from cache without re-running
  double milliseconds = 0.0; // wall-clock time of this pass
};

// A recallable OpenCL optimizer.
//
// "Recallable" means each pass is keyed by name and cached: the first call runs
// the work and measures it; later calls with the same name are *recalled*
// instantly from the cache (recalled=true) instead of recomputing. Every pass,
// recalled or not, is timed and the elapsed time is reported, so the cost of
// optimizing (and the win from recalling) is always visible.
class Optimizer {
public:
  // Process-wide shared instance.
  static Optimizer &instance();

  // Lazily bring up the shared OpenCL context. Returns true if a usable GPU (or
  // CPU-OpenCL) device was found. Safe to call repeatedly: the first call does
  // the work, the rest are recalled. A no-op (returns false) when the build was
  // compiled without OpenCL support.
  bool ensureOpenCL();

  // True once `ensureOpenCL` has found an active OpenCL device.
  bool openCLAvailable() const;

  // Run, or recall, an optimization pass identified by `name`. `work` performs
  // the actual optimization and returns success. On a repeat call with the same
  // name, `work` is NOT re-run -- the cached result is returned with
  // recalled=true. The pass is timed either way and a one-line summary is
  // printed to stderr.
  OptimizeResult optimize(const std::string &name,
                          const std::function<bool()> &work);

  // Drop a cached pass so the next `optimize(name, ...)` re-runs the work.
  void forget(const std::string &name);

  // Clear every cached pass and the history.
  void reset();

  // Most recent result recorded for each name, in insertion order.
  const std::vector<OptimizeResult> &history() const;

private:
  Optimizer() = default;
  Optimizer(const Optimizer &) = delete;
  Optimizer &operator=(const Optimizer &) = delete;

  std::vector<OptimizeResult> m_history;
  bool m_openclReady = false;
};

// Apply the optimizer to every piece of OpenCL-backed logic in the engine and
// print a timed summary. "For now" this drives the GPU base64 kernel build and
// the shared OpenCL context bring-up; new GPU paths register their pass here.
// Returns the per-pass results. Calling it again recalls the cached passes.
std::vector<OptimizeResult> optimizeAll();

} // namespace Optimizer
} // namespace DesktopWebview

#endif // OPTIMIZER_HPP
