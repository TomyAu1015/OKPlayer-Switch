#include "netdisk_auth.hpp"
#include "http_client.hpp"
#include "source_loader.hpp"
#include <regex>
#include <sstream>
#include <algorithm>
#include <cstring>

namespace okplayer {

namespace {

static const char* QUARK_WEB_UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";

// 夸克 PC 客户端专用 UA (绕过原画直链 23018 限制)
static const char* QUARK_CLIENT_UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) quark-cloud-drive/2.5.56 Chrome/100.0.4896.160 "
    "Electron/18.3.5.12-a038f7b798 Safari/537.36 Channel/pckk_other_ch";

static const char* BAIDU_PAN_UA =
    "netdisk;P2SP;3.0.10.22;Windows;10.0.19045";

std::string regexExtract(const std::string& text, const std::string& pattern, int group = 1) {
    try {
        std::regex re(pattern);
        std::smatch m;
        if (std::regex_search(text, m, re) && static_cast<int>(m.size()) > group) {
            return m[group].str();
        }
    } catch (...) {}
    return "";
}

bool isVideoFilename(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){ return std::tolower(c); });
    static const char* exts[] = {".mp4", ".mkv", ".m3u8", ".ts", ".flv", ".avi", ".mov", ".webm", ".rmvb", ".wmv", ".iso"};
    for (const char* ext : exts) {
        if (lower.size() >= std::strlen(ext) &&
            lower.compare(lower.size() - std::strlen(ext), std::strlen(ext), ext) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

// =============================================================================
// 1. 夸克网盘扫码与解析实现
// =============================================================================

QRLoginSession NetdiskAuth::startQuarkQRLogin() {
    QRLoginSession s;
    s.provider = NetdiskProvider::QUARK;
    s.request_id = HttpClient::generateUuidV4();
    s.created_ms = HttpClient::currentTimeMillis();

    std::string api = "https://uop.quark.cn/cas/ajax/getTokenForQrcodeLogin"
                      "?client_id=532&v=1.2&request_id=" + s.request_id;
    HttpResponse resp = HttpClient::get(api, {{"User-Agent", QUARK_WEB_UA}, {"Referer", "https://pan.quark.cn/"}});
    std::string token = regexExtract(resp.body, "\"token\"\\s*:\\s*\"([^\"]+)\"");
    if (!token.empty()) {
        s.qr_token = token;
        // 夸克 APP 扫码专用短链协议 (移植自 SwitchTVBox，彻底解决 b.quark.cn 报「出错了」)
        s.qr_url = "https://su.quark.cn/4_eMHBJ?token=" + token +
                   "&client_id=532&ssb=weblogin&uc_param_str=&uc_biz_str=S%3Acustom%7COPT%3ASAREA%400%7COPT%3AIMMERSIVE%401%7COPT%3ABACK_BTN_STYLE%400";
        s.state = QRLoginState::WAITING_SCAN;
        s.status_text = "请打开手机【夸克APP】扫一扫登录夸克网盘";
        generateQrMatrix(s.qr_url, s.qr_size, s.qr_modules);
    } else {
        s.state = QRLoginState::EXPIRED_OR_ERROR;
        s.qr_size = 0;
        s.qr_modules.clear();
        s.status_text = "获取夸克二维码失败(" + (resp.error.empty() ? "网络异常" : resp.error) + ")，请按[A]或点刷新重试";
    }
    return s;
}

bool NetdiskAuth::pollQuarkQRLogin(QRLoginSession& session, AppConfig& cfg) {
    if (session.state != QRLoginState::WAITING_SCAN &&
        session.state != QRLoginState::SCANNED_WAIT_CONFIRM) {
        return false;
    }
    std::string req_id = HttpClient::generateUuidV4();
    std::string poll_url = "https://uop.quark.cn/cas/ajax/getServiceTicketByQrcodeToken"
                           "?client_id=532&v=1.2&token=" + session.qr_token +
                           "&request_id=" + req_id;
    HttpResponse resp = HttpClient::get(poll_url, {{"User-Agent", QUARK_WEB_UA}, {"Referer", "https://pan.quark.cn/"}});
    if (!resp.ok()) return false;

    std::string status_code = regexExtract(resp.body, "\"status\"\\s*:\\s*(\\d+)");
    std::string service_ticket = regexExtract(resp.body, "\"service_ticket\"\\s*:\\s*\"([^\"]+)\"");

    if (status_code == "2000000" && !service_ticket.empty()) {
        std::string info_url = "https://pan.quark.cn/account/info?lw=scan&st=" + service_ticket;
        HttpResponse info_resp = HttpClient::get(
            info_url,
            {{"User-Agent", QUARK_WEB_UA}, {"Referer", "https://pan.quark.cn/"}}
        );
        std::string merged_cookie = HttpClient::mergeSetCookies(cfg.quark_cookie, resp.set_cookies);
        merged_cookie = HttpClient::mergeSetCookies(merged_cookie, info_resp.set_cookies);

        // 调用 file/sort 触发夸克下发完整 __puus / __pus Cookie
        HttpResponse drive_resp = HttpClient::get(
            "https://drive-pc.quark.cn/1/clouddrive/file/sort?pr=ucpro&fr=pc&pdir_fid=0&_page=1&_size=1",
            {{"User-Agent", QUARK_WEB_UA}, {"Origin", "https://pan.quark.cn"}, {"Referer", "https://pan.quark.cn/"}, {"Cookie", merged_cookie}}
        );
        merged_cookie = HttpClient::mergeSetCookies(merged_cookie, drive_resp.set_cookies);

        cfg.quark_cookie = merged_cookie;
        std::string nick = regexExtract(info_resp.body, "\"nickname\"\\s*:\\s*\"([^\"]+)\"");
        cfg.quark_nickname = nick.empty() ? "夸克已授权用户" : nick;
        SourceLoader::saveAppConfig(cfg);

        session.state = QRLoginState::CONFIRMED_SUCCESS;
        session.status_text = "夸克网盘登录成功！账号：" + cfg.quark_nickname;
        return true;
    } else if (status_code == "50004001") {
        session.state = QRLoginState::SCANNED_WAIT_CONFIRM;
        session.status_text = "已扫码，请在手机夸克APP上点击【确认登录】";
    } else if (status_code == "50004002") {
        session.state = QRLoginState::EXPIRED_OR_ERROR;
        session.status_text = "二维码已过期，请按 [A] 或点击刷新重新获取";
    }
    return false;
}

bool NetdiskAuth::verifyQuarkCookie(AppConfig& cfg) {
    if (cfg.quark_cookie.empty()) return false;
    HttpResponse resp = HttpClient::get(
        "https://pan.quark.cn/account/info?fr=pc&platform=pc",
        {
            {"User-Agent", QUARK_WEB_UA},
            {"Origin", "https://pan.quark.cn"},
            {"Referer", "https://pan.quark.cn/"},
            {"Cookie", cfg.quark_cookie}
        }
    );
    if (!resp.set_cookies.empty()) {
        cfg.quark_cookie = HttpClient::mergeSetCookies(cfg.quark_cookie, resp.set_cookies);
    }
    std::string nick = regexExtract(resp.body, "\"nickname\"\\s*:\\s*\"([^\"]+)\"");
    if (!nick.empty()) {
        cfg.quark_nickname = nick;
        SourceLoader::saveAppConfig(cfg);
        return true;
    }
    return false;
}

std::vector<NetdiskFileItem> NetdiskAuth::listQuarkPersonalFiles(
    const AppConfig& cfg,
    const std::string& pdir_fid
) {
    std::vector<NetdiskFileItem> items;
    if (cfg.quark_cookie.empty()) return items;

    std::string url = "https://drive-pc.quark.cn/1/clouddrive/file/sort"
                      "?pr=ucpro&fr=pc&uc_param_str=&pdir_fid=" + HttpClient::urlEncode(pdir_fid) +
                      "&_page=1&_size=100&_fetch_total=1&_fetch_sub_dirs=0&_sort=file_type:asc,updated_at:desc";
    HttpResponse resp = HttpClient::get(
        url,
        {
            {"User-Agent", QUARK_WEB_UA},
            {"Origin", "https://pan.quark.cn"},
            {"Referer", "https://pan.quark.cn/"},
            {"Cookie", cfg.quark_cookie}
        }
    );
    if (!resp.ok()) return items;

    std::regex item_re("\\{[^{}]*?\"fid\"\\s*:\\s*\"([^\"]+)\"[^{}]*?\"file_name\"\\s*:\\s*\"([^\"]+)\"[^{}]*?\\}");
    for (auto it = std::sregex_iterator(resp.body.begin(), resp.body.end(), item_re);
         it != std::sregex_iterator(); ++it) {
        std::string block = (*it)[0].str();
        NetdiskFileItem fi;
        fi.fid = (*it)[1].str();
        fi.file_name = (*it)[2].str();
        fi.pdir_fid = pdir_fid;
        fi.is_dir = (block.find("\"dir\":true") != std::string::npos ||
                     block.find("\"file_type\":0") != std::string::npos);
        fi.is_video = !fi.is_dir && isVideoFilename(fi.file_name);
        std::string sz = regexExtract(block, "\"size\"\\s*:\\s*(\\d+)");
        if (!sz.empty()) fi.size_bytes = std::stoll(sz);
        items.push_back(fi);
    }
    return items;
}

std::vector<NetdiskFileItem> NetdiskAuth::listQuarkShareFiles(
    const AppConfig& cfg,
    const std::string& share_url,
    std::string& out_pwd_id,
    std::string& out_stoken,
    const std::string& pdir_fid
) {
    std::vector<NetdiskFileItem> items;
    out_pwd_id = regexExtract(share_url, "/s/([a-zA-Z0-9]+)");
    std::string passcode = regexExtract(share_url, "pwd=([a-zA-Z0-9]+)");
    if (out_pwd_id.empty()) return items;

    std::map<std::string, std::string> hdrs = {
        {"User-Agent", QUARK_WEB_UA},
        {"Origin", "https://pan.quark.cn"},
        {"Referer", "https://pan.quark.cn/"},
        {"Cookie", cfg.quark_cookie}
    };

    if (out_stoken.empty()) {
        std::string token_api = "https://drive-pc.quark.cn/1/clouddrive/share/sharepage/token?pr=ucpro&fr=pc";
        std::string body = "{\"pwd_id\":\"" + out_pwd_id + "\",\"passcode\":\"" + passcode + "\"}";
        HttpResponse tr = HttpClient::postJson(token_api, body, hdrs);
        out_stoken = regexExtract(tr.body, "\"stoken\"\\s*:\\s*\"([^\"]+)\"");
    }
    if (out_stoken.empty()) return items;

    std::string detail_api =
        "https://drive-pc.quark.cn/1/clouddrive/share/sharepage/detail?pr=ucpro&fr=pc"
        "&pwd_id=" + HttpClient::urlEncode(out_pwd_id) +
        "&stoken=" + HttpClient::urlEncode(out_stoken) +
        "&pdir_fid=" + HttpClient::urlEncode(pdir_fid) +
        "&force=0&_page=1&_size=100&_sort=file_type:asc,file_name:asc";
    HttpResponse dr = HttpClient::get(detail_api, hdrs);
    if (!dr.ok()) return items;

    std::regex item_re("\\{[^{}]*?\"fid\"\\s*:\\s*\"([^\"]+)\"[^{}]*?\"file_name\"\\s*:\\s*\"([^\"]+)\"[^{}]*?\\}");
    for (auto it = std::sregex_iterator(dr.body.begin(), dr.body.end(), item_re);
         it != std::sregex_iterator(); ++it) {
        std::string block = (*it)[0].str();
        NetdiskFileItem fi;
        fi.fid = (*it)[1].str();
        fi.file_name = (*it)[2].str();
        fi.pdir_fid = pdir_fid;
        fi.share_fid_token = regexExtract(block, "\"share_fid_token\"\\s*:\\s*\"([^\"]+)\"");
        fi.is_dir = (block.find("\"dir\":true") != std::string::npos ||
                     block.find("\"file_type\":0") != std::string::npos);
        fi.is_video = !fi.is_dir && isVideoFilename(fi.file_name);
        items.push_back(fi);
    }
    return items;
}

PlayResult NetdiskAuth::resolveQuarkPlayUrl(
    const AppConfig& cfg,
    const std::string& pwd_id,
    const std::string& stoken,
    const std::string& fid,
    const std::string& share_fid_token
) {
    PlayResult res;
    if (cfg.quark_cookie.empty()) {
        res.error_msg = "请先按 [-] 键或在【夸克网盘】页扫码登录夸克账号";
        return res;
    }

    std::map<std::string, std::string> hdrs = {
        {"User-Agent", QUARK_CLIENT_UA},
        {"Origin", "https://pan.quark.cn"},
        {"Referer", "https://pan.quark.cn/"},
        {"Cookie", cfg.quark_cookie}
    };

    std::string target_fid = fid;
    // 若来自他人分享链接，先快速转存至用户网盘临时目录以获取 4K 原画与转码播放权限
    if (!pwd_id.empty() && !stoken.empty() && !share_fid_token.empty()) {
        std::string save_api = "https://drive-pc.quark.cn/1/clouddrive/share/sharepage/save?pr=ucpro&fr=pc";
        std::string save_body = "{\"fid_list\":[\"" + fid + "\"],\"fid_token_list\":[\"" + share_fid_token +
                                "\"],\"to_pdir_fid\":\"" + cfg.quark_temp_dir_fid +
                                "\",\"pwd_id\":\"" + pwd_id + "\",\"stoken\":\"" + stoken +
                                "\",\"pdir_fid\":\"0\",\"scene\":\"link\"}";
        HttpResponse sr = HttpClient::postJson(save_api, save_body, hdrs);
        std::string task_id = regexExtract(sr.body, "\"task_id\"\\s*:\\s*\"([^\"]+)\"");
        if (!task_id.empty()) {
            for (int i = 0; i < 6; ++i) {
                std::string task_url = "https://drive-pc.quark.cn/1/clouddrive/task?pr=ucpro&fr=pc&task_id=" +
                                       task_id + "&retry_index=" + std::to_string(i);
                HttpResponse tr = HttpClient::get(task_url, hdrs);
                std::string top_fid = regexExtract(tr.body, "\"save_as_top_fids\"\\s*:\\s*\\[\\s*\"([^\"]+)\"");
                if (!top_fid.empty()) {
                    target_fid = top_fid;
                    break;
                }
            }
        }
    }

    // 1. 请求夸克 4K/原画高速直链 (使用 QUARK_CLIENT_UA 绕过 23018 限速错误)
    std::string dl_api = "https://drive-pc.quark.cn/1/clouddrive/file/download?pr=ucpro&fr=pc&sys=win32&ve=2.5.56";
    std::string dl_body = "{\"fids\":[\"" + target_fid + "\"]}";
    HttpResponse dlr = HttpClient::postJson(dl_api, dl_body, hdrs);
    std::string raw_url = regexExtract(dlr.body, "\"download_url\"\\s*:\\s*\"([^\"]+)\"");
    if (!raw_url.empty()) {
        StreamOption opt;
        opt.label = "夸克 4K/原画直链";
        opt.url = raw_url;
        opt.headers = hdrs;
        res.qualities.push_back(opt);
    }

    // 2. 请求夸克云端 HLS 多码率转码流 (4K / 2K / 超清 / 高清)
    std::string play_api = "https://drive-pc.quark.cn/1/clouddrive/file/v2/play?pr=ucpro&fr=pc";
    std::string play_body = "{\"fid\":\"" + target_fid + "\",\"resolutions\":\"normal,low,high,super,2k,4k\",\"supports\":\"fmp4,m3u8\"}";
    HttpResponse pr = HttpClient::postJson(play_api, play_body, hdrs);
    std::regex v_re("\\{[^{}]*?\"resolution\"\\s*:\\s*\"([^\"]+)\"[^{}]*?\"url\"\\s*:\\s*\"([^\"]+)\"[^{}]*?\\}");
    for (auto it = std::sregex_iterator(pr.body.begin(), pr.body.end(), v_re);
         it != std::sregex_iterator(); ++it) {
        StreamOption opt;
        opt.label = "夸克云转码 (" + (*it)[1].str() + ")";
        opt.url = (*it)[2].str();
        opt.headers = hdrs;
        res.qualities.push_back(opt);
    }

    if (!res.qualities.empty()) {
        res.url = res.qualities.front().url;
        res.headers = res.qualities.front().headers;
    } else {
        res.error_msg = "未能从夸克网盘获取播放直链，请检查夸克Cookie或VIP转码权限";
    }
    return res;
}

// =============================================================================
// 2. 百度网盘扫码与解析实现 (passport.baidu.com/v2/api/getqrcode -> unicast -> qrbdusslogin)
// =============================================================================

QRLoginSession NetdiskAuth::startBaiduQRLogin() {
    QRLoginSession s;
    s.provider = NetdiskProvider::BAIDU;
    s.request_id = HttpClient::generateUuidV4();
    s.created_ms = HttpClient::currentTimeMillis();

    std::string api = "https://passport.baidu.com/v2/api/getqrcode?lp=pc&qrloginfrom=pc&tpl=netdisk&apiver=v3&gid=" +
                      s.request_id + "&tt=" + std::to_string(s.created_ms) + "&callback=bd__cbs__qr";
    HttpResponse resp = HttpClient::get(api, {{"User-Agent", QUARK_WEB_UA}, {"Referer", "https://pan.baidu.com/"}});
    std::string sign = regexExtract(resp.body, "\"sign\"\\s*:\\s*\"([^\"]+)\"");
    std::string imgurl = regexExtract(resp.body, "\"imgurl\"\\s*:\\s*\"([^\"]+)\"");
    if (!sign.empty()) {
        s.qr_token = sign;
        // 处理 imgurl 中的反斜杠转义并补全 https://
        std::string clean_img;
        for (size_t i = 0; i < imgurl.size(); ++i) {
            if (imgurl[i] == '\\' && i + 1 < imgurl.size() && imgurl[i + 1] == '/') {
                continue;
            }
            clean_img.push_back(imgurl[i]);
        }
        if (!clean_img.empty() && clean_img.rfind("http", 0) != 0) {
            clean_img = "https://" + clean_img;
        }
        if (!clean_img.empty()) {
            HttpResponse img_resp = HttpClient::get(clean_img, {{"User-Agent", QUARK_WEB_UA}, {"Referer", "https://pan.baidu.com/"}});
            if (img_resp.ok() && img_resp.body.size() > 64) {
                s.qr_png_bytes = std::move(img_resp.body);
            }
        }
        s.qr_url = "https://wappass.baidu.com/wp/?qrlogin=1&tpl=netdisk&sign=" + sign;
        s.state = QRLoginState::WAITING_SCAN;
        s.status_text = "请打开手机【百度网盘APP】扫一扫登录百度网盘";
        generateQrMatrix(s.qr_url, s.qr_size, s.qr_modules);
    } else {
        s.state = QRLoginState::EXPIRED_OR_ERROR;
        s.qr_size = 0;
        s.qr_modules.clear();
        s.status_text = "获取百度二维码失败(" + (resp.error.empty() ? "网络异常" : resp.error) + ")，请按[A]或点刷新重试";
    }
    return s;
}

bool NetdiskAuth::pollBaiduQRLogin(QRLoginSession& session, AppConfig& cfg) {
    if (session.state != QRLoginState::WAITING_SCAN &&
        session.state != QRLoginState::SCANNED_WAIT_CONFIRM) {
        return false;
    }
    std::string poll_url = "https://passport.baidu.com/channel/unicast?channel_id=" +
                           session.qr_token + "&tpl=netdisk&apiver=v3&callback=bd__cbs__poll&tt=" +
                           std::to_string(HttpClient::currentTimeMillis());
    HttpResponse resp = HttpClient::get(poll_url, {{"User-Agent", QUARK_WEB_UA}, {"Referer", "https://pan.baidu.com/"}}, 8000);
    if (!resp.ok() || resp.body.empty()) return false;

    std::string unescaped;
    for (size_t i = 0; i < resp.body.size(); ++i) {
        if (resp.body[i] == '\\' && i + 1 < resp.body.size() && resp.body[i + 1] == '"') {
            unescaped.push_back('"');
            ++i;
        } else {
            unescaped.push_back(resp.body[i]);
        }
    }

    std::string status = regexExtract(unescaped, "\"status\"\\s*:\\s*(\\d+)");
    std::string temp_v = regexExtract(unescaped, "\"v\"\\s*:\\s*\"([^\"]+)\"");

    if (status == "1") {
        session.state = QRLoginState::SCANNED_WAIT_CONFIRM;
        session.status_text = "已扫码，请在手机百度网盘APP上点击【确认登录】";
        return false;
    }
    if (status == "0" && !temp_v.empty()) {
        std::string now_s = std::to_string(HttpClient::currentTimeMillis());
        std::string login_url = "https://passport.baidu.com/v3/login/main/qrbdusslogin?tpl=netdisk&apiver=v3"
                                "&qrcode=1&loginVersion=v4&alg=v3&bduss=" + temp_v +
                                "&gid=" + session.request_id + "&time=" + now_s +
                                "&traceid=&callback=bd__cbs__login&u=https%253A%252F%252Fpan.baidu.com%252Fdisk%252Fmain";
        HttpResponse lr = HttpClient::get(login_url, {{"User-Agent", QUARK_WEB_UA}, {"Referer", "https://pan.baidu.com/"}});

        std::string bduss = regexExtract(lr.body, "\"bduss\"\\s*:\\s*\"([^\"]+)\"");
        std::string netdisk_stoken = regexExtract(lr.body, "netdisk#([a-zA-Z0-9_\\-]+)");
        std::string display_name = regexExtract(lr.body, "\"displayName\"\\s*:\\s*\"([^\"]+)\"");

        std::string merged_cookie = HttpClient::mergeSetCookies(cfg.baidu_cookie, lr.set_cookies);
        if (!bduss.empty() && merged_cookie.find("BDUSS=") == std::string::npos) {
            merged_cookie += (merged_cookie.empty() ? "" : "; ") + std::string("BDUSS=") + bduss;
        }
        if (!netdisk_stoken.empty() && merged_cookie.find("STOKEN=") == std::string::npos) {
            merged_cookie += (merged_cookie.empty() ? "" : "; ") + std::string("STOKEN=") + netdisk_stoken;
        }

        // 访问盘主页与 gettemplatevariable 进一步激活网盘 Web 会话 Cookie
        HttpResponse disk_resp = HttpClient::get(
            "https://pan.baidu.com/disk/main",
            {{"User-Agent", QUARK_WEB_UA}, {"Referer", "https://passport.baidu.com/"}, {"Cookie", merged_cookie}}
        );
        merged_cookie = HttpClient::mergeSetCookies(merged_cookie, disk_resp.set_cookies);

        cfg.baidu_bduss = bduss;
        cfg.baidu_stoken = netdisk_stoken;
        cfg.baidu_cookie = merged_cookie;
        cfg.baidu_username = display_name.empty() ? "百度网盘用户" : display_name;
        verifyBaiduCookie(cfg);
        SourceLoader::saveAppConfig(cfg);

        session.state = QRLoginState::CONFIRMED_SUCCESS;
        session.status_text = "百度网盘登录成功！账号：" + cfg.baidu_username;
        return true;
    }
    return false;
}

bool NetdiskAuth::verifyBaiduCookie(AppConfig& cfg) {
    if (cfg.baidu_cookie.empty()) return false;
    HttpResponse resp = HttpClient::get(
        "https://pan.baidu.com/rest/2.0/xpan/nas?method=uinfo",
        {
            {"User-Agent", BAIDU_PAN_UA},
            {"Referer", "https://pan.baidu.com/disk/main"},
            {"Cookie", cfg.baidu_cookie}
        }
    );
    std::string uname = regexExtract(resp.body, "\"baidu_name\"\\s*:\\s*\"([^\"]+)\"");
    if (uname.empty()) uname = regexExtract(resp.body, "\"netdisk_name\"\\s*:\\s*\"([^\"]+)\"");
    if (!uname.empty()) {
        cfg.baidu_username = uname;
        SourceLoader::saveAppConfig(cfg);
        return true;
    }
    return false;
}

std::vector<NetdiskFileItem> NetdiskAuth::listBaiduPersonalFiles(
    const AppConfig& cfg,
    const std::string& dir_path
) {
    std::vector<NetdiskFileItem> items;
    if (cfg.baidu_cookie.empty()) return items;

    std::string url = "https://pan.baidu.com/api/list?order=time&desc=1&showempty=0&web=1&page=1&num=100&dir=" +
                      HttpClient::urlEncode(dir_path);
    HttpResponse resp = HttpClient::get(
        url,
        {
            {"User-Agent", BAIDU_PAN_UA},
            {"Referer", "https://pan.baidu.com/disk/main"},
            {"Cookie", cfg.baidu_cookie}
        }
    );
    if (!resp.ok()) return items;

    std::regex item_re("\\{[^{}]*?\"fs_id\"\\s*:\\s*(\\d+)[^{}]*?\"server_filename\"\\s*:\\s*\"([^\"]+)\"[^{}]*?\\}");
    for (auto it = std::sregex_iterator(resp.body.begin(), resp.body.end(), item_re);
         it != std::sregex_iterator(); ++it) {
        std::string block = (*it)[0].str();
        NetdiskFileItem fi;
        fi.fid = (*it)[1].str();
        fi.file_name = (*it)[2].str();
        fi.pdir_fid = regexExtract(block, "\"path\"\\s*:\\s*\"([^\"]+)\"");
        fi.is_dir = (block.find("\"isdir\":1") != std::string::npos);
        fi.is_video = !fi.is_dir && isVideoFilename(fi.file_name);
        std::string sz = regexExtract(block, "\"size\"\\s*:\\s*(\\d+)");
        if (!sz.empty()) fi.size_bytes = std::stoll(sz);
        items.push_back(fi);
    }
    return items;
}

std::vector<NetdiskFileItem> NetdiskAuth::listBaiduShareFiles(
    const AppConfig& cfg,
    const std::string& share_url,
    std::string& out_surl,
    std::string& out_uk,
    std::string& out_shareid
) {
    std::vector<NetdiskFileItem> items;
    out_surl = regexExtract(share_url, "/s/1([a-zA-Z0-9_\\-]+)");
    if (out_surl.empty()) out_surl = regexExtract(share_url, "surl=([a-zA-Z0-9_\\-]+)");
    std::string pwd = regexExtract(share_url, "pwd=([a-zA-Z0-9]+)");
    if (out_surl.empty()) return items;

    std::string cookie = cfg.baidu_cookie;
    std::map<std::string, std::string> hdrs = {
        {"User-Agent", QUARK_WEB_UA},
        {"Referer", "https://pan.baidu.com/"},
        {"Cookie", cookie}
    };

    if (!pwd.empty()) {
        std::string verify_url = "https://pan.baidu.com/share/verify?surl=" + out_surl +
                                 "&t=" + std::to_string(HttpClient::currentTimeMillis()) +
                                 "&channel=chunlei&web=1&app_id=250528&clienttype=0";
        HttpResponse vr = HttpClient::postForm(verify_url, "pwd=" + pwd + "&vcode=&vcode_str=", hdrs);
        cookie = HttpClient::mergeSetCookies(cookie, vr.set_cookies);
        hdrs["Cookie"] = cookie;
    }

    std::string wx_url = "https://pan.baidu.com/share/wxlist?channel=weixin&version=2.2.2&clienttype=25&web=1"
                         "&shorturl=" + out_surl + "&page=1&num=100&order=time&desc=1";
    HttpResponse lr = HttpClient::get(wx_url, hdrs);
    out_uk = regexExtract(lr.body, "\"uk\"\\s*:\\s*(\\d+)");
    out_shareid = regexExtract(lr.body, "\"shareid\"\\s*:\\s*(\\d+)");

    std::regex item_re("\\{[^{}]*?\"fs_id\"\\s*:\\s*\"?(\\d+)\"?[^{}]*?\"server_filename\"\\s*:\\s*\"([^\"]+)\"[^{}]*?\\}");
    for (auto it = std::sregex_iterator(lr.body.begin(), lr.body.end(), item_re);
         it != std::sregex_iterator(); ++it) {
        std::string block = (*it)[0].str();
        NetdiskFileItem fi;
        fi.fid = (*it)[1].str();
        fi.file_name = (*it)[2].str();
        fi.pdir_fid = regexExtract(block, "\"path\"\\s*:\\s*\"([^\"]+)\"");
        fi.is_dir = (block.find("\"isdir\":1") != std::string::npos || block.find("\"isdir\":\"1\"") != std::string::npos);
        fi.is_video = !fi.is_dir && isVideoFilename(fi.file_name);
        items.push_back(fi);
    }
    return items;
}

PlayResult NetdiskAuth::resolveBaiduPlayUrl(
    const AppConfig& cfg,
    const std::string& fs_id,
    const std::string& path_in_drive
) {
    PlayResult res;
    if (cfg.baidu_cookie.empty()) {
        res.error_msg = "请先按 [-] 键或在【百度网盘】页扫码登录百度网盘账号";
        return res;
    }

    std::map<std::string, std::string> hdrs = {
        {"User-Agent", BAIDU_PAN_UA},
        {"Referer", "https://pan.baidu.com/disk/main"},
        {"Cookie", cfg.baidu_cookie}
    };

    std::string target_path = path_in_drive.empty() ? ("/OKPlayer_Temp/" + fs_id + ".mp4") : path_in_drive;
    std::string encoded_path = HttpClient::urlEncode(target_path);

    // 1. 百度网盘官方 M3U8_AUTO_1080 / 720 云端实时流 (无需等待完整下载即可在 Switch MPV/FFmpeg 秒播)
    StreamOption m3u8_1080;
    m3u8_1080.label = "百度网盘 M3U8_1080P 云转码流";
    m3u8_1080.url = "https://pan.baidu.com/api/streaming?path=" + encoded_path +
                    "&app_id=250528&clienttype=0&type=M3U8_AUTO_1080&vip=2";
    m3u8_1080.headers = hdrs;
    res.qualities.push_back(m3u8_1080);

    StreamOption m3u8_720;
    m3u8_720.label = "百度网盘 M3U8_720P 流畅流";
    m3u8_720.url = "https://pan.baidu.com/api/streaming?path=" + encoded_path +
                   "&app_id=250528&clienttype=0&type=M3U8_AUTO_720&vip=2";
    m3u8_720.headers = hdrs;
    res.qualities.push_back(m3u8_720);

    // 2. 百度网盘 PCS 原画直链
    StreamOption pcs_raw;
    pcs_raw.label = "百度网盘 PCS 原画直链";
    pcs_raw.url = "https://d.pcs.baidu.com/rest/2.0/pcs/file?method=download&app_id=250528&path=" + encoded_path;
    pcs_raw.headers = hdrs;
    res.qualities.push_back(pcs_raw);

    res.url = res.qualities.front().url;
    res.headers = res.qualities.front().headers;
    return res;
}

// =============================================================================
// 3. 标准 ISO/IEC 18004 二维码编码器 (支持 Version 1-15, Byte 模式, ECC-M)
// =============================================================================
namespace {

struct QrEncoder {
    int version = 1;
    int size = 21;
    std::vector<uint8_t> modules;
    std::vector<uint8_t> is_func;

    static uint8_t gfMul(uint8_t x, uint8_t y) {
        int z = 0;
        for (int i = 7; i >= 0; --i) {
            z = (z << 1) ^ ((z >> 7) * 0x11D);
            z ^= ((y >> i) & 1) * x;
        }
        return static_cast<uint8_t>(z);
    }

    static std::vector<uint8_t> rsGenPoly(int degree) {
        std::vector<uint8_t> res(degree, 0);
        res[degree - 1] = 1;
        uint8_t root = 1;
        for (int i = 0; i < degree; ++i) {
            for (int j = 0; j < degree; ++j) {
                res[j] = gfMul(res[j], root);
                if (j + 1 < degree) res[j] ^= res[j + 1];
            }
            root = gfMul(root, 0x02);
        }
        return res;
    }

    static std::vector<uint8_t> rsRemainder(const std::vector<uint8_t>& data, const std::vector<uint8_t>& div) {
        std::vector<uint8_t> res(div.size(), 0);
        for (uint8_t b : data) {
            uint8_t factor = b ^ res[0];
            res.erase(res.begin());
            res.push_back(0);
            for (size_t i = 0; i < div.size(); ++i) {
                res[i] ^= gfMul(div[i], factor);
            }
        }
        return res;
    }

    void setFunc(int x, int y, bool dark) {
        if (x >= 0 && x < size && y >= 0 && y < size) {
            modules[y * size + x] = dark ? 1 : 0;
            is_func[y * size + x] = 1;
        }
    }

    void drawFinder(int cx, int cy) {
        for (int dy = -4; dy <= 4; ++dy) {
            for (int dx = -4; dx <= 4; ++dx) {
                int dist = std::max(std::abs(dx), std::abs(dy));
                int xx = cx + dx, yy = cy + dy;
                if (xx >= 0 && xx < size && yy >= 0 && yy < size) {
                    setFunc(xx, yy, dist != 2 && dist != 4);
                }
            }
        }
    }

    void drawAlignment(int cx, int cy) {
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                setFunc(cx + dx, cy + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
            }
        }
    }

    std::vector<int> alignmentPositions() const {
        if (version == 1) return {};
        int num = version / 7 + 2;
        int step = (version == 32) ? 26 : ((version * 4 + num * 2 + 1) / (num * 2 - 2)) * 2;
        std::vector<int> pos{6};
        for (int i = 0, p = size - 7; i < num - 1; ++i, p -= step) {
            pos.insert(pos.begin() + 1, p);
        }
        return pos;
    }

    void drawFunctionPatterns() {
        for (int i = 0; i < size; ++i) {
            setFunc(6, i, i % 2 == 0);
            setFunc(i, 6, i % 2 == 0);
        }
        drawFinder(3, 3);
        drawFinder(size - 4, 3);
        drawFinder(3, size - 4);

        auto align = alignmentPositions();
        for (size_t i = 0; i < align.size(); ++i) {
            for (size_t j = 0; j < align.size(); ++j) {
                if (!((i == 0 && j == 0) || (i == 0 && j == align.size() - 1) || (i == align.size() - 1 && j == 0))) {
                    drawAlignment(align[i], align[j]);
                }
            }
        }
        // Format info placeholders
        for (int i = 0; i <= 5; ++i) setFunc(8, i, false);
        setFunc(8, 7, false);
        setFunc(8, 8, false);
        setFunc(7, 8, false);
        for (int i = 9; i < 15; ++i) setFunc(14 - i, 8, false);
        for (int i = 0; i < 8; ++i) setFunc(size - 1 - i, 8, false);
        for (int i = 8; i < 15; ++i) setFunc(8, size - 15 + i, false);
        setFunc(8, size - 8, true);

        // Version info (v >= 7)
        if (version >= 7) {
            int rem = version;
            for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
            int bits = (version << 12) | rem;
            for (int i = 0; i < 18; ++i) {
                bool bit = ((bits >> i) & 1) != 0;
                int a = size - 11 + i % 3;
                int b = i / 3;
                setFunc(a, b, bit);
                setFunc(b, a, bit);
            }
        }
    }

    void drawFormatBits(int mask) {
        // ECC-M = 0 (bits 00)
        int data = (0 << 3) | mask;
        int rem = data;
        for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
        int bits = ((data << 10) | rem) ^ 0x5412;
        for (int i = 0; i <= 5; ++i) setFunc(8, i, ((bits >> i) & 1) != 0);
        setFunc(8, 7, ((bits >> 6) & 1) != 0);
        setFunc(8, 8, ((bits >> 7) & 1) != 0);
        setFunc(7, 8, ((bits >> 8) & 1) != 0);
        for (int i = 9; i < 15; ++i) setFunc(14 - i, 8, ((bits >> i) & 1) != 0);
        for (int i = 0; i < 8; ++i) setFunc(size - 1 - i, 8, ((bits >> i) & 1) != 0);
        for (int i = 8; i < 15; ++i) setFunc(8, size - 15 + i, ((bits >> i) & 1) != 0);
    }

    void drawCodewords(const std::vector<uint8_t>& cw) {
        size_t i = 0;
        for (int right = size - 1; right >= 1; right -= 2) {
            if (right == 6) right = 5;
            for (int vert = 0; vert < size; ++vert) {
                for (int j = 0; j < 2; ++j) {
                    int x = right - j;
                    bool upward = ((right + 1) & 2) == 0;
                    int y = upward ? (size - 1 - vert) : vert;
                    if (!is_func[y * size + x] && i < cw.size() * 8) {
                        modules[y * size + x] = ((cw[i >> 3] >> (7 - (i & 7))) & 1);
                        ++i;
                    }
                }
            }
        }
    }

    void applyMask(int mask) {
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                if (is_func[y * size + x]) continue;
                bool inv = false;
                switch (mask) {
                    case 0: inv = (x + y) % 2 == 0; break;
                    case 1: inv = y % 2 == 0; break;
                    case 2: inv = x % 3 == 0; break;
                    case 3: inv = (x + y) % 3 == 0; break;
                    case 4: inv = (x / 3 + y / 2) % 2 == 0; break;
                    case 5: inv = x * y % 2 + x * y % 3 == 0; break;
                    case 6: inv = (x * y % 2 + x * y % 3) % 2 == 0; break;
                    case 7: inv = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
                }
                if (inv) modules[y * size + x] ^= 1;
            }
        }
    }

    int getPenalty() const {
        int res = 0;
        for (int y = 0; y < size; ++y) {
            int runX = 0, runY = 0;
            uint8_t colX = 2, colY = 2;
            for (int x = 0; x < size; ++x) {
                uint8_t cx = modules[y * size + x];
                if (cx == colX) { ++runX; if (runX == 5) res += 3; else if (runX > 5) res += 1; }
                else { colX = cx; runX = 1; }
                uint8_t cy = modules[x * size + y];
                if (cy == colY) { ++runY; if (runY == 5) res += 3; else if (runY > 5) res += 1; }
                else { colY = cy; runY = 1; }
            }
        }
        for (int y = 0; y < size - 1; ++y) {
            for (int x = 0; x < size - 1; ++x) {
                uint8_t c = modules[y * size + x];
                if (c == modules[y * size + x + 1] && c == modules[(y + 1) * size + x] && c == modules[(y + 1) * size + x + 1]) {
                    res += 3;
                }
            }
        }
        return res;
    }

    bool encode(const std::string& text) {
        // ECC-M tables for Versions 1..15:
        // {dataCodewords, numBlocks, eccPerBlock}
        static const int ECC_M[16][3] = {
            {0,0,0},
            {16,1,10}, {28,1,16}, {44,1,26}, {64,2,18}, {86,2,24},
            {108,4,16}, {124,4,18}, {154,4,22}, {182,5,22}, {216,5,26},
            {254,5,30}, {290,8,22}, {334,9,22}, {365,9,24}, {415,10,24}
        };
        static const int TOTAL_CODEWORDS[16] = {
            0, 26, 44, 70, 100, 134, 172, 196, 242, 292, 346, 404, 466, 532, 581, 655
        };

        int len = static_cast<int>(text.size());
        version = 0;
        for (int v = 1; v <= 15; ++v) {
            int cc_bits = (v <= 9) ? 8 : 16;
            int needed_bits = 4 + cc_bits + len * 8;
            if (needed_bits <= ECC_M[v][0] * 8) {
                version = v;
                break;
            }
        }
        if (version == 0) return false;

        size = version * 4 + 17;
        modules.assign(size * size, 0);
        is_func.assign(size * size, 0);

        // Bitstream
        std::vector<uint8_t> bits;
        auto pushBits = [&](uint32_t val, int n, int& bit_pos) {
            for (int i = n - 1; i >= 0; --i) {
                if ((bit_pos & 7) == 0) bits.push_back(0);
                if ((val >> i) & 1) bits.back() |= (1 << (7 - (bit_pos & 7)));
                ++bit_pos;
            }
        };
        int bp = 0;
        pushBits(0x4, 4, bp); // Byte mode
        pushBits(static_cast<uint32_t>(len), (version <= 9) ? 8 : 16, bp);
        for (unsigned char c : text) pushBits(c, 8, bp);

        int data_cap = ECC_M[version][0];
        int term = std::min(4, data_cap * 8 - bp);
        pushBits(0, term, bp);
        if ((bp & 7) != 0) pushBits(0, 8 - (bp & 7), bp);
        for (int pad = 0; static_cast<int>(bits.size()) < data_cap; ++pad) {
            bits.push_back((pad & 1) ? 0x11 : 0xEC);
        }

        // Split into blocks & append RS ECC
        int num_blocks = ECC_M[version][1];
        int ecc_len = ECC_M[version][2];
        int total_cw = TOTAL_CODEWORDS[version];
        int raw_ecc_blocks_short = num_blocks - (total_cw % num_blocks);
        int short_block_len = total_cw / num_blocks;
        auto poly = rsGenPoly(ecc_len);

        std::vector<std::vector<uint8_t>> blocks(num_blocks);
        std::vector<std::vector<uint8_t>> eccs(num_blocks);
        size_t pos = 0;
        for (int b = 0; b < num_blocks; ++b) {
            int dat_len = short_block_len - ecc_len + (b < raw_ecc_blocks_short ? 0 : 1);
            blocks[b].assign(bits.begin() + pos, bits.begin() + pos + dat_len);
            eccs[b] = rsRemainder(blocks[b], poly);
            pos += dat_len;
        }

        std::vector<uint8_t> final_cw;
        int max_dat = short_block_len - ecc_len + ((raw_ecc_blocks_short < num_blocks) ? 1 : 0);
        for (int i = 0; i < max_dat; ++i) {
            for (int b = 0; b < num_blocks; ++b) {
                if (i < static_cast<int>(blocks[b].size())) final_cw.push_back(blocks[b][i]);
            }
        }
        for (int i = 0; i < ecc_len; ++i) {
            for (int b = 0; b < num_blocks; ++b) {
                final_cw.push_back(eccs[b][i]);
            }
        }

        drawFunctionPatterns();
        drawCodewords(final_cw);

        int best_mask = 0;
        int best_pen = 1e9;
        auto base_modules = modules;
        for (int m = 0; m < 8; ++m) {
            modules = base_modules;
            applyMask(m);
            drawFormatBits(m);
            int p = getPenalty();
            if (p < best_pen) {
                best_pen = p;
                best_mask = m;
            }
        }
        modules = base_modules;
        applyMask(best_mask);
        drawFormatBits(best_mask);
        return true;
    }
};

} // namespace

void NetdiskAuth::generateQrMatrix(const std::string& text, int& out_size, std::vector<uint8_t>& out_modules) {
    QrEncoder enc;
    if (enc.encode(text)) {
        out_size = enc.size;
        out_modules = std::move(enc.modules);
    } else {
        out_size = 0;
        out_modules.clear();
    }
}

} // namespace okplayer
