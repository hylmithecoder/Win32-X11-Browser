#include "../include/Video.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <cstring>

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

FileVideoSource::FileVideoSource(const std::string &filePath) : m_filePath(filePath) {
  std::ifstream file(filePath, std::ios::binary);
  if (!file) {
    return;
  }
  m_fileData.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  
  if (m_fileData.size() < 24) {
    return;
  }

  if (m_fileData[0] != 'R' || m_fileData[1] != 'A' || m_fileData[2] != 'W' || m_fileData[3] != 'V') {
    return;
  }

  std::memcpy(&m_width, &m_fileData[4], 4);
  std::memcpy(&m_height, &m_fileData[8], 4);
  std::memcpy(&m_frameRate, &m_fileData[12], 8);
  std::memcpy(&m_frameCount, &m_fileData[20], 4);

  size_t expectedSize = 24 + static_cast<size_t>(m_width) * m_height * 4 * m_frameCount;
  if (m_fileData.size() >= expectedSize && m_width > 0 && m_height > 0 && m_frameCount > 0) {
    m_valid = true;
  }
}

bool FileVideoSource::frameAt(int index, Image::Bitmap &out) {
  if (!m_valid || index < 0 || index >= m_frameCount) {
    return false;
  }

  out.width = m_width;
  out.height = m_height;
  out.pixels.resize(static_cast<size_t>(m_width) * m_height);

  size_t frameSize = static_cast<size_t>(m_width) * m_height * 4;
  size_t offset = 24 + index * frameSize;

  for (int i = 0; i < m_width * m_height; ++i) {
    size_t pxOffset = offset + i * 4;
    out.pixels[i] = Paint::Color{
      m_fileData[pxOffset + 0],
      m_fileData[pxOffset + 1],
      m_fileData[pxOffset + 2],
      m_fileData[pxOffset + 3]
    };
  }
  return true;
}

} // namespace Video
} // namespace DesktopWebview
