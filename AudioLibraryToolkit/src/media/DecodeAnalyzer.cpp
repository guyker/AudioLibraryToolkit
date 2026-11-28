#include "DecodeAnalyzer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

extern "C" {
#include <libavutil/md5.h>
#include <libavutil/sha.h>
}

#include "CommonUtils.h"
#include "FFmpegWrapper.h"
#include "PlatformUtils.h"
#include "FFmpeg.h"

namespace {

struct FormatCloser
{
    void operator()(AVFormatContext* context) const
    {
        if (context != nullptr) avformat_close_input(&context);
    }
};

struct CodecCloser
{
    void operator()(AVCodecContext* context) const
    {
        avcodec_free_context(&context);
    }
};

struct PacketCloser
{
    void operator()(AVPacket* packet) const
    {
        av_packet_free(&packet);
    }
};

struct FrameCloser
{
    void operator()(AVFrame* frame) const
    {
        av_frame_free(&frame);
    }
};

using FormatPtr = std::unique_ptr<AVFormatContext, FormatCloser>;
using CodecPtr = std::unique_ptr<AVCodecContext, CodecCloser>;
using PacketPtr = std::unique_ptr<AVPacket, PacketCloser>;
using FramePtr = std::unique_ptr<AVFrame, FrameCloser>;

std::string ffmpegError(int result)
{
    std::array<char, AV_ERROR_MAX_STRING_SIZE> message{};
    av_strerror(result, message.data(), message.size());
    return message.data();
}

std::string hexDigest(const std::uint8_t* bytes, std::size_t size)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (std::size_t index = 0; index < size; ++index) {
        result.push_back(digits[bytes[index] >> 4]);
        result.push_back(digits[bytes[index] & 0x0f]);
    }
    return result;
}

constexpr int strictErrorChecks = AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_BUFFER | AV_EF_EXPLODE;

int openInput(const std::filesystem::path& path, FormatPtr& format, const char* expected = nullptr)
{
#ifdef _WIN32
    const auto utf8Path = PlatformUtils::WideToUTF8(PlatformUtils::WindowsLongPath(path));
#else
    const auto utf8Path = path.string();
#endif
    AVFormatContext* raw = nullptr;
    if (expected) {
        raw = avformat_alloc_context();
        if (!raw) return AVERROR(ENOMEM);
        raw->error_recognition = strictErrorChecks;
        // Probe the bytes; forcing the expected demuxer would hide a container mismatch.
        raw->format_whitelist = av_strdup(expected);
        raw->protocol_whitelist = av_strdup("file");
        if (!raw->format_whitelist || !raw->protocol_whitelist) {
            avformat_free_context(raw);
            return AVERROR(ENOMEM);
        }
    }
    const int result = avformat_open_input(&raw, utf8Path.c_str(), nullptr, nullptr);
    format.reset(raw);
    return result;
}

// Raw demuxers can probe past arbitrary leading junk. Inspect the same opened
// input, allowing leading ID3v2 tags but not a resync to an unrelated stream.
bool hasNativeHeader(AVIOContext* input, bool flac)
{
    if (!input) return false;
    const auto saved = avio_tell(input);
    if (saved < 0 || avio_seek(input, 0, SEEK_SET) != 0) return false;
    bool matched = false;
    std::array<std::uint8_t, 10> header{};
    while (avio_read(input, header.data(), static_cast<int>(header.size())) == static_cast<int>(header.size())) {
        if (header[0] != 'I' || header[1] != 'D' || header[2] != '3') {
            matched = flac
                ? (header[0] == 'f' && header[1] == 'L' && header[2] == 'a' && header[3] == 'C')
                : (header[0] == 0xff && (header[1] & 0xe6) == 0xe2 && (header[1] & 0x18) != 8);
            break;
        }
        if (header[3] < 2 || header[3] > 4 || header[4] == 255 ||
            (header[3] != 4 && (header[5] & 0x10)) ||
            ((header[6] | header[7] | header[8] | header[9]) & 0x80)) break;
        const auto size = (header[6] << 21) | (header[7] << 14) | (header[8] << 7) | header[9];
        if (avio_skip(input, size + ((header[5] & 0x10) ? 10 : 0)) < 0) break;
    }
    return avio_seek(input, saved, SEEK_SET) == saved && matched;
}

struct ChannelStats
{
    long double sum{ 0 };
    long double sumSquares{ 0 };
    double peak{ 0 };
    std::uint64_t clipped{ 0 };
    std::uint64_t zeroCrossings{ 0 };
    double previous{ 0 };
    bool hasPrevious{ false };
};

std::optional<double> parseFiniteDouble(const char* value)
{
    if (value == nullptr) return std::nullopt;
    const std::string text(value);
    double parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        !std::isfinite(parsed)) {
        return std::nullopt;
    }
    return parsed;
}

class EbuR128Filter
{
public:
    ~EbuR128Filter()
    {
        avfilter_graph_free(&_graph);
    }

