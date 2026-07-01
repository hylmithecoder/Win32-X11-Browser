#ifndef AUDIO_PLAYER_HPP
#define AUDIO_PLAYER_HPP

#include "Audio.hpp"
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace DesktopWebview {
namespace Audio {

class AudioPlayer {
public:
  AudioPlayer();
  ~AudioPlayer();

  AudioPlayer(const AudioPlayer &) = delete;
  AudioPlayer &operator=(const AudioPlayer &) = delete;

  bool play(const std::string &filePath);
  // Play from an in-memory buffer (e.g. bytes fetched over HTTP). `ext` is the
  // lowercase format hint ("mp3" / "wav"). Use this instead of play() when the
  // source is not a local file path.
  bool playData(const std::vector<std::uint8_t> &bytes, const std::string &ext);
  // Decode data without starting playback (useful for querying duration first).
  bool loadData(const std::vector<std::uint8_t> &bytes, const std::string &ext);
  // Start playback of previously loaded data.
  bool startPlayback();
  void stop();
  bool isPlaying() const;

  void pause();
  void resume();
  bool isPaused() const { return m_paused.load(); }
  void seek(double seconds);
  double currentPosition() const;
  double durationSeconds() const;

  int sampleRate() const { return m_sampleRate; }
  int channels() const { return m_channels; }

private:
  bool loadWav(const std::string &path);
  bool loadMp3(const std::string &path);
  bool loadWavData(const std::uint8_t *data, std::size_t len);
  bool loadMp3Data(const std::uint8_t *data, std::size_t len);

  void playbackThread();

  std::vector<std::int16_t> m_pcm;
  int m_sampleRate = 44100;
  int m_channels = 2;

  std::atomic<bool> m_playing{false};
  std::atomic<bool> m_paused{false};
  std::atomic<bool> m_stopRequested{false};
  std::atomic<long> m_seekFrame{-1};
  std::atomic<long> m_currentFrame{0};
  std::thread m_thread;
};

} // namespace Audio
} // namespace DesktopWebview

#endif // AUDIO_PLAYER_HPP
