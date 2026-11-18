#pragma once
#include <SDL2/SDL.h>
#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <atomic>
#include <cstdint>

namespace okplayer {

class VideoPlayer {
public:
    VideoPlayer();
    ~VideoPlayer();

    void start(const std::string& url, const std::map<std::string, std::string>& headers);
    void stop();
    void togglePause();
    void seekRelative(double delta_sec);
    void seekTo(double target_sec);
    void setVolume(int vol_percent);
    void setSpeed(float speed);

    // 在主渲染线程调用，将最新解码出的视频帧上传并绘制到全屏纹理
    void renderVideoFrame(SDL_Renderer* renderer, const SDL_Rect& dst_rect);

    bool isPlaying() const { return running_.load(); }
    bool isPaused() const { return paused_.load(); }
    double getPositionSec() const { return pos_sec_.load(); }
    double getDurationSec() const { return dur_sec_.load(); }
    std::string getVideoStats() const;

private:
    void decodeThreadLoop(std::string url, std::map<std::string, std::string> headers);

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> paused_{false};
    std::atomic<double> pos_sec_{0.0};
    std::atomic<double> dur_sec_{1440.0};
    std::atomic<double> seek_target_sec_{-1.0};
    std::atomic<int> volume_{85};
    std::atomic<float> speed_{1.0f};

    mutable std::mutex frame_mutex_;
    std::vector<uint8_t> rgba_frame_;
    int frame_w_ = 0;
    int frame_h_ = 0;
    bool frame_dirty_ = false;
    std::string stats_str_ = "正在连接视频流...";

    SDL_Texture* video_tex_ = nullptr;
    int tex_w_ = 0;
    int tex_h_ = 0;
    SDL_AudioDeviceID audio_dev_ = 0;
};

} // namespace okplayer
