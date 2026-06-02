#include "video/video_writer.hpp"

#include <spdlog/spdlog.h>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
#include <libavutil/opt.h>
#include <libavutil/rational.h>
#include <libavutil/mathematics.h>
}

namespace wmr {

VideoWriter::VideoWriter() = default;

VideoWriter::~VideoWriter() {
    close();
}

bool VideoWriter::setup_audio_streams(const std::string& audio_source) {
    if (audio_source.empty()) return true;

    AVFormatContext* in_fmt_ctx = nullptr;
    int ret = avformat_open_input(&in_fmt_ctx, audio_source.c_str(), nullptr, nullptr);
    if (ret < 0) {
        spdlog::warn("VideoWriter: cannot open '{}' for audio setup: error {}", audio_source, ret);
        return true; // Non-fatal — proceed without audio
    }

    ret = avformat_find_stream_info(in_fmt_ctx, nullptr);
    if (ret < 0) {
        spdlog::warn("VideoWriter: cannot find stream info for audio: error {}", ret);
        avformat_close_input(&in_fmt_ctx);
        return true;
    }

    for (unsigned int i = 0; i < in_fmt_ctx->nb_streams; ++i) {
        if (in_fmt_ctx->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }

        AVStream* in_stream = in_fmt_ctx->streams[i];
        AVStream* out_stream = avformat_new_stream(fmt_ctx_, nullptr);
        if (!out_stream) {
            spdlog::error("VideoWriter: failed to create audio output stream");
            avformat_close_input(&in_fmt_ctx);
            return false;
        }

        ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
        if (ret < 0) {
            spdlog::error("VideoWriter: failed to copy audio codec params: error {}", ret);
            avformat_close_input(&in_fmt_ctx);
            return false;
        }
        out_stream->codecpar->codec_tag = 0;

        audio_mappings_.push_back({static_cast<int>(i), static_cast<int>(out_stream->index)});
        spdlog::info("VideoWriter: audio stream {} -> output stream {}",
                     i, out_stream->index);
    }

    avformat_close_input(&in_fmt_ctx);
    return true;
}

bool VideoWriter::open(const std::string& path, int width, int height, double fps,
                       const EncodeOptions& opts,
                       const std::string& audio_source) {
    close();

    if (width <= 0 || height <= 0 || fps <= 0.0) {
        spdlog::error("VideoWriter: invalid parameters ({}x{}, {:.2f} fps)", width, height, fps);
        return false;
    }

    fps_ = fps;
    audio_source_path_ = audio_source;

    // Allocate output format context for MP4
    int ret = avformat_alloc_output_context2(&fmt_ctx_, nullptr, "mp4", path.c_str());
    if (ret < 0 || !fmt_ctx_) {
        spdlog::error("VideoWriter: failed to allocate output context: error {}", ret);
        close();
        return false;
    }

    // Find encoder
    const AVCodec* codec = avcodec_find_encoder_by_name(opts.codec.c_str());
    if (!codec) {
        spdlog::error("VideoWriter: encoder '{}' not found", opts.codec);
        close();
        return false;
    }

    // Create video stream
    video_stream_ = avformat_new_stream(fmt_ctx_, codec);
    if (!video_stream_) {
        spdlog::error("VideoWriter: failed to create video stream");
        close();
        return false;
    }

    // Allocate codec context
    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        spdlog::error("VideoWriter: failed to allocate codec context");
        close();
        return false;
    }

    // Configure codec parameters
    codec_ctx_->width = width;
    codec_ctx_->height = height;
    codec_ctx_->time_base = av_inv_q(av_d2q(fps, 100000));
    codec_ctx_->framerate = av_d2q(fps, 100000);
    codec_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
    codec_ctx_->gop_size = 12;
    codec_ctx_->max_b_frames = 2;

    // Set x264/x265 encoder options
    if (opts.codec.find("264") != std::string::npos ||
        opts.codec.find("265") != std::string::npos) {
        av_opt_set(codec_ctx_->priv_data, "crf", std::to_string(opts.crf).c_str(), 0);
        av_opt_set(codec_ctx_->priv_data, "preset", opts.preset.c_str(), 0);
        if (!opts.profile.empty()) {
            av_opt_set(codec_ctx_->priv_data, "profile", opts.profile.c_str(), 0);
        }
    }