    bool Initialize(const AVFrame& frame, std::string& error)
    {
        const auto* formatName = av_get_sample_fmt_name(
            static_cast<AVSampleFormat>(frame.format));
        if (formatName == nullptr || frame.sample_rate <= 0 ||
            frame.ch_layout.nb_channels <= 0) {
            error = "Decoder format is not supported by the EBU R128 filter.";
            return false;
        }
        std::array<char, 128> layout{};
        int result = av_channel_layout_describe(&frame.ch_layout, layout.data(), layout.size());
        if (result < 0) {
            error = "Cannot describe decoder channel layout: " + ffmpegError(result);
            return false;
        }
        _graph = avfilter_graph_alloc();
        if (_graph == nullptr) {
            error = "Cannot allocate the EBU R128 filter graph.";
            return false;
        }
        const std::string arguments = "time_base=1/" + std::to_string(frame.sample_rate) +
            ":sample_rate=" + std::to_string(frame.sample_rate) +
            ":sample_fmt=" + formatName +
            ":channels=" + std::to_string(frame.ch_layout.nb_channels) +
            ":channel_layout=" + layout.data();
        result = avfilter_graph_create_filter(&_source,
            avfilter_get_by_name("abuffer"), "source", arguments.c_str(), nullptr, _graph);
        if (result < 0) {
            error = "Cannot initialize the EBU R128 filter source: " + ffmpegError(result);
            return false;
        }
        AVFilterContext* ebur128 = nullptr;
        result = avfilter_graph_create_filter(&ebur128,
            avfilter_get_by_name("ebur128"), "ebur128",
            "peak=true:metadata=1:framelog=quiet", nullptr, _graph);
        if (result < 0) {
            error = "Cannot initialize the EBU R128 filter: " + ffmpegError(result);
            return false;
        }
        result = avfilter_graph_create_filter(&_sink,
            avfilter_get_by_name("abuffersink"), "sink", nullptr, nullptr, _graph);
        if (result < 0) {
            error = "Cannot initialize the EBU R128 filter sink: " + ffmpegError(result);
            return false;
        }
        result = avfilter_link(_source, 0, ebur128, 0);
        if (result >= 0) result = avfilter_link(ebur128, 0, _sink, 0);
        if (result >= 0) result = avfilter_graph_config(_graph, nullptr);
        if (result < 0) {
            error = "Cannot configure the EBU R128 filter graph: " + ffmpegError(result);
            return false;
        }
        _initialized = true;
        return true;
    }

    bool AddFrame(const AVFrame& frame, AudioSignalResult& signal, std::string& error)
    {
        const int result = av_buffersrc_add_frame_flags(
            _source, const_cast<AVFrame*>(&frame), AV_BUFFERSRC_FLAG_KEEP_REF);
        if (result < 0) {
            error = "Cannot submit decoded samples to the EBU R128 filter: " + ffmpegError(result);
            return false;
        }
        return Drain(signal, error);
    }

