#include "../include/Video.hpp"
#include "../include/Audio.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <thread>

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

FileVideoSource::FileVideoSource(const std::string &filePath)
    : m_filePath(filePath) {
  std::ifstream file(filePath, std::ios::binary);
  if (!file) {
    return;
  }
  m_fileData.assign(std::istreambuf_iterator<char>(file),
                    std::istreambuf_iterator<char>());

  if (m_fileData.size() < 24) {
    return;
  }

  if (m_fileData[0] != 'R' || m_fileData[1] != 'A' || m_fileData[2] != 'W' ||
      m_fileData[3] != 'V') {
    return;
  }

  std::memcpy(&m_width, &m_fileData[4], 4);
  std::memcpy(&m_height, &m_fileData[8], 4);
  std::memcpy(&m_frameRate, &m_fileData[12], 8);
  std::memcpy(&m_frameCount, &m_fileData[20], 4);

  size_t expectedSize =
      24 + static_cast<size_t>(m_width) * m_height * 4 * m_frameCount;
  if (m_fileData.size() >= expectedSize && m_width > 0 && m_height > 0 &&
      m_frameCount > 0) {
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
    out.pixels[i] =
        Paint::Color{m_fileData[pxOffset + 0], m_fileData[pxOffset + 1],
                     m_fileData[pxOffset + 2], m_fileData[pxOffset + 3]};
  }
  return true;
}

// ---------------------------------------------------------------------------
// FfmpegVideoSource: real video decode via libav* (mp4/webm/mkv/...).
// ---------------------------------------------------------------------------
#ifdef DWV_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

struct FfmpegVideoSource::Impl {
  AVFormatContext *fmt = nullptr;
  AVCodecContext *codec = nullptr;
  SwsContext *sws = nullptr;
  AVFrame *frame = nullptr; // decoded (native pixel format)
  AVFrame *rgba = nullptr;  // converted RGBA
  AVPacket *packet = nullptr;
  uint8_t *rgbaBuf = nullptr;
  int streamIndex = -1;
  int width = 0;
  int height = 0;
  double fps = 0.0;
  int frameCount = 0;
  bool valid = false;

  // Sequential-decode bookkeeping: time (seconds, relative to stream start) of
  // the most recently decoded frame, so we can decode forward without seeking
  // when frames are requested in sequence (the common playback case).
  double lastDecodedTime = -1.0;
  int64_t startTime = 0; // stream start_time in stream time_base units

  // Audio state
  int audioStreamIndex = -1;
  AVCodecContext *audioCodecCtx = nullptr;
  SwrContext *audioSwr = nullptr;
  AVFrame *audioFrame = nullptr;
  AVFormatContext *audioFmt = nullptr; // separate fmt ctx for audio thread
  int audioSampleRateVal = 0;
  int audioChannelsVal = 0;
  std::thread audioThread;
  std::atomic<bool> audioActive{false};

  ~Impl() {
    stopAudio();
    if (audioFrame)
      av_frame_free(&audioFrame);
    if (audioSwr)
      swr_free(&audioSwr);
    if (audioCodecCtx)
      avcodec_free_context(&audioCodecCtx);
    if (audioFmt)
      avformat_close_input(&audioFmt);

    if (sws)
      sws_freeContext(sws);
    if (rgbaBuf)
      av_free(rgbaBuf);
    if (rgba)
      av_frame_free(&rgba);
    if (frame)
      av_frame_free(&frame);
    if (packet)
      av_packet_free(&packet);
    if (codec)
      avcodec_free_context(&codec);
    if (fmt)
      avformat_close_input(&fmt);
  }

  void startAudio() {
    if (audioActive || audioStreamIndex < 0 || !audioCodecCtx) {
      return;
    }
    audioActive = true;
    audioThread = std::thread(&Impl::audioPlaybackLoop, this);
  }

  void stopAudio() {
    if (!audioActive) {
      return;
    }
    audioActive = false;
    if (audioThread.joinable()) {
      audioThread.join();
    }
  }

  bool open(const std::string &url) {
    if (avformat_open_input(&fmt, url.c_str(), nullptr, nullptr) != 0) {
      return false;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
      return false;
    }
    streamIndex =
        av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (streamIndex < 0) {
      return false;
    }
    AVStream *st = fmt->streams[streamIndex];
    const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
      return false;
    }
    codec = avcodec_alloc_context3(dec);
    if (!codec || avcodec_parameters_to_context(codec, st->codecpar) < 0 ||
        avcodec_open2(codec, dec, nullptr) < 0) {
      return false;
    }
    width = codec->width;
    height = codec->height;
    if (width <= 0 || height <= 0) {
      return false;
    }

