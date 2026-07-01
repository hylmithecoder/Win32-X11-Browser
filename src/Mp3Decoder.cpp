#include "../include/Mp3Decoder.hpp"
#include "../include/Debugger.hpp"

#include <algorithm>
#include <cstring>

#ifdef DWV_HAVE_FFMPEG
#include <cstdio> // SEEK_SET/CUR/END for the memory seek callback
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#endif

using namespace Debug;

#ifdef DWV_HAVE_FFMPEG
namespace {

// Reader state so FFmpeg can decode straight from an in-memory buffer (the
// bytes we already fetched over HTTP), avoiding a temp file or re-download.
struct MemBuffer {
  const uint8_t *data;
  size_t size;
  size_t pos;
};

int memRead(void *opaque, uint8_t *buf, int bufSize) {
  auto *mb = static_cast<MemBuffer *>(opaque);
  size_t remaining = mb->size - mb->pos;
  if (remaining == 0)
    return AVERROR_EOF;
  int n = static_cast<int>(std::min(static_cast<size_t>(bufSize), remaining));
  std::memcpy(buf, mb->data + mb->pos, n);
  mb->pos += n;
  return n;
}

int64_t memSeek(void *opaque, int64_t offset, int whence) {
  auto *mb = static_cast<MemBuffer *>(opaque);
  if (whence == AVSEEK_SIZE)
    return static_cast<int64_t>(mb->size);
  size_t newPos;
  switch (whence & ~AVSEEK_FORCE) {
  case SEEK_SET:
    newPos = static_cast<size_t>(offset);
    break;
  case SEEK_CUR:
    newPos = mb->pos + static_cast<size_t>(offset);
    break;
  case SEEK_END:
    newPos = mb->size + static_cast<size_t>(offset);
    break;
  default:
    return -1;
  }
  if (newPos > mb->size)
    return -1;
  mb->pos = newPos;
  return static_cast<int64_t>(newPos);
}

// Decode every audio frame from an opened format context into interleaved S16
// PCM. Mirrors the resampler setup already used in Video.cpp, with the same
// libav version guards.
bool decodeAudioStream(AVFormatContext *fmt, int &outSr, int &outCh,
                       std::vector<int16_t> &pcm) {
  if (avformat_find_stream_info(fmt, nullptr) < 0)
    return false;
  int streamIdx =
      av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (streamIdx < 0)
    return false;

  AVStream *st = fmt->streams[streamIdx];
  const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
  if (!dec)
    return false;
  AVCodecContext *ctx = avcodec_alloc_context3(dec);
  if (!ctx)
    return false;

  bool ok = false;
  AVFrame *frame = nullptr;
  AVPacket *pkt = nullptr;
  SwrContext *swr = nullptr;
  uint8_t *resampleBuf = nullptr;

  do {
    if (avcodec_parameters_to_context(ctx, st->codecpar) < 0)
      break;
    if (avcodec_open2(ctx, dec, nullptr) < 0)
      break;

    int sr = ctx->sample_rate;
#if LIBAVUTIL_VERSION_MAJOR >= 57
    int ch = ctx->ch_layout.nb_channels;
#else
    int ch = ctx->channels;
#endif
    if (sr <= 0 || ch <= 0)
      break;

    // Resampler: native sample format -> S16 interleaved, same rate/channels.
#if LIBAVUTIL_VERSION_MAJOR >= 57
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, ch);
    if (swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_S16, sr,
                            &ctx->ch_layout, ctx->sample_fmt, sr, 0,
                            nullptr) < 0)
      break;
#else
    int64_t outLayout = av_get_default_channel_layout(ch);
    swr = swr_alloc_set_opts(nullptr, outLayout, AV_SAMPLE_FMT_S16, sr,
                             ctx->channel_layout, ctx->sample_fmt, sr, 0,
                             nullptr);
#endif
    if (!swr || swr_init(swr) < 0)
      break;

    frame = av_frame_alloc();
    pkt = av_packet_alloc();
    if (!frame || !pkt)
      break;

