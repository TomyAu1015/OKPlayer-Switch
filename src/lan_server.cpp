#include "lan_server.hpp"
#include "source_loader.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <sstream>
#include <fstream>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace okplayer {

namespace {

std::string urlDecode(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') {
            out.push_back(' ');
        } else if (in[i] == '%' && i + 2 < in.size()) {
            int v = 0;
            std::istringstream iss(in.substr(i + 1, 2));
            if (iss >> std::hex >> v) {
                out.push_back(static_cast<char>(v));
                i += 2;
            } else {
                out.push_back(in[i]);
            }
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

std::string getFormField(const std::string& body, const std::string& key) {
    std::stringstream ss(body);
    std::string pair;
    while (std::getline(ss, pair, '&')) {
        auto eq = pair.find('=');
        if (eq != std::string::npos) {
            std::string k = urlDecode(pair.substr(0, eq));
            std::string v = urlDecode(pair.substr(eq + 1));
            if (k == key) return v;
        }
    }
    return "";
}

} // namespace

bool LanWebServer::start(int port, AppConfig* cfg, ActionCallback cb) {
    stop();
    port_ = port;
    cfg_ = cfg;
    callback_ = cb;

    std::string ip_str = "127.0.0.1";
#ifdef __SWITCH__
    u32 ip = 0;
    if (R_SUCCEEDED(nifmGetCurrentIpAddress(&ip)) && ip != 0) {
        struct in_addr addr;
        addr.s_addr = ip;
        ip_str = inet_ntoa(addr);
    }
#endif
    lan_url_ = "http://" + ip_str + ":" + std::to_string(port_);

    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) return false;

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port_));

    if (bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }
    if (listen(server_fd_, 4) < 0) {
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    int flags = fcntl(server_fd_, F_GETFL, 0);
    fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK);

    running_ = true;
    worker_ = std::thread(&LanWebServer::serverLoop, this);
    return true;
}

void LanWebServer::stop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
    if (server_fd_ >= 0) {
        close(server_fd_);
        server_fd_ = -1;
    }
}

void LanWebServer::serverLoop() {
    while (running_) {
        sockaddr_in cli_addr{};
        socklen_t cli_len = sizeof(cli_addr);
        int cli_fd = accept(server_fd_, reinterpret_cast<sockaddr*>(&cli_addr), &cli_len);
        if (cli_fd < 0) {
            usleep(40000);
            continue;
        }

        char buf[8192];
        ssize_t n = recv(cli_fd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            std::string req(buf, static_cast<size_t>(n));
            auto body_pos = req.find("\r\n\r\n");
            std::string body = (body_pos != std::string::npos) ? req.substr(body_pos + 4) : "";

            if (req.rfind("POST /action", 0) == 0 && cfg_) {
                std::string act = getFormField(body, "do");
                if (act == "setting") {
                    std::string text = getFormField(body, "text");
                    std::string name = getFormField(body, "name");
                    if (!text.empty()) {
                        cfg_->source_url = text;
                        if (!name.empty()) cfg_->source_name = name;
                        SourceLoader::saveAppConfig(*cfg_);
                        if (callback_) callback_("reload_source", name, text);
                    }
                } else if (act == "cookie") {
                    std::string q_ck = getFormField(body, "quark_cookie");
                    std::string b_ck = getFormField(body, "baidu_cookie");
                    if (!q_ck.empty()) cfg_->quark_cookie = q_ck;
                    if (!b_ck.empty()) cfg_->baidu_cookie = b_ck;
                    SourceLoader::saveAppConfig(*cfg_);
                    if (callback_) callback_("update_cookie", q_ck, b_ck);
                } else if (act == "search") {
                    std::string word = getFormField(body, "word");
                    if (callback_) callback_("search", "word", word);
                } else if (act == "push") {
                    std::string url = getFormField(body, "url");
                    if (callback_) callback_("push", "url", url);
                }
                const char* ok_resp =
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\n\r\n{\"code\":0}";
                send(cli_fd, ok_resp, std::strlen(ok_resp), 0);
            } else {
                // 返回内置 Switch 远程配置与网盘 Cookie 管理网页
                std::ostringstream html;
                html << "<!DOCTYPE html><html lang='zh-CN'><head><meta charset='UTF-8'>"
                     << "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                     << "<title>OK影视-Switch 远程配置中心</title>"
                     << "<style>body{background:#12161f;color:#e6edf3;font-family:sans-serif;max-width:680px;margin:20px auto;padding:16px}"
                     << ".card{background:#1c2230;border-radius:12px;padding:18px;margin-bottom:16px;border:1px solid #2e384d}"
                     << "h2{margin-top:0;color:#58a6ff;font-size:18px}"
                     << "input,textarea{width:100%;box-sizing:border-box;background:#0d1117;color:#fff;border:1px solid #30363d;border-radius:8px;padding:10px;margin:8px 0}"
                     << "button{background:#238636;color:#fff;border:none;border-radius:8px;padding:10px 20px;font-size:15px;cursor:pointer}</style></head><body>"
                     << "<div class='card'><h2>1. 源地址直接填写 (已适配 .js.md5 猫源与 OK影视源)</h2>"
                     << "<form method='POST' action='/action'><input type='hidden' name='do' value='setting'>"
                     << "<input name='name' placeholder='源名称' value='" << (cfg_ ? cfg_->source_name : "王二小放牛娃") << "'>"
                     << "<input name='text' placeholder='请输入接口地址...' value='" << (cfg_ ? cfg_->source_url : DEFAULT_WEX_SOURCE_URL) << "'>"
                     << "<button type='submit'>保存并立即刷新 Switch 源</button></form></div>"
                     << "<div class='card'><h2>2. 夸克网盘 / 百度网盘 Cookie 远程绑定</h2>"
                     << "<form method='POST' action='/action'><input type='hidden' name='do' value='cookie'>"
                     << "<label>夸克网盘 Cookie (pan.quark.cn):</label>"
                     << "<textarea name='quark_cookie' rows='3' placeholder='粘贴 __puus=...; __pus=...'>" << (cfg_ ? cfg_->quark_cookie : "") << "</textarea>"
                     << "<label>百度网盘 Cookie (BDUSS=...; STOKEN=...):</label>"
                     << "<textarea name='baidu_cookie' rows='3' placeholder='粘贴 BDUSS=...; STOKEN=...'>" << (cfg_ ? cfg_->baidu_cookie : "") << "</textarea>"
                     << "<button type='submit'>同步网盘凭证至 Switch</button></form></div>"
                     << "<div class='card'><h2>3. 远程搜索与链接推送 (支持直接推送夸克/百度分享链接)</h2>"
                     << "<form method='POST' action='/action'><input type='hidden' name='do' value='search'>"
                     << "<input name='word' placeholder='输入片名或粘贴 pan.quark.cn/s/... 分享链接'>"
                     << "<button type='submit'>在 Switch 上搜索/解析</button></form></div>"
                     << "</body></html>";
                std::string page = html.str();
                std::ostringstream hdr;
                hdr << "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: "
                    << page.size() << "\r\n\r\n" << page;
                std::string full = hdr.str();
                send(cli_fd, full.data(), full.size(), 0);
            }
        }
        close(cli_fd);
    }
}

} // namespace okplayer
