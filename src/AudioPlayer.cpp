#include "../include/AudioPlayer.hpp"
#include "../include/Debugger.hpp"
#include "../include/Mp3Decoder.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>

using namespace Debug;

namespace DesktopWebview {
namespace Audio {

AudioPlayer::AudioPlayer() {}
AudioPlayer::~AudioPlayer() { stop(); }

bool AudioPlayer::isPlaying() const {
  return m_playing.load() && !m_paused.load();
}

double AudioPlayer::currentPosition() const {
  if (m_sampleRate <= 0)
    return 0.0;
  return static_cast<double>(m_currentFrame.load()) / m_sampleRate;
}

void AudioPlayer::pause() { m_paused = true; }

void AudioPlayer::resume() { m_paused = false; }

void AudioPlayer::seek(double seconds) {
  if (m_sampleRate <= 0 || seconds < 0.0)
    return;
  long frame = static_cast<long>(seconds * m_sampleRate);
  m_seekFrame = frame;
  // If paused, apply immediately so the thread picks it up
}

double AudioPlayer::durationSeconds() const {
  if (m_sampleRate <= 0 || m_channels <= 0)
    return 0.0;
  return static_cast<double>(m_pcm.size() / m_channels) / m_sampleRate;
}

// ---------------------------------------------------------------------------
// WAV loader (native, no library)
// ---------------------------------------------------------------------------
static uint16_t readU16LE(const uint8_t *p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
static uint32_t readU32LE(const uint8_t *p) {
  return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) |
                               (p[3] << 24));
}

bool AudioPlayer::loadWav(const std::string &path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f)
    return false;
  std::streamsize size = f.tellg();
  if (size <= 0)
    return false;
  f.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> bytes(static_cast<size_t>(size));
  if (!f.read(reinterpret_cast<char *>(bytes.data()), size))
    return false;
  return loadWavData(bytes.data(), bytes.size());
}

bool AudioPlayer::loadWavData(const std::uint8_t *data, std::size_t len) {
  if (!data || len < 44)
    return false;

  if (std::memcmp(data, "RIFF", 4) != 0 ||
      std::memcmp(data + 8, "WAVE", 4) != 0) {
    return false;
  }

  uint16_t audioFormat = readU16LE(data + 20);
  uint16_t numChannels = readU16LE(data + 22);
  uint32_t sampleRate = readU32LE(data + 24);
  uint16_t bitsPerSample = readU16LE(data + 34);

  if (audioFormat != 1) {
    DEBUG_LOGF("WAV: unsupported format %d (only PCM=1)", LogLevel::WARNING,
               audioFormat);
    return false;
  }
  if (bitsPerSample != 16 && bitsPerSample != 8) {
    DEBUG_LOGF("WAV: unsupported bit depth %d", LogLevel::WARNING,
               bitsPerSample);
    return false;
  }

  m_sampleRate = static_cast<int>(sampleRate);
  m_channels = numChannels;

  // Walk chunks from offset 12 to find "data".
  size_t off = 12;
  size_t dataOff = 0, dataSize = 0;
  while (off + 8 <= len) {
    uint32_t chunkSize = readU32LE(data + off + 4);
    if (std::memcmp(data + off, "data", 4) == 0) {
      dataOff = off + 8;
      dataSize = chunkSize;
      break;
    }
    off += 8 + chunkSize + (chunkSize & 1); // chunks are word-aligned
  }
  if (dataOff == 0)
    return false;
  // Clamp to what is actually present in the buffer.
  dataSize = std::min(dataSize, len - dataOff);
  const uint8_t *src = data + dataOff;

  if (bitsPerSample == 16) {
    size_t samples = dataSize / 2;
    m_pcm.resize(samples);
    for (size_t i = 0; i < samples; ++i) {
      m_pcm[i] = static_cast<int16_t>(src[i * 2] | (src[i * 2 + 1] << 8));
    }
  } else {
    m_pcm.resize(dataSize);
    for (size_t i = 0; i < dataSize; ++i) {
      m_pcm[i] = static_cast<int16_t>((src[i] - 128) * 256);
    }
  }

  DEBUG_LOGF("WAV loaded: %d Hz, %d ch, %zu samples (%.1fs)", LogLevel::INFO,
             m_sampleRate, m_channels, m_pcm.size() / m_channels,
             durationSeconds());
  return !m_pcm.empty();
}

