#include "video_reader.hpp"

#include <spdlog/spdlog.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

namespace wmr {

VideoReader::VideoReader() = default;

VideoReader::~VideoReader() {
    close();
}

bool VideoReader::open(const std::string& path) {
    close();

    // Open input file
    int ret = avformat_open_input(&fmt_ctx_, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        spdlog::error("VideoReader: failed to open '{}': error {}", path, ret);
        return false;
    }

    ret = avformat_find_stream_info(fmt_ctx_, nullptr);
    if (ret < 0) {
        spdlog::error("VideoReader: failed to find stream info: error {}", ret);
        close();
        return false;
    }

    // Find best video stream
    ret = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (ret < 0) {
        spdlog::error("VideoReader: no video stream found: error {}", ret);
        close();
        return false;
    }
    video_stream_idx_ = ret;

    // Get codec parameters and find decoder
    auto* stream = fmt_ctx_->streams[video_stream_idx_];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        spdlog::error("VideoReader: unsupported codec");
        close();
        return false;
    }

    // Allocate codec context
    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        spdlog::error("VideoReader: failed to allocate codec context");
        close();
        return false;
    }

    ret = avcodec_parameters_to_context(codec_ctx_, stream->codecpar);
    if (ret < 0) {
        spdlog::error("VideoReader: failed to copy codec params: error {}", ret);
        close();
        return false;
    }

    // Let FFmpeg auto-detect thread count
    codec_ctx_->thread_count = 0;

    ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        spdlog::error("VideoReader: failed to open codec: error {}", ret);
        close();
        return false;
    }

    // Extract metadata
    width_ = codec_ctx_->width;
    height_ = codec_ctx_->height;

    // Frame rate from stream
    AVRational fr = stream->r_frame_rate;
    if (fr.num > 0 && fr.den > 0) {
        fps_ = static_cast<double>(fr.num) / fr.den;
    } else {
        fps_ = 0.0;
    }

    // Frame count: prefer nb_frames, fall back to duration-based estimate
    if (stream->nb_frames > 0) {
        frame_count_ = stream->nb_frames;
    } else if (fmt_ctx_->duration > 0 && fps_ > 0.0) {
        frame_count_ = static_cast<int64_t>(
            static_cast<double>(fmt_ctx_->duration) / AV_TIME_BASE * fps_);
    } else {
        frame_count_ = 0;
    }

    // Allocate packet and frame
    packet_ = av_packet_alloc();
    if (!packet_) {
        spdlog::error("VideoReader: failed to allocate packet");
        close();
        return false;
    }

    frame_ = av_frame_alloc();
    if (!frame_) {
        spdlog::error("VideoReader: failed to allocate frame");
        close();
        return false;
    }

    eof_ = false;

    spdlog::info("VideoReader: opened '{}' ({}x{}, {:.2f} fps, {} frames)",
                 path, width_, height_, fps_, frame_count_);
    return true;
}

bool VideoReader::next_frame(cv::Mat& out) {
    if (!codec_ctx_ || !fmt_ctx_) {
        return false;
    }
    return decode_next_frame(out);
}

bool VideoReader::decode_next_frame(cv::Mat& out) {
    while (true) {
        int ret = av_read_frame(fmt_ctx_, packet_);
        if (ret < 0) {
            if (ret == AVERROR_EOF || avio_feof(fmt_ctx_->pb)) {
                // Drain decoder
                if (!eof_) {
                    eof_ = true;
                    av_packet_unref(packet_);
                    ret = avcodec_send_packet(codec_ctx_, nullptr);
                    if (ret < 0) {
                        return false;
                    }
                }
            } else {
                spdlog::error("VideoReader: error reading frame: error {}", ret);
                return false;
            }
        } else {
            if (packet_->stream_index != video_stream_idx_) {
                av_packet_unref(packet_);
                continue;
            }
            ret = avcodec_send_packet(codec_ctx_, packet_);
            av_packet_unref(packet_);
            if (ret < 0 && ret != AVERROR(EAGAIN)) {
                spdlog::error("VideoReader: error sending packet: error {}", ret);
                return false;
            }
        }

        ret = avcodec_receive_frame(codec_ctx_, frame_);
        if (ret == 0) {
            convert_to_bgr(frame_, out);
            av_frame_unref(frame_);
            return true;
        } else if (ret == AVERROR_EOF) {
            return false;
        } else if (ret == AVERROR(EAGAIN)) {
            // Need more input
            if (eof_) {
                return false;
            }
            continue;
        } else {
            spdlog::error("VideoReader: error receiving frame: error {}", ret);
            return false;
        }
    }
}