    // Open encoder
    ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        spdlog::error("VideoWriter: failed to open encoder: error {}", ret);
        close();
        return false;
    }

    // Copy codec parameters to stream
    ret = avcodec_parameters_from_context(video_stream_->codecpar, codec_ctx_);
    if (ret < 0) {
        spdlog::error("VideoWriter: failed to copy codec params to stream: error {}", ret);
        close();
        return false;
    }

    // Set up audio streams BEFORE writing the header
    if (!setup_audio_streams(audio_source)) {
        close();
        return false;
    }

    // Open output file
    if (!(fmt_ctx_->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&fmt_ctx_->pb, path.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            spdlog::error("VideoWriter: failed to open output file '{}': error {}", path, ret);
            close();
            return false;
        }
    }

    // Write header — now includes both video and audio streams
    ret = avformat_write_header(fmt_ctx_, nullptr);
    if (ret < 0) {
        spdlog::error("VideoWriter: failed to write header: error {}", ret);
        close();
        return false;
    }
    header_written_ = true;

    // Allocate encoding resources
    packet_ = av_packet_alloc();
    if (!packet_) {
        spdlog::error("VideoWriter: failed to allocate packet");
        close();
        return false;
    }

    frame_ = av_frame_alloc();
    if (!frame_) {
        spdlog::error("VideoWriter: failed to allocate frame");
        close();
        return false;
    }

    frame_->format = AV_PIX_FMT_YUV420P;
    frame_->width = width;
    frame_->height = height;
    ret = av_frame_get_buffer(frame_, 0);
    if (ret < 0) {
        spdlog::error("VideoWriter: failed to allocate frame buffer: error {}", ret);
        close();
        return false;
    }

    // Create SwsContext for BGR24 -> YUV420P conversion
    sws_ctx_ = sws_getContext(
        width, height, AV_PIX_FMT_BGR24,
        width, height, AV_PIX_FMT_YUV420P,
        SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!sws_ctx_) {
        spdlog::error("VideoWriter: failed to create SwsContext");
        close();
        return false;
    }

    pts_counter_ = 0;
    audio_copied_ = false;

    spdlog::info("VideoWriter: opened '{}' ({}x{}, {:.2f} fps, codec={}, crf={}, preset={})",
                 path, width, height, fps, opts.codec, opts.crf, opts.preset);
    return true;
}

bool VideoWriter::write_frame(const cv::Mat& frame) {
    if (!codec_ctx_ || !sws_ctx_ || !frame_) {
        spdlog::error("VideoWriter: not open");
        return false;
    }

    if (frame.empty() || frame.type() != CV_8UC3) {
        spdlog::error("VideoWriter: invalid frame (empty={}, type={})",
                       frame.empty(), frame.type());
        return false;
    }

    // Make frame writable
    int ret = av_frame_make_writable(frame_);
    if (ret < 0) {
        spdlog::error("VideoWriter: failed to make frame writable: error {}", ret);
        return false;
    }

    // Convert BGR24 -> YUV420P
    const uint8_t* src_ptrs[1] = { frame.data };
    int src_stride[1] = { static_cast<int>(frame.step1()) };

    sws_scale(sws_ctx_,
              src_ptrs, src_stride,
              0, frame.rows,
              frame_->data, frame_->linesize);

    // Set PTS
    frame_->pts = pts_counter_++;

    // Send frame to encoder
    ret = avcodec_send_frame(codec_ctx_, frame_);
    if (ret < 0) {
        spdlog::error("VideoWriter: error sending frame: error {}", ret);
        return false;
    }

    // Receive encoded packets
    while (ret >= 0) {
        ret = avcodec_receive_packet(codec_ctx_, packet_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            spdlog::error("VideoWriter: error receiving packet: error {}", ret);
            return false;
        }

        // Rescale PTS from codec time_base to stream time_base
        av_packet_rescale_ts(packet_, codec_ctx_->time_base, video_stream_->time_base);
        packet_->stream_index = video_stream_->index;

        ret = av_interleaved_write_frame(fmt_ctx_, packet_);
        if (ret < 0) {
            spdlog::error("VideoWriter: error writing packet: error {}", ret);
            return false;
        }
    }

    return true;
}

