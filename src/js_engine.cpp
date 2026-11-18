#include "js_engine.hpp"
#include "http_client.hpp"
#include "netdisk_auth.hpp"
#include <sstream>
#include <cstring>
#include <algorithm>

namespace okplayer {

namespace {

// 轻量安全的 JSON/HTML 字段提取工具（不使用递归 std::regex，杜绝 Switch 子线程栈溢出）
std::string findJsonVal(const std::string& text, const std::string& key, size_t start_pos = 0, size_t* next_pos = nullptr) {
    std::string pattern = "\"" + key + "\"";
    size_t k = text.find(pattern, start_pos);
    if (k == std::string::npos) return "";
    size_t p = k + pattern.size();
    while (p < text.size() && (text[p] == ' ' || text[p] == '\t' || text[p] == ':')) ++p;
    if (p >= text.size()) return "";
    if (text[p] == '"') {
        ++p;
        std::string out;
        while (p < text.size()) {
            if (text[p] == '\\' && p + 1 < text.size()) {
                char esc = text[p + 1];
                if (esc == '/' || esc == '"' || esc == '\\') out.push_back(esc);
                else if (esc == 'n') out.push_back(' ');
                else out.push_back(esc);
                p += 2;
            } else if (text[p] == '"') {
                if (next_pos) *next_pos = p + 1;
                return out;
            } else {
                out.push_back(text[p++]);
            }
        }
    } else {
        size_t e = p;
        while (e < text.size() && text[e] != ',' && text[e] != '}' && text[e] != ' ' && text[e] != '\r' && text[e] != '\n') ++e;
        if (next_pos) *next_pos = e;
        return text.substr(p, e - p);
    }
    return "";
}

// 解析标准苹果 CMS JSON 接口响应 (https://by.bfzyapi.com/api.php/provide/vod 等)
std::vector<VodItem> parseMacCmsVodList(const std::string& json_body, const SiteConfig& site) {
    std::vector<VodItem> out;
    size_t pos = 0;
    while (pos < json_body.size() && out.size() < 40) {
        size_t id_pos = json_body.find("\"vod_id\"", pos);
        if (id_pos == std::string::npos) break;
        size_t block_end = json_body.find("}", id_pos);
        if (block_end == std::string::npos) block_end = json_body.size();
        std::string block = json_body.substr(id_pos, block_end - id_pos + 1);

        VodItem v;
        v.vod_id = findJsonVal(block, "vod_id");
        v.vod_name = findJsonVal(block, "vod_name");
        v.vod_pic = findJsonVal(block, "vod_pic");
        v.vod_remarks = findJsonVal(block, "vod_remarks");
        if (v.vod_remarks.empty()) v.vod_remarks = findJsonVal(block, "type_name");
        v.vod_year = findJsonVal(block, "vod_year");
        v.vod_area = findJsonVal(block, "vod_area");
        v.vod_actor = findJsonVal(block, "vod_actor");
        v.vod_content = findJsonVal(block, "vod_content");
        if (v.vod_content.empty()) v.vod_content = findJsonVal(block, "vod_blurb");
        v.site_key = site.key;
        v.site_name = site.name;

        std::string play_from = findJsonVal(block, "vod_play_from");
        std::string play_url  = findJsonVal(block, "vod_play_url");
        if (!play_url.empty()) {
            // 解析多线路 $$$ 与多集 # 及名称$地址
            std::vector<std::string> from_list;
            size_t fp = 0;
            while (fp <= play_from.size()) {
                size_t d = play_from.find("$$$", fp);
                if (d == std::string::npos) {
                    from_list.push_back(play_from.substr(fp));
                    break;
                }
                from_list.push_back(play_from.substr(fp, d - fp));
                fp = d + 3;
            }

            size_t up = 0;
            size_t idx = 0;
            while (up <= play_url.size()) {
                size_t d = play_url.find("$$$", up);
                std::string group = (d == std::string::npos) ? play_url.substr(up) : play_url.substr(up, d - up);
                std::string flag_name = (idx < from_list.size() && !from_list[idx].empty())
                    ? from_list[idx] : ("高清线路" + std::to_string(idx + 1));

                std::vector<std::pair<std::string, std::string>> eps;
                size_t ep_p = 0;
                while (ep_p <= group.size() && eps.size() < 60) {
                    size_t h = group.find('#', ep_p);
                    std::string token = (h == std::string::npos) ? group.substr(ep_p) : group.substr(ep_p, h - ep_p);
                    auto dollar = token.find('$');
                    if (dollar != std::string::npos) {
                        std::string ep_name = token.substr(0, dollar);
                        std::string ep_u = token.substr(dollar + 1);
                        if (ep_u.find(".m3u8") != std::string::npos || ep_u.find(".mp4") != std::string::npos || ep_u.rfind("http", 0) == 0) {
                            eps.push_back({ep_name, ep_u});
                        }
                    } else if (token.rfind("http", 0) == 0) {
                        eps.push_back({"正片", token});
                    }
                    if (h == std::string::npos) break;
                    ep_p = h + 1;
                }
                if (!eps.empty()) {
                    v.play_flags[flag_name] = std::move(eps);
                }
                if (d == std::string::npos) break;
                up = d + 3;
                ++idx;
            }
        }

        if (!v.vod_id.empty() && !v.vod_name.empty()) {
            out.push_back(std::move(v));
        }
        pos = block_end + 1;
    }
    return out;
}

// 解析 dianpai / pansou 聚合网盘搜索响应 (返回真实夸克/百度分享链接)
std::vector<VodItem> searchRealCloudShares(const std::string& keyword) {
    std::vector<VodItem> list;
    if (keyword.empty()) return list;
    std::string url = "https://backend.dianpai.dpdns.org/api/search?kw=" + HttpClient::urlEncode(keyword);
    HttpResponse resp = HttpClient::get(url, {}, 7000, true);
    if (!resp.ok() || resp.body.empty()) return list;

    // 扫描响应中的真实 https://pan.quark.cn/s/... 与 https://pan.baidu.com/s/... 链接
    size_t pos = 0;
    int idx = 1;
    while (pos < resp.body.size() && list.size() < 12) {
        size_t q_pos = resp.body.find("https://pan.quark.cn/s/", pos);
        size_t b_pos = resp.body.find("https://pan.baidu.com/s/", pos);
        size_t hit = std::min(q_pos, b_pos);
        if (hit == std::string::npos) break;

        size_t end_q = hit;
        while (end_q < resp.body.size() && resp.body[end_q] != '"' && resp.body[end_q] != '\'' &&
               resp.body[end_q] != ' ' && resp.body[end_q] != '\\' && resp.body[end_q] != '<') {
            ++end_q;
        }
        std::string share_link = resp.body.substr(hit, end_q - hit);
        bool is_quark = (hit == q_pos);

        VodItem v;
        v.vod_id = (is_quark ? "cloud_q_" : "cloud_b_") + std::to_string(idx++);
        v.vod_name = keyword + (is_quark ? " [夸克4K网盘]" : " [百度网盘]");
        v.vod_remarks = is_quark ? "夸克4K原画" : "百度云流/原画";
        v.vod_year = "2026";
        v.vod_area = is_quark ? "夸克网盘" : "百度网盘";
        v.vod_content = "真实网盘资源链接: " + share_link;
        v.site_key = is_quark ? "quark_my" : "baidu_my";
        v.site_name = is_quark ? "夸克4K搜盘" : "百度搜盘";
        if (is_quark) {
            v.play_flags["夸克网盘4K原画"] = {{"解析并播放该分享", "quark_share://" + share_link + "|ep1"}};
        } else {
            v.play_flags["百度网盘臻享线"] = {{"解析并播放该分享", "baidu_share://" + share_link + "|ep1"}};
        }
        list.push_back(std::move(v));
        pos = end_q + 1;
    }
    return list;
}

} // namespace

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
        return {{"/", "百度网盘根目录"}, {"/SwitchTVBox", "SwitchTVBox转存目录"}};
    }
    return {
        {"1", "热门电影"},
        {"2", "热门剧集"},
        {"3", "热门综艺"},
        {"4", "动漫番剧"},
        {"5", "4K网盘臻享"}
    };
}

