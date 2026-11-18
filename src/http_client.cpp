#include "http_client.hpp"
#include <curl/curl.h>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <random>
#include <algorithm>
#include <cctype>

namespace okplayer {

namespace {

size_t writeBodyCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total = size * nmemb;
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, total);
    return total;
}

size_t writeHeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t total = size * nitems;
    auto* resp = static_cast<HttpResponse*>(userdata);
    std::string line(buffer, total);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        while (!val.empty() && val.front() == ' ') val.erase(val.begin());
        std::string lower_key = key;
        std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lower_key == "set-cookie") {
            resp->set_cookies.push_back(val);
        }
        resp->headers[lower_key] = val;
    }
    return total;
}

// 解析 URL 中的 Basic Auth (如 http://wexfnw:wexfnw@cat.xn--4kq62z5rby2qupq9ub.top/index.js.md5)
void splitUrlUserPass(const std::string& raw_url, std::string& clean_url, std::string& userpwd) {
    clean_url = raw_url;
    userpwd.clear();
    auto scheme_pos = raw_url.find("://");
    if (scheme_pos == std::string::npos) return;
    size_t host_start = scheme_pos + 3;
    size_t slash_pos = raw_url.find('/', host_start);
    size_t authority_end = (slash_pos == std::string::npos) ? raw_url.size() : slash_pos;
    std::string authority = raw_url.substr(host_start, authority_end - host_start);
    auto at_pos = authority.rfind('@');
    if (at_pos != std::string::npos) {
        userpwd = authority.substr(0, at_pos);
        std::string hostport = authority.substr(at_pos + 1);
        clean_url = raw_url.substr(0, host_start) + hostport +
                    ((slash_pos == std::string::npos) ? "" : raw_url.substr(slash_pos));
    }
}

// 轻量级内置 MD5 实现 (RFC 1321，确保脱离外部库也能校验 index.js.md5)
struct Md5Ctx {
    uint32_t state[4];
    uint32_t count[2];
    uint8_t buffer[64];
};

inline uint32_t rotl32(uint32_t x, uint32_t n) { return (x << n) | (x >> (32 - n)); }

void md5Transform(uint32_t state[4], const uint8_t block[64]) {
    static const uint32_t s[64] = {
        7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
        5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
        4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
        6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
    };
    static const uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };
    uint32_t M[16];
    for (int i = 0; i < 16; ++i) {
        M[i] = static_cast<uint32_t>(block[i*4]) |
               (static_cast<uint32_t>(block[i*4+1]) << 8) |
               (static_cast<uint32_t>(block[i*4+2]) << 16) |
               (static_cast<uint32_t>(block[i*4+3]) << 24);
    }
    uint32_t A = state[0], B = state[1], C = state[2], D = state[3];
    for (uint32_t i = 0; i < 64; ++i) {
        uint32_t F, g;
        if (i < 16) { F = (B & C) | ((~B) & D); g = i; }
        else if (i < 32) { F = (D & B) | ((~D) & C); g = (5 * i + 1) % 16; }
        else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) % 16; }
        else { F = C ^ (B | (~D)); g = (7 * i) % 16; }
        F = F + A + K[i] + M[g];
        A = D; D = C; C = B; B = B + rotl32(F, s[i]);
    }
    state[0] += A; state[1] += B; state[2] += C; state[3] += D;
}

} // namespace

void HttpClient::globalInit() {
    curl_global_init(CURL_GLOBAL_ALL);
}

void HttpClient::globalCleanup() {
    curl_global_cleanup();
}

static HttpResponse performRequest(
    const std::string& method,
    const std::string& raw_url,
    const std::string* body_ptr,
    const std::map<std::string, std::string>& headers,
    int timeout_ms,
    bool follow_location
) {
    HttpResponse resp;
    CURL* curl = curl_easy_init();
    if (!curl) {
        resp.error = "curl_easy_init failed";
        return resp;
    }

    std::string clean_url, userpwd;
    splitUrlUserPass(raw_url, clean_url, userpwd);

    curl_easy_setopt(curl, CURLOPT_URL, clean_url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBodyCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, writeHeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, follow_location ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); // 开启 gzip/deflate 自动解压

    if (!userpwd.empty()) {
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd.c_str());
    }

    struct curl_slist* header_list = nullptr;
    bool has_ua = false;
    for (const auto& kv : headers) {
        std::string lk = kv.first;
        std::transform(lk.begin(), lk.end(), lk.begin(), [](unsigned char c){ return std::tolower(c); });
        if (lk == "user-agent") has_ua = true;
        std::string line = kv.first + ": " + kv.second;
        header_list = curl_slist_append(header_list, line.c_str());
    }
    if (!has_ua) {
        header_list = curl_slist_append(
            header_list,
            "User-Agent: okhttp/5.0.0-alpha.14 (NintendoSwitch; OKPlayer/3.7.2)"
        );
    }
    if (!userpwd.empty()) {
        std::string auth_hdr = "Authorization: Basic " + HttpClient::base64Encode(userpwd);
        header_list = curl_slist_append(header_list, auth_hdr.c_str());
    }
    if (header_list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    if (method == "POST" && body_ptr) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_ptr->c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_ptr->size()));
    }

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        resp.error = curl_easy_strerror(res);
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status_code);
        char* eff_url = nullptr;
        if (curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff_url) == CURLE_OK && eff_url) {
            resp.effective_url = eff_url;
        }
    }

    if (header_list) curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);
    return resp;
}

HttpResponse HttpClient::get(
    const std::string& url,
    const std::map<std::string, std::string>& headers,
    int timeout_ms,
    bool follow_location
) {
    return performRequest("GET", url, nullptr, headers, timeout_ms, follow_location);
}

