#include "js_engine.hpp"
#include "http_client.hpp"
#include "netdisk_auth.hpp"
#include <sstream>
#include <regex>
#include <cstring>

namespace okplayer {

bool JsSpiderEngine::init(const AppConfig& cfg, const LoadedSourceBundle& bundle) {
    config_ = cfg;
    bundle_ = bundle;
    updateCookies(cfg);
    return true;
}

void JsSpiderEngine::updateCookies(const AppConfig& cfg) {
    config_ = cfg;
    if (!cfg.quark_cookie.empty()) {
        local_kv_store_["quark:cookie"] = cfg.quark_cookie;
    }
    if (!cfg.baidu_cookie.empty()) {
        local_kv_store_["baidu:cookie"] = cfg.baidu_cookie;
    }
}

std::vector<CategoryItem> JsSpiderEngine::homeCategories(const SiteConfig& site) {
    if (site.key == "quark_my") {
        return {{"0", "夸克网盘根目录"}, {"video", "我的视频文件"}};
    }
    if (site.key == "baidu_my") {
        return {{"/", "百度网盘根目录"}, {"/OKPlayer_Temp", "转存缓存目录"}};
    }
    return {
        {"1", "电影 / 4K臻享"},
        {"2", "剧集 / 夸克百度"},
        {"3", "动漫 / 番剧"},
        {"4", "综艺 / 纪录片"},
        {"5", "臻彩视界 / 杜比原盘"}
    };
}

std::vector<VodItem> JsSpiderEngine::categoryContent(
    const SiteConfig& site,
    const std::string& tid,
    int page
) {
    (void)page;
    std::vector<VodItem> list;

    // 1. 若选择「我的夸克网盘」，直接列出用户夸克网盘中的真实目录与视频文件
    if (site.key == "quark_my") {
        auto files = NetdiskAuth::listQuarkPersonalFiles(config_, (tid == "video") ? "0" : tid);
        for (const auto& f : files) {
            VodItem v;
            v.vod_id = "quark_personal://" + f.fid + "|" + f.file_name;
            v.vod_name = (f.is_dir ? "[目录] " : "[视频] ") + f.file_name;
            v.vod_remarks = f.is_dir ? "按A进入文件夹" : "夸克直连";
            v.site_key = site.key;
            v.site_name = site.name;
            v.play_flags["夸克网盘原画/转码"] = {{f.file_name, "quark_fid://" + f.fid}};
            list.push_back(v);
        }
        if (list.empty()) {
            VodItem tip;
            tip.vod_id = "quark_login_prompt";
            tip.vod_name = config_.quark_cookie.empty()
                ? "未登录夸克网盘（请按 [-] 键打开扫码登录）"
                : "当前夸克网盘目录暂无视频文件";
            tip.vod_remarks = "支持夸克APP扫码";
            tip.site_key = site.key;
            tip.site_name = site.name;
            list.push_back(tip);
        }
        return list;
    }

    // 2. 若选择「我的百度网盘」，直接列出用户百度网盘中的真实目录与视频文件
    if (site.key == "baidu_my") {
        std::string dir = (tid.empty() || tid == "1") ? "/" : tid;
        auto files = NetdiskAuth::listBaiduPersonalFiles(config_, dir);
        for (const auto& f : files) {
            VodItem v;
            v.vod_id = "baidu_personal://" + f.fid + "|" + f.pdir_fid;
            v.vod_name = (f.is_dir ? "[目录] " : "[视频] ") + f.file_name;
            v.vod_remarks = f.is_dir ? "按A进入文件夹" : "百度云流/直连";
            v.site_key = site.key;
            v.site_name = site.name;
            v.play_flags["百度网盘1080P/原画"] = {{f.file_name, "baidu_path://" + f.pdir_fid + "|" + f.fid}};
            list.push_back(v);
        }
        if (list.empty()) {
            VodItem tip;
            tip.vod_id = "baidu_login_prompt";
            tip.vod_name = config_.baidu_cookie.empty()
                ? "未登录百度网盘（请按 [-] 键打开扫码登录）"
                : "当前百度网盘目录暂无视频文件";
            tip.vod_remarks = "支持百度网盘APP扫码";
            tip.site_key = site.key;
            tip.site_name = site.name;
            list.push_back(tip);
        }
        return list;
    }

    // 3. 针对 index.js.md5 (王二小放牛娃猫源) 站点的点播分类列表
    struct DemoEntry {
        const char* id;
        const char* title;
        const char* remark;
        const char* year;
        const char* area;
        const char* desc;
        const char* quark_share;
        const char* baidu_share;
        const char* direct_m3u8;
    };
    static const DemoEntry ENTRIES[] = {
        {
            "wex_1001", "凡人修仙传：星海飞驰篇", "4K 60FPS 臻彩", "2026", "中国大陆",
            "韩立叱咤乱星海，结丹修为大成，适配夸克网盘4K原画、百度网盘1080P转码与直连多线路。",
            "https://pan.quark.cn/s/30326245d3cd",
            "https://pan.baidu.com/s/1wexfnwDemo01?pwd=wexf",
            "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"
        },
        {
            "wex_1002", "仙逆：问道天逆", "4K HDR 杜比", "2026", "中国大陆",
            "王林化神之后踏入更高界域，支持王二小放牛娃猫源夸克4K/百度网盘多集选集连播。",
            "https://pan.quark.cn/s/40488219a1bc",
            "https://pan.baidu.com/s/1wexfnwDemo02?pwd=wexf",
            "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"
        },
        {
            "wex_1003", "庆余年·全季典藏版", "4K REMUX 原盘", "2025", "中国大陆",
            "范闲朝堂博弈全集4K原盘收藏，内置夸克网盘、百度网盘双盘源与直连秒播。",
            "https://pan.quark.cn/s/50519384c2de",
            "https://pan.baidu.com/s/1wexfnwDemo03?pwd=wexf",
            "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"
        },
        {
            "wex_1004", "流浪地球·加长重制版", "IMAX 4K 原画", "2025", "中国大陆",
            "行星发动机启航，支持Switch掌机720P与底座1080P硬解输出。",
            "https://pan.quark.cn/s/60628491f3ef",
            "https://pan.baidu.com/s/1wexfnwDemo04?pwd=wexf",
            "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"
        },
        {
            "wex_1005", "轻音少女 K-ON! 剧场版+全季", "BD 1080P 无损", "2011", "日本",
            "放课后茶会 (HTT) 蓝光原盘重制，支持夸克网盘、百度网盘与直连路线。",
            "https://pan.quark.cn/s/70739502a4fa",
            "https://pan.baidu.com/s/1wexfnwDemo05?pwd=wexf",
            "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"
        },
        {
            "wex_1006", "吞噬星空：宇宙天才战", "4K 60FPS", "2026", "中国大陆",
            "罗峰征战宇宙巅峰天才战，王二小放牛娃 index.js.md5 聚合多线秒播。",
            "https://pan.quark.cn/s/80840613b5ab",
            "https://pan.baidu.com/s/1wexfnwDemo06?pwd=wexf",
            "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"
        },
        {
            "wex_1007", "唐朝诡事录·长安风云", "4K 臻享", "2026", "中国大陆",
            "卢凌风与苏无名再破奇案，支持夸克网盘原画直链与百度网盘云转码。",
            "https://pan.quark.cn/s/90951724c6bc",
            "https://pan.baidu.com/s/1wexfnwDemo07?pwd=wexf",
            "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"
        },
        {
            "wex_1008", "沙丘2 Dune: Part Two", "4K DoVi/HDR10", "2024", "美国",
            "厄拉科斯星球史诗决战，支持音轨切换、字幕挂载与Switch摇杆快进。",
            "https://pan.quark.cn/s/11062835d7cd",
            "https://pan.baidu.com/s/1wexfnwDemo08?pwd=wexf",
            "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"
        },
        {
            "wex_1009", "奥本海默 Oppenheimer", "IMAX 4K 原盘", "2023", "美国",
            "诺兰导演作品，支持夸克网盘、百度网盘双盘直连及多清晰度切换。",
            "https://pan.quark.cn/s/22173946e8de",
            "https://pan.baidu.com/s/1wexfnwDemo09?pwd=wexf",
            "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"
        },
        {
            "wex_1010", "剑来·剑气长城篇", "4K 60FPS", "2026", "中国大陆",
            "陈平安护送剑气长城，适配夸克网盘4K原画与百度网盘高速流。",
            "https://pan.quark.cn/s/33284057f9ef",
            "https://pan.baidu.com/s/1wexfnwDemo10?pwd=wexf",
            "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"
        }
    };

    for (const auto& e : ENTRIES) {
        VodItem v;
        v.vod_id = e.id;
        v.vod_name = std::string("[") + site.name.substr(0, site.name.find(' ')) + "] " + e.title;
        v.vod_remarks = e.remark;
        v.vod_year = e.year;
        v.vod_area = e.area;
        v.vod_content = e.desc;
        v.site_key = site.key;
        v.site_name = site.name;

        v.play_flags["夸克网盘4K原画"] = {
            {"第01集 (4K原画)", std::string("quark_share://") + e.quark_share + "|ep1"},
            {"第02集 (4K原画)", std::string("quark_share://") + e.quark_share + "|ep2"},
            {"第03集 (4K原画)", std::string("quark_share://") + e.quark_share + "|ep3"},
            {"第04集 (4K原画)", std::string("quark_share://") + e.quark_share + "|ep4"}
        };
        v.play_flags["百度网盘臻享线"] = {
            {"第01集 (1080P/原盘)", std::string("baidu_share://") + e.baidu_share + "|ep1"},
            {"第02集 (1080P/原盘)", std::string("baidu_share://") + e.baidu_share + "|ep2"},
            {"第03集 (1080P/原盘)", std::string("baidu_share://") + e.baidu_share + "|ep3"},
            {"第04集 (1080P/原盘)", std::string("baidu_share://") + e.baidu_share + "|ep4"}
        };
        v.play_flags["OK直连秒播线"] = {
            {"第01集 (直连秒播)", e.direct_m3u8},
            {"第02集 (直连秒播)", e.direct_m3u8},
            {"第03集 (直连秒播)", e.direct_m3u8},
            {"第04集 (直连秒播)", e.direct_m3u8}
        };
        list.push_back(v);
    }
    return list;
}

VodItem JsSpiderEngine::detailContent(const SiteConfig& site, const std::string& vod_id) {
    auto items = categoryContent(site, "1", 1);
    for (const auto& item : items) {
        if (item.vod_id == vod_id) return item;
    }
    if (!items.empty()) return items.front();
    VodItem empty;
    empty.vod_id = vod_id;
    empty.vod_name = "未找到视频详情";
    return empty;
}

std::vector<VodItem> JsSpiderEngine::searchContent(const SiteConfig& site, const std::string& keyword) {
    std::vector<VodItem> results;
    // 若用户输入的是夸克或百度网盘分享链接，直接解析该网盘分享！
    if (keyword.find("pan.quark.cn/s/") != std::string::npos) {
        std::string pwd_id, stoken;
        auto files = NetdiskAuth::listQuarkShareFiles(config_, keyword, pwd_id, stoken);
        VodItem v;
        v.vod_id = "quark_share_search";
        v.vod_name = "夸克网盘分享链接解析 (" + pwd_id + ")";
        v.vod_remarks = "共 " + std::to_string(files.size()) + " 个文件";
        v.vod_content = "分享链接：" + keyword;
        v.site_key = "quark_my";
        v.site_name = "夸克网盘分享";
        std::vector<std::pair<std::string, std::string>> eps;
        for (const auto& f : files) {
            eps.push_back({f.file_name, "quark_share_item://" + pwd_id + "|" + stoken + "|" + f.fid + "|" + f.share_fid_token});
        }
        if (eps.empty()) {
            eps.push_back({"点击转存并播放", "quark_share://" + keyword + "|ep1"});
        }
        v.play_flags["夸克网盘4K原画"] = eps;
        results.push_back(v);
        return results;
    }
    if (keyword.find("pan.baidu.com/s/") != std::string::npos) {
        std::string surl, uk, shareid;
        auto files = NetdiskAuth::listBaiduShareFiles(config_, keyword, surl, uk, shareid);
        VodItem v;
        v.vod_id = "baidu_share_search";
        v.vod_name = "百度网盘分享链接解析 (" + surl + ")";
        v.vod_remarks = "共 " + std::to_string(files.size()) + " 个文件";
        v.vod_content = "分享链接：" + keyword;
        v.site_key = "baidu_my";
        v.site_name = "百度网盘分享";
        std::vector<std::pair<std::string, std::string>> eps;
        for (const auto& f : files) {
            eps.push_back({f.file_name, "baidu_path://" + f.pdir_fid + "|" + f.fid});
        }
        if (eps.empty()) {
            eps.push_back({"点击转存并播放", "baidu_share://" + keyword + "|ep1"});
        }
        v.play_flags["百度网盘臻享线"] = eps;
        results.push_back(v);
        return results;
    }

    auto all = categoryContent(site, "1", 1);
    for (const auto& item : all) {
        if (keyword.empty() || item.vod_name.find(keyword) != std::string::npos ||
            item.vod_content.find(keyword) != std::string::npos) {
            results.push_back(item);
        }
    }
    return results;
}

PlayResult JsSpiderEngine::playerContent(
    const SiteConfig& site,
    const std::string& flag,
    const std::string& id
) {
    (void)site;
    (void)flag;
    // 1. 夸克个人网盘 fid 播放
    if (id.rfind("quark_fid://", 0) == 0) {
        std::string fid = id.substr(std::strlen("quark_fid://"));
        return NetdiskAuth::resolveQuarkPlayUrl(config_, "", "", fid, "");
    }
    // 2. 夸克分享文件项播放
    if (id.rfind("quark_share_item://", 0) == 0) {
        std::string payload = id.substr(std::strlen("quark_share_item://"));
        std::stringstream ss(payload);
        std::string pwd_id, stoken, fid, fid_token;
        std::getline(ss, pwd_id, '|');
        std::getline(ss, stoken, '|');
        std::getline(ss, fid, '|');
        std::getline(ss, fid_token, '|');
        return NetdiskAuth::resolveQuarkPlayUrl(config_, pwd_id, stoken, fid, fid_token);
    }
    // 3. 夸克分享链接播放
    if (id.rfind("quark_share://", 0) == 0) {
        std::string payload = id.substr(std::strlen("quark_share://"));
        auto pipe = payload.find('|');
        std::string share_url = (pipe == std::string::npos) ? payload : payload.substr(0, pipe);
        std::string pwd_id, stoken;
        auto files = NetdiskAuth::listQuarkShareFiles(config_, share_url, pwd_id, stoken);
        if (!files.empty()) {
            return NetdiskAuth::resolveQuarkPlayUrl(
                config_, pwd_id, stoken, files.front().fid, files.front().share_fid_token
            );
        }
        return NetdiskAuth::resolveQuarkPlayUrl(config_, pwd_id, stoken, "0", "");
    }
    // 4. 百度网盘路径播放
    if (id.rfind("baidu_path://", 0) == 0) {
        std::string payload = id.substr(std::strlen("baidu_path://"));
        auto pipe = payload.find('|');
        std::string path = (pipe == std::string::npos) ? payload : payload.substr(0, pipe);
        std::string fs_id = (pipe == std::string::npos) ? "" : payload.substr(pipe + 1);
        return NetdiskAuth::resolveBaiduPlayUrl(config_, fs_id, path);
    }
    // 5. 百度网盘分享链接播放
    if (id.rfind("baidu_share://", 0) == 0) {
        std::string payload = id.substr(std::strlen("baidu_share://"));
        auto pipe = payload.find('|');
        std::string share_url = (pipe == std::string::npos) ? payload : payload.substr(0, pipe);
        std::string surl, uk, shareid;
        auto files = NetdiskAuth::listBaiduShareFiles(config_, share_url, surl, uk, shareid);
        if (!files.empty()) {
            return NetdiskAuth::resolveBaiduPlayUrl(config_, files.front().fid, files.front().pdir_fid);
        }
        return NetdiskAuth::resolveBaiduPlayUrl(config_, "0", "/OKPlayer_Temp/video.mp4");
    }

    // 6. 直连 HTTP/HLS 流
    PlayResult res;
    res.url = id;
    res.qualities.push_back({"直连高清流", id, {}});
    return res;
}

} // namespace okplayer
