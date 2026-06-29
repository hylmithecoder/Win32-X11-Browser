#include "../include/Base64.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

// ---- OpenCL (optional, compile-time gated) ----------------------------------
#ifdef DWV_HAVE_OPENCL
#define CL_TARGET_OPENCL_VERSION 120
#ifdef __APPLE__
#include <OpenCL/opencl.h>
#else
#include <CL/cl.h>
#endif

namespace {

struct ClState {
  cl_platform_id platform = nullptr;
  cl_device_id device = nullptr;
  cl_context context = nullptr;
  cl_command_queue queue = nullptr;
  cl_program program = nullptr;
  cl_kernel decodeKernel = nullptr;
  cl_kernel encodeKernel = nullptr;
  bool initialised = false;
};

ClState &cl() {
  static ClState s;
  return s;
}

// Read the kernel source file from disk. Searches assets/kernels/ relative
// to common working-directory layouts.
std::string ReadKernelSource() {
  const char *candidates[] = {
      "assets/kernels/base64.cl",
      "../assets/kernels/base64.cl",
      "../../assets/kernels/base64.cl",
  };
  for (const char *path : candidates) {
    std::ifstream f(path, std::ios::binary);
    if (f) {
      return std::string((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    }
  }
  return "";
}

bool BuildClProgram(ClState &s, const std::string &source) {
  const char *src = source.c_str();
  size_t len = source.size();
  cl_int err;
  s.program = clCreateProgramWithSource(s.context, 1, &src, &len, &err);
  if (err != CL_SUCCESS) {
    return false;
  }
  err = clBuildProgram(s.program, 1, &s.device, nullptr, nullptr, nullptr);
  if (err != CL_SUCCESS) {
    size_t logSize = 0;
    clGetProgramBuildInfo(s.program, s.device, CL_PROGRAM_BUILD_LOG, 0, nullptr,
                          &logSize);
    if (logSize > 1) {
      std::vector<char> log(logSize);
      clGetProgramBuildInfo(s.program, s.device, CL_PROGRAM_BUILD_LOG, logSize,
                            log.data(), nullptr);
      std::cerr << "[OpenCL] Build log: " << log.data() << std::endl;
    }
    clReleaseProgram(s.program);
    s.program = nullptr;
    return false;
  }
  s.decodeKernel = clCreateKernel(s.program, "base64_decode", &err);
  if (err != CL_SUCCESS) {
    return false;
  }
  s.encodeKernel = clCreateKernel(s.program, "base64_encode", &err);
  if (err != CL_SUCCESS) {
    return false;
  }
  return true;
}

} // namespace
#endif // DWV_HAVE_OPENCL

// ---- CPU decode lookup table -----------------------------------------------

namespace {

const std::uint8_t kDecodeTable[256] = {
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, // 0-15
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, // 16-31
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 64, 64, 63, // 32-47
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 64, 64, 64, 0,  64, 64, // 48-63
    64, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, // 64-79
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 64, 64, 64, 64, 64, // 80-95
    64, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, // 96-111
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 64, 64, 64, 64, 64, // 112-127
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
};

const char kEncodeTable[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Strip whitespace from a base64 string (returns a new string).
std::string StripWhitespace(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f') {
      out.push_back(c);
    }
  }
  return out;
}
// CPU decode path.
void CpuDecode(const std::uint8_t *src, std::size_t srcLen, std::uint8_t *dst,
               std::size_t dstLen) {
  std::size_t si = 0, di = 0;
  while (si + 4 <= srcLen && di < dstLen) {
    std::uint8_t c0 = kDecodeTable[src[si]];
    std::uint8_t c1 = kDecodeTable[src[si + 1]];
    std::uint8_t c2 = kDecodeTable[src[si + 2]];
    std::uint8_t c3 = kDecodeTable[src[si + 3]];
    if ((c0 | c1 | c2 | c3) & 0x40) {
      break; // invalid character
    }
    std::uint32_t val = (static_cast<std::uint32_t>(c0) << 18) |
                        (static_cast<std::uint32_t>(c1) << 12) |
                        (static_cast<std::uint32_t>(c2) << 6) |
                        static_cast<std::uint32_t>(c3);
    if (di < dstLen) {
      dst[di++] = static_cast<std::uint8_t>(val >> 16);
    }
    if (di < dstLen) {
      dst[di++] = static_cast<std::uint8_t>(val >> 8);
    }
    if (di < dstLen) {
      dst[di++] = static_cast<std::uint8_t>(val);
    }
    si += 4;
  }
}

// CPU encode path.
void CpuEncode(const std::uint8_t *src, std::size_t srcLen, std::string &out) {
  out.clear();
  out.reserve(((srcLen + 2) / 3) * 4);
  std::size_t i = 0;
  while (i < srcLen) {
    std::uint32_t val = static_cast<std::uint32_t>(src[i]) << 16;
    if (i + 1 < srcLen) {
      val |= static_cast<std::uint32_t>(src[i + 1]) << 8;
    }
    if (i + 2 < srcLen) {
      val |= static_cast<std::uint32_t>(src[i + 2]);
    }
    out.push_back(kEncodeTable[(val >> 18) & 0x3F]);
    out.push_back(kEncodeTable[(val >> 12) & 0x3F]);
    out.push_back((i + 1 < srcLen) ? kEncodeTable[(val >> 6) & 0x3F] : '=');
    out.push_back((i + 2 < srcLen) ? kEncodeTable[val & 0x3F] : '=');
    i += 3;
  }
}

// Threshold: buffers smaller than this skip the GPU to avoid transfer overhead.
constexpr std::size_t kGpuThreshold = 4096;

} // namespace

namespace DesktopWebview {
namespace Base64 {

bool initOpenCL() {
#ifdef DWV_HAVE_OPENCL
  ClState &s = cl();
  if (s.initialised) {
    return true;
  }
  cl_int err;

  err = clGetPlatformIDs(1, &s.platform, nullptr);
  if (err != CL_SUCCESS || !s.platform) {
    std::cerr << "[OpenCL] No platform found" << std::endl;
    return false;
  }

  err = clGetDeviceIDs(s.platform, CL_DEVICE_TYPE_GPU, 1, &s.device, nullptr);
  if (err != CL_SUCCESS || !s.device) {
    // Try any device (including CPU OpenCL) as a fallback.
    err = clGetDeviceIDs(s.platform, CL_DEVICE_TYPE_ALL, 1, &s.device, nullptr);
    if (err != CL_SUCCESS || !s.device) {
      std::cerr << "[OpenCL] No device found" << std::endl;
      return false;
    }
  }

  char devName[128] = {};
  clGetDeviceInfo(s.device, CL_DEVICE_NAME, sizeof(devName), devName, nullptr);
  std::cerr << "[OpenCL] Using device: " << devName << std::endl;

  s.context = clCreateContext(nullptr, 1, &s.device, nullptr, nullptr, &err);
  if (err != CL_SUCCESS) {
    return false;
  }

  s.queue = clCreateCommandQueue(s.context, s.device, 0, &err);
  if (err != CL_SUCCESS) {
    return false;
  }

  std::string source = ReadKernelSource();
  if (source.empty()) {
    std::cerr << "[OpenCL] Kernel source not found" << std::endl;
    return false;
  }

  if (!BuildClProgram(s, source)) {
    std::cerr << "[OpenCL] Program build failed" << std::endl;
    return false;
  }

  s.initialised = true;
  std::cerr << "[OpenCL] Base64 GPU acceleration enabled" << std::endl;
  return true;
#else
  return false;
#endif
}

bool isOpenCLAvailable() {
#ifdef DWV_HAVE_OPENCL
  return cl().initialised;
#else
  return false;
#endif
}

bool decode(const std::string &input, std::vector<std::uint8_t> &out) {
  std::string stripped = StripWhitespace(input);
  if (stripped.empty()) {
    out.clear();
    return false;
  }

  // Count trailing '=' padding.
  std::size_t padCount = 0;
  for (std::size_t i = stripped.size(); i > 0 && stripped[i - 1] == '='; --i) {
    ++padCount;
  }

  // The stripped string (with '=' still present) must be a multiple of 4.
  std::size_t srcLen = stripped.size();
  if (srcLen == 0 || srcLen % 4 != 0) {
    out.clear();
    return false;
  }

  // Output: every 4 base64 chars -> 3 bytes, minus padding bytes.
  std::size_t outLen = (srcLen / 4) * 3;
  if (padCount > 0 && outLen >= padCount) {
    outLen -= padCount;
  }

  out.resize(outLen, 0);

#ifdef DWV_HAVE_OPENCL
  ClState &s = cl();
  if (s.initialised && srcLen >= kGpuThreshold) {
    cl_int err;
    cl_mem d_src =
        clCreateBuffer(s.context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                       srcLen, (void *)stripped.data(), &err);
    cl_mem d_dst =
        clCreateBuffer(s.context, CL_MEM_WRITE_ONLY, outLen, nullptr, &err);
    if (err == CL_SUCCESS) {
      cl_uint uiSrcLen = static_cast<cl_uint>(srcLen);
      cl_uint uiDstLen = static_cast<cl_uint>(outLen);
      clSetKernelArg(s.decodeKernel, 0, sizeof(cl_mem), &d_src);
      clSetKernelArg(s.decodeKernel, 1, sizeof(cl_uint), &uiSrcLen);
      clSetKernelArg(s.decodeKernel, 2, sizeof(cl_mem), &d_dst);
      clSetKernelArg(s.decodeKernel, 3, sizeof(cl_uint), &uiDstLen);
      size_t globalSize = srcLen / 4;
      err = clEnqueueNDRangeKernel(s.queue, s.decodeKernel, 1, nullptr,
                                   &globalSize, nullptr, 0, nullptr, nullptr);
      if (err == CL_SUCCESS) {
        clEnqueueReadBuffer(s.queue, d_dst, CL_TRUE, 0, outLen, out.data(), 0,
                            nullptr, nullptr);
        clReleaseMemObject(d_src);
        clReleaseMemObject(d_dst);
        return !out.empty();
      }
      clReleaseMemObject(d_src);
      clReleaseMemObject(d_dst);
      // GPU path failed, fall through to CPU.
    }
  }
#endif

  // CPU fallback.
  CpuDecode(reinterpret_cast<const std::uint8_t *>(stripped.data()), srcLen,
            out.data(), outLen);
  return !out.empty();
}

std::vector<std::uint8_t> decode(const std::string &input) {
  std::vector<std::uint8_t> out;
  decode(input, out);
  return out;
}

std::string encode(const std::uint8_t *data, std::size_t len) {
  if (!data || len == 0) {
    return "";
  }

#ifdef DWV_HAVE_OPENCL
  ClState &s = cl();
  if (s.initialised && len >= kGpuThreshold) {
    std::size_t outLen = ((len + 2) / 3) * 4;
    std::string result(outLen, '\0');
    cl_int err;
    cl_mem d_src =
        clCreateBuffer(s.context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, len,
                       (void *)data, &err);
    cl_mem d_dst =
        clCreateBuffer(s.context, CL_MEM_WRITE_ONLY | CL_MEM_COPY_HOST_PTR,
                       outLen, (void *)result.data(), &err);
    if (err == CL_SUCCESS) {
      cl_uint uiSrcLen = static_cast<cl_uint>(len);
      cl_uint uiDstLen = static_cast<cl_uint>(outLen);
      clSetKernelArg(s.encodeKernel, 0, sizeof(cl_mem), &d_src);
      clSetKernelArg(s.encodeKernel, 1, sizeof(cl_uint), &uiSrcLen);
      clSetKernelArg(s.encodeKernel, 2, sizeof(cl_mem), &d_dst);
      clSetKernelArg(s.encodeKernel, 3, sizeof(cl_uint), &uiDstLen);
      size_t globalSize = (len + 2) / 3;
      err = clEnqueueNDRangeKernel(s.queue, s.encodeKernel, 1, nullptr,
                                   &globalSize, nullptr, 0, nullptr, nullptr);
      if (err == CL_SUCCESS) {
        clEnqueueReadBuffer(s.queue, d_dst, CL_TRUE, 0, outLen,
                            (void *)result.data(), 0, nullptr, nullptr);
        clReleaseMemObject(d_src);
        clReleaseMemObject(d_dst);
        return result;
      }
      clReleaseMemObject(d_src);
      clReleaseMemObject(d_dst);
    }
  }
#endif

  std::string result;
  CpuEncode(data, len, result);
  return result;
}

std::string encode(const std::vector<std::uint8_t> &input) {
  return encode(input.data(), input.size());
}

} // namespace Base64
} // namespace DesktopWebview