    bool Finish(AudioSignalResult& signal, std::string& error)
    {
        if (!_initialized) return false;
        const int result = av_buffersrc_add_frame_flags(_source, nullptr, 0);
        if (result < 0 && result != AVERROR_EOF) {
            error = "Cannot flush the EBU R128 filter: " + ffmpegError(result);
            return false;
        }
        return Drain(signal, error);
    }

private:
    bool Drain(AudioSignalResult& signal, std::string& error)
    {
        FramePtr filtered(av_frame_alloc());
        if (!filtered) {
            error = "Cannot allocate EBU R128 output frame.";
            return false;
        }
        while (true) {
            const int result = av_buffersink_get_frame(_sink, filtered.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
            if (result < 0) {
                error = "Cannot read EBU R128 output: " + ffmpegError(result);
                return false;
            }
            static constexpr std::array keys{
                std::pair{"lavfi.r128.I", "integratedLufs"},
                std::pair{"lavfi.r128.LRA", "loudnessRangeLU"},
                std::pair{"lavfi.r128.true_peak", "truePeakDbtp"}
            };
            for (const auto& [sourceKey, targetKey] : keys) {
                if (auto* entry = av_dict_get(filtered->metadata, sourceKey, nullptr, 0)) {
                    if (const auto value = parseFiniteDouble(entry->value)) {
                        signal.measurements[targetKey] = *value;
                    }
                }
            }
            av_frame_unref(filtered.get());
        }
    }

    AVFilterGraph* _graph{ nullptr };
    AVFilterContext* _source{ nullptr };
    AVFilterContext* _sink{ nullptr };
    bool _initialized{ false };
};

double readSample(const AVFrame& frame, AVSampleFormat format, int channel, int sample)
{
    const auto* data = frame.extended_data;
    const bool planar = av_sample_fmt_is_planar(format) != 0;
    const auto baseFormat = av_get_packed_sample_fmt(format);
    const int plane = planar ? channel : 0;
    const int channels = frame.ch_layout.nb_channels;
    const auto index = planar ? sample : static_cast<std::ptrdiff_t>(sample) * channels + channel;
    switch (baseFormat) {
    case AV_SAMPLE_FMT_U8:
        return (static_cast<const std::uint8_t*>(static_cast<const void*>(data[plane]))[index] - 128) / 128.0;
    case AV_SAMPLE_FMT_S16:
        return static_cast<const std::int16_t*>(static_cast<const void*>(data[plane]))[index] / 32768.0;
    case AV_SAMPLE_FMT_S32:
        return static_cast<const std::int32_t*>(static_cast<const void*>(data[plane]))[index] / 2147483648.0;
    case AV_SAMPLE_FMT_S64:
        return static_cast<double>(static_cast<const std::int64_t*>(static_cast<const void*>(data[plane]))[index] /
            9223372036854775808.0L);
    case AV_SAMPLE_FMT_FLT:
        return static_cast<const float*>(static_cast<const void*>(data[plane]))[index];
    case AV_SAMPLE_FMT_DBL:
        return static_cast<const double*>(static_cast<const void*>(data[plane]))[index];
    default:
        return std::numeric_limits<double>::quiet_NaN();
    }
}

std::optional<std::int64_t> readIntegerSample(const std::uint8_t* plane, std::ptrdiff_t index,
    AVSampleFormat packedFormat)
{
    switch (packedFormat) {
    case AV_SAMPLE_FMT_U8:
        return static_cast<std::int64_t>(plane[index]) - 128;
    case AV_SAMPLE_FMT_S16:
        return reinterpret_cast<const std::int16_t*>(plane)[index];
    case AV_SAMPLE_FMT_S32:
        return reinterpret_cast<const std::int32_t*>(plane)[index];
    case AV_SAMPLE_FMT_S64:
        return reinterpret_cast<const std::int64_t*>(plane)[index];
    default:
        return std::nullopt;
    }
}

bool isFullScaleIntegerSample(std::int64_t sample, int sampleBits, int declaredBits)
{
    const int usedBits = declaredBits > 0 && declaredBits <= sampleBits
        ? declaredBits : sampleBits;
    const int shift = sampleBits - usedBits;
    const std::int64_t normalized = shift == 0 ? sample : sample >> shift;
    if (usedBits == 64) {
        return normalized == (std::numeric_limits<std::int64_t>::min)() ||
            normalized == (std::numeric_limits<std::int64_t>::max)();
    }
    const auto minimum = -(std::int64_t{ 1 } << (usedBits - 1));
    const auto maximum = (std::int64_t{ 1 } << (usedBits - 1)) - 1;
    return normalized == minimum || normalized == maximum;
}

void updateHash(AVSHA* hash, const std::uint8_t* data, std::size_t size)
{
    while (size > 0) {
        const auto chunk = static_cast<unsigned int>(std::min<std::size_t>(
            size, std::numeric_limits<unsigned int>::max()));
        av_sha_update(hash, data, chunk);
        data += chunk;
        size -= chunk;
    }
}

void appendLittleEndian(std::vector<std::uint8_t>& bytes, std::uint64_t value, int bits)
{
    for (int bit = 0; bit < bits; bit += 8) {
        bytes.push_back(static_cast<std::uint8_t>(value >> bit));
    }
}

bool appendFrameSamples(const AVFrame& frame, AVSampleFormat format,
    AVSHA* pcmHash, AVMD5* flacHash, int declaredBits,
    std::vector<ChannelStats>& stats, std::uint64_t& sampleCount,
    std::uint64_t& integerOrMask, int& integerSampleBits,
    std::uint64_t& firstAudibleSample, std::uint64_t& lastAudibleSample,
    long double& stereoProductSum, bool analyzeSignal,
    std::string& error)
{
    const int channels = frame.ch_layout.nb_channels;
    const int bytesPerSample = av_get_bytes_per_sample(format);
    if (channels <= 0 || channels != static_cast<int>(stats.size()) || bytesPerSample <= 0) {
        error = "Decoder returned an unsupported sample format or channel layout.";
        return false;
    }
    const bool planar = av_sample_fmt_is_planar(format) != 0;
    const auto baseFormat = av_get_packed_sample_fmt(format);
    const bool integerFormat = baseFormat == AV_SAMPLE_FMT_U8 ||
        baseFormat == AV_SAMPLE_FMT_S16 || baseFormat == AV_SAMPLE_FMT_S32 ||
        baseFormat == AV_SAMPLE_FMT_S64;
    if (integerFormat) integerSampleBits = bytesPerSample * 8;
    std::vector<std::uint8_t> interleaved;
    std::vector<std::uint8_t> flacPcmBytes;
    const auto totalBytes = static_cast<std::size_t>(frame.nb_samples) *
        static_cast<std::size_t>(channels) * static_cast<std::size_t>(bytesPerSample);
    interleaved.reserve(totalBytes);
    if (flacHash != nullptr && declaredBits > 0) {
        flacPcmBytes.reserve(static_cast<std::size_t>(frame.nb_samples) *
            static_cast<std::size_t>(channels) *
            static_cast<std::size_t>((declaredBits + 7) / 8));
    }
    for (int sample = 0; sample < frame.nb_samples; ++sample) {
        std::array<double, 2> stereoValues{};
        bool audible = false;
        for (int channel = 0; channel < channels; ++channel) {
            const auto* plane = frame.extended_data[planar ? channel : 0];
            const auto index = planar ? sample : static_cast<std::ptrdiff_t>(sample) * channels + channel;
            if (analyzeSignal) {
                const double value = readSample(frame, format, channel, sample);
                if (!std::isfinite(value)) {
                    error = "Decoder returned a non-finite or unsupported sample.";
                    return false;
                }
                auto& channelStats = stats[static_cast<std::size_t>(channel)];
                if (channel < static_cast<int>(stereoValues.size())) {
                    stereoValues[static_cast<std::size_t>(channel)] = value;
                }
                audible = audible || std::abs(value) >= 0.001;
                channelStats.sum += value;
                channelStats.sumSquares += static_cast<long double>(value) * value;
                channelStats.peak = std::max(channelStats.peak, std::abs(value));
                if (integerFormat) {
                    const auto integerSample = readIntegerSample(plane, index, baseFormat);
                    if (!integerSample) {
                        error = "Decoder returned an unsupported integer sample format.";
                        return false;
                    }
                    if (isFullScaleIntegerSample(
                        *integerSample, bytesPerSample * 8, declaredBits)) {
                        ++channelStats.clipped;
                    }
                }
                else if (std::abs(value) >= 1.0) {
                    ++channelStats.clipped;
                }
                if (channelStats.hasPrevious &&
                    ((channelStats.previous < 0 && value >= 0) ||
                        (channelStats.previous > 0 && value <= 0))) {
                    ++channelStats.zeroCrossings;
                }
                channelStats.previous = value;
                channelStats.hasPrevious = true;
            }

            const auto* source = plane + index * bytesPerSample;
            interleaved.insert(interleaved.end(), source, source + bytesPerSample);

            if (flacHash != nullptr && integerFormat && declaredBits > 0 &&
                declaredBits <= integerSampleBits) {
                const auto raw = readIntegerSample(plane, index, baseFormat);
                if (!raw) continue;
                const int shift = integerSampleBits - declaredBits;
                const std::int64_t normalized = shift == 0 ? *raw : (*raw >> shift);
                integerOrMask |= static_cast<std::uint64_t>(normalized);
                appendLittleEndian(flacPcmBytes, static_cast<std::uint64_t>(normalized), declaredBits);
            }
        }
        if (analyzeSignal && channels >= 2) {
            stereoProductSum += static_cast<long double>(stereoValues[0]) * stereoValues[1];
        }
        if (analyzeSignal && audible) {
            const auto absoluteSample = sampleCount + static_cast<std::uint64_t>(sample);
            firstAudibleSample = std::min(firstAudibleSample, absoluteSample);
            lastAudibleSample = absoluteSample;
        }
    }
    updateHash(pcmHash, interleaved.data(), interleaved.size());
    if (!flacPcmBytes.empty()) {
        av_md5_update(flacHash, flacPcmBytes.data(), flacPcmBytes.size());
    }
    sampleCount += static_cast<std::uint64_t>(frame.nb_samples);
    return true;
}

// FLAC hashes interleaved signed samples sign-extended to the next whole byte.
// The MP3 presentation instead preserves every bit of its native float output.
const char* serializeFrame(const AVFrame& frame, AVSampleFormat fmt, int bits, bool flac,
    std::vector<std::uint8_t>& bytes)
{
    const auto packed = av_get_packed_sample_fmt(fmt);
    const int shift = av_get_bytes_per_sample(fmt) * 8 - bits;
    if (shift < 0 || (flac ? (packed != AV_SAMPLE_FMT_S16 && packed != AV_SAMPLE_FMT_S32) :
        packed != AV_SAMPLE_FMT_FLT)) return "Unsupported decoded PCM representation.";
    const bool planar = av_sample_fmt_is_planar(fmt) != 0;
    const int channels = frame.ch_layout.nb_channels;
    if (!frame.extended_data) return "Missing decoded sample data.";
    for (int plane = 0; plane < (planar ? channels : 1); ++plane) {
        if (!frame.extended_data[plane]) return "Missing decoded sample plane.";
    }
    bytes.clear();
    bytes.reserve(static_cast<std::size_t>(frame.nb_samples) * channels * ((bits + 7) / 8));
    for (int i = 0; i < frame.nb_samples; ++i) {
        for (int ch = 0; ch < channels; ++ch) {
            const auto* plane = frame.extended_data[planar ? ch : 0];
            const int index = planar ? i : i * channels + ch;
            if (flac) {
                const auto value = *readIntegerSample(plane, index, packed);
                if ((value & ((std::int64_t{1} << shift) - 1)) != 0) {
                    return "Decoded precision exceeds the declared FLAC width.";
                }
                appendLittleEndian(bytes, static_cast<std::uint64_t>(value >> shift), bits);
            }
            else {
                const float value = reinterpret_cast<const float*>(plane)[index];
                if (!std::isfinite(value)) return "Decoder returned a non-finite MP3 sample.";
                appendLittleEndian(bytes, std::bit_cast<std::uint32_t>(value), 32);
            }
        }
    }
    return nullptr;
}

void fail(IntegrityResult& integrity, MediaRecord& record,
    std::string message, std::optional<std::int64_t> position = std::nullopt)
{
    integrity.status = "failed";
    integrity.decodeErrors = 1;
    integrity.firstErrorPosition = position;
    integrity.error = std::move(message);
    record.decodeSupport = "failed";
    if (record.scanTier == "analysis") {
        record.signal.status = "failed";
        record.analysisStatus = "failed";
    }
}

// Drain after every packet so a successful send cannot leave an error hidden at EOF.
// The scan keeps its historical diagnostic position; repair only attributes failures
// to an actual packet, and uses a synchronous native decoder without delayed frames.
struct DecodeFailure
{
    std::int64_t position = -1;
    bool invalidBitstream = false;
};

std::string decodePackets(AVFormatContext& format, AVCodecContext& codec, int streamIndex,
    AVPacket& packet, AVFrame& frame, bool strict, DecodeFailure& failure, auto&& validatePacket, auto&& accept)
{
    std::string error;
    auto receive = [&]() {
        int result;
        while ((result = avcodec_receive_frame(&codec, &frame)) >= 0) {
            if (strict && (frame.decode_error_flags != 0 ||
                (frame.flags & (AV_FRAME_FLAG_CORRUPT | AV_FRAME_FLAG_DISCARD)) != 0)) {
                failure.invalidBitstream = frame.decode_error_flags != 0 || (frame.flags & AV_FRAME_FLAG_CORRUPT);
                error = "Decoder returned a corrupt, concealed or discarded audio frame.";
            }
            else {
                accept(frame, error);
            }
            av_frame_unref(&frame);
            if (!error.empty()) return AVERROR_INVALIDDATA;
        }
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
            failure.invalidBitstream = result == AVERROR_INVALIDDATA;
            error = "Audio decode failed: " + ffmpegError(result);
        }
        return result;
    };