void VideoReader::convert_to_bgr(AVFrame* frame, cv::Mat& out) {
    // Create SwsContext once (or recreate if dimensions change)
    if (!sws_ctx_ || frame->width != width_ || frame->height != height_) {
        if (sws_ctx_) {
            sws_freeContext(sws_ctx_);
        }
        sws_ctx_ = sws_getContext(
            frame->width, frame->height,
            static_cast<AVPixelFormat>(frame->format),
            width_, height_,
            AV_PIX_FMT_BGR24,
            SWS_BILINEAR,
            nullptr, nullptr, nullptr);
        if (!sws_ctx_) {
            spdlog::error("VideoReader: failed to create SwsContext");
            return;
        }
    }

    out.create(height_, width_, CV_8UC3);

    // sws_scale fills the Mat data directly
    uint8_t* dst_ptrs[1] = { out.data };
    int dst_stride[1] = { static_cast<int>(out.step1()) };

    sws_scale(sws_ctx_,
              frame->data, frame->linesize,
              0, frame->height,
              dst_ptrs, dst_stride);
}

bool VideoReader::seek(int64_t frame_index) {
    if (!fmt_ctx_ || video_stream_idx_ < 0) {
        return false;
    }

    if (frame_index < 0) {
        frame_index = 0;
    }

    auto* stream = fmt_ctx_->streams[video_stream_idx_];
    int64_t timestamp = 0;

    // Convert frame index to stream time_base
    if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) {
        timestamp = frame_index *
                    static_cast<int64_t>(stream->time_base.den) *
                    static_cast<int64_t>(stream->avg_frame_rate.den) /
                    (static_cast<int64_t>(stream->time_base.num) *
                     static_cast<int64_t>(stream->avg_frame_rate.num));
    } else if (fps_ > 0.0) {
        // Fallback: use stream time_base directly
        timestamp = static_cast<int64_t>(
            static_cast<double>(frame_index) / fps_ / av_q2d(stream->time_base));
    }

    // Flush decoder before seeking
    if (codec_ctx_) {
        avcodec_flush_buffers(codec_ctx_);
    }

    // Try seeking to the target timestamp
    int ret = av_seek_frame(fmt_ctx_, video_stream_idx_, timestamp, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        // Fallback: use avformat_seek_file for more robust seeking
        int64_t min_ts = (frame_index > 5) ? timestamp - 5 * stream->time_base.den /
                          (stream->avg_frame_rate.num > 0 ? stream->avg_frame_rate.num : 24) : INT64_MIN;
        ret = avformat_seek_file(fmt_ctx_, video_stream_idx_,
                                 min_ts, timestamp, timestamp, AVSEEK_FLAG_BACKWARD);
        if (ret < 0) {
            spdlog::error("VideoReader: seek to frame {} failed: error {}", frame_index, ret);
            return false;
        }
    }

    eof_ = false;
    return true;
}

void VideoReader::close() {
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    if (frame_) {
        av_frame_free(&frame_);
    }
    if (packet_) {
        av_packet_free(&packet_);
    }
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
    }
    if (fmt_ctx_) {
        avformat_close_input(&fmt_ctx_);
    }

    video_stream_idx_ = -1;
    frame_count_ = 0;
    fps_ = 0.0;
    width_ = 0;
    height_ = 0;
    eof_ = false;
}

} // namespace wmr
