#include "Optimizer.hpp"
#include "Base64.hpp"
#include "Cl.hpp"

#include <chrono>
#include <iostream>

namespace DesktopWebview {
namespace Optimizer {

namespace {

// Elapsed wall time, in milliseconds, since a steady_clock start point.
double MillisSince(std::chrono::steady_clock::time_point start) {
  auto end = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(end - start).count();
}

} // namespace

Optimizer &Optimizer::instance() {
  static Optimizer s;
  return s;
}

bool Optimizer::ensureOpenCL() {
  // Bring up the one shared OpenCL context (see Cl). Idempotent: repeat calls
  // are recalled inside Cl::init().
  m_openclReady = Cl::init();
  return m_openclReady;
}

// Always report the shared module's live state so passes that bring the GPU
// online (e.g. the base64 kernel build) are reflected immediately.
bool Optimizer::openCLAvailable() const { return Cl::available(); }

OptimizeResult Optimizer::optimize(const std::string &name,
                                   const std::function<bool()> &work) {
  auto start = std::chrono::steady_clock::now();

  // Recall: if this pass already ran, return the cached result without
  // re-running `work`. The lookup itself is still timed so the recall cost is
  // visible (and provably near-zero).
  for (OptimizeResult &cached : m_history) {
    if (cached.name == name) {
      OptimizeResult r = cached;
      r.recalled = true;
      r.milliseconds = MillisSince(start);
      std::cerr << "[Optimizer] " << name << ": recalled in " << r.milliseconds
                << " ms (" << (r.usedGpu ? "GPU" : "CPU") << ")" << std::endl;
      return r;
    }
  }

  // First run: do the work and measure it.
  OptimizeResult r;
  r.name = name;
  r.usedGpu = openCLAvailable();
  r.ok = work ? work() : false;
  // The work may have brought the GPU online (e.g. a kernel build); re-read.
  r.usedGpu = openCLAvailable();
  r.recalled = false;
  r.milliseconds = MillisSince(start);

  m_history.push_back(r);
  std::cerr << "[Optimizer] " << name << ": " << (r.ok ? "optimized" : "FAILED")
            << " in " << r.milliseconds << " ms ("
            << (r.usedGpu ? "GPU" : "CPU") << ")" << std::endl;
  return r;
}

void Optimizer::forget(const std::string &name) {
  for (auto it = m_history.begin(); it != m_history.end(); ++it) {
    if (it->name == name) {
      m_history.erase(it);
      return;
    }
  }
}

void Optimizer::reset() {
  m_history.clear();
  m_openclReady = false;
}

const std::vector<OptimizeResult> &Optimizer::history() const {
  return m_history;
}

std::vector<OptimizeResult> optimizeAll() {
  Optimizer &opt = Optimizer::instance();

  std::cerr << "[Optimizer] applying OpenCL optimization to all logic..."
            << std::endl;
  auto wallStart = std::chrono::steady_clock::now();

  // Pass 1: bring up the shared OpenCL context (device probe).
  opt.optimize("opencl-context", [&opt]() { return opt.ensureOpenCL(); });

  // Pass 2: the base64 GPU kernel build. This is the engine's existing OpenCL
  // logic; optimizing it here compiles/caches the kernel up front so the first
  // real decode does not pay the build cost. Falls back to CPU cleanly.
  opt.optimize("base64-gpu", []() {
    Base64::initOpenCL();
    return true; // success either way: GPU when available, CPU otherwise.
  });

  double totalMs = std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - wallStart)
                       .count();
  std::cerr << "[Optimizer] all passes done in " << totalMs << " ms; GPU "
            << (opt.openCLAvailable() ? "active" : "unavailable (CPU fallback)")
            << std::endl;

  return opt.history();
}

} // namespace Optimizer
} // namespace DesktopWebview
