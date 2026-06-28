#include "../include/Audio.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>

#if defined(__linux__) || defined(__gnu_linux__)
#include <alsa/asoundlib.h>
#elif defined(_WIN32)
#ifndef INITGUID
#define INITGUID
#endif
#include <windows.h>
// Order matters: mmdeviceapi/audioclient after windows.h + INITGUID so the
// CLSID/IID symbols are defined locally (no extra GUID lib to link).
#include <audioclient.h>
#include <mmdeviceapi.h>
#endif

namespace DesktopWebview {
namespace Audio {

std::vector<std::int16_t> generateTone(double freqHz, double seconds,
                                       int sampleRate, int channels,
                                       double amplitude) {
  std::vector<std::int16_t> samples;
  if (sampleRate <= 0 || channels <= 0 || seconds <= 0) {
    return samples;
  }
  amplitude = std::clamp(amplitude, 0.0, 1.0);
  long frames = static_cast<long>(seconds * sampleRate);
  samples.resize(static_cast<size_t>(frames) * channels);

  const double twoPiF = 2.0 * 3.14159265358979323846 * freqHz;
  for (long i = 0; i < frames; ++i) {
    double t = static_cast<double>(i) / sampleRate;
    double v = std::sin(twoPiF * t) * amplitude;
    auto s = static_cast<std::int16_t>(std::lround(v * 32767.0));
    for (int c = 0; c < channels; ++c) {
      samples[static_cast<size_t>(i) * channels + c] = s;
    }
  }
  return samples;
}

// ===========================================================================
// Platform PIMPL
// ===========================================================================
struct AudioOutput::Impl {
  bool open = false;
  int sampleRate = 0;
  int channels = 0;

#if defined(__linux__) || defined(__gnu_linux__)
  snd_pcm_t *pcm = nullptr;
#elif defined(_WIN32)
  bool comInit = false;
  IMMDeviceEnumerator *enumerator = nullptr;
  IMMDevice *device = nullptr;
  IAudioClient *client = nullptr;
  IAudioRenderClient *render = nullptr;
  WAVEFORMATEX *mixFormat = nullptr;
  UINT32 bufferFrames = 0;
  bool floatFormat = false;
#endif
};

AudioOutput::AudioOutput() : m_impl(new Impl()) {}
AudioOutput::~AudioOutput() {
  close();
  delete m_impl;
}

bool AudioOutput::isOpen() const { return m_impl->open; }
int AudioOutput::sampleRate() const { return m_impl->sampleRate; }
int AudioOutput::channels() const { return m_impl->channels; }

// ===========================================================================
// Linux (ALSA)
// ===========================================================================
#if defined(__linux__) || defined(__gnu_linux__)

bool AudioOutput::open(int sampleRate, int channels) {
  if (m_impl->open) {
    close();
  }
  int err = snd_pcm_open(&m_impl->pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
  if (err < 0) {
    std::cerr << "ALSA: cannot open default device: " << snd_strerror(err)
              << std::endl;
    m_impl->pcm = nullptr;
    return false;
  }
  // High-level parameter helper: S16 interleaved, soft-resample on, ~100ms.
  err = snd_pcm_set_params(m_impl->pcm, SND_PCM_FORMAT_S16_LE,
                           SND_PCM_ACCESS_RW_INTERLEAVED, channels, sampleRate,
                           1, 100000);
  if (err < 0) {
    std::cerr << "ALSA: cannot set params: " << snd_strerror(err) << std::endl;
    snd_pcm_close(m_impl->pcm);
    m_impl->pcm = nullptr;
    return false;
  }
  m_impl->open = true;
  m_impl->sampleRate = sampleRate;
  m_impl->channels = channels;
  return true;
}

long AudioOutput::write(const std::int16_t *interleaved, long frameCount) {
  if (!m_impl->open || !m_impl->pcm || !interleaved || frameCount <= 0) {
    return -1;
  }
  long written = 0;
  const std::int16_t *p = interleaved;
  while (written < frameCount) {
    snd_pcm_sframes_t n = snd_pcm_writei(m_impl->pcm, p, frameCount - written);
    if (n < 0) {
      n = snd_pcm_recover(m_impl->pcm, static_cast<int>(n), 1);
      if (n < 0) {
        return written;
      }
      continue;
    }
    written += n;
    p += n * m_impl->channels;
  }
  return written;
}

void AudioOutput::close() {
  if (m_impl->pcm) {
    snd_pcm_drain(m_impl->pcm);
    snd_pcm_close(m_impl->pcm);
    m_impl->pcm = nullptr;
  }
  m_impl->open = false;
}

// ===========================================================================
// Windows (WASAPI, shared mode)
// ===========================================================================
#elif defined(_WIN32)

bool AudioOutput::open(int sampleRate, int channels) {
  if (m_impl->open) {
    close();
  }

  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  m_impl->comInit = SUCCEEDED(hr);

  hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                        __uuidof(IMMDeviceEnumerator),
                        reinterpret_cast<void **>(&m_impl->enumerator));
  if (FAILED(hr)) {
    std::cerr << "WASAPI: CoCreateInstance failed" << std::endl;
    close();
    return false;
  }
  hr = m_impl->enumerator->GetDefaultAudioEndpoint(eRender, eConsole,
                                                   &m_impl->device);
  if (FAILED(hr)) {
    std::cerr << "WASAPI: no default render endpoint" << std::endl;
    close();
    return false;
  }
  hr = m_impl->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void **>(&m_impl->client));
  if (FAILED(hr)) {
    close();
    return false;
  }
  hr = m_impl->client->GetMixFormat(&m_impl->mixFormat);
  if (FAILED(hr) || !m_impl->mixFormat) {
    close();
    return false;
  }