HttpResponse HttpClient::postJson(
    const std::string& url,
    const std::string& json_body,
    const std::map<std::string, std::string>& headers,
    int timeout_ms
) {
    auto hdrs = headers;
    if (hdrs.find("Content-Type") == hdrs.end() && hdrs.find("content-type") == hdrs.end()) {
        hdrs["Content-Type"] = "application/json;charset=UTF-8";
    }
    return performRequest("POST", url, &json_body, hdrs, timeout_ms, true);
}

HttpResponse HttpClient::postForm(
    const std::string& url,
    const std::string& form_body,
    const std::map<std::string, std::string>& headers,
    int timeout_ms
) {
    auto hdrs = headers;
    if (hdrs.find("Content-Type") == hdrs.end() && hdrs.find("content-type") == hdrs.end()) {
        hdrs["Content-Type"] = "application/x-www-form-urlencoded;charset=UTF-8";
    }
    return performRequest("POST", url, &form_body, hdrs, timeout_ms, true);
}

std::string HttpClient::md5Hex(const std::string& input) {
    uint32_t state[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    uint64_t bit_len = static_cast<uint64_t>(input.size()) * 8;
    std::vector<uint8_t> msg(input.begin(), input.end());
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) {
        msg.push_back(0x00);
    }
    for (int i = 0; i < 8; ++i) {
        msg.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xff));
    }
    for (size_t offset = 0; offset < msg.size(); offset += 64) {
        md5Transform(state, &msg[offset]);
    }
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < 4; ++i) {
        oss << std::setw(2) << (state[i] & 0xff)
            << std::setw(2) << ((state[i] >> 8) & 0xff)
            << std::setw(2) << ((state[i] >> 16) & 0xff)
            << std::setw(2) << ((state[i] >> 24) & 0xff);
    }
    return oss.str();
}

static const char B64_TABLE[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string HttpClient::base64Encode(const std::string& input) {
    std::string out;
    int val = 0, valb = -6;
    for (uint8_t c : input) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(B64_TABLE[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(B64_TABLE[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

std::string HttpClient::base64Decode(const std::string& input) {
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[static_cast<unsigned char>(B64_TABLE[i])] = i;
    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : input) {
        if (T[c] == -1) {
            if (c == '=') break;
            continue;
        }
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

std::string HttpClient::urlEncode(const std::string& input) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    for (unsigned char c : input) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << std::uppercase << '%' << std::setw(2) << int(c) << std::nouppercase;
        }
    }
    return escaped.str();
}

std::string HttpClient::extractBaseDirUrl(const std::string& url) {
    auto qpos = url.find('?');
    std::string clean = (qpos == std::string::npos) ? url : url.substr(0, qpos);
    auto slash = clean.rfind('/');
    if (slash == std::string::npos || slash < 8) return clean + "/";
    return clean.substr(0, slash + 1);
}

std::string HttpClient::resolveRelativeUrl(const std::string& base_url, const std::string& rel_url) {
    if (rel_url.find("http://") == 0 || rel_url.find("https://") == 0 ||
        rel_url.find("romfs:/") == 0 || rel_url.find("sdmc:/") == 0) {
        return rel_url;
    }
    std::string base_dir = extractBaseDirUrl(base_url);
    if (rel_url.rfind("./", 0) == 0) {
        return base_dir + rel_url.substr(2);
    }
    if (rel_url.rfind("/", 0) == 0) {
        auto scheme = base_dir.find("://");
        if (scheme != std::string::npos) {
            auto host_slash = base_dir.find('/', scheme + 3);
            if (host_slash != std::string::npos) {
                return base_dir.substr(0, host_slash) + rel_url;
            }
        }
    }
    return base_dir + rel_url;
}

std::string HttpClient::mergeSetCookies(
    const std::string& existing_cookie,
    const std::vector<std::string>& set_cookies
) {
    std::map<std::string, std::string> jar;
    auto parsePairs = [&](const std::string& str, bool first_pair_only) {
        std::stringstream ss(str);
        std::string item;
        bool first = true;
        while (std::getline(ss, item, ';')) {
            if (first_pair_only && !first) break;
            first = false;
            auto eq = item.find('=');
            if (eq != std::string::npos) {
                std::string k = item.substr(0, eq);
                std::string v = item.substr(eq + 1);
                while (!k.empty() && k.front() == ' ') k.erase(k.begin());
                while (!k.empty() && k.back() == ' ') k.pop_back();
                while (!v.empty() && v.front() == ' ') v.erase(v.begin());
                while (!v.empty() && v.back() == ' ') v.pop_back();
                if (!k.empty() && !v.empty()) {
                    jar[k] = v;
                }
            }
        }
    };
    if (!existing_cookie.empty()) {
        parsePairs(existing_cookie, false);
    }
    for (const auto& sc : set_cookies) {
        parsePairs(sc, true);
    }
    std::ostringstream oss;
    bool first = true;
    for (const auto& kv : jar) {
        if (!first) oss << "; ";
        first = false;
        oss << kv.first << "=" << kv.second;
    }
    return oss.str();
}

std::string HttpClient::generateUuidV4() {
    static std::mt19937 rng(static_cast<unsigned int>(currentTimeMillis()));
    static const char* hex = "0123456789abcdef";
    std::string uuid = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";
    for (char& c : uuid) {
        if (c == 'x') c = hex[rng() % 16];
        else if (c == 'y') c = hex[(rng() % 4) + 8];
    }
    return uuid;
}

uint64_t HttpClient::currentTimeMillis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

} // namespace okplayer
