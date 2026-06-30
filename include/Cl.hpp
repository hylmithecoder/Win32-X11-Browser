#ifndef CL_HPP
#define CL_HPP

#include <string>

// The single place the engine touches OpenCL. Every compute path (base64 today;
// image scaling, paint compositing, video colour-convert next) asks this module
// for the one shared context/queue and its kernels, so the platform, device,
// context and queue are created exactly once and the GPU is reused across the
// whole program. Built without OpenCL (or when no device is found at runtime),
// available() returns false and callers use their CPU fallback.

#ifdef DWV_HAVE_OPENCL
#define CL_TARGET_OPENCL_VERSION 120
#ifdef __APPLE__
#include <OpenCL/opencl.h>
#else
#include <CL/cl.h>
#endif
#endif

namespace DesktopWebview {
namespace Cl {

// Bring up the shared context (idempotent, lazy). Returns available(). A GPU
// device is preferred over any other; only if no GPU exists is a CPU/other
// device used.
bool init();

// True once a usable OpenCL device is active.
bool available();

// True if the active device reports CL_DEVICE_TYPE_GPU.
bool isGpu();

// Name of the active device ("" when unavailable).
std::string deviceName();

#ifdef DWV_HAVE_OPENCL
// Shared handles. Valid only when available(); do not release them.
cl_context context();
cl_command_queue queue();
cl_device_id device();

// Build the program at `clPath` (cached per path) and return its `kernelName`
// kernel (cached per path + name). Returns nullptr on failure. This is the
// "recall": the first call compiles the kernel, later calls reuse it instantly.
cl_kernel kernel(const std::string &clPath, const std::string &kernelName);
#endif

} // namespace Cl
} // namespace DesktopWebview

#endif // CL_HPP
