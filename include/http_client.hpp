#pragma once
#include <string>
#include <map>
#include <vector>
#include <cstdint>

namespace okplayer {

struct HttpResponse {
    long status_code = 0;
    std::string body;
    std::map<std::string, std::string> headers;
    std::vector<std::string> set_cookies;
    std::string effective_url;
    std::string error;
    bool ok() const { return status_code >= 200 && status_code < 400; }
};

class HttpClient {
public:
    static void globalInit();
    static void globalCleanup();

    // 自动解析 URL 中的 http://user:pass@host/ (如 wexfnw:wexfnw@cat.xn--4kq62z5rby2qupq9ub.top)
    static HttpResponse get(
        const std::string& url,
        const std::map<std::string, std::string>& headers = {},
        int timeout_ms = 15000,
        bool follow_location = true
    );

    static HttpResponse postJson(
        const std::string& url,
        const std::string& json_body,
        const std::map<std::string, std::string>& headers = {},
        int timeout_ms = 15000
    );

    static HttpResponse postForm(
        const std::string& url,
        const std::string& form_body,
        const std::map<std::string, std::string>& headers = {},
        int timeout_ms = 15000
    );

    // 辅助工具 (移植自 APK Ln2/b 与 LC1/a)
    static std::string md5Hex(const std::string& input);
    static std::string base64Encode(const std::string& input);
    static std::string base64Decode(const std::string& input);
    static std::string urlEncode(const std::string& input);
    static std::string extractBaseDirUrl(const std::string& url);
    static std::string resolveRelativeUrl(const std::string& base_url, const std::string& rel_url);
    static std::string mergeSetCookies(const std::string& existing_cookie, const std::vector<std::string>& set_cookies);
    static std::string generateUuidV4();
    static uint64_t currentTimeMillis();
};

} // namespace okplayer
