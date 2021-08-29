#include "Fingerprinter.h"


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

    // hlen chosen arbitrarily, it controls the quality
    m_resampler.setup(m_codec->sample_rate, 16000, m_codec->channels, /*hlen=*/32);
    m_resampler.inp_count = 2048;
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

void Fingerprinter::threadLoop()
{
    std::lock_guard<std::mutex> lock(m_quitMutex);

    float inbuf[1024];

    while (m_signatures.is_enabled()) {
        Fingerprint signature;
        signature.m_valid = true;

        // decode

        { // resample
            size_t num_input_samples = sizeof(inbuf) / (sizeof(float) * num_channels);
            if (num_input_samples * num_channels > buffer.size()) {
                num_input_samples = buffer.size() / num_channels;
            }
            copy(buffer.begin(), buffer.begin() + num_input_samples * num_channels, inbuf);

            vresampler.inp_count = num_input_samples;
            vresampler.inp_data = inbuf;

            int err = vresampler.process();
            assert(err == 0);

            size_t consumed_samples = num_input_samples - vresampler.inp_count;
            total_consumed_samples += consumed_samples;
            buffer.erase(buffer.begin(), buffer.begin() + consumed_samples * num_channels);
        }

        // Not enough samples yet
        if (!m_dft.is_data_valid()) {
            continue;
        }
        std::complex<double> DC_bin = dft.dft[0];

        m_signatures.enqueue(std::move(signature));
    }
}

std::string Fingerprinter::ffmpegError(const int ret)
{
    char errbuf[1024];
    av_strerror(ret, errbuf, 1024);
    return std::string(errbuf);
}
