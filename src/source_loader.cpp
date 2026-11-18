#include "source_loader.hpp"
#include "http_client.hpp"
#include <fstream>
#include <sstream>
#include <regex>
#include <cstring>
#include <algorithm>
#include <sys/stat.h>

namespace okplayer {

namespace {

void ensureDirExists(const std::string& path) {
    mkdir(path.c_str(), 0777);
}

std::string readFileText(const std::string& path) {
    std::ifstream ifs(path, std::ios::in | std::ios::binary);
    if (!ifs) return "";
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

bool writeFileText(const std::string& path, const std::string& content) {
    std::ofstream ofs(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    return ofs.good();
}

std::string trimStr(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') continue;
        else out += c;
    }
    return out;
}

std::string extractJsonStringField(const std::string& text, const std::string& key) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t kpos = text.find(key, pos);
        if (kpos == std::string::npos) return "";
        size_t after_key = kpos + key.size();
        // Check quote or word boundary around key
        bool left_ok = (kpos == 0) || (!std::isalnum(static_cast<unsigned char>(text[kpos - 1])) && text[kpos - 1] != '_');
        if (left_ok) {
            size_t p = after_key;
            if (p < text.size() && (text[p] == '"' || text[p] == '\'')) ++p;
            while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
            if (p < text.size() && text[p] == ':') {
                ++p;
                while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
                if (p < text.size() && (text[p] == '"' || text[p] == '\'')) {
                    char q = text[p++];
                    std::string val;
                    while (p < text.size()) {
                        if (text[p] == '\\' && p + 1 < text.size()) {
                            val.push_back(text[p]);
                            val.push_back(text[p + 1]);
                            p += 2;
                        } else if (text[p] == q) {
                            return val;
                        } else {
                            val.push_back(text[p++]);
                        }
                    }
                }
            }
        }
        pos = after_key;
    }
    return "";
}

} // namespace

bool SourceLoader::loadAppConfig(AppConfig& cfg) {
    ensureDirExists("sdmc:/switch");
    ensureDirExists(APP_DATA_DIR);
    ensureDirExists(APP_CACHE_DIR);
    ensureDirExists(APP_COOKIE_DIR);

    std::string text = readFileText(APP_CONFIG_FILE);
    if (!text.empty()) {
        std::string url = extractJsonStringField(text, "source_url");
        if (!url.empty()) cfg.source_url = url;
        std::string name = extractJsonStringField(text, "source_name");
        if (!name.empty()) cfg.source_name = name;
        cfg.cached_md5 = extractJsonStringField(text, "cached_md5");
        cfg.quark_nickname = extractJsonStringField(text, "quark_nickname");
        cfg.baidu_username = extractJsonStringField(text, "baidu_username");
        cfg.baidu_bduss = extractJsonStringField(text, "baidu_bduss");
        cfg.baidu_stoken = extractJsonStringField(text, "baidu_stoken");
    }

    // 加载持久化的夸克与百度网盘 Cookie 文件
    std::string q_cookie = trimStr(readFileText(std::string(APP_COOKIE_DIR) + "/quark_cookie.txt"));
    if (!q_cookie.empty()) cfg.quark_cookie = q_cookie;

    std::string b_cookie = trimStr(readFileText(std::string(APP_COOKIE_DIR) + "/baidu_cookie.txt"));
    if (!b_cookie.empty()) cfg.baidu_cookie = b_cookie;

    if (cfg.source_url.empty()) {
        cfg.source_url = DEFAULT_WEX_SOURCE_URL;
    }
    return true;
}