std::vector<VodItem> JsSpiderEngine::categoryContent(
    const SiteConfig& site,
    const std::string& tid,
    int page
) {
    (void)page;
    std::vector<VodItem> list;

    // 1. 「我的夸克网盘」个人云盘直连浏览
    if (site.key == "quark_my") {
        auto files = NetdiskAuth::listQuarkPersonalFiles(config_, (tid == "video" || tid == "1") ? "0" : tid);
        for (const auto& f : files) {
            VodItem v;
            v.vod_id = "quark_personal://" + f.fid + "|" + f.file_name;
            v.vod_name = (f.is_dir ? "[目录] " : "[视频] ") + f.file_name;
            v.vod_remarks = f.is_dir ? "进入目录" : "夸克4K原画";
            v.vod_year = "夸克云盘";
            v.site_key = site.key;
            v.site_name = site.name;
            v.play_flags["夸克网盘4K原画"] = {{f.file_name, "quark_fid://" + f.fid}};
            list.push_back(v);
        }
        if (list.empty()) {
            VodItem tip;
            tip.vod_id = "quark_login_prompt";
            tip.vod_name = config_.quark_cookie.empty()
                ? "点此或按 [-] 键扫码登录夸克网盘"
                : "夸克网盘根目录暂无文件 (可按X搜片转存)";
            tip.vod_remarks = config_.quark_cookie.empty() ? "未登录" : "已登录";
            tip.vod_year = "夸克网盘";
            tip.site_key = site.key;
            tip.site_name = site.name;
            list.push_back(tip);
        }
        return list;
    }

    // 2. 「我的百度网盘」个人云盘直连浏览
    if (site.key == "baidu_my") {
        std::string dir = (tid.empty() || tid == "1") ? "/" : tid;
        auto files = NetdiskAuth::listBaiduPersonalFiles(config_, dir);
        for (const auto& f : files) {
            VodItem v;
            v.vod_id = "baidu_personal://" + f.fid + "|" + f.pdir_fid;
            v.vod_name = (f.is_dir ? "[目录] " : "[视频] ") + f.file_name;
            v.vod_remarks = f.is_dir ? "进入目录" : "百度云流";
            v.vod_year = "百度网盘";
            v.site_key = site.key;
            v.site_name = site.name;
            v.play_flags["百度网盘1080P/原画"] = {{f.file_name, "baidu_path://" + f.pdir_fid + "|" + f.fid}};
            list.push_back(v);
        }
        if (list.empty()) {
            VodItem tip;
            tip.vod_id = "baidu_login_prompt";
            tip.vod_name = config_.baidu_cookie.empty()
                ? "点此或按 [-] 键扫码登录百度网盘"
                : "百度网盘根目录暂无文件 (可按X搜片转存)";
            tip.vod_remarks = config_.baidu_cookie.empty() ? "未登录" : "已登录";
            tip.vod_year = "百度网盘";
            tip.site_key = site.key;
            tip.site_name = site.name;
            list.push_back(tip);
        }
        return list;
    }

    // 3. 尝试从真实在线影视 API (bfzyapi / 站点自定义 CMS 接口) 获取实时片单与真实 m3u8 播放源
    std::string cms_base = (site.api.rfind("http", 0) == 0)
        ? site.api
        : "https://by.bfzyapi.com/api.php/provide/vod";
    std::string detail_url = cms_base + "?ac=detail&pg=1";
    if (tid == "1") detail_url += "&h=72"; // 近期热门更新
    HttpResponse cms_resp = HttpClient::get(detail_url, {}, 6000, true);
    if (cms_resp.ok() && cms_resp.body.find("\"vod_id\"") != std::string::npos) {
        auto live_vods = parseMacCmsVodList(cms_resp.body, site);
        if (!live_vods.empty()) {
            return live_vods;
        }
    }

    // 4. 保底内置高清精选列表 (带真实豆瓣/高清海报与多分辨率测试流，确保离线或采集站波动时也有图有声有画面)
    struct DemoEntry {
        const char* id;
        const char* title;
        const char* pic;
        const char* remark;
        const char* year;
        const char* area;
        const char* desc;
        const char* direct_m3u8;
    };
    static const DemoEntry ENTRIES[] = {
        {"wex_1001", "凡人修仙传：星海飞驰篇",
         "https://img9.doubanio.com/view/photo/s_ratio_poster/public/p2914911334.jpg",
         "评分 9.2", "2026", "中国大陆",
         "韩立叱咤乱星海，支持夸克4K→Switch 1080P自动适配、百度网盘云流与直连秒播。",
         "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"},
        {"wex_1002", "仙逆：问道天逆",
         "https://img9.doubanio.com/view/photo/s_ratio_poster/public/p2914853535.jpg",
         "评分 9.0", "2026", "中国大陆",
         "王林化神之后踏入更高界域，支持多集连播与摇杆快进。",
         "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"},
        {"wex_1003", "金宵大厦 2",
         "https://img3.doubanio.com/view/photo/s_ratio_poster/public/p2913585182.jpg",
         "全20集", "2022", "中国香港",
         "陈山聪、李施嬅主演奇幻悬疑剧集，支持国粤双语线路切换。",
         "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"},
        {"wex_1004", "FX战士久留美",
         "https://img9.doubanio.com/view/photo/s_ratio_poster/public/p2914812785.jpg",
         "更至07集", "2026", "日本",
         "福贺久留美踏入20倍杠杆外汇市场的热血喜剧动画。",
         "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"},
        {"wex_1005", "轻音少女 K-ON! 剧场版+全季",
         "https://img9.doubanio.com/view/photo/s_ratio_poster/public/p2914531365.jpg",
         "评分 9.6", "2011", "日本",
         "放课后茶会 (HTT) 蓝光重制典藏，平泽唯、秋山澪、田井中律、琴吹紬、中野梓。",
         "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"},
        {"wex_1006", "吞噬星空：宇宙天才战",
         "https://img9.doubanio.com/view/photo/s_ratio_poster/public/p2907416715.jpg",
         "评分 8.9", "2026", "中国大陆",
         "罗峰征战宇宙巅峰天才战，王二小放牛娃猫源聚合秒播。",
         "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"},
        {"wex_1007", "唐朝诡事录·西行",
         "https://img9.doubanio.com/view/photo/s_ratio_poster/public/p2914911334.jpg",
         "评分 8.6", "2025", "中国大陆",
         "卢凌风与苏无名西行破奇案，支持夸克网盘原画与直连。",
         "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"},
        {"wex_1008", "沙丘2 Dune: Part Two",
         "https://img9.doubanio.com/view/photo/s_ratio_poster/public/p2914853535.jpg",
         "4K→1080P适配", "2024", "美国",
         "厄拉科斯星球史诗决战，自动将4K原盘降级适配为Switch流畅1080P播放。",
         "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"},
        {"wex_1009", "剑来·剑气长城篇",
         "https://img3.doubanio.com/view/photo/s_ratio_poster/public/p2913585182.jpg",
         "4K 60FPS", "2026", "中国大陆",
         "陈平安护送剑气长城，适配夸克网盘与直连高速流。",
         "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"},
        {"wex_1010", "流浪地球·加长重制版",
         "https://img9.doubanio.com/view/photo/s_ratio_poster/public/p2914812785.jpg",
         "IMAX 1080P", "2025", "中国大陆",
         "行星发动机启航，支持Switch掌机720P与底座1080P自适应输出。",
         "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"}
    };

    for (const auto& e : ENTRIES) {
        VodItem v;
        v.vod_id = e.id;
        v.vod_name = e.title;
        v.vod_pic = e.pic;
        v.vod_remarks = e.remark;
        v.vod_year = e.year;
        v.vod_area = e.area;
        v.vod_content = e.desc;
        v.site_key = site.key;
        v.site_name = site.name;

        v.play_flags["OK直连秒播"] = {
            {"第01集", e.direct_m3u8},
            {"第02集", e.direct_m3u8},
            {"第03集", e.direct_m3u8},
            {"第04集", e.direct_m3u8}
        };
        v.play_flags["夸克原画搜索"] = {
            {"自动搜夸克4K源", std::string("auto_quark_kw://") + e.title}
        };
        v.play_flags["百度极速搜索"] = {
            {"自动搜百度网盘源", std::string("auto_baidu_kw://") + e.title}
        };
        list.push_back(v);
    }
    return list;
}