bool VideoWriter::copy_audio() {
    if (audio_mappings_.empty()) {
        audio_copied_ = true;
        return true;
    }

    if (!fmt_ctx_ || !header_written_) {
        spdlog::error("VideoWriter: not open, cannot copy audio");
        return false;
    }

    if (audio_source_path_.empty()) {
        spdlog::warn("VideoWriter: no audio source path set");
        return false;
    }

    // Open a fresh input context for reading audio packets
    AVFormatContext* in_fmt_ctx = nullptr;
    int ret = avformat_open_input(&in_fmt_ctx, audio_source_path_.c_str(), nullptr, nullptr);
    if (ret < 0) {
        spdlog::error("VideoWriter: failed to open input for audio copy: error {}", ret);
        return false;
    }

    ret = avformat_find_stream_info(in_fmt_ctx, nullptr);
    if (ret < 0) {
        spdlog::error("VideoWriter: failed to find stream info for audio: error {}", ret);
        avformat_close_input(&in_fmt_ctx);
        return false;
    }

    AVPacket* read_pkt = av_packet_alloc();
    if (!read_pkt) {
        spdlog::error("VideoWriter: failed to allocate read packet");
        avformat_close_input(&in_fmt_ctx);
        return false;
    }

    int64_t audio_packets_written = 0;

    while (true) {
        ret = av_read_frame(in_fmt_ctx, read_pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                break;
            }
            spdlog::warn("VideoWriter: error reading audio packet: error {}", ret);
            break;
        }

        for (const auto& mapping : audio_mappings_) {
            if (read_pkt->stream_index == mapping.in_stream_idx) {
                AVStream* in_s = in_fmt_ctx->streams[mapping.in_stream_idx];
                AVStream* out_s = fmt_ctx_->streams[mapping.out_stream_idx];

                av_packet_rescale_ts(read_pkt, in_s->time_base, out_s->time_base);
                read_pkt->stream_index = mapping.out_stream_idx;
                read_pkt->pos = -1;

                ret = av_interleaved_write_frame(fmt_ctx_, read_pkt);
                if (ret < 0) {
                    spdlog::warn("VideoWriter: error writing audio packet: error {}", ret);
                } else {
                    ++audio_packets_written;
                }
                break;
            }
        }

        av_packet_unref(read_pkt);
    }

    av_packet_free(&read_pkt);
    avformat_close_input(&in_fmt_ctx);

    spdlog::info("VideoWriter: copied {} audio packets", audio_packets_written);
    audio_copied_ = true;
    return true;
}

