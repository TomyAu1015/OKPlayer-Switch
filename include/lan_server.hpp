#pragma once
#include "types.hpp"
#include <string>
#include <functional>
#include <atomic>
#include <thread>

namespace okplayer {

class LanWebServer {
public:
    using ActionCallback = std::function<void(const std::string& action, const std::string& k1, const std::string& v1)>;

    bool start(int port, AppConfig* cfg, ActionCallback cb);
    void stop();
    std::string getLanUrl() const { return lan_url_; }

private:
    void serverLoop();
    int server_fd_ = -1;
    int port_ = 9978;
    std::atomic<bool> running_{false};
    std::thread worker_;
    AppConfig* cfg_ = nullptr;
    ActionCallback callback_;
    std::string lan_url_ = "http://127.0.0.1:9978";
};

} // namespace okplayer