    int result;
    while ((result = av_read_frame(&format, &packet)) >= 0) {
        if (packet.stream_index == streamIndex) {
            failure.position = packet.pos >= 0 || strict ? packet.pos : packet.pts;
            if (strict && ((packet.flags & (AV_PKT_FLAG_CORRUPT | AV_PKT_FLAG_DISCARD)) != 0 ||
                packet.size <= 0)) {
                failure.invalidBitstream = (packet.flags & AV_PKT_FLAG_CORRUPT) != 0;
                return "Demuxer returned a corrupt, discarded or empty audio packet.";
            }
            if (!validatePacket(packet)) return "Incomplete or unsupported native audio packet.";
            result = avcodec_send_packet(&codec, &packet);
            if (result == AVERROR(EAGAIN)) {
                if (strict) failure.position = -1; // A pending frame predates this packet.
                receive();
                if (!error.empty()) return error;
                if (strict) failure.position = packet.pos;
                result = avcodec_send_packet(&codec, &packet);
            }
            if (result < 0) {
                failure.invalidBitstream = result == AVERROR_INVALIDDATA;
                return "Decoder rejected an audio packet: " + ffmpegError(result);
            }
            result = receive();
            if (!error.empty()) return error;
            if (strict && result != AVERROR(EAGAIN)) return "Decoder ended before input EOF.";
        }
        av_packet_unref(&packet);
    }
    if (strict) failure.position = -1;
    if (result != AVERROR_EOF) return "Audio packet read failed: " + ffmpegError(result);
    if (strict && format.pb && format.pb->error < 0 && format.pb->error != AVERROR_EOF) {
        return "Audio input read failed: " + ffmpegError(format.pb->error);
    }
    result = avcodec_send_packet(&codec, nullptr);
    if (result < 0 && result != AVERROR_EOF) {
        failure.invalidBitstream = result == AVERROR_INVALIDDATA;
        return "Decoder flush failed: " + ffmpegError(result);
    }
    result = receive();
    if (strict && error.empty() && result != AVERROR_EOF) return "Decoder did not drain to EOF.";
    return error;
}

