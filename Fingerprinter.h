#pragma once

#include <SlidingDFT/sliding_dft.hpp>
#include <zita-resampler/resampler.h>
#include <dlib/pipe.h>

extern "C" {
#include <libavformat/avformat.h>
}

#include <memory>

struct AVFormatContext;
struct AVCodecContext;
struct AVCodec;
struct AVPacket;
struct SwrContext;

struct Fingerprinter;

struct Fingerprint
{
    enum FrequencyBand {
        Band_250_520,
        Band_520_1450,
        Band_1450_3500,
        Band_3500_5500,
        BandCount
    };
    std::array<std::vector<uint8_t>, BandCount> peaks;

    bool isValid() const { return m_valid; }

private:
    friend struct Fingerprinter;
    bool m_valid = false;
};

struct Fingerprinter
{
    ~Fingerprinter();

    bool open(const char *filePath);

    Fingerprint getFingerprint();

private:
    void threadLoop();
    static std::string ffmpegError(const int ret);

    SlidingDFT<int16_t, 1024> m_dft;

    // Because ffmpeg takes double pointers, because it is a C library, but
    // that is incompatible with how we store them.
    static void formatDeleter(AVFormatContext *fmt);
    static void codecDeleter(AVCodecContext *codec);
    static void frameDeleter(AVFrame *frame);
    static void resamplerDeleter(SwrContext *context);

    dlib::pipe<Fingerprint> m_signatures;

    // ffmpeg
    std::unique_ptr<AVFormatContext, decltype(&formatDeleter)> m_format;
    std::unique_ptr<AVCodecContext, decltype(&codecDeleter)> m_decoder;
    std::unique_ptr<AVPacket, decltype(&::av_packet_unref)> m_packet;
    std::unique_ptr<AVFrame, decltype(&frameDeleter)> m_frame;
    std::unique_ptr<SwrContext, decltype(&resamplerDeleter)> m_resampler;
    int m_audioStream = -1;

    std::mutex m_quitMutex;
};
