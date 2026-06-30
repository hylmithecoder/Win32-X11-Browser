#include "../include/Cl.hpp"

#ifdef DWV_HAVE_OPENCL

#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>

namespace DesktopWebview {
namespace Cl {
namespace {

struct State {
  bool tried = false;
  bool ready = false;
  bool gpu = false;
  std::string name;
  cl_platform_id platform = nullptr;
  cl_device_id device = nullptr;
  cl_context context = nullptr;
  cl_command_queue queue = nullptr;
  std::map<std::string, cl_program> programs; // keyed by source path
  std::map<std::string, cl_kernel> kernels;   // keyed by "path|kernel"
};

State &state() {
  static State s;
  return s;
}

std::string ReadFile(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return "";
  }
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Choose the device to offload to: a GPU on any platform if one exists,
// otherwise the first available device (e.g. a CPU-OpenCL runtime).
bool PickDevice(cl_platform_id &outPlat, cl_device_id &outDev, bool &outGpu) {
  cl_uint numPlat = 0;
  if (clGetPlatformIDs(0, nullptr, &numPlat) != CL_SUCCESS || numPlat == 0) {
    return false;
  }
  std::vector<cl_platform_id> plats(numPlat);
  clGetPlatformIDs(numPlat, plats.data(), nullptr);
  for (cl_platform_id p : plats) {
    cl_device_id d = nullptr;
    if (clGetDeviceIDs(p, CL_DEVICE_TYPE_GPU, 1, &d, nullptr) == CL_SUCCESS &&
        d) {
      outPlat = p;
      outDev = d;
      outGpu = true;
      return true;
    }
  }
  for (cl_platform_id p : plats) {
    cl_device_id d = nullptr;
    if (clGetDeviceIDs(p, CL_DEVICE_TYPE_ALL, 1, &d, nullptr) == CL_SUCCESS &&
        d) {
      outPlat = p;
      outDev = d;
      outGpu = false;
      return true;
    }
  }
  return false;
}

} // namespace

bool init() {
  State &s = state();
  if (s.tried) {
    return s.ready;
  }
  s.tried = true;

  if (!PickDevice(s.platform, s.device, s.gpu)) {
    std::cerr << "[Cl] No OpenCL platform/device found (CPU fallback)"
              << std::endl;
    return false;
  }

  char name[256] = {0};
  clGetDeviceInfo(s.device, CL_DEVICE_NAME, sizeof(name), name, nullptr);
  s.name = name;

  cl_int err = CL_SUCCESS;
  s.context = clCreateContext(nullptr, 1, &s.device, nullptr, nullptr, &err);
  if (err != CL_SUCCESS || !s.context) {
    std::cerr << "[Cl] Failed to create context" << std::endl;
    return false;
  }
  s.queue = clCreateCommandQueue(s.context, s.device, 0, &err);
  if (err != CL_SUCCESS || !s.queue) {
    std::cerr << "[Cl] Failed to create command queue" << std::endl;
    clReleaseContext(s.context);
    s.context = nullptr;
    return false;
  }

  s.ready = true;
  std::cerr << "[Cl] OpenCL acceleration enabled on " << s.name << " ("
            << (s.gpu ? "GPU" : "non-GPU device") << ")" << std::endl;
  return true;
}

bool available() { return state().ready; }
bool isGpu() { return state().ready && state().gpu; }
std::string deviceName() { return state().name; }

cl_context context() { return state().context; }
cl_command_queue queue() { return state().queue; }
cl_device_id device() { return state().device; }

cl_kernel kernel(const std::string &clPath, const std::string &kernelName) {
  State &s = state();
  if (!s.ready && !init()) {
    return nullptr;
  }

  std::string kkey = clPath + "|" + kernelName;
  auto kit = s.kernels.find(kkey);
  if (kit != s.kernels.end()) {
    return kit->second; // recall: kernel already built
  }

  // Build (or recall) the program for this source path.
  cl_program prog = nullptr;
  auto pit = s.programs.find(clPath);
  if (pit != s.programs.end()) {
    prog = pit->second;
  } else {
    std::string src = ReadFile(clPath);
    if (src.empty()) {
      std::cerr << "[Cl] kernel source not found: " << clPath << std::endl;
      return nullptr;
    }
    const char *csrc = src.c_str();
    size_t len = src.size();
    cl_int err = CL_SUCCESS;
    prog = clCreateProgramWithSource(s.context, 1, &csrc, &len, &err);
    if (err != CL_SUCCESS) {
      std::cerr << "[Cl] clCreateProgramWithSource failed for " << clPath
                << std::endl;
      return nullptr;
    }
    if (clBuildProgram(prog, 1, &s.device, nullptr, nullptr, nullptr) !=
        CL_SUCCESS) {
      size_t logSize = 0;
      clGetProgramBuildInfo(prog, s.device, CL_PROGRAM_BUILD_LOG, 0, nullptr,
                            &logSize);
      std::vector<char> log(logSize + 1, '\0');
      clGetProgramBuildInfo(prog, s.device, CL_PROGRAM_BUILD_LOG, logSize,
                            log.data(), nullptr);
      std::cerr << "[Cl] build failed for " << clPath << ": " << log.data()
                << std::endl;
      clReleaseProgram(prog);
      return nullptr;
    }
    s.programs[clPath] = prog;
  }

  cl_int err = CL_SUCCESS;
  cl_kernel k = clCreateKernel(prog, kernelName.c_str(), &err);
  if (err != CL_SUCCESS) {
    std::cerr << "[Cl] kernel '" << kernelName << "' not found in " << clPath
              << std::endl;
    return nullptr;
  }
  s.kernels[kkey] = k;
  return k;
}

} // namespace Cl
} // namespace DesktopWebview

#else // !DWV_HAVE_OPENCL

namespace DesktopWebview {
namespace Cl {
bool init() { return false; }
bool available() { return false; }
bool isGpu() { return false; }
std::string deviceName() { return ""; }
} // namespace Cl
} // namespace DesktopWebview

#endif // DWV_HAVE_OPENCL