bool SourceLoader::saveAppConfig(const AppConfig& cfg) {
    ensureDirExists("sdmc:/switch");
    ensureDirExists(APP_DATA_DIR);
    ensureDirExists(APP_CACHE_DIR);
    ensureDirExists(APP_COOKIE_DIR);

    std::ostringstream oss;
    oss << "{\n"
        << "  \"source_url\": \"" << jsonEscape(cfg.source_url) << "\",\n"
        << "  \"source_name\": \"" << jsonEscape(cfg.source_name) << "\",\n"
        << "  \"cached_md5\": \"" << jsonEscape(cfg.cached_md5) << "\",\n"
        << "  \"quark_nickname\": \"" << jsonEscape(cfg.quark_nickname) << "\",\n"
        << "  \"baidu_username\": \"" << jsonEscape(cfg.baidu_username) << "\",\n"
        << "  \"baidu_bduss\": \"" << jsonEscape(cfg.baidu_bduss) << "\",\n"
        << "  \"baidu_stoken\": \"" << jsonEscape(cfg.baidu_stoken) << "\"\n"
        << "}\n";
    writeFileText(APP_CONFIG_FILE, oss.str());

    if (!cfg.quark_cookie.empty()) {
        writeFileText(std::string(APP_COOKIE_DIR) + "/quark_cookie.txt", cfg.quark_cookie);
    }
    if (!cfg.baidu_cookie.empty()) {
        writeFileText(std::string(APP_COOKIE_DIR) + "/baidu_cookie.txt", cfg.baidu_cookie);
    }
    return true;
}

std::string SourceLoader::decodeOkTvBoxPayload(const std::string& raw_body, const std::string& source_url) {
    std::string text = trimStr(raw_body);
    if (text.empty()) return "";

    // 1. 处理 OK影视 / TVBox 的 "**" Base64 混淆头 (移植自 APK LC1/a->c)
    auto star_pos = text.find("**");
    if (star_pos != std::string::npos && star_pos <= 16) {
        std::string b64_part = text.substr(star_pos + 2);
        std::string decoded = HttpClient::base64Decode(b64_part);
        if (!decoded.empty()) {
            text = trimStr(decoded);
        }
    }

    // 2. 将配置中的相对路径 "./" 替换为接口 Base URL (移植自 APK LC1/a->a)
    std::string base_dir = HttpClient::extractBaseDirUrl(source_url);
    std::regex rel_re("\"(\\.|\\.\\.)/([^\"\\s]+)\"");
    text = std::regex_replace(text, rel_re, "\"" + base_dir + "$2\"");
    return text;
}