// A parser may concatenate or resynchronize packets. On failure verify its
// reported position/size against the actual bytes of this same open input.
bool matchesSourcePacket(AVIOContext* input, const AVPacket& packet, std::int64_t position)
{
    if (!input || position < 0 || position != packet.pos || packet.size <= 0 || !packet.data ||
        position > INT64_MAX - packet.size || avio_seek(input, position, SEEK_SET) != position) {
        return false;
    }
    std::array<std::uint8_t, 8192> bytes{};
    for (int i = 0; i < packet.size;) {
        const int n = std::min(packet.size - i, static_cast<int>(bytes.size()));
        if (avio_read(input, bytes.data(), n) != n ||
            !std::equal(bytes.data(), bytes.data() + n, packet.data + i)) return false;
        i += n;
    }
    return true;
}

}

std::optional<std::string> DecodeAnalyzer::FileSha256(const std::filesystem::path& path)
{
#ifdef _WIN32
    std::ifstream input(std::filesystem::path(PlatformUtils::WindowsLongPath(path)), std::ios::binary);
#else
    std::ifstream input(path, std::ios::binary);
#endif
    if (!input) return std::nullopt;
    std::unique_ptr<AVSHA, decltype(&av_free)> hash(av_sha_alloc(), &av_free);
    if (!hash || av_sha_init(hash.get(), 256) < 0) return std::nullopt;
    std::array<std::uint8_t, 64 * 1024> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()),
            static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) av_sha_update(hash.get(), buffer.data(), static_cast<unsigned int>(count));
    }
    if (!input.eof() || input.bad()) return std::nullopt;
    std::array<std::uint8_t, 32> digest{};
    av_sha_final(hash.get(), digest.data());
    return hexDigest(digest.data(), digest.size());
}

