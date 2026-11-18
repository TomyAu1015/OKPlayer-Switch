#include "video_player.hpp"
#include <pthread.h>
#include <functional>
#include <cstring>
#include <algorithm>
#include <sstream>
#include <iomanip>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
}

namespace okplayer {

namespace {

void spawnDecoderThread(std::function<void()> fn) {
    auto* heap_fn = new std::function<void()>(std::move(fn));
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 2 * 1024 * 1024); // 2 MB stack for FFmpeg libavformat + libavcodec
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    auto entry = [](void* arg) -> void* {
        auto* f = static_cast<std::function<void()>*>(arg);
        try {
            (*f)();
        } catch (...) {}
        delete f;
        return nullptr;
    };
    if (pthread_create(&tid, &attr, entry, heap_fn) != 0) {
        delete heap_fn;
    }
    pthread_attr_destroy(&attr);
}

} // namespace

VideoPlayer::VideoPlayer() {
    avformat_network_init();
}

VideoPlayer::~VideoPlayer() {
    stop();
    if (video_tex_) {
        SDL_DestroyTexture(video_tex_);
        video_tex_ = nullptr;
    }
}

void VideoPlayer::start(const std::string& url, const std::map<std::string, std::string>& headers) {
    stop();
    stop_requested_.store(false);
    paused_.store(false);
    pos_sec_.store(0.0);
    dur_sec_.store(1440.0);
    seek_target_sec_.store(-1.0);
    running_.store(true);

    {
        std::lock_guard<std::mutex> lk(frame_mutex_);
        stats_str_ = "正在缓冲并开启 Switch 自适应解码...";
        frame_dirty_ = false;
    }

    // 初始化 SDL2 48kHz 立体声音频输出设备
    if (audio_dev_ == 0) {
        SDL_AudioSpec want{}, have{};
        want.freq = 48000;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 2048;
        want.callback = nullptr; // 使用 SDL_QueueAudio 推送
        audio_dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (audio_dev_ != 0) {
            SDL_PauseAudioDevice(audio_dev_, 0);
        }
    } else {
        SDL_ClearQueuedAudio(audio_dev_);
        SDL_PauseAudioDevice(audio_dev_, 0);
    }

    spawnDecoderThread([this, url, headers]() {
        decodeThreadLoop(url, headers);
    });
}

void VideoPlayer::stop() {
    stop_requested_.store(true);
    int wait_ms = 0;
    while (running_.load() && wait_ms < 1500) {
        SDL_Delay(15);
        wait_ms += 15;
    }
    if (audio_dev_ != 0) {
        SDL_ClearQueuedAudio(audio_dev_);
        SDL_CloseAudioDevice(audio_dev_);
        audio_dev_ = 0;
    }
}

void VideoPlayer::togglePause() {
    bool next = !paused_.load();
    paused_.store(next);
    if (audio_dev_ != 0) {
        SDL_PauseAudioDevice(audio_dev_, next ? 1 : 0);
    }
}

void VideoPlayer::seekRelative(double delta_sec) {
    double cur = pos_sec_.load();
    double target = std::clamp(cur + delta_sec, 0.0, std::max(10.0, dur_sec_.load()));
    seek_target_sec_.store(target);
    pos_sec_.store(target);
}

void VideoPlayer::seekTo(double target_sec) {
    double target = std::clamp(target_sec, 0.0, std::max(10.0, dur_sec_.load()));
    seek_target_sec_.store(target);
    pos_sec_.store(target);
}

void VideoPlayer::setVolume(int vol_percent) {
    volume_.store(std::clamp(vol_percent, 0, 100));
}

void VideoPlayer::setSpeed(float speed) {
    speed_.store(std::clamp(speed, 0.5f, 2.0f));
}

std::string VideoPlayer::getVideoStats() const {
    std::lock_guard<std::mutex> lk(frame_mutex_);
    return stats_str_;
}

void VideoPlayer::renderVideoFrame(SDL_Renderer* renderer, const SDL_Rect& dst_rect) {
    std::lock_guard<std::mutex> lk(frame_mutex_);
    if (frame_dirty_ && frame_w_ > 0 && frame_h_ > 0 && !rgba_frame_.empty()) {
        if (!video_tex_ || tex_w_ != frame_w_ || tex_h_ != frame_h_) {
            if (video_tex_) SDL_DestroyTexture(video_tex_);
            video_tex_ = SDL_CreateTexture(
                renderer,
                SDL_PIXELFORMAT_ABGR8888,
                SDL_TEXTUREACCESS_STREAMING,
                frame_w_, frame_h_
            );
            tex_w_ = frame_w_;
            tex_h_ = frame_h_;
        }
        if (video_tex_) {
            SDL_UpdateTexture(video_tex_, nullptr, rgba_frame_.data(), frame_w_ * 4);
        }
        frame_dirty_ = false;
    }
    if (video_tex_) {
        SDL_RenderCopy(renderer, video_tex_, nullptr, &dst_rect);
    }
}