bool VideoWriter::copy_audio_range(double start_sec, double end_sec) {
    if (audio_mappings_.empty()) {
        audio_copied_ = true;
        return true;
    }

    if (!fmt_ctx_ || !header_written_) {
        spdlog::error("VideoWriter: not open, cannot copy audio range");
        return false;
    }

    if (audio_source_path_.empty()) {
        spdlog::warn("VideoWriter: no audio source path set");
        return false;
    }

    // Open fresh input context
    AVFormatContext* in_fmt_ctx = nullptr;
    int ret = avformat_open_input(&in_fmt_ctx, audio_source_path_.c_str(), nullptr, nullptr);
    if (ret < 0) {
        spdlog::error("VideoWriter: failed to open input for audio range copy: error {}", ret);
        return false;
    }

    ret = avformat_find_stream_info(in_fmt_ctx, nullptr);
    if (ret < 0) {
        spdlog::error("VideoWriter: failed to find stream info for audio range: error {}", ret);
        avformat_close_input(&in_fmt_ctx);
        return false;
    }

    // Seek to start time (AV_TIME_BASE units, stream index -1 for all streams)
    int64_t start_ts_av = static_cast<int64_t>(start_sec * AV_TIME_BASE);
    ret = avformat_seek_file(in_fmt_ctx, -1, INT64_MIN, start_ts_av, INT64_MAX,
                             AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        // Seek failed — fall back to sequential scan from beginning
        spdlog::debug("VideoWriter: audio seek failed (error {}), using sequential scan", ret);
    }

    AVPacket* read_pkt = av_packet_alloc();
    if (!read_pkt) {
        spdlog::error("VideoWriter: failed to allocate read packet for audio range");
        avformat_close_input(&in_fmt_ctx);
        return false;
    }

    int64_t audio_packets_written = 0;

    while (true) {
        ret = av_read_frame(in_fmt_ctx, read_pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) break;
            spdlog::warn("VideoWriter: error reading audio packet: error {}", ret);
            break;
        }

        for (const auto& mapping : audio_mappings_) {
            if (read_pkt->stream_index != mapping.in_stream_idx) continue;

            AVStream* in_s = in_fmt_ctx->streams[mapping.in_stream_idx];
            AVStream* out_s = fmt_ctx_->streams[mapping.out_stream_idx];

            // Convert packet PTS to seconds
            double pkt_sec = (read_pkt->pts != AV_NOPTS_VALUE)
                ? av_q2d(in_s->time_base) * static_cast<double>(read_pkt->pts)
                : -1.0;

            // Skip packets before the range start
            if (pkt_sec >= 0.0 && pkt_sec < start_sec) {
                break;
            }

            // Stop at range end (exclusive)
            if (pkt_sec >= 0.0 && pkt_sec >= end_sec) {
                av_packet_unref(read_pkt);
                // Force exit from outer loop
                av_packet_free(&read_pkt);
                avformat_close_input(&in_fmt_ctx);
                spdlog::info("VideoWriter: copied {} audio packets (range {:.2f}s-{:.2f}s)",
                             audio_packets_written, start_sec, end_sec);
                audio_copied_ = true;
                return true;
            }

            // Rescale to output time_base first
            av_packet_rescale_ts(read_pkt, in_s->time_base, out_s->time_base);

            // Subtract start offset so audio begins at PTS 0
            int64_t offset_tb = av_rescale_q(start_ts_av, AV_TIME_BASE_Q, out_s->time_base);
            read_pkt->pts -= offset_tb;
            read_pkt->dts -= offset_tb;

            // Guard against negative PTS (packets at seek boundary)
            if (read_pkt->pts < 0) {
                break;
            }

            read_pkt->stream_index = mapping.out_stream_idx;
            read_pkt->pos = -1;

            ret = av_interleaved_write_frame(fmt_ctx_, read_pkt);
            if (ret < 0) {
                spdlog::warn("VideoWriter: error writing audio packet: error {}", ret);
            } else {
                ++audio_packets_written;
            }
            break;
        }

        av_packet_unref(read_pkt);
    }

    av_packet_free(&read_pkt);
    avformat_close_input(&in_fmt_ctx);

    spdlog::info("VideoWriter: copied {} audio packets (range {:.2f}s-{:.2f}s)",
                 audio_packets_written, start_sec, end_sec);
    audio_copied_ = true;
    return true;
}

void VideoWriter::close() {
    // Flush encoder
    if (codec_ctx_ && header_written_) {
        int ret = avcodec_send_frame(codec_ctx_, nullptr);
        if (ret >= 0) {
            while (true) {
                ret = avcodec_receive_packet(codec_ctx_, packet_);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                }
                if (ret < 0) {
                    break;
                }
                av_packet_rescale_ts(packet_, codec_ctx_->time_base, video_stream_->time_base);
                packet_->stream_index = video_stream_->index;
                av_interleaved_write_frame(fmt_ctx_, packet_);
            }
        }
    }

    // Write trailer
    if (header_written_ && fmt_ctx_) {
        av_write_trailer(fmt_ctx_);
        header_written_ = false;
    }

    // Free resources in reverse order
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
        if (!(fmt_ctx_->oformat->flags & AVFMT_NOFILE) && fmt_ctx_->pb) {
            avio_closep(&fmt_ctx_->pb);
        }
        avformat_free_context(fmt_ctx_);
        fmt_ctx_ = nullptr;
    }

    video_stream_ = nullptr;
    pts_counter_ = 0;
    fps_ = 0.0;
    audio_copied_ = false;
    audio_mappings_.clear();
    audio_source_path_.clear();
}

} // namespace wmr
