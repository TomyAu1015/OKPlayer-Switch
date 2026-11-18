#pragma once
#include "types.hpp"
#include "source_loader.hpp"
#include <string>
#include <vector>

namespace okplayer {

class JsSpiderEngine {
public:
    bool init(const AppConfig& cfg, const LoadedSourceBundle& bundle);
    void updateCookies(const AppConfig& cfg);

    // 对应 APK Lcom/fongmi/quickjs/crawler/Spider 与 CatPawOpen /spider/<key>/<method>
    std::vector<CategoryItem> homeCategories(const SiteConfig& site);
    std::vector<VodItem> categoryContent(const SiteConfig& site, const std::string& tid, int page);
    VodItem detailContent(const SiteConfig& site, const std::string& vod_id);
    std::vector<VodItem> searchContent(const SiteConfig& site, const std::string& keyword);
    PlayResult playerContent(const SiteConfig& site, const std::string& flag, const std::string& id);

private:
    AppConfig config_;
    LoadedSourceBundle bundle_;
    std::map<std::string, std::string> local_kv_store_; // 对应 APK com.fongmi.quickjs.method.Local
};

} // namespace okplayer