    AVRational fr = av_guess_frame_rate(fmt, st, nullptr);
    fps = (fr.num && fr.den) ? av_q2d(fr) : 25.0;
    startTime = (st->start_time == AV_NOPTS_VALUE) ? 0 : st->start_time;

    if (st->nb_frames > 0) {
      frameCount = static_cast<int>(st->nb_frames);
    } else if (fmt->duration > 0) {
      double seconds = fmt->duration / (double)AV_TIME_BASE;
      frameCount = static_cast<int>(seconds * fps);
    } else {
      frameCount = -1; // unknown / unbounded
    }

    frame = av_frame_alloc();
    rgba = av_frame_alloc();
    packet = av_packet_alloc();
    if (!frame || !rgba || !packet) {
      return false;
    }
    int bufSize = av_image_get_buffer_size(AV_PIX_FMT_RGBA, width, height, 1);
    rgbaBuf = static_cast<uint8_t *>(av_malloc(bufSize));
    if (!rgbaBuf) {
      return false;
    }
    av_image_fill_arrays(rgba->data, rgba->linesize, rgbaBuf, AV_PIX_FMT_RGBA,
                         width, height, 1);
    sws = sws_getContext(width, height, codec->pix_fmt, width, height,
                         AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr,
                         nullptr);
    if (!sws) {
      return false;
    }
    // Try to set up audio (non-fatal if no audio stream or decoder).
    setupAudio(url);

    valid = true;
    return true;
  }

  // Time (seconds, relative to stream start) of a decoded frame.
  double frameTime(const AVFrame *f) const {
    AVStream *st = fmt->streams[streamIndex];
    int64_t pts = (f->best_effort_timestamp != AV_NOPTS_VALUE)
                      ? f->best_effort_timestamp
                      : f->pts;
    if (pts == AV_NOPTS_VALUE) {
      return lastDecodedTime + (fps > 0 ? 1.0 / fps : 0.04);
    }
    return (pts - startTime) * av_q2d(st->time_base);
  }

  // Decode the next frame from the stream into `frame`. Returns false at EOF.
  bool decodeNext() {
    while (true) {
      int ret = avcodec_receive_frame(codec, frame);
      if (ret == 0) {
        return true;
      }
      if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        return false;
      }
      // Need more input.
      int rd = av_read_frame(fmt, packet);
      if (rd < 0) {
        // Flush the decoder at EOF.
        avcodec_send_packet(codec, nullptr);
        if (avcodec_receive_frame(codec, frame) == 0) {
          return true;
        }
        return false;
      }
      if (packet->stream_index == streamIndex) {
        avcodec_send_packet(codec, packet);
      }
      av_packet_unref(packet);
    }
  }

  void seekToTime(double seconds) {
    AVStream *st = fmt->streams[streamIndex];
    int64_t ts = startTime + (int64_t)(seconds / av_q2d(st->time_base));
    av_seek_frame(fmt, streamIndex, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(codec);
    lastDecodedTime = -1.0;
  }

  // Set up audio decoding (best-effort; non-fatal on failure).
  void setupAudio(const std::string &url) {
    audioStreamIndex =
        av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioStreamIndex < 0) {
      return;
    }
    // Open a separate format context for the audio thread so video and
    // audio decoding do not share a single AVFormatContext (not thread-safe).
    if (avformat_open_input(&audioFmt, url.c_str(), nullptr, nullptr) != 0) {
      return;
    }
    if (avformat_find_stream_info(audioFmt, nullptr) < 0) {
      avformat_close_input(&audioFmt);
      audioFmt = nullptr;
      return;
    }

    // Re-find audio stream index in the separate audio format context.
    int aIdx =
        av_find_best_stream(audioFmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (aIdx < 0) {
      avcodec_free_context(&audioCodecCtx);
      audioCodecCtx = nullptr;
      avformat_close_input(&audioFmt);
      audioFmt = nullptr;
      return;
    }
    audioStreamIndex = aIdx;

    AVStream *st = audioFmt->streams[audioStreamIndex];
    const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
      avformat_close_input(&audioFmt);
      audioFmt = nullptr;
      return;
    }
    audioCodecCtx = avcodec_alloc_context3(dec);
    if (!audioCodecCtx ||
        avcodec_parameters_to_context(audioCodecCtx, st->codecpar) < 0 ||
        avcodec_open2(audioCodecCtx, dec, nullptr) < 0) {
      avcodec_free_context(&audioCodecCtx);
      audioCodecCtx = nullptr;
      avformat_close_input(&audioFmt);
      audioFmt = nullptr;
      return;
    }

    audioSampleRateVal = audioCodecCtx->sample_rate;
#if LIBAVUTIL_VERSION_MAJOR >= 57
    audioChannelsVal = audioCodecCtx->ch_layout.nb_channels;
#else
    audioChannelsVal = audioCodecCtx->channels;
#endif

    // Set up resampler: convert native sample format -> S16 interleaved.
#if LIBAVUTIL_VERSION_MAJOR >= 57
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, audioChannelsVal);
    swr_alloc_set_opts2(&audioSwr, &outLayout, AV_SAMPLE_FMT_S16,
                        audioSampleRateVal, &audioCodecCtx->ch_layout,
                        audioCodecCtx->sample_fmt, audioCodecCtx->sample_rate,
                        0, nullptr);