    auto drain = [&]() {
      while (avcodec_receive_frame(ctx, frame) == 0) {
        int outSamples = static_cast<int>(av_rescale_rnd(
            swr_get_delay(swr, sr) + frame->nb_samples, sr, sr, AV_ROUND_UP));
        int bufSize = av_samples_get_buffer_size(nullptr, ch, outSamples,
                                                 AV_SAMPLE_FMT_S16, 1);
        if (bufSize <= 0)
          continue;
        resampleBuf = static_cast<uint8_t *>(av_realloc(resampleBuf, bufSize));
        uint8_t *planes[1] = {resampleBuf};
        int converted =
            swr_convert(swr, planes, outSamples,
                        const_cast<const uint8_t **>(frame->extended_data),
                        frame->nb_samples);
        if (converted > 0) {
          const int16_t *s = reinterpret_cast<const int16_t *>(resampleBuf);
          pcm.insert(pcm.end(), s, s + static_cast<size_t>(converted) * ch);
        }
      }
    };

    while (av_read_frame(fmt, pkt) >= 0) {
      if (pkt->stream_index == streamIdx &&
          avcodec_send_packet(ctx, pkt) == 0) {
        drain();
      }
      av_packet_unref(pkt);
    }
    // Flush any buffered frames.
    avcodec_send_packet(ctx, nullptr);
    drain();

    outSr = sr;
    outCh = ch;
    ok = !pcm.empty();
  } while (false);

  av_free(resampleBuf);
  if (pkt)
    av_packet_free(&pkt);
  if (frame)
    av_frame_free(&frame);
  if (swr)
    swr_free(&swr);
  avcodec_free_context(&ctx);
  return ok;
}

} // namespace
#endif // DWV_HAVE_FFMPEG

namespace DesktopWebview {
namespace Audio {

Mp3Decoder::Mp3Decoder() {}

int Mp3Decoder::decode(const std::string &filePath) {
#ifdef DWV_HAVE_FFMPEG
  AVFormatContext *fmt = nullptr;
  if (avformat_open_input(&fmt, filePath.c_str(), nullptr, nullptr) != 0)
    return 0;
  m_pcm.clear();
  bool ok = decodeAudioStream(fmt, m_sampleRate, m_channels, m_pcm);
  avformat_close_input(&fmt);
  if (!ok)
    return 0;
  DEBUG_LOGF("MP3(ffmpeg): %d Hz, %d ch, %zu samples", LogLevel::INFO,
             m_sampleRate, m_channels, m_pcm.size());
  return static_cast<int>(m_pcm.size() / m_channels);
#else
  (void)filePath;
  DEBUG_LOGF("MP3: built without FFmpeg; cannot decode", LogLevel::WARNING);
  return 0;
#endif
}

int Mp3Decoder::decodeBytes(const uint8_t *data, size_t len) {
  if (!data || len == 0)
    return 0;
#ifdef DWV_HAVE_FFMPEG
  // Wrap the buffer in a custom AVIO context so FFmpeg reads from memory.
  const int kIoBufSize = 4096;
  unsigned char *ioBuf = static_cast<unsigned char *>(av_malloc(kIoBufSize));
  if (!ioBuf)
    return 0;
  MemBuffer mb{data, len, 0};
  AVIOContext *avio =
      avio_alloc_context(ioBuf, kIoBufSize, 0, &mb, memRead, nullptr, memSeek);
  if (!avio) {
    av_free(ioBuf);
    return 0;
  }
  AVFormatContext *fmt = avformat_alloc_context();
  if (!fmt) {
    av_free(avio->buffer);
    avio_context_free(&avio);
    return 0;
  }
  fmt->pb = avio;
  fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

  m_pcm.clear();
  bool ok = false;
  // On failure avformat_open_input frees `fmt` and nulls it, but leaves our
  // custom AVIO context alone, so we free that ourselves below.
  if (avformat_open_input(&fmt, nullptr, nullptr, nullptr) == 0) {
    ok = decodeAudioStream(fmt, m_sampleRate, m_channels, m_pcm);
    avformat_close_input(&fmt);
  }
  av_free(avio->buffer);
  avio_context_free(&avio);

  if (!ok)
    return 0;
  DEBUG_LOGF("MP3(ffmpeg): %d Hz, %d ch, %zu samples", LogLevel::INFO,
             m_sampleRate, m_channels, m_pcm.size());
  return static_cast<int>(m_pcm.size() / m_channels);
#else
  (void)data;
  (void)len;
  DEBUG_LOGF("MP3: built without FFmpeg; cannot decode", LogLevel::WARNING);
  return 0;
#endif
}

} // namespace Audio
} // namespace DesktopWebview
