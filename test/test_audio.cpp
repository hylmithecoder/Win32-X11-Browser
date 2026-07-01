#include "Audio.hpp"
#include <cmath>
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

static void ToneTests() {
  std::cout << "=========================================================="
            << std::endl;
  std::cout << "PCM tone generation" << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  int rate = 44100;
  int ch = 2;
  double secs = 0.1;
  std::vector<std::int16_t> tone = Audio::generateTone(440.0, secs, rate, ch);

  long expectedFrames = static_cast<long>(secs * rate);
  Check("sample count = frames * channels",
        static_cast<long>(tone.size()) == expectedFrames * ch);
  Check("not silent", [&] {
    for (auto s : tone) {
      if (s != 0) {
        return true;
      }
    }
    return false;
  }());

  // Stereo channels identical (mono replicated).
  bool channelsMatch = true;
  for (long i = 0; i < expectedFrames; ++i) {
    if (tone[i * ch] != tone[i * ch + 1]) {
      channelsMatch = false;
      break;
    }
  }
  Check("L and R channels identical", channelsMatch);

  // First sample of a sine starting at phase 0 is ~0.
  Check("starts near zero", std::abs(tone[0]) < 100);

  // Amplitude respected: peak <= amplitude * full scale (+rounding).
  std::int16_t peak = 0;
  for (auto s : tone) {
    peak = std::max<std::int16_t>(peak, static_cast<std::int16_t>(std::abs(s)));
  }
  Check("peak within amplitude 0.3 (~9830)", peak <= 9900 && peak > 9000);

  // Degenerate inputs return empty.
  Check("zero duration -> empty",
        Audio::generateTone(440, 0, rate, ch).empty());
}

static void DeviceTests() {
  std::cout << "\n=========================================================="
            << std::endl;
  std::cout << "Device open/write (best-effort; headless-safe)" << std::endl;
  std::cout << "=========================================================="
            << std::endl;

  Audio::AudioOutput out;
  Check("starts closed", !out.isOpen());

  bool opened = out.open(44100, 2);
  if (!opened) {
    std::cout << "  [SKIP] no audio device available (headless) - API still ok"
              << std::endl;
    Check("write on closed device returns -1", out.write(nullptr, 0) == -1);
    return;
  }

  std::cout << "  [INFO] device open at " << out.sampleRate() << " Hz, "
            << out.channels() << " ch" << std::endl;
  Check("isOpen after open", out.isOpen());

  std::vector<std::int16_t> tone =
      Audio::generateTone(440.0, 0.15, out.sampleRate(), out.channels());
  long frames = static_cast<long>(tone.size() / out.channels());
  long written = out.write(tone.data(), frames);
  Check("wrote all frames", written == frames);

  out.close();
  Check("closed after close()", !out.isOpen());
}

int main() {
  ToneTests();
  DeviceTests();

  std::cout << "\n=========================================================="
            << std::endl;
  if (g_failures == 0) {
    std::cout << "All audio tests passed." << std::endl;
  } else {
    std::cout << g_failures << " audio test(s) failed." << std::endl;
  }
  std::cout << "=========================================================="
            << std::endl;
  return g_failures == 0 ? 0 : 1;
}