RepairPcmProof DecodeAnalyzer::Prove(const std::filesystem::path& path, bool flac)
{
    const auto failure = [](std::string message) {
        RepairPcmProof proof;
        proof.error = std::move(message);
        return proof;
    };
    FFmpeg::ScopedLogCapture logCapture(path, "repair-decode");
    const char* nativeFormat = flac ? "flac" : "mp3";
    FormatPtr format;
    int result = openInput(path, format, nativeFormat);
    if (result < 0) return failure("Cannot open native audio input: " + ffmpegError(result));
    if (!hasNativeHeader(format->pb, flac)) return failure("Cannot verify the native audio header.");
    result = avformat_find_stream_info(format.get(), nullptr);
    if (result < 0) return failure("Cannot read audio stream information: " + ffmpegError(result));
    int streamIndex = -1;
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        const auto& stream = *format->streams[i];
        if (stream.codecpar->codec_type == AVMEDIA_TYPE_AUDIO && streamIndex < 0) {
            streamIndex = static_cast<int>(i);
        }
        else if (stream.codecpar->codec_type != AVMEDIA_TYPE_VIDEO ||
            !(stream.disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            return failure("Expected one native audio program with optional attached pictures.");
        }
    }
    if (streamIndex < 0) return failure("No native audio stream was found.");
    const auto& stream = *format->streams[streamIndex];
    const auto& params = *stream.codecpar;
    if (params.codec_id != (flac ? AV_CODEC_ID_FLAC : AV_CODEC_ID_MP3)) {
        return failure("Audio codec does not match the expected native format.");
    }
    const int bits = flac ? params.bits_per_raw_sample : 32;
    const int rate = params.sample_rate;
    const int channels = params.ch_layout.nb_channels;
    if (rate <= 0 || !av_channel_layout_check(&params.ch_layout) ||
        params.ch_layout.order != AV_CHANNEL_ORDER_NATIVE || channels > (flac ? 8 : 2) ||
        (flac && (bits < 4 || bits > 32 || params.extradata_size != 34))) {
        return failure("Unverified audio precision, sample rate or channel layout.");
    }
    const auto channelMask = params.ch_layout.u.mask;
    const AVCodec* decoder = avcodec_find_decoder_by_name(flac ? "flac" : "mp3float");
    if (!decoder) return failure("Expected native audio decoder is unavailable.");
    CodecPtr codec(avcodec_alloc_context3(decoder));
    if (!codec) return failure("Cannot allocate the native audio decoder.");
    result = avcodec_parameters_to_context(codec.get(), &params);
    if (result < 0) return failure("Cannot configure the native audio decoder: " + ffmpegError(result));
    codec->thread_count = 1;
    codec->err_recognition = strictErrorChecks;
    codec->error_concealment = 0;
    codec->pkt_timebase = stream.time_base;
    result = avcodec_open2(codec.get(), decoder, nullptr);
    if (result < 0) return failure("Cannot open the native audio decoder: " + ffmpegError(result));

    FramePtr frame(av_frame_alloc());
    PacketPtr packet(av_packet_alloc());
    std::unique_ptr<AVSHA, decltype(&av_free)> sha(av_sha_alloc(), &av_free);
    std::unique_ptr<AVMD5, decltype(&av_free)> md5(flac ? av_md5_alloc() : nullptr, &av_free);
    if (!packet || !frame || !sha || (flac && !md5) || av_sha_init(sha.get(), 256) < 0) {
        return failure("Unable to allocate PCM proof state.");
    }
    if (md5) av_md5_init(md5.get());
    std::vector<std::uint8_t> bytes;
    AVSampleFormat firstFormat = AV_SAMPLE_FMT_NONE;
    std::uint64_t samples = 0;
    DecodeFailure rejected;
    // The MP3 decoder can return complete-looking PCM from a short final packet by
    // reading padding. Require libav's independent Layer III parser to emit a whole
    // frame from the packet *without* its EOF flush (which emits truncated data).
    std::unique_ptr<AVCodecParserContext, decltype(&av_parser_close)> parser(
        flac ? nullptr : av_parser_init(AV_CODEC_ID_MP3), &av_parser_close);
    CodecPtr parserCodec(flac ? nullptr : avcodec_alloc_context3(decoder));
    if (!flac && (!parser || !parserCodec)) return failure("Cannot allocate the Layer III packet parser.");
    auto validate = [&](const AVPacket& input) {
        if (flac) return true;
        if (input.size < 4 || (input.data[1] & 6) != 2) return false;
        std::uint8_t* parsed = nullptr;
        int size = 0;
        const int used = av_parser_parse2(parser.get(), parserCodec.get(), &parsed, &size,
            input.data, input.size, input.pts, input.dts, input.pos);
        return used == input.size && size == input.size && parsed == input.data;
    };
    auto error = decodePackets(*format, *codec, streamIndex, *packet, *frame, true, rejected, validate,
        [&](const AVFrame& decoded, std::string& reason) {
        const auto fmt = static_cast<AVSampleFormat>(decoded.format);
        if ((firstFormat != AV_SAMPLE_FMT_NONE && firstFormat != fmt) || decoded.sample_rate != rate ||
            decoded.ch_layout.order != AV_CHANNEL_ORDER_NATIVE || decoded.ch_layout.u.mask != channelMask ||
            decoded.ch_layout.nb_channels != channels || (flac && codec->bits_per_raw_sample != bits)) {
            reason = "Changed or unsupported decoded PCM interpretation.";
            return;
        }
        firstFormat = fmt;
        if (decoded.nb_samples <= 0 || decoded.nb_samples > (flac ? 65535 : 1152) ||
            samples > UINT64_MAX - decoded.nb_samples) {
            reason = "Invalid decoded sample count.";
            return;
        }
        if (const char* e = serializeFrame(decoded, fmt, bits, flac, bytes)) {
            reason = e;
            return;
        }
        updateHash(sha.get(), bytes.data(), bytes.size());
        if (md5) av_md5_update(md5.get(), bytes.data(), bytes.size());
        samples += static_cast<std::uint64_t>(decoded.nb_samples);
    });
    if (!error.empty()) {
        auto proof = failure(std::move(error));
        proof.invalidBitstream = rejected.invalidBitstream;
        if (matchesSourcePacket(format->pb, *packet, rejected.position)) {
            proof.errorOffset = static_cast<std::uint64_t>(rejected.position);
            proof.errorPacketSize = static_cast<std::uint64_t>(packet->size);
        }
        return proof;
    }
    for (const auto& log : logCapture.TakeLogs()) {
        if (log.level == "error") return failure(log.component + ": " + log.message);
    }
    if (samples == 0) return failure("Decoder produced no audio samples.");
    RepairPcmProof proof;
    proof.bits = bits;
    proof.samples = samples;
    proof.format = std::string(nativeFormat) + ":" + av_get_sample_fmt_name(firstFormat) +
        ":" + std::to_string(bits) + "bit:" + std::to_string(rate) + "Hz:channel-mask=" +
        std::to_string(channelMask) + ":interleaved-le";
    std::array<std::uint8_t, 32> shaBytes{};
    av_sha_final(sha.get(), shaBytes.data());
    proof.pcmSha256 = hexDigest(shaBytes.data(), shaBytes.size());
    if (md5) {
        std::array<std::uint8_t, 16> md5Bytes{};
        av_md5_final(md5.get(), md5Bytes.data());
        proof.flacMd5 = hexDigest(md5Bytes.data(), md5Bytes.size());
    }
    proof.clean = true;
    return proof;
}

