#pragma once

#include <opencv2/core.hpp>
#include <string>
#include <cstdint>

extern "C" {
struct AVFormatContext;
struct AVCodecContext;
struct SwsContext;
struct AVStream;
struct AVPacket;
struct AVFrame;
}

namespace wmr {

struct EncodeOptions {
    std::string codec = "libx264";
    int crf = 14;
    std::string preset = "slow";
    std::string profile = "high";
};

class VideoWriter {
public:
    VideoWriter();
    ~VideoWriter();

    // Non-copyable, non-movable
    VideoWriter(const VideoWriter&) = delete;
    VideoWriter& operator=(const VideoWriter&) = delete;

    // Opens output, sets up video encoder. If audio_source is non-empty,
    // reads audio stream info from that file and creates output audio streams
    // BEFORE writing the MP4 header (required for valid MP4).
    bool open(const std::string& path, int width, int height, double fps,
              const EncodeOptions& opts = {},
              const std::string& audio_source = "");
    bool write_frame(const cv::Mat& frame);
    bool copy_audio();
    bool copy_audio_range(double start_sec, double end_sec);
    void close();

private:
    bool setup_audio_streams(const std::string& audio_source);

    AVFormatContext* fmt_ctx_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;
    AVStream* video_stream_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    int64_t pts_counter_ = 0;
    double fps_ = 0.0;
    bool header_written_ = false;
    bool audio_copied_ = false;
    std::string audio_source_path_;

    struct AudioMapping {
        int in_stream_idx;
        int out_stream_idx;
    };
    std::vector<AudioMapping> audio_mappings_;
};

} // namespace wmr
