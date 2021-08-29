#include "Fingerprinter.h"

extern "C" {
#define __STDC_CONSTANT_MACROS
#include <libswresample/swresample.h>
}


Fingerprinter::~Fingerprinter()
{
    m_signatures.disable();

    std::lock_guard<std::mutex> lock(m_quitMutex);
}

bool Fingerprinter::open(const char *filePath)
{
    AVFormatContext *formatContext = nullptr;
    int ret = avformat_open_input(&formatContext, filePath, nullptr, nullptr);
    m_format.reset(formatContext);
    if (ret < 0) {
        std::cerr << "Error opening " << filePath << ": " << ffmpegError(ret);
        return false;
    }

    ret = avformat_find_stream_info(m_format.get(), nullptr);
    if (ret < 0) {
        std::cerr << "Could not find stream info for " << filePath << ": " << ffmpegError(ret);
        return false;
    }

    if (m_format->nb_streams != 1) {
        fprintf(stderr, "Expected one audio input stream, but found %d\n",
                m_format->nb_streams);
        return false;
    }

    AVCodec *codec = nullptr;
    m_audioStream = av_find_best_stream(formatContext, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (!codec) {
        std::cerr << "Failed to find codec for " << filePath << std::endl;
        return false;
    }

    m_decoder.reset(avcodec_alloc_context3(codec));
    if (!m_decoder) {
        std::cerr << "Failed to allocate codec for " << filePath << std::endl;
        return false;
    }

    ret = avcodec_parameters_to_context(m_decoder.get(), m_format->streams[0]->codecpar);
    if (ret < 0) {
        std::cerr << "Could not get codec parameters for " << filePath << ": " << ffmpegError(ret);
        return false;
    }
    ret = avcodec_open2(m_decoder.get(), codec, nullptr);
    if (ret < 0) {
        std::cerr << "Could not load codec for " << filePath << ": " << ffmpegError(ret);
        return false;
    }

    m_resampler.reset(swr_alloc_set_opts(
            nullptr,
            AV_CH_LAYOUT_MONO,
            AV_SAMPLE_FMT_S16,
            16000,
            av_get_default_channel_layout(m_decoder->channels),
            m_decoder->sample_fmt,
            m_decoder->sample_rate,
            0, // log level offset
            nullptr // logging context
        ));
    if (!m_resampler) {
        std::cerr << "Failed to create resampler" << std::endl;
        return false;
    }
    ret = swr_init(m_resampler.get());
    if (ret < 0) {
        std::cerr << "Could not init resampler for " << filePath << ": " << ffmpegError(ret);
        return false;
    }

    m_packet.reset(av_packet_alloc());
    m_frame.reset(av_frame_alloc());

    // hlen chosen arbitrarily, it controls the quality
    //m_resampler.setup(m_decoder->sample_rate, 16000, m_decoder->channels, /*hlen=*/32);
    //m_resampler.inp_count = 2048;
    return true;
}

void Fingerprinter::formatDeleter(AVFormatContext *fmt)
{
    if (!fmt) {
        return;
    }
    avformat_close_input(&fmt);
    avformat_free_context(fmt);
}

void Fingerprinter::codecDeleter(AVCodecContext *codec)
{
    if (!codec) {
        return;
    }
    avcodec_free_context(&codec);
}

void Fingerprinter::frameDeleter(AVFrame *frame)
{
    if (!frame) {
        return;
    }
    av_frame_free(&frame);
}

void Fingerprinter::threadLoop()
{
    std::lock_guard<std::mutex> lock(m_quitMutex);

    std::array<std::array<int16_t, 1025>, 256>  spreadFFTs;
    for (std::array<int16_t, 1025> &fft : spreadFFTs) {
        fft.fill(0);
    }
    size_t currSpreadFFT = 0;

    std::vector<int16_t> buffer;
    while (m_signatures.is_enabled()) {
        // decode
        av_packet_unref(m_packet.get());
        int ret = av_read_frame(m_format.get(), m_packet.get());

        if (ret == AVERROR(EAGAIN)) {
            continue;
        }
        if (ret < 0) {
            std::cerr << "could not read frame " << ffmpegError(ret);
            break;
        }

        if (m_packet->stream_index != m_audioStream) {
            continue;
        }

        ret = avcodec_send_packet(m_decoder.get(), m_packet.get());
        if (ret < 0) {
            std::cerr << "Error while trying to send packet " << ffmpegError(ret);
            break;
        }

        ret = avcodec_receive_frame(m_decoder.get(), m_frame.get());
        if (ret == AVERROR(EAGAIN)) {
            continue;
        }
        if (ret < 0) {
            std::cerr << "could not receive frame " << ffmpegError(ret);
            break;
        }
        int dst_nb_samples = av_rescale_rnd(swr_get_delay(m_resampler.get(), m_decoder->sample_rate) +
                                        m_frame->nb_samples, 16000, m_decoder->sample_rate, AV_ROUND_UP);
        buffer.resize(dst_nb_samples);

        unsigned char *ptr = (unsigned char*)buffer.data();
        ret = swr_convert(m_resampler.get(),
                &ptr,
                buffer.size(),
                (const uint8_t **)m_frame->extended_data,
                m_frame->nb_samples
            );
        if (ret < 0) {
            std::cerr << "error resampling " << ffmpegError(ret) << std::endl;
            break;
        }
        for (const int16_t sample : buffer) {
            m_dft.update(sample);
        }

        // Not enough samples yet
        if (!m_dft.is_data_valid()) {
            continue;
        }
        std::vector<int16_t> reals;
        for (std::complex<int16_t> val : m_dft.dft) {
            const float real = val.real() * val.real() + val.imag() * val.imag();
            reals.push_back(std::max(real / (1 << 17), 0.0000000001f));
        }

        // frequency peak spreading
        for (size_t i=0; i<reals.size() - 3; i++) {
            reals[i] = std::max(reals[i], std::max(reals[i+1], reals[i+2]));
        }
        // time spreading
        for (size_t i=0; i<reals.size() - 3; i++) {
            for (int count = 6; count>=1; count /= 2) {
                const int index = (currSpreadFFT - count) % spreadFFTs.size();
                spreadFFTs[index] = std::max(spreadFFTs[index][i], reals[i]);
            }
        }

        currSpreadFFT++;
        if (currSpreadFFT < 46) {
            continue;
        }
        std::array<int16_t, 1025> &hist = spreadFFTs[(currSpreadFFT - 1) % spreadFFTs.size()];
        for (size_t bin = 46 + 10; bin < 1014; bin++) {
            if (hist[bin] < 1./64. || hist[bin-4]) {
                continue;
            }
        }

        Fingerprint signature;
        signature.m_valid = true;
        m_signatures.enqueue(std::move(signature));
    }
}

std::string Fingerprinter::ffmpegError(const int ret)
{
    char errbuf[1024];
    av_strerror(ret, errbuf, 1024);
    return std::string(errbuf);
}
