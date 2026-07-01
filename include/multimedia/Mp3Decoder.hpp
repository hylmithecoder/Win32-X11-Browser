#ifndef MP3_DECODER_HPP
#define MP3_DECODER_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace DesktopWebview {
namespace Audio {

// Decodes an MP3 (or any audio container FFmpeg supports) into interleaved
// signed 16-bit PCM. Decoding is delegated to FFmpeg (libav*). When the project
// is built without FFmpeg (DWV_HAVE_FFMPEG undefined) the decode calls fail
// gracefully and return 0.
class Mp3Decoder {
public:
  Mp3Decoder();

  // Decode from a local file path. Returns the number of decoded frames per
  // channel, or 0 on failure.
  int decode(const std::string &filePath);
  // Decode from an already-loaded in-memory buffer (e.g. bytes fetched over
  // HTTP). Returns the number of decoded frames per channel, or 0 on failure.
  int decodeBytes(const uint8_t *data, size_t len);

  int sampleRate() const { return m_sampleRate; }
  int channels() const { return m_channels; }
  const std::vector<std::int16_t> &pcm() const { return m_pcm; }

private:
  int m_sampleRate = 44100;
  int m_channels = 2;
  std::vector<std::int16_t> m_pcm;
};

} // namespace Audio
} // namespace DesktopWebview

#endif // MP3_DECODER_HPP
