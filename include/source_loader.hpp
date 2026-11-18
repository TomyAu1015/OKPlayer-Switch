#pragma once
#include "types.hpp"
#include <string>
#include <vector>

namespace okplayer {

struct LoadedSourceBundle {
    bool success = false;
    bool is_js_md5_bundle = false;
    std::string source_url;
    std::string base_dir_url;
    std::string remote_md5;
    std::string index_config_js;
    std::string index_js;
    std::vector<SiteConfig> sites;
    std::string status_message;
};

class SourceLoader {
public:
    static bool loadAppConfig(AppConfig& cfg);
    static bool saveAppConfig(const AppConfig& cfg);

    // 核心：加载并解析用户输入的源地址
    // 完美适配 http://wexfnw:wexfnw@cat.xn--4kq62z5rby2qupq9ub.top/index.js.md5 (CatVodOpen / CatPawOpen 猫源)
    // 同时兼容 OK影视 3.7.2 标准 JSON / Base64(**) / AES-CBC(2423...2324) 接口
    static LoadedSourceBundle fetchAndParseSource(AppConfig& cfg);

    // 解密 OK影视 加密配置 (移植自 APK LC1/a)
    static std::string decodeOkTvBoxPayload(const std::string& raw_body, const std::string& source_url);

    // 解析 index.config.js 或 TVBox JSON 提取站点列表并注入夸克/百度网盘 Cookie
    static std::vector<SiteConfig> parseSitesFromConfigText(
        const std::string& config_text,
        const std::string& base_dir_url,
        const AppConfig& app_cfg
    );
};

} // namespace okplayer
