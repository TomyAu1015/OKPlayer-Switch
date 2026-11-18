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
    std::regex re("(?:\"" + key + "\"|'" + key + "'|\\b" + key + "\\b)\\s*:\\s*(?:\"((?:[^\"\\\\]|\\\\.)*)\"|'((?:[^'\\\\]|\\\\.)*)')");
    std::smatch m;
    if (std::regex_search(text, m, re)) {
        return m[1].matched ? m[1].str() : m[2].str();
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

    // 1. 解析 index.config.js 或 TVBox JSON 中的站点对象 { key: "...", name: "...", api: "..." }
    std::regex site_block_re("\\{[^{}]*?(?:\"key\"|'key'|\\bkey\\b)\\s*:\\s*(?:\"([^\"]+)\"|'([^']+)')([^{}]*)\\}");
    auto begin = std::sregex_iterator(config_text.begin(), config_text.end(), site_block_re);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        std::string full_block = (*it)[0].str();
        std::string key = (*it)[1].matched ? (*it)[1].str() : (*it)[2].str();
        std::string name = extractJsonStringField(full_block, "name");
        std::string api = extractJsonStringField(full_block, "api");
        std::string ext = extractJsonStringField(full_block, "ext");

        if (key.empty() || name.empty()) continue;
        // 过滤非视频站点的配置块
        if (api.empty() && full_block.find("spider") == std::string::npos &&
            full_block.find("type") == std::string::npos) {
            // 在 CatVodOpen / CatPawOpen index.config.js 中，vod.sites 或导出表也可能使用 key + name
            if (key == "quark" || key == "baidu" || key == "ali" || key == "uc") continue;
        }

        SiteConfig sc;
        sc.key = key;
        sc.name = name;
        sc.api = api.empty() ? ("/spider/" + key + "/0") : HttpClient::resolveRelativeUrl(base_dir_url, api);
        sc.ext = ext;
        sc.type = 3;
        sites.push_back(sc);
    }

    // 2. 若源为加密/混淆的 CatVodOpen/王二小放牛娃 index.js.md5 打包格式，确保完整的王二小核心网盘与点播站点就绪
    if (sites.empty()) {
        struct BuiltinWexSite {
            const char* key;
            const char* name;
            const char* api;
        };
        static const BuiltinWexSite WEX_SITES[] = {
            {"wogg",     "玩偶哥哥 (夸克/百度4K)", "/spider/wogg/0"},
            {"muou",     "木偶网盘 (夸克/百度4K)", "/spider/muou/0"},
            {"erxiao",   "二小放牛娃 (夸克/百度4K)", "/spider/erxiao/0"},
            {"libo",     "立播网盘 (夸克/百度)",   "/spider/libo/0"},
            {"shandian", "闪电UC/夸克 (4K臻享)",   "/spider/shandian/0"},
            {"xiaomi",   "小米网盘 (夸克/百度)",   "/spider/xiaomi/0"},
            {"zhizhen",  "至臻网盘 (4K原盘)",     "/spider/zhizhen/0"},
            {"labi",     "蜡笔网盘 (夸克/百度)",   "/spider/labi/0"},
            {"nangua",   "南瓜影视 (直连秒播)",    "/spider/nangua/0"},
            {"jianpian", "荐片影视 (高清直连)",    "/spider/jianpian/0"},
            {"hanju",    "韩剧看看 (直连路线)",    "/spider/hanju/0"},
            {"bili",     "哔哩合集 (番剧/纪录片)", "/spider/bili/0"},
            {"quark_my", "我的夸克网盘 (直连云盘)", "builtin://quark_drive"},
            {"baidu_my", "我的百度网盘 (直连云盘)", "builtin://baidu_drive"}
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