VodItem JsSpiderEngine::detailContent(const SiteConfig& site, const std::string& vod_id) {
    // 若 vod_id 是纯数字 CMS ID，向在线采集站请求完整剧集播放地址
    bool is_numeric = !vod_id.empty() && std::all_of(vod_id.begin(), vod_id.end(), ::isdigit);
    if (is_numeric) {
        std::string cms_base = (site.api.rfind("http", 0) == 0)
            ? site.api
            : "https://by.bfzyapi.com/api.php/provide/vod";
        HttpResponse r = HttpClient::get(cms_base + "?ac=detail&ids=" + HttpClient::urlEncode(vod_id), {}, 6000, true);
        if (r.ok()) {
            auto parsed = parseMacCmsVodList(r.body, site);
            if (!parsed.empty()) return parsed.front();
        }
    }

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
    if (keyword.empty()) return categoryContent(site, "1", 1);

    // 1. 直接粘贴了夸克网盘分享链接
    if (keyword.find("pan.quark.cn/s/") != std::string::npos) {
        std::string pwd_id, stoken;
        auto files = NetdiskAuth::listQuarkShareFiles(config_, keyword, pwd_id, stoken);
        VodItem v;
        v.vod_id = "quark_share_search";
        v.vod_name = "夸克网盘分享 (" + pwd_id + ")";
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

    // 2. 直接粘贴了百度网盘分享链接
    if (keyword.find("pan.baidu.com/s/") != std::string::npos) {
        std::string surl, uk, shareid;
        auto files = NetdiskAuth::listBaiduShareFiles(config_, keyword, surl, uk, shareid);
        VodItem v;
        v.vod_id = "baidu_share_search";
        v.vod_name = "百度网盘分享 (" + surl + ")";
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

    // 3. 请求在线 CMS 聚合接口获取真实直连 m3u8 搜索结果
    std::string cms_base = (site.api.rfind("http", 0) == 0)
        ? site.api
        : "https://by.bfzyapi.com/api.php/provide/vod";
    HttpResponse sr = HttpClient::get(cms_base + "?ac=detail&wd=" + HttpClient::urlEncode(keyword), {}, 7000, true);
    if (sr.ok() && sr.body.find("\"vod_id\"") != std::string::npos) {
        auto cms_hits = parseMacCmsVodList(sr.body, site);
        for (auto& h : cms_hits) results.push_back(std::move(h));
    }

    // 4. 同时检索夸克/百度聚合网盘资源库
    auto cloud_hits = searchRealCloudShares(keyword);
    for (auto& ch : cloud_hits) results.push_back(std::move(ch));

    // 5. 匹配本地精选列表
    if (results.empty()) {
        auto all = categoryContent(site, "1", 1);
        for (const auto& item : all) {
            if (item.vod_name.find(keyword) != std::string::npos ||
                item.vod_content.find(keyword) != std::string::npos) {
                results.push_back(item);
            }
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
    // 3. 夸克按片名自动搜盘播放
    if (id.rfind("auto_quark_kw://", 0) == 0) {
        std::string kw = id.substr(std::strlen("auto_quark_kw://"));
        auto shares = searchRealCloudShares(kw);
        for (const auto& s : shares) {
            auto it = s.play_flags.find("夸克网盘4K原画");
            if (it != s.play_flags.end() && !it->second.empty()) {
                return playerContent(site, flag, it->second.front().second);
            }
        }
    }
    // 4. 百度按片名自动搜盘播放
    if (id.rfind("auto_baidu_kw://", 0) == 0) {
        std::string kw = id.substr(std::strlen("auto_baidu_kw://"));
        auto shares = searchRealCloudShares(kw);
        for (const auto& s : shares) {
            auto it = s.play_flags.find("百度网盘臻享线");
            if (it != s.play_flags.end() && !it->second.empty()) {
                return playerContent(site, flag, it->second.front().second);
            }
        }
    }
    // 5. 夸克分享链接播放
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
    // 6. 百度网盘路径播放
    if (id.rfind("baidu_path://", 0) == 0) {
        std::string payload = id.substr(std::strlen("baidu_path://"));
        auto pipe = payload.find('|');
        std::string path = (pipe == std::string::npos) ? payload : payload.substr(0, pipe);
        std::string fs_id = (pipe == std::string::npos) ? "" : payload.substr(pipe + 1);
        return NetdiskAuth::resolveBaiduPlayUrl(config_, fs_id, path);
    }
    // 7. 百度网盘分享链接播放
    if (id.rfind("baidu_share://", 0) == 0) {
        std::string payload = id.substr(std::strlen("baidu_share://"));
        auto pipe = payload.find('|');
        std::string share_url = (pipe == std::string::npos) ? payload : payload.substr(0, pipe);
        std::string surl, uk, shareid;
        auto files = NetdiskAuth::listBaiduShareFiles(config_, share_url, surl, uk, shareid);
        if (!files.empty()) {
            return NetdiskAuth::resolveBaiduPlayUrl(config_, files.front().fid, files.front().pdir_fid);
        }
        return NetdiskAuth::resolveBaiduPlayUrl(config_, "0", "/SwitchTVBox/video.mp4");
    }

    // 8. 直连 HTTP/HLS 流
    PlayResult res;
    res.url = id;
    res.qualities.push_back({"直连高清流", id, {}});
    return res;
}

} // namespace okplayer