  // Detect whether the shared mix format is float32 or 16-bit PCM.
  WAVEFORMATEX *f = m_impl->mixFormat;
  if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
    m_impl->floatFormat = true;
  } else if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    // The SubFormat GUID's Data1 field is the underlying format tag
    // (KSDATAFORMAT_SUBTYPE_IEEE_FLOAT -> 0x0003). Compare that directly to
    // avoid depending on the GUID symbol, which MinGW does not always emit.
    auto *ext = reinterpret_cast<WAVEFORMATEXTENSIBLE *>(f);
    m_impl->floatFormat = (ext->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT);
  } else {
    m_impl->floatFormat = false; // assume PCM16
  }

  REFERENCE_TIME bufferDuration = 10000000; // 1s, 100ns units
  hr = m_impl->client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, bufferDuration,
                                  0, m_impl->mixFormat, nullptr);
  if (FAILED(hr)) {
    std::cerr << "WASAPI: Initialize failed" << std::endl;
    close();
    return false;
  }
  hr = m_impl->client->GetBufferSize(&m_impl->bufferFrames);
  if (FAILED(hr)) {
    close();
    return false;
  }
  hr = m_impl->client->GetService(__uuidof(IAudioRenderClient),
                                  reinterpret_cast<void **>(&m_impl->render));
  if (FAILED(hr)) {
    close();
    return false;
  }
  m_impl->client->Start();

  // Shared mode plays at the device mix format; expose that to the caller.
  m_impl->sampleRate = static_cast<int>(m_impl->mixFormat->nSamplesPerSec);
  m_impl->channels = m_impl->mixFormat->nChannels;
  (void)sampleRate;
  (void)channels;
  m_impl->open = true;
  return true;
}

long AudioOutput::write(const std::int16_t *interleaved, long frameCount) {
  if (!m_impl->open || !m_impl->render || !interleaved || frameCount <= 0) {
    return -1;
  }
  int devCh = m_impl->channels;
  long written = 0;

  while (written < frameCount) {
    UINT32 padding = 0;
    if (FAILED(m_impl->client->GetCurrentPadding(&padding))) {
      return written;
    }
    UINT32 available = m_impl->bufferFrames - padding;
    if (available == 0) {
      Sleep(5);
      continue;
    }
    UINT32 todo =
        static_cast<UINT32>(std::min<long>(available, frameCount - written));

    BYTE *buffer = nullptr;
    if (FAILED(m_impl->render->GetBuffer(todo, &buffer))) {
      return written;
    }

    for (UINT32 i = 0; i < todo; ++i) {
      const std::int16_t *src = interleaved + (written + i) * devCh;
      if (m_impl->floatFormat) {
        float *dst = reinterpret_cast<float *>(buffer) + i * devCh;
        for (int c = 0; c < devCh; ++c) {
          dst[c] = src[c] / 32768.0f;
        }
      } else {
        std::int16_t *dst =
            reinterpret_cast<std::int16_t *>(buffer) + i * devCh;
        for (int c = 0; c < devCh; ++c) {
          dst[c] = src[c];
        }
      }
    }
    m_impl->render->ReleaseBuffer(todo, 0);
    written += todo;
  }
  return written;
}

void AudioOutput::close() {
  if (m_impl->client) {
    m_impl->client->Stop();
  }
  if (m_impl->render) {
    m_impl->render->Release();
    m_impl->render = nullptr;
  }
  if (m_impl->mixFormat) {
    CoTaskMemFree(m_impl->mixFormat);
    m_impl->mixFormat = nullptr;
  }
  if (m_impl->client) {
    m_impl->client->Release();
    m_impl->client = nullptr;
  }
  if (m_impl->device) {
    m_impl->device->Release();
    m_impl->device = nullptr;
  }
  if (m_impl->enumerator) {
    m_impl->enumerator->Release();
    m_impl->enumerator = nullptr;
  }
  if (m_impl->comInit) {
    CoUninitialize();
    m_impl->comInit = false;
  }
  m_impl->open = false;
}

// ===========================================================================
// Other platforms: no-op sink
// ===========================================================================
#else

bool AudioOutput::open(int, int) { return false; }
long AudioOutput::write(const std::int16_t *, long) { return -1; }
void AudioOutput::close() { m_impl->open = false; }

#endif

} // namespace Audio
} // namespace DesktopWebview
