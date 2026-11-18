#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace okplayer {

static constexpr const char* DEFAULT_WEX_SOURCE_URL =
    "http://wexfnw:wexfnw@cat.xn--4kq62z5rby2qupq9ub.top/index.js.md5";

static constexpr const char* APP_DATA_DIR    = "sdmc:/switch/OKPlayer-Switch";
static constexpr const char* APP_CONFIG_FILE = "sdmc:/switch/OKPlayer-Switch/config.json";
static constexpr const char* APP_CACHE_DIR   = "sdmc:/switch/OKPlayer-Switch/cache";
static constexpr const char* APP_COOKIE_DIR  = "sdmc:/switch/OKPlayer-Switch/cookies";

struct AppConfig {
    std::string source_url = DEFAULT_WEX_SOURCE_URL;
    std::string source_name = "王二小放牛娃 (index.js.md5)";
    std::string cached_md5;

    // 夸克网盘登录凭证
    std::string quark_cookie;
    std::string quark_nickname;
    std::string quark_temp_dir_fid = "0";

    // 百度网盘登录凭证
    std::string baidu_cookie;
    std::string baidu_bduss;
    std::string baidu_stoken;
    std::string baidu_username;

    // 阿里云盘 / UC网盘可选凭证 (供 index.config.js 爬虫读取)
    std::string ali_token;
    std::string uc_cookie;

    // 播放偏好
    int default_quality_index = 0; // 0: 4K/原画优先, 1: 1080P转码优先
    int player_buffer_mb = 32;
    bool auto_skip_intro = false;
    int lan_port = 9978;
};

struct SiteConfig {
    std::string key;
    std::string name;
    int type = 3; // 3 = Spider (QuickJS / CatPaw Node), 1 = CMS JSON, 4 = App
    std::string api;
    std::string ext;
    std::string jar;
    bool searchable = true;
    bool quick_search = true;
    bool filterable = true;
};

struct CategoryItem {
    std::string type_id;
    std::string type_name;
};

struct VodItem {
    std::string vod_id;
    std::string vod_name;
    std::string vod_pic;
    std::string vod_remarks;
    std::string vod_year;
    std::string vod_area;
    std::string vod_actor;
    std::string vod_director;
    std::string vod_content;
    std::string site_key;
    std::string site_name;
    // 播放线路: flag_name -> list of (episode_name, episode_url)
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> play_flags;
};

struct StreamOption {
    std::string label;      // 例如 "夸克4K原画直链", "夸克1080P云端转码", "百度M3U8_1080P"
    std::string url;
    std::map<std::string, std::string> headers;
};

struct PlayResult {
    bool parse = false;
    std::string url;
    std::map<std::string, std::string> headers;
    std::vector<StreamOption> qualities;
    std::string error_msg;
};

struct NetdiskFileItem {
    std::string fid;
    std::string file_name;
    std::string pdir_fid;
    std::string share_fid_token;
    int64_t size_bytes = 0;
    bool is_dir = false;
    bool is_video = false;
    uint64_t updated_at = 0;
};

enum class NetdiskProvider {
    QUARK = 0,
    BAIDU = 1
};

enum class QRLoginState {
    IDLE = 0,
    WAITING_SCAN = 1,
    SCANNED_WAIT_CONFIRM = 2,
    CONFIRMED_SUCCESS = 3,
    EXPIRED_OR_ERROR = 4
};

struct QRLoginSession {
    NetdiskProvider provider = NetdiskProvider::QUARK;
    QRLoginState state = QRLoginState::IDLE;
    std::string qr_token;      // Quark token or Baidu sign
    std::string qr_url;        // URL encoded into the QR matrix
    std::string request_id;
    std::string status_text;
    uint64_t created_ms = 0;
    int qr_size = 0;
    std::vector<uint8_t> qr_modules; // qr_size * qr_size (1 = black, 0 = white)
};

} // namespace okplayer