#else
    int64_t outLayout = av_get_default_channel_layout(audioChannelsVal);
    audioSwr = swr_alloc_set_opts(
        nullptr, outLayout, AV_SAMPLE_FMT_S16, audioSampleRateVal,
        audioCodecCtx->channel_layout, audioCodecCtx->sample_fmt,
        audioCodecCtx->sample_rate, 0, nullptr);
#endif
    if (!audioSwr || swr_init(audioSwr) < 0) {
      swr_free(&audioSwr);
      audioSwr = nullptr;
      avcodec_free_context(&audioCodecCtx);
      audioCodecCtx = nullptr;
      avformat_close_input(&audioFmt);
      audioFmt = nullptr;
      return;
    }

    audioFrame = av_frame_alloc();
    if (!audioFrame) {
      swr_free(&audioSwr);
      audioSwr = nullptr;
      avcodec_free_context(&audioCodecCtx);
      audioCodecCtx = nullptr;
      avformat_close_input(&audioFmt);
      audioFmt = nullptr;
    }
  }

  // Background thread: decode audio packets and play through AudioOutput.
  void audioPlaybackLoop() {
    Audio::AudioOutput audioOut;
    if (!audioOut.open(audioSampleRateVal, audioChannelsVal)) {
      audioActive = false;
      return;
    }

    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
      audioActive = false;
      return;
    }

    // Seek audio stream to beginning.
    AVStream *st = audioFmt->streams[audioStreamIndex];
    int64_t seekTs = (st->start_time == AV_NOPTS_VALUE) ? 0 : st->start_time;
    av_seek_frame(audioFmt, audioStreamIndex, seekTs, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(audioCodecCtx);

    uint8_t *resampleBuf = nullptr;

    while (audioActive) {
      int ret = avcodec_receive_frame(audioCodecCtx, audioFrame);
      if (ret == 0) {
        int outSamples = av_rescale_rnd(
            swr_get_delay(audioSwr, audioCodecCtx->sample_rate) +
                audioFrame->nb_samples,
            audioSampleRateVal, audioCodecCtx->sample_rate, AV_ROUND_UP);
        int bufSize = av_samples_get_buffer_size(
            nullptr, audioChannelsVal, outSamples, AV_SAMPLE_FMT_S16, 1);
        resampleBuf = static_cast<uint8_t *>(av_realloc(resampleBuf, bufSize));
        uint8_t *planes[1] = {resampleBuf};
        int converted =
            swr_convert(audioSwr, planes, outSamples,
                        const_cast<const uint8_t **>(audioFrame->extended_data),
                        audioFrame->nb_samples);
        if (converted > 0) {
          audioOut.write(reinterpret_cast<const int16_t *>(resampleBuf),
                         converted);
        }
        continue;
      }
      if (ret == AVERROR_EOF) {
        break;
      }
      if (ret != AVERROR(EAGAIN)) {
        break;
      }

      // Need more input packets.
      av_packet_unref(pkt);
      int rd = av_read_frame(audioFmt, pkt);
      if (rd < 0) {
        // Flush decoder.
        avcodec_send_packet(audioCodecCtx, nullptr);
        continue;
      }
      if (pkt->stream_index == audioStreamIndex) {
        avcodec_send_packet(audioCodecCtx, pkt);
      }
    }

    av_free(resampleBuf);
    av_packet_free(&pkt);
    audioOut.close();
    audioActive = false;
  }

  bool getFrameAt(int index, Image::Bitmap &out) {
    if (!valid) {
      return false;
    }
    if (frameCount >= 0 && index >= frameCount) {
      index = frameCount - 1;
    }
    if (index < 0) {
      index = 0;
    }
    double target = (fps > 0) ? index / fps : 0.0;

    // Decide whether to seek: rewind, or a large forward jump.
    bool needSeek = (lastDecodedTime < 0) || (target < lastDecodedTime) ||
                    (target - lastDecodedTime > 1.0);
    if (needSeek) {
      seekToTime(target);
    }

    // Decode forward until we reach (or pass) the target time.
    int guard = 0;
    while (guard++ < 100000) {
      if (!decodeNext()) {
        break; // EOF: use whatever we last decoded (if any)
      }
      lastDecodedTime = frameTime(frame);
      if (lastDecodedTime + (fps > 0 ? 0.5 / fps : 0.0) >= target) {
        break;
      }
    }

    // Convert the current `frame` to RGBA and copy into the bitmap.
    sws_scale(sws, frame->data, frame->linesize, 0, height, rgba->data,
              rgba->linesize);
    out.width = width;
    out.height = height;
    out.pixels.resize(static_cast<size_t>(width) * height);
    const uint8_t *src = rgba->data[0];
    int stride = rgba->linesize[0];
    for (int y = 0; y < height; ++y) {
      const uint8_t *row = src + static_cast<size_t>(y) * stride;
      for (int x = 0; x < width; ++x) {
        const uint8_t *p = row + x * 4;
        out.pixels[static_cast<size_t>(y) * width + x] =
            Paint::Color{p[0], p[1], p[2], p[3]};
      }
    }
    return true;
  }
};