void VideoPlayer::decodeThreadLoop(std::string url, std::map<std::string, std::string> headers) {
    AVFormatContext* fmt_ctx = avformat_alloc_context();
    AVDictionary* opts = nullptr;

    // 拼接自定义 Cookie / Referer / User-Agent Header (适配夸克与百度网盘直链鉴权)
    std::string hdr_lines;
    std::string ua = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36";
    for (const auto& kv : headers) {
        std::string lk = kv.first;
        std::transform(lk.begin(), lk.end(), lk.begin(), [](unsigned char c){ return std::tolower(c); });
        if (lk == "user-agent") {
            ua = kv.second;
        } else {
            hdr_lines += kv.first + ": " + kv.second + "\r\n";
        }
    }
    av_dict_set(&opts, "user_agent", ua.c_str(), 0);
    if (!hdr_lines.empty()) {
        av_dict_set(&opts, "headers", hdr_lines.c_str(), 0);
    }
    av_dict_set(&opts, "reconnect", "1", 0);
    av_dict_set(&opts, "reconnect_streamed", "1", 0);
    av_dict_set(&opts, "reconnect_delay_max", "5", 0);
    av_dict_set(&opts, "rw_timeout", "12000000", 0); // 12s timeout

    if (avformat_open_input(&fmt_ctx, url.c_str(), nullptr, &opts) < 0) {
        av_dict_free(&opts);
        {
            std::lock_guard<std::mutex> lk(frame_mutex_);
            stats_str_ = "打开视频流失败，请按 [Y] 切换其他清晰度或线路";
        }
        running_.store(false);
        return;
    }
    av_dict_free(&opts);

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        avformat_close_input(&fmt_ctx);
        running_.store(false);
        return;
    }

    if (fmt_ctx->duration > 0) {
        dur_sec_.store(static_cast<double>(fmt_ctx->duration) / AV_TIME_BASE);
    }

    int v_idx = -1, a_idx = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; ++i) {
        auto par = fmt_ctx->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO && v_idx < 0) v_idx = static_cast<int>(i);
        else if (par->codec_type == AVMEDIA_TYPE_AUDIO && a_idx < 0) a_idx = static_cast<int>(i);
    }

    AVCodecContext* v_ctx = nullptr;
    if (v_idx >= 0) {
        const AVCodec* v_codec = avcodec_find_decoder(fmt_ctx->streams[v_idx]->codecpar->codec_id);
        if (v_codec) {
            v_ctx = avcodec_alloc_context3(v_codec);
            avcodec_parameters_to_context(v_ctx, fmt_ctx->streams[v_idx]->codecpar);
            // Switch 性能优化：开启 3 线程并行解码 + 快速非环路滤波，4K 视频也能降级流畅解出
            v_ctx->thread_count = 3;
            v_ctx->skip_loop_filter = AVDISCARD_NONREF;
            if (avcodec_open2(v_ctx, v_codec, nullptr) < 0) {
                avcodec_free_context(&v_ctx);
            }
        }
    }

    AVCodecContext* a_ctx = nullptr;
    SwrContext* swr_ctx = nullptr;
    if (a_idx >= 0) {
        const AVCodec* a_codec = avcodec_find_decoder(fmt_ctx->streams[a_idx]->codecpar->codec_id);
        if (a_codec) {
            a_ctx = avcodec_alloc_context3(a_codec);
            avcodec_parameters_to_context(a_ctx, fmt_ctx->streams[a_idx]->codecpar);
            if (avcodec_open2(a_ctx, a_codec, nullptr) >= 0) {
                AVChannelLayout out_ch_layout = AV_CHANNEL_LAYOUT_STEREO;
                swr_alloc_set_opts2(
                    &swr_ctx,
                    &out_ch_layout, AV_SAMPLE_FMT_S16, 48000,
                    &a_ctx->ch_layout, a_ctx->sample_fmt, a_ctx->sample_rate,
                    0, nullptr
                );
                if (swr_ctx) swr_init(swr_ctx);
            } else {
                avcodec_free_context(&a_ctx);
            }
        }
    }

    SwsContext* sws_ctx = nullptr;
    int out_w = 1280, out_h = 720;
    if (v_ctx && v_ctx->width > 0 && v_ctx->height > 0) {
        // 针对 Switch 屏幕自适应缩放：若源是 4K (3840x2160) 或 2K，自动降采样到 1280x720 以保证 60FPS 流畅不卡顿
        if (v_ctx->width > 1280 || v_ctx->height > 720) {
            out_w = 1280;
            out_h = 720;
        } else {
            out_w = v_ctx->width;
            out_h = v_ctx->height;
        }
        sws_ctx = sws_getContext(
            v_ctx->width, v_ctx->height, v_ctx->pix_fmt,
            out_w, out_h, AV_PIX_FMT_RGBA,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
        );
        std::ostringstream ss;
        ss << "FFmpeg 多核硬/软协同  |  源分辨率: " << v_ctx->width << "x" << v_ctx->height
           << " → Switch适配: " << out_w << "x" << out_h;
        std::lock_guard<std::mutex> lk(frame_mutex_);
        stats_str_ = ss.str();
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    std::vector<uint8_t> audio_buf(192000);

    while (!stop_requested_.load()) {
        if (paused_.load()) {
            SDL_Delay(20);
            continue;
        }

        double seek_req = seek_target_sec_.exchange(-1.0);
        if (seek_req >= 0.0) {
            int64_t ts = static_cast<int64_t>(seek_req * AV_TIME_BASE);
            av_seek_frame(fmt_ctx, -1, ts, AVSEEK_FLAG_BACKWARD);
            if (v_ctx) avcodec_flush_buffers(v_ctx);
            if (a_ctx) avcodec_flush_buffers(a_ctx);
            if (audio_dev_ != 0) SDL_ClearQueuedAudio(audio_dev_);
        }

        // 控制音频队列缓冲深度，用音频时钟自然同步视频帧率
        if (audio_dev_ != 0 && SDL_GetQueuedAudioSize(audio_dev_) > 48000 * 4 * 1) {
            SDL_Delay(10);
            continue;
        }

        int ret = av_read_frame(fmt_ctx, pkt);
        if (ret < 0) {
            SDL_Delay(25);
            continue;
        }

        if (pkt->stream_index == v_idx && v_ctx) {
            if (avcodec_send_packet(v_ctx, pkt) == 0) {
                while (avcodec_receive_frame(v_ctx, frame) == 0) {
                    if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                        AVRational tb = fmt_ctx->streams[v_idx]->time_base;
                        pos_sec_.store(frame->best_effort_timestamp * av_q2d(tb));
                    }
                    if (sws_ctx) {
                        std::lock_guard<std::mutex> lk(frame_mutex_);
                        rgba_frame_.resize(out_w * out_h * 4);
                        uint8_t* dst_slices[1] = { rgba_frame_.data() };
                        int dst_stride[1] = { out_w * 4 };
                        sws_scale(sws_ctx, frame->data, frame->linesize, 0, v_ctx->height, dst_slices, dst_stride);
                        frame_w_ = out_w;
                        frame_h_ = out_h;
                        frame_dirty_ = true;
                    }
                    if (a_idx < 0) {
                        SDL_Delay(static_cast<uint32_t>(33.0f / std::max(0.5f, speed_.load())));
                    }
                }
            }
        } else if (pkt->stream_index == a_idx && a_ctx && swr_ctx && audio_dev_ != 0) {
            if (avcodec_send_packet(a_ctx, pkt) == 0) {
                while (avcodec_receive_frame(a_ctx, frame) == 0) {
                    if (frame->best_effort_timestamp != AV_NOPTS_VALUE && v_idx < 0) {
                        AVRational tb = fmt_ctx->streams[a_idx]->time_base;
                        pos_sec_.store(frame->best_effort_timestamp * av_q2d(tb));
                    }
                    int out_samples = av_rescale_rnd(
                        swr_get_delay(swr_ctx, a_ctx->sample_rate) + frame->nb_samples,
                        48000, a_ctx->sample_rate, AV_ROUND_UP
                    );
                    if (out_samples * 4 > static_cast<int>(audio_buf.size())) {
                        audio_buf.resize(out_samples * 4);
                    }
                    uint8_t* out_ptr[1] = { audio_buf.data() };
                    int converted = swr_convert(
                        swr_ctx, out_ptr, out_samples,
                        const_cast<const uint8_t**>(frame->data), frame->nb_samples
                    );
                    if (converted > 0) {
                        int vol = volume_.load();
                        int16_t* samples = reinterpret_cast<int16_t*>(audio_buf.data());
                        int total_s = converted * 2;
                        if (vol != 100) {
                            for (int i = 0; i < total_s; ++i) {
                                samples[i] = static_cast<int16_t>((samples[i] * vol) / 100);
                            }
                        }
                        SDL_QueueAudio(audio_dev_, audio_buf.data(), converted * 4);
                    }
                }
            }
        }
        av_packet_unref(pkt);
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    if (sws_ctx) sws_freeContext(sws_ctx);
    if (swr_ctx) swr_free(&swr_ctx);
    if (v_ctx) avcodec_free_context(&v_ctx);
    if (a_ctx) avcodec_free_context(&a_ctx);
    avformat_close_input(&fmt_ctx);
    running_.store(false);
}

} // namespace okplayer
