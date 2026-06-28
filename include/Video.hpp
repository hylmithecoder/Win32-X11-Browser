#ifndef VIDEO_HPP
#define VIDEO_HPP

#include "Image.hpp"
#include "Paint.hpp"

namespace DesktopWebview {
namespace Video {

// A source of decoded video frames. A real demuxer/decoder (e.g. ffmpeg's
// libavformat/libavcodec) plugs in behind this interface later; for now the
// pipeline is driven by a deterministic synthetic generator so the
// frame-fetch + timing path is exercisable without a codec dependency.
class VideoSource {
public:
  virtual ~VideoSource() = default;

  virtual int width() const = 0;
  virtual int height() const = 0;
  virtual double frameRate() const = 0;
  // Total frame count, or -1 for an unbounded/live stream.
  virtual int frameCount() const = 0;

  // Render frame `index` into `out`. Returns false if the index is out of
  // range (past the end of a bounded source).
  virtual bool frameAt(int index, Image::Bitmap &out) = 0;
};

// Map a playback time (seconds) to a frame index for `src`. Clamps to the last
// frame for bounded sources; never returns negative.
int frameIndexForTime(const VideoSource &src, double seconds);

// A deterministic test/preview source: a vertical bar that sweeps left-to-right
// across a solid background, one step per frame. Useful for verifying the
// render + timing path without real media.
class SyntheticVideoSource : public VideoSource {
public:
  SyntheticVideoSource(int width, int height, double frameRate, int frameCount,
                       Paint::Color background = Paint::Color{20, 20, 30, 255},
                       Paint::Color bar = Paint::Color{255, 140, 0, 255});

  int width() const override { return m_width; }
  int height() const override { return m_height; }
  double frameRate() const override { return m_frameRate; }
  int frameCount() const override { return m_frameCount; }
  bool frameAt(int index, Image::Bitmap &out) override;

private:
  int m_width;
  int m_height;
  double m_frameRate;
  int m_frameCount;
  Paint::Color m_background;
  Paint::Color m_bar;
};

// A video source that decodes a custom uncompressed raw video file (.rawv).
// The file layout is:
// - Magic string: "RAWV" (4 bytes)
// - Width: uint32_t (4 bytes)
// - Height: uint32_t (4 bytes)
// - Framerate: double (8 bytes)
// - FrameCount: uint32_t (4 bytes)
// - Data: Raw RGBA pixel array for all frames (width * height * 4 * frameCount
// bytes)
class FileVideoSource : public VideoSource {
public:
  explicit FileVideoSource(const std::string &filePath);

  int width() const override { return m_width; }
  int height() const override { return m_height; }
  double frameRate() const override { return m_frameRate; }
  int frameCount() const override { return m_frameCount; }
  bool frameAt(int index, Image::Bitmap &out) override;

  bool valid() const { return m_valid; }

private:
  std::string m_filePath;
  int m_width = 0;
  int m_height = 0;
  double m_frameRate = 0.0;
  int m_frameCount = 0;
  bool m_valid = false;
  std::vector<std::uint8_t> m_fileData;
};

} // namespace Video
} // namespace DesktopWebview

#endif // VIDEO_HPP