// ---------------------------------------------------------------------------
// MP3 loader (native decoder, no external libraries)
// ---------------------------------------------------------------------------
bool AudioPlayer::loadMp3(const std::string &path) {
  Mp3Decoder decoder;
  if (decoder.decode(path) <= 0) {
    DEBUG_LOGF("MP3: decode failed for %s", LogLevel::WARNING, path.c_str());
    return false;
  }

  m_sampleRate = decoder.sampleRate();
  m_channels = decoder.channels();
  m_pcm = decoder.pcm();

  DEBUG_LOGF("MP3 loaded: %d Hz, %d ch, %zu samples (%.1fs)", LogLevel::INFO,
             m_sampleRate, m_channels, m_pcm.size() / m_channels,
             durationSeconds());
  return !m_pcm.empty();
}

bool AudioPlayer::loadMp3Data(const std::uint8_t *data, std::size_t len) {
  Mp3Decoder decoder;
  if (decoder.decodeBytes(data, len) <= 0) {
    DEBUG_LOGF("MP3: decode failed (%zu bytes in memory)", LogLevel::WARNING,
               len);
    return false;
  }

  m_sampleRate = decoder.sampleRate();
  m_channels = decoder.channels();
  m_pcm = decoder.pcm();

  DEBUG_LOGF("MP3 loaded: %d Hz, %d ch, %zu samples (%.1fs)", LogLevel::INFO,
             m_sampleRate, m_channels, m_pcm.size() / m_channels,
             durationSeconds());
  return !m_pcm.empty();
}

// ---------------------------------------------------------------------------
// Play / Stop
// ---------------------------------------------------------------------------
bool AudioPlayer::play(const std::string &filePath) {
  stop();

  std::string ext;
  auto dot = filePath.rfind('.');
  if (dot != std::string::npos) {
    ext = filePath.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  }

  bool loaded = false;
  if (ext == "wav") {
    loaded = loadWav(filePath);
  } else if (ext == "mp3") {
    loaded = loadMp3(filePath);
  } else {
    DEBUG_LOGF("AudioPlayer: unsupported format '.%s'", LogLevel::WARNING,
               ext.c_str());
    return false;
  }

  if (!loaded)
    return false;

  return startPlayback();
}

bool AudioPlayer::loadData(const std::vector<std::uint8_t> &bytes,
                           const std::string &ext) {
  stop();
  m_paused = false;
  m_currentFrame = 0;
  m_seekFrame = -1;

  bool loaded;
  if (ext == "wav") {
    loaded = loadWavData(bytes.data(), bytes.size());
  } else {
    loaded = loadMp3Data(bytes.data(), bytes.size());
  }
  return loaded;
}

bool AudioPlayer::playData(const std::vector<std::uint8_t> &bytes,
                           const std::string &ext) {
  if (!loadData(bytes, ext))
    return false;
  return startPlayback();
}

bool AudioPlayer::startPlayback() {
  m_stopRequested = false;
  m_paused = false;
  m_playing = true;
  m_currentFrame = 0;
  m_seekFrame = -1;
  m_thread = std::thread(&AudioPlayer::playbackThread, this);
  return true;
}

void AudioPlayer::stop() {
  m_stopRequested = true;
  m_paused = false;
  if (m_thread.joinable()) {
    m_thread.join();
  }
  m_playing = false;
  m_pcm.clear();
}

void AudioPlayer::playbackThread() {
  AudioOutput out;
  if (!out.open(m_sampleRate, m_channels)) {
    DEBUG_LOGF("AudioPlayer: cannot open audio device", LogLevel::WARNING);
    m_playing = false;
    return;
  }

  constexpr long kChunkFrames = 2048;
  long totalFrames = static_cast<long>(m_pcm.size() / m_channels);
  long pos = 0;

  while (pos < totalFrames && !m_stopRequested.load()) {
    // Handle seek requests
    long seekTo = m_seekFrame.exchange(-1);
    if (seekTo >= 0) {
      pos = std::min(seekTo, totalFrames - 1);
      if (pos < 0)
        pos = 0;
    }

    // Handle pause
    while (m_paused.load() && !m_stopRequested.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (m_stopRequested.load())
      break;

    m_currentFrame = pos;

    long todo = std::min(kChunkFrames, totalFrames - pos);
    long written = out.write(m_pcm.data() + pos * m_channels, todo);
    if (written <= 0)
      break;
    pos += written;
  }

  out.close();
  if (!m_stopRequested.load()) {
    // Natural end of playback
    m_currentFrame = totalFrames;
  }
  m_playing = false;
  m_paused = false;
}

} // namespace Audio
} // namespace DesktopWebview
