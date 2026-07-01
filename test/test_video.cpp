#include "Image.hpp"
#include "Paint.hpp"
#include "Video.hpp"
#include <iostream>
#include <string>

using namespace DesktopWebview;

static int g_failures = 0;

static void Check(const std::string &label, bool condition) {
  if (condition) {
    std::cout << "  [PASS] " << label << std::endl;
  } else {
    std::cout << "  [FAIL] " << label << std::endl;
    ++g_failures;
  }
}

// Find the leftmost column containing the bar colour, or -1.
static int BarColumn(const Image::Bitmap &frame, Paint::Color bar) {
  for (int x = 0; x < frame.width; ++x) {
    if (frame.at(x, frame.height / 2) == bar) {
      return x;
    }
  }
  return -1;
}

int main() {
  std::cout << "=========================================================="
            << std::endl;
  std::cout << "Video frame source + timing" << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  Paint::Color bar{255, 140, 0, 255};
  Video::SyntheticVideoSource src(160, 90, 30.0, 60);

  Check("metadata: 160x90", src.width() == 160 && src.height() == 90);
  Check("metadata: 30fps, 60 frames",
        src.frameRate() == 30.0 && src.frameCount() == 60);

  // Timing: 30fps means t=1.0s -> frame 30, t=0 -> 0, beyond end clamps.
  Check("t=0 -> frame 0", Video::frameIndexForTime(src, 0.0) == 0);
  Check("t=1.0s -> frame 30", Video::frameIndexForTime(src, 1.0) == 30);
  Check("t=0.5s -> frame 15", Video::frameIndexForTime(src, 0.5) == 15);
  Check("t past end clamps to last frame",
        Video::frameIndexForTime(src, 100.0) == 59);
  Check("negative time clamps to 0", Video::frameIndexForTime(src, -5.0) == 0);

  // Frame content + motion.
  Image::Bitmap first, mid, last, past;
  Check("frame 0 renders", src.frameAt(0, first));
  Check("frame 0 size matches source",
        first.width == 160 && first.height == 90);
  Check("frame 30 renders", src.frameAt(30, mid));
  Check("last frame renders", src.frameAt(59, last));
  Check("frame past end fails", !src.frameAt(60, past));

  int x0 = BarColumn(first, bar);
  int x30 = BarColumn(mid, bar);
  int x59 = BarColumn(last, bar);
  Check("bar present in frames", x0 >= 0 && x30 >= 0 && x59 >= 0);
  Check("bar starts at left edge", x0 == 0);
  Check("bar moves right over time", x0 < x30 && x30 < x59);

  // Background present away from the bar.
  Check("background between source and bar end",
        first.at(159, 45) == Paint::Color({20, 20, 30, 255}));

  std::cout << "\n=========================================================="
            << std::endl;
  if (g_failures == 0) {
    std::cout << "All video tests passed." << std::endl;
  } else {
    std::cout << g_failures << " video test(s) failed." << std::endl;
  }
  std::cout << "=========================================================="
            << std::endl;
  return g_failures == 0 ? 0 : 1;
}