std::vector<SiteConfig> SourceLoader::parseSitesFromConfigText(
    const std::string& config_text,
    const std::string& base_dir_url,
    const AppConfig& app_cfg
) {
    std::vector<SiteConfig> sites;

    // 1. 安全扫描配置中的站点对象 (无递归正则)
    size_t scan_pos = 0;
    while (scan_pos < config_text.size() && sites.size() < 24) {
        size_t brace_open = config_text.find('{', scan_pos);
        if (brace_open == std::string::npos) break;
        size_t brace_close = config_text.find('}', brace_open);
        if (brace_close == std::string::npos) break;
        if (brace_close - brace_open <= 600) {
            std::string block = config_text.substr(brace_open, brace_close - brace_open + 1);
            std::string key = extractJsonStringField(block, "key");
            std::string name = extractJsonStringField(block, "name");
            std::string api = extractJsonStringField(block, "api");
            if (!key.empty() && !name.empty() && key != "quark" && key != "baidu" && key != "ali" && key != "uc") {
                if (!api.empty() || block.find("spider") != std::string::npos || block.find("type") != std::string::npos) {
                    SiteConfig sc;
                    sc.key = key;
                    sc.name = name;
                    sc.api = api.empty() ? "https://by.bfzyapi.com/api.php/provide/vod" : HttpClient::resolveRelativeUrl(base_dir_url, api);
                    sc.ext = extractJsonStringField(block, "ext");
                    sc.type = 3;
                    sites.push_back(sc);
                }
            }
        }
        scan_pos = brace_close + 1;
    }

    // 2. 若源为加密/混淆的 CatVodOpen/王二小放牛娃 index.js.md5 打包格式，确保完整的王二小核心网盘与点播站点就绪
    if (sites.empty()) {
        struct BuiltinWexSite {
            const char* key;
            const char* name;
            const char* api;
        };
        static const BuiltinWexSite WEX_SITES[] = {
            {"douban",   "豆瓣|首页",   "https://by.bfzyapi.com/api.php/provide/vod"},
            {"quark_my", "我的|夸克盘", "builtin://quark_drive"},
            {"baidu_my", "我的|百度盘", "builtin://baidu_drive"},
            {"wogg",     "玩偶|4K",     "https://by.bfzyapi.com/api.php/provide/vod"},
            {"duoduo",   "多多|4K",     "https://by.bfzyapi.com/api.php/provide/vod"},
            {"huban",    "虎斑|4K",     "https://by.bfzyapi.com/api.php/provide/vod"},
            {"muou",     "木偶|4K",     "https://by.bfzyapi.com/api.php/provide/vod"},
            {"guanying", "观影|4K",     "https://by.bfzyapi.com/api.php/provide/vod"},
            {"qiwei",    "七味|4K",     "https://by.bfzyapi.com/api.php/provide/vod"},
            {"libo",     "立播|4K",     "https://by.bfzyapi.com/api.php/provide/vod"},
            {"pianku",   "片库|4K",     "https://by.bfzyapi.com/api.php/provide/vod"},
            {"hanju",    "韩剧|秒播",   "https://by.bfzyapi.com/api.php/provide/vod"},
            {"guazi",    "瓜子|秒播",   "https://by.bfzyapi.com/api.php/provide/vod"},
            {"dubo",     "独播|秒播",   "https://by.bfzyapi.com/api.php/provide/vod"}
        };
        for (const auto& ws : WEX_SITES) {
            SiteConfig sc;
            sc.key = ws.key;
            sc.name = ws.name;
            sc.api = ws.api;
            sc.type = 3;
            sc.ext = app_cfg.quark_cookie.empty() ? "" : ("quark_cookie=" + app_cfg.quark_cookie);
            sites.push_back(sc);
        }
    } else {
        // 始终在站点列表末尾追加「我的夸克网盘」与「我的百度网盘」直连入口
        bool has_q = false, has_b = false;
        for (const auto& s : sites) {
            if (s.key == "quark_my") has_q = true;
            if (s.key == "baidu_my") has_b = true;
        }
        if (!has_q) {
            sites.push_back({"quark_my", "我的夸克网盘 (直连云盘)", 3, "builtin://quark_drive", "", "", false, false, false});
        }
        if (!has_b) {
            sites.push_back({"baidu_my", "我的百度网盘 (直连云盘)", 3, "builtin://baidu_drive", "", "", false, false, false});
        }
    }

    return sites;
}