FfmpegVideoSource::FfmpegVideoSource(const std::string &pathOrUrl)
    : m_impl(std::make_unique<Impl>()) {
  // libav initialises networking lazily; nothing global required on modern
  // ffmpeg. Strip a leading file:// for local paths.
  std::string url = pathOrUrl;
  if (url.rfind("file://", 0) == 0) {
    url = url.substr(7);
  }
  m_impl->open(url);
}

FfmpegVideoSource::~FfmpegVideoSource() = default;

int FfmpegVideoSource::width() const { return m_impl->width; }
int FfmpegVideoSource::height() const { return m_impl->height; }
double FfmpegVideoSource::frameRate() const { return m_impl->fps; }
int FfmpegVideoSource::frameCount() const { return m_impl->frameCount; }
bool FfmpegVideoSource::valid() const { return m_impl->valid; }
bool FfmpegVideoSource::frameAt(int index, Image::Bitmap &out) {
  return m_impl->getFrameAt(index, out);
}

bool FfmpegVideoSource::hasAudio() const {
  return m_impl && m_impl->audioStreamIndex >= 0;
}
int FfmpegVideoSource::audioSampleRate() const {
  return m_impl ? m_impl->audioSampleRateVal : 0;
}
int FfmpegVideoSource::audioChannels() const {
  return m_impl ? m_impl->audioChannelsVal : 0;
}
void FfmpegVideoSource::startAudio() {
  if (m_impl) {
    m_impl->startAudio();
  }
}
void FfmpegVideoSource::stopAudio() {
  if (m_impl) {
    m_impl->stopAudio();
  }
}

#else // !DWV_HAVE_FFMPEG

struct FfmpegVideoSource::Impl {};
FfmpegVideoSource::FfmpegVideoSource(const std::string &) {}
FfmpegVideoSource::~FfmpegVideoSource() = default;
int FfmpegVideoSource::width() const { return 0; }
int FfmpegVideoSource::height() const { return 0; }
double FfmpegVideoSource::frameRate() const { return 0.0; }
int FfmpegVideoSource::frameCount() const { return 0; }
bool FfmpegVideoSource::valid() const { return false; }
bool FfmpegVideoSource::frameAt(int, Image::Bitmap &) { return false; }
bool FfmpegVideoSource::hasAudio() const { return false; }
int FfmpegVideoSource::audioSampleRate() const { return 0; }
int FfmpegVideoSource::audioChannels() const { return 0; }
void FfmpegVideoSource::startAudio() {}
void FfmpegVideoSource::stopAudio() {}

#endif // DWV_HAVE_FFMPEG

std::unique_ptr<VideoSource> openVideoFile(const std::string &pathOrUrl) {
  // Custom uncompressed format.
  if (pathOrUrl.size() >= 5 &&
      pathOrUrl.substr(pathOrUrl.size() - 5) == ".rawv") {
    auto fvs = std::make_unique<FileVideoSource>(pathOrUrl);
    if (fvs->valid()) {
      return fvs;
    }
  } else {
    auto ff = std::make_unique<FfmpegVideoSource>(pathOrUrl);
    if (ff->valid()) {
      return ff;
    }
  }
  // Fallback preview so the pipeline still renders something.
  return std::make_unique<SyntheticVideoSource>(320, 240, 30.0, 300);
}

} // namespace Video
} // namespace DesktopWebview
