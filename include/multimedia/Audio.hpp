#ifndef AUDIO_HPP
#define AUDIO_HPP

#include <cstdint>
#include <vector>

namespace DesktopWebview {
namespace Audio {

// A blocking raw-PCM playback sink for the system's default output device:
// ALSA on Linux, WASAPI on Windows. Samples are interleaved signed 16-bit
// (S16). Compressed-format decoding (mp3/ogg/...) is a separate later stage;
// this renders already-decoded PCM.
//
// The platform handles live behind a PIMPL so this header pulls in no
// ALSA/Windows dependencies.
class AudioOutput {
public:
  AudioOutput();
  ~AudioOutput();

  AudioOutput(const AudioOutput &) = delete;
  AudioOutput &operator=(const AudioOutput &) = delete;

  // Open the default device. Returns false if no device is available (e.g. a
  // headless machine), leaving the object closed.
  bool open(int sampleRate = 44100, int channels = 2);

  // Write `frameCount` interleaved frames (each frame is `channels` samples).
  // Blocks until the data is queued to the device. Returns the number of frames
  // accepted, or -1 on error / when not open.
  long write(const std::int16_t *interleaved, long frameCount);

  void close();

  bool isOpen() const;
  int sampleRate() const;
  int channels() const;

private:
  struct Impl;
  Impl *m_impl;
};

// Generate an interleaved S16 sine tone: `freqHz` for `seconds`, replicated
// across `channels`. `amplitude` is 0..1 of full scale. Pure helper, useful for
// tests and demos.
std::vector<std::int16_t> generateTone(double freqHz, double seconds,
                                       int sampleRate = 44100, int channels = 2,
                                       double amplitude = 0.3);

} // namespace Audio
} // namespace DesktopWebview

#endif // AUDIO_HPP