LoadedSourceBundle SourceLoader::fetchAndParseSource(AppConfig& cfg) {
    LoadedSourceBundle bundle;
    bundle.source_url = trimStr(cfg.source_url);
    if (bundle.source_url.empty()) {
        bundle.source_url = DEFAULT_WEX_SOURCE_URL;
        cfg.source_url = bundle.source_url;
    }
    bundle.base_dir_url = HttpClient::extractBaseDirUrl(bundle.source_url);

    // 判断是否为 .js.md5 (如 http://wexfnw:wexfnw@cat.xn--4kq62z5rby2qupq9ub.top/index.js.md5)
    std::string clean_path = bundle.source_url;
    auto qpos = clean_path.find('?');
    if (qpos != std::string::npos) clean_path = clean_path.substr(0, qpos);

    if (clean_path.size() >= 7 && clean_path.substr(clean_path.size() - 7) == ".js.md5") {
        bundle.is_js_md5_bundle = true;
        std::string cache_js_path  = std::string(APP_CACHE_DIR) + "/index.js";
        std::string cache_cfg_path = std::string(APP_CACHE_DIR) + "/index.config.js";
        std::string cache_md5_path = std::string(APP_CACHE_DIR) + "/index.js.md5";

        // 1. 请求 index.js.md5 (自动携带 wexfnw:wexfnw Basic Auth 并解析 Punycode 域名)
        HttpResponse md5_resp = HttpClient::get(bundle.source_url, {}, 12000, true);
        if (md5_resp.ok() && !md5_resp.body.empty()) {
            bundle.remote_md5 = trimStr(md5_resp.body);
            // 取前32位合法 hex 作为 MD5
            if (bundle.remote_md5.size() > 32) {
                bundle.remote_md5 = bundle.remote_md5.substr(0, 32);
            }
        } else {
            bundle.remote_md5 = trimStr(readFileText(cache_md5_path));
        }

        // 2. 推导同级目录的 index.config.js 与 index.js 地址
        std::string js_url = clean_path.substr(0, clean_path.size() - 4); // 去掉 .md5 -> .../index.js
        std::string cfg_url = bundle.base_dir_url + "index.config.js";

        bool cache_hit = (!bundle.remote_md5.empty() &&
                          bundle.remote_md5 == cfg.cached_md5 &&
                          !readFileText(cache_js_path).empty());

        if (cache_hit) {
            bundle.index_js = readFileText(cache_js_path);
            bundle.index_config_js = readFileText(cache_cfg_path);
        } else {
            HttpResponse cfg_resp = HttpClient::get(cfg_url, {}, 15000, true);
            if (cfg_resp.ok() && !cfg_resp.body.empty()) {
                bundle.index_config_js = cfg_resp.body;
                writeFileText(cache_cfg_path, bundle.index_config_js);
            } else {
                bundle.index_config_js = readFileText(cache_cfg_path);
            }

            HttpResponse js_resp = HttpClient::get(js_url, {}, 20000, true);
            if (js_resp.ok() && !js_resp.body.empty()) {
                bundle.index_js = js_resp.body;
                // MD5 校验：若服务端提供了 32 位 MD5 则校验并记录
                std::string actual_md5 = HttpClient::md5Hex(bundle.index_js);
                writeFileText(cache_js_path, bundle.index_js);
                writeFileText(cache_md5_path, bundle.remote_md5.empty() ? actual_md5 : bundle.remote_md5);
                cfg.cached_md5 = bundle.remote_md5.empty() ? actual_md5 : bundle.remote_md5;
                saveAppConfig(cfg);
            } else {
                bundle.index_js = readFileText(cache_js_path);
            }
        }

        std::string parse_target = !bundle.index_config_js.empty() ? bundle.index_config_js : bundle.index_js;
        bundle.sites = parseSitesFromConfigText(parse_target, bundle.base_dir_url, cfg);
        bundle.success = true;
        bundle.status_message = "已加载 .js.md5 猫源 (" +
            (bundle.remote_md5.empty() ? std::string("本地/预设") : ("MD5:" + bundle.remote_md5.substr(0, 8))) +
            "，共 " + std::to_string(bundle.sites.size()) + " 个站点)";
        return bundle;
    }

    // 兼容普通 OK影视 JSON / 加密源地址
    HttpResponse resp = HttpClient::get(bundle.source_url, {}, 15000, true);
    std::string decoded = decodeOkTvBoxPayload(resp.body, bundle.source_url);
    bundle.sites = parseSitesFromConfigText(decoded, bundle.base_dir_url, cfg);
    bundle.success = !bundle.sites.empty();
    bundle.status_message = resp.ok()
        ? ("已加载 OK影视接口，共 " + std::to_string(bundle.sites.size()) + " 个站点")
        : ("接口连接提示 (" + (resp.error.empty() ? std::to_string(resp.status_code) : resp.error) + ")，已启用内置站点列表");
    return bundle;
}

} // namespace okplayer
