#pragma once

#include <opencv2/core.hpp>
#include <string>
#include <cstdint>

extern "C" {
struct AVFormatContext;
struct AVCodecContext;
struct SwsContext;
struct AVPacket;
struct AVFrame;
}

namespace wmr {

class VideoReader {
public:
    VideoReader();
    ~VideoReader();

    // Non-copyable, non-movable
    VideoReader(const VideoReader&) = delete;
    VideoReader& operator=(const VideoReader&) = delete;

    bool open(const std::string& path);
    bool next_frame(cv::Mat& out);
    bool seek(int64_t frame_index);
    void close();

    int64_t frame_count() const { return frame_count_; }
    double fps() const { return fps_; }
    int width() const { return width_; }
    int height() const { return height_; }
    AVFormatContext* format_context() const { return fmt_ctx_; }
    int video_stream_index() const { return video_stream_idx_; }

private:
    bool decode_next_frame(cv::Mat& out);
    void convert_to_bgr(AVFrame* frame, cv::Mat& out);

    AVFormatContext* fmt_ctx_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    int video_stream_idx_ = -1;
    int64_t frame_count_ = 0;
    double fps_ = 0.0;
    int width_ = 0;
    int height_ = 0;
    bool eof_ = false;
};

} // namespace wmr