static void analyzeAudio(const std::filesystem::path& filePath,
    MediaRecord& record, bool includeSignalAnalysis)
{
    auto& integrity = record.integrity;
    integrity.status = "failed";
    if (const auto digest = DecodeAnalyzer::FileSha256(filePath)) {
        integrity.fileSha256 = *digest;
    }
    else {
        integrity.error = "Unable to read the complete source file for SHA-256.";
    }

    FormatPtr format;
    int result = openInput(filePath, format);
    if (result < 0) {
        fail(integrity, record, "Cannot open audio stream: " + ffmpegError(result));
        return;
    }
    result = avformat_find_stream_info(format.get(), nullptr);
    if (result < 0) {
        fail(integrity, record, "Cannot read audio stream information: " + ffmpegError(result));
        return;
    }
    const int streamIndex = av_find_best_stream(format.get(), AVMEDIA_TYPE_AUDIO,
        -1, -1, nullptr, 0);
    if (streamIndex < 0) {
        fail(integrity, record, "No decodable audio stream was found: " + ffmpegError(streamIndex));
        return;
    }
    const AVStream* stream = format->streams[streamIndex];
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (decoder == nullptr) {
        integrity.status = "unsupported";
        integrity.error = "No decoder is available for this audio codec.";
        record.decodeSupport = "unsupported";
        if (includeSignalAnalysis) {
            record.signal.status = "failed";
            record.analysisStatus = "failed";
        }
        return;
    }
    CodecPtr codec(avcodec_alloc_context3(decoder));
    if (!codec) {
        fail(integrity, record, "Unable to allocate decoder context.");
        return;
    }
    result = avcodec_parameters_to_context(codec.get(), stream->codecpar);
    if (result < 0) {
        fail(integrity, record, "Cannot configure audio decoder: " + ffmpegError(result));
        return;
    }
    codec->err_recognition = strictErrorChecks;
    result = avcodec_open2(codec.get(), decoder, nullptr);
    if (result < 0) {
        fail(integrity, record, "Cannot initialize audio decoder: " + ffmpegError(result));
        return;
    }
    PacketPtr packet(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    auto* rawPcmHash = av_sha_alloc();
    auto* rawFlacHash = av_md5_alloc();
    if (!packet || !frame || rawPcmHash == nullptr || rawFlacHash == nullptr ||
        av_sha_init(rawPcmHash, 256) < 0) {
        if (rawPcmHash != nullptr) av_free(rawPcmHash);
        if (rawFlacHash != nullptr) av_free(rawFlacHash);
        fail(integrity, record, "Unable to allocate decode analysis state.");
        return;
    }
    std::unique_ptr<AVSHA, decltype(&av_free)> pcmHash(rawPcmHash, &av_free);
    std::unique_ptr<AVMD5, decltype(&av_free)> flacHash(rawFlacHash, &av_free);
    av_md5_init(flacHash.get());

    const int channelCount = codec->ch_layout.nb_channels;
    if (channelCount <= 0) {
        fail(integrity, record, "Decoder reported an invalid channel count.");
        return;
    }
    std::vector<ChannelStats> stats(static_cast<std::size_t>(channelCount));
    std::uint64_t sampleCount = 0;
    std::uint64_t integerOrMask = 0;
    int integerSampleBits = 0;
    std::uint64_t firstAudibleSample = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t lastAudibleSample = 0;
    long double stereoProductSum = 0;
    int declaredBits = record.declaredBitsPerSample.value_or(
        stream->codecpar->bits_per_raw_sample);
    const bool isFlac = stream->codecpar->codec_id == AV_CODEC_ID_FLAC;
    const int md5Bits = isFlac ? declaredBits : 0;
    DecodeFailure rejected;
    EbuR128Filter signalFilter;
    bool signalFilterStarted = false;
    bool signalFilterAvailable = includeSignalAnalysis;
    std::string signalFilterError;

    auto decodeError = decodePackets(*format, *codec, streamIndex, *packet, *frame, false,
        rejected, [](const AVPacket&) { return true; }, [&](const AVFrame& decoded, std::string& error) {
        if (includeSignalAnalysis && signalFilterAvailable) {
            if (!signalFilterStarted) {
                signalFilterStarted = true;
                signalFilterAvailable = signalFilter.Initialize(decoded, signalFilterError);
            }
            if (signalFilterAvailable && !signalFilter.AddFrame(decoded, record.signal, signalFilterError)) {
                signalFilterAvailable = false;
            }
        }
        appendFrameSamples(decoded, static_cast<AVSampleFormat>(decoded.format),
            pcmHash.get(), isFlac ? flacHash.get() : nullptr, md5Bits, stats,
            sampleCount, integerOrMask, integerSampleBits, firstAudibleSample,
            lastAudibleSample, stereoProductSum, includeSignalAnalysis, error);
    });
    if (!decodeError.empty()) {
        fail(integrity, record, std::move(decodeError), rejected.position >= 0
            ? std::optional<std::int64_t>(rejected.position) : std::nullopt);
        return;
    }
    if (includeSignalAnalysis && signalFilterAvailable &&
        !signalFilter.Finish(record.signal, signalFilterError)) {
        signalFilterAvailable = false;
    }
    if (sampleCount == 0) {
        fail(integrity, record, "Decoder produced no audio samples.");
        return;
    }

    integrity.status = integrity.fileSha256 ? "ok" : "partial";
    integrity.decodedSamples = sampleCount;
    if (record.totalSamples && *record.totalSamples != sampleCount) {
        integrity.status = "failed";
        integrity.error = "Decoded sample count " + std::to_string(sampleCount) +
            " differs from declared sample count " + std::to_string(*record.totalSamples) + ".";
        record.findings.push_back({
            "SAMPLE_COUNT_MISMATCH", "error", "high", *integrity.error
        });
    }
    std::array<std::uint8_t, 32> pcmDigest{};
    av_sha_final(pcmHash.get(), pcmDigest.data());
    integrity.pcmSha256 = hexDigest(pcmDigest.data(), pcmDigest.size());
    if (isFlac && record.flacMd5 && declaredBits > 0 &&
        declaredBits <= integerSampleBits) {
        std::array<std::uint8_t, 16> decodedMd5{};
        av_md5_final(flacHash.get(), decodedMd5.data());
        const auto decodedDigest = hexDigest(decodedMd5.data(), decodedMd5.size());
        integrity.flacMd5Matches = decodedDigest == *record.flacMd5;
        if (!*integrity.flacMd5Matches) {
            spdlog::error("FLAC MD5 mismatch for '{}': decoded {}, declared {}.",
                CommonUtils::path_to_utf8(filePath), decodedDigest, *record.flacMd5);
            integrity.status = "failed";
            integrity.error = "Decoded PCM does not match the FLAC STREAMINFO MD5.";
        }
    }
    record.decodeSupport = integrity.status == "failed" ? "failed" : "verified";

    if (!includeSignalAnalysis) return;
    auto& signal = record.signal;
    signal.status = "partial";
    signal.method = signalFilterAvailable
        ? "decoded-sample-stats-v1; ebur128"
        : "decoded-sample-stats-v1";
    if (!signalFilterAvailable && !signalFilterError.empty()) {
        spdlog::warn("Signal filter unavailable for '{}': {}",
            CommonUtils::path_to_utf8(filePath), signalFilterError);
    }
    std::uint64_t totalClipped = 0;
    for (std::size_t channel = 0; channel < stats.size(); ++channel) {
        const auto& item = stats[channel];
        const auto prefix = "channel" + std::to_string(channel) + ".";
        const auto count = static_cast<long double>(sampleCount);
        const double rms = std::sqrt(static_cast<double>(item.sumSquares / count));
        const double rmsDb = rms > 0 ? 20.0 * std::log10(rms) :
            -std::numeric_limits<double>::infinity();
        const double peakDb = item.peak > 0 ? 20.0 * std::log10(item.peak) :
            -std::numeric_limits<double>::infinity();
        signal.measurements[prefix + "dcOffset"] =
            static_cast<double>(item.sum / count);
        signal.measurements[prefix + "rmsDbfs"] = std::isfinite(rmsDb) ? rmsDb : -120.0;
        signal.measurements[prefix + "samplePeakDbfs"] =
            std::isfinite(peakDb) ? peakDb : -120.0;
        signal.measurements[prefix + "zeroCrossings"] =
            static_cast<double>(item.zeroCrossings);
        totalClipped += item.clipped;
    }
    if (record.sampleRateHz && *record.sampleRateHz > 0) {
        const auto rate = static_cast<double>(*record.sampleRateHz);
        const auto first = firstAudibleSample == std::numeric_limits<std::uint64_t>::max()
            ? sampleCount : firstAudibleSample;
        const auto lastEnd = firstAudibleSample == std::numeric_limits<std::uint64_t>::max()
            ? 0 : lastAudibleSample + 1;
        signal.leadingSilenceSeconds = static_cast<double>(first) / rate;
        signal.trailingSilenceSeconds =
            static_cast<double>(sampleCount - std::min(sampleCount, lastEnd)) / rate;
        signal.measurements["leadingSilenceSeconds"] = *signal.leadingSilenceSeconds;
        signal.measurements["trailingSilenceSeconds"] = *signal.trailingSilenceSeconds;
        signal.measurements["silenceThresholdDbfs"] = -60.0;
    }
    if (stats.size() == 2) {
        const long double count = static_cast<long double>(sampleCount);
        const long double leftMean = stats[0].sum / count;
        const long double rightMean = stats[1].sum / count;
        const long double covariance = stereoProductSum / count - leftMean * rightMean;
        const long double leftVariance = stats[0].sumSquares / count - leftMean * leftMean;
        const long double rightVariance = stats[1].sumSquares / count - rightMean * rightMean;
        if (leftVariance > 0 && rightVariance > 0) {
            const double correlation = static_cast<double>(covariance /
                std::sqrt(leftVariance * rightVariance));
            signal.measurements["stereoCorrelation"] = correlation;
            if (correlation > 0.999) {
                record.findings.push_back({
                    "DUAL_MONO", "info", "high",
                    "Left/right decoded channel correlation is " + std::to_string(correlation) + "."
                });
            }
            else if (correlation < -0.999) {
                record.findings.push_back({
                    "PHASE_INVERTED_CHANNEL", "warning", "high",
                    "Left/right decoded channel correlation is " + std::to_string(correlation) + "."
                });
            }
        }
    }
    signal.clippedSamples = totalClipped;
    if (!isFlac) {
        signal.usedBitsStatus = "unsupported";
    }
    else if (integerSampleBits > 0 && declaredBits > 0 && declaredBits <= integerSampleBits &&
        integerOrMask != 0) {
        const int trailingZeroBits = std::countr_zero(integerOrMask);
        signal.usedBits = std::max(1, declaredBits - trailingZeroBits);
        signal.usedBitsStatus = "ok";
    }
    else if (integerSampleBits == 0) {
        signal.usedBitsStatus = "unsupported";
    }
    else {
        signal.usedBitsStatus = "unavailable";
    }
    if (signal.usedBits && record.declaredBitsPerSample &&
        *signal.usedBits < *record.declaredBitsPerSample) {
        record.findings.push_back({
            "ZERO_PADDED_HIRES", "warning", "high",
            "Observed integer samples use " + std::to_string(*signal.usedBits) +
                " of " + std::to_string(*record.declaredBitsPerSample) +
                " declared bits; this indicates zero-padded low bits, not effective resolution."
        });
    }
    if (totalClipped > 0) {
        record.findings.push_back({
            "CLIPPED_SAMPLES", "warning", "medium",
            std::to_string(totalClipped) + " decoded samples reached or exceeded full scale."
        });
    }
    const auto leadingSilence = signal.leadingSilenceSeconds.value_or(0.0);
    const auto trailingSilence = signal.trailingSilenceSeconds.value_or(0.0);
    if (leadingSilence >= 0.5 || trailingSilence >= 0.5) {
        record.findings.push_back({
            "LARGE_DIGITAL_SILENCE", "info", "medium",
            "Leading silence: " + std::to_string(leadingSilence) +
                " s; trailing silence: " + std::to_string(trailingSilence) +
                " s at a -60 dBFS threshold."
        });
    }
}

void DecodeAnalyzer::Analyze(const std::filesystem::path& filePath,
    MediaRecord& record, bool includeSignalAnalysis)
{
    FFmpeg::ScopedLogCapture logCapture(filePath, "decode");
    analyzeAudio(filePath, record, includeSignalAnalysis);
    for (const auto& log : logCapture.TakeLogs()) {
        record.diagnostics.push_back({
            log.stage, "ffmpeg", DiagnosticCode::FfmpegLog, ParseDiagnosticSeverity(log.level).value(),
            log.component + ": " + log.message
        });
    }
    if (record.integrity.status == "failed" && record.integrity.error) {
        record.diagnostics.push_back({
            "decode", "ffmpeg", DiagnosticCode::DecodeFailed, DiagnosticSeverity::Error, *record.integrity.error
        });
    }
    else if (record.integrity.status == "unsupported" && record.integrity.error) {
        record.diagnostics.push_back({
            "decode", "ffmpeg", DiagnosticCode::DecodeUnsupported, DiagnosticSeverity::Warning, *record.integrity.error
        });
    }
}
