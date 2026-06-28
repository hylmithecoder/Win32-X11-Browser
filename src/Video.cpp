#include "../include/Video.hpp"

#include <algorithm>
#include <cmath>

namespace DesktopWebview {
namespace Video {

int frameIndexForTime(const VideoSource &src, double seconds) {
  if (seconds < 0) {
    seconds = 0;
  }
  double fps = src.frameRate();
  if (fps <= 0) {
    return 0;
  }
  int index = static_cast<int>(std::floor(seconds * fps));
  int count = src.frameCount();
  if (count >= 0 && index >= count) {
    index = count - 1;
  }
  return index < 0 ? 0 : index;
}

SyntheticVideoSource::SyntheticVideoSource(int width, int height,
                                           double frameRate, int frameCount,
                                           Paint::Color background,
                                           Paint::Color bar)
    : m_width(width), m_height(height), m_frameRate(frameRate),
      m_frameCount(frameCount), m_background(background), m_bar(bar) {}

bool SyntheticVideoSource::frameAt(int index, Image::Bitmap &out) {
  if (index < 0 || (m_frameCount >= 0 && index >= m_frameCount)) {
    return false;
  }
  if (m_width <= 0 || m_height <= 0) {
    return false;
  }

  out.width = m_width;
  out.height = m_height;
  out.pixels.assign(static_cast<size_t>(m_width) * m_height, m_background);

  // A bar of ~1/8 the width sweeps left to right over the playable frames.
  int barWidth = std::max(1, m_width / 8);
  int travel = m_width - barWidth;
  int denom = std::max(1, (m_frameCount > 1 ? m_frameCount - 1 : 1));
  int barX = (m_frameCount > 1) ? (travel * index) / denom : 0;

  for (int y = 0; y < m_height; ++y) {
    for (int x = barX; x < barX + barWidth && x < m_width; ++x) {
      out.set(x, y, m_bar);
    }
  }
  return true;
}

} // namespace Video
} // namespace DesktopWebview
