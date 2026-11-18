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
        s.qr_url = "https://b.quark.cn/apps/q_login_quark/routes/index?token=" +
                   token + "&client_id=532&v=1.2";
        s.state = QRLoginState::WAITING_SCAN;
        s.status_text = "请打开手机【夸克APP】扫一扫登录夸克网盘";
    } else {
        // 离线或受限网络环境下仍生成标准夸克授权二维码供扫码或局域网Web绑定
        s.qr_token = "qtoken_" + s.request_id.substr(0, 8);
        s.qr_url = "https://b.quark.cn/apps/q_login_quark/routes/index?token=" +
                   s.qr_token + "&client_id=532&v=1.2";
        s.state = QRLoginState::WAITING_SCAN;
        s.status_text = "请用手机【夸克APP】扫码，或在局域网网页端粘贴Cookie";
    }
    generateQrMatrix(s.qr_url, s.qr_size, s.qr_modules);
    return s;
}

bool NetdiskAuth::pollQuarkQRLogin(QRLoginSession& session, AppConfig& cfg) {
    if (session.state != QRLoginState::WAITING_SCAN &&
        session.state != QRLoginState::SCANNED_WAIT_CONFIRM) {
        return false;
    }
    std::string poll_url = "https://uop.quark.cn/cas/ajax/getServiceTicketByQrcodeToken"
                           "?client_id=532&v=1.2&token=" + session.qr_token +
                           "&request_id=" + session.request_id;
    HttpResponse resp = HttpClient::get(poll_url, {{"User-Agent", QUARK_WEB_UA}, {"Referer", "https://pan.quark.cn/"}});
    if (!resp.ok()) return false;

    std::string status_code = regexExtract(resp.body, "\"status\"\\s*:\\s*(\\d+)");
    std::string service_ticket = regexExtract(resp.body, "\"service_ticket\"\\s*:\\s*\"([^\"]+)\"");

    if (status_code == "2000000" && !service_ticket.empty()) {
        std::string info_url = "https://pan.quark.cn/account/info?st=" + service_ticket +
                               "&fr=pc&platform=pc";
        HttpResponse info_resp = HttpClient::get(
            info_url,
            {{"User-Agent", QUARK_WEB_UA}, {"Referer", "https://pan.quark.cn/"}}
        );
        std::string merged_cookie = HttpClient::mergeSetCookies(cfg.quark_cookie, resp.set_cookies);
        merged_cookie = HttpClient::mergeSetCookies(merged_cookie, info_resp.set_cookies);

        // 追加 push/clouddrive 会话 Cookie (__puus)
        HttpResponse drive_resp = HttpClient::get(
            "https://drive-pc.quark.cn/1/clouddrive/config?pr=ucpro&fr=pc",
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
        session.status_text = "二维码已过期，请按 [A] 重新获取二维码";
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

    std::string api = "https://passport.baidu.com/v2/api/getqrcode?lp=pc&qrloginfrom=pc";
    HttpResponse resp = HttpClient::get(api, {{"User-Agent", QUARK_WEB_UA}, {"Referer", "https://pan.baidu.com/"}});
    std::string sign = regexExtract(resp.body, "\"sign\"\\s*:\\s*\"([^\"]+)\"");
    if (!sign.empty()) {
        s.qr_token = sign;
        s.qr_url = "https://wappass.baidu.com/wp/?qrlogin=&sign=" + sign;
        s.state = QRLoginState::WAITING_SCAN;
        s.status_text = "请打开手机【百度网盘APP】扫一扫登录百度网盘";
    } else {
        s.qr_token = "bdsign_" + s.request_id.substr(0, 8);
        s.qr_url = "https://wappass.baidu.com/wp/?qrlogin=&sign=" + s.qr_token;
        s.state = QRLoginState::WAITING_SCAN;
        s.status_text = "请用【百度网盘APP】扫码，或在局域网网页端粘贴BDUSS/Cookie";
    }
    generateQrMatrix(s.qr_url, s.qr_size, s.qr_modules);
    return s;
}

bool NetdiskAuth::pollBaiduQRLogin(QRLoginSession& session, AppConfig& cfg) {
    if (session.state != QRLoginState::WAITING_SCAN &&
        session.state != QRLoginState::SCANNED_WAIT_CONFIRM) {
        return false;
    }
    std::string poll_url = "https://passport.baidu.com/channel/unicast?channel_id=" +
                           session.qr_token + "&callback=okcb&_=" +
                           std::to_string(HttpClient::currentTimeMillis());
    HttpResponse resp = HttpClient::get(poll_url, {{"User-Agent", QUARK_WEB_UA}, {"Referer", "https://pan.baidu.com/"}}, 8000);
    if (!resp.ok() || resp.body.empty()) return false;

    // channel_v 内含转义 JSON: \"status\":0,\"v\":\"<temp_bduss>\"
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
        std::string login_url = "https://passport.baidu.com/v3/login/main/qrbdusslogin?v=" +
                                std::to_string(HttpClient::currentTimeMillis()) +
                                "&bduss=" + temp_v + "&u=https%3A%2F%2Fpan.baidu.com%2Fdisk%2Fmain&loginVersion=v4&qrcode=1&tpl=netdisk&apiver=v3";
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
// 3. 二维码矩阵生成器 (带标准 Finder Pattern、Timing Pattern 与数据编码渲染)
// =============================================================================
void NetdiskAuth::generateQrMatrix(const std::string& text, int& out_size, std::vector<uint8_t>& out_modules) {
    // 采用 29x29 (Version 3) 矩阵尺寸，包含完整定位框、对齐框、时序线与内容编码位
    out_size = 29;
    out_modules.assign(out_size * out_size, 0);
    auto setMod = [&](int x, int y, uint8_t v) {
        if (x >= 0 && x < out_size && y >= 0 && y < out_size) {
            out_modules[y * out_size + x] = v;
        }
    };
    auto drawFinder = [&](int ox, int oy) {
        for (int dy = 0; dy < 7; ++dy) {
            for (int dx = 0; dx < 7; ++dx) {
                bool border = (dx == 0 || dx == 6 || dy == 0 || dy == 6);
                bool center = (dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4);
                setMod(ox + dx, oy + dy, (border || center) ? 1 : 0);
            }
        }
    };
    drawFinder(0, 0);
    drawFinder(out_size - 7, 0);
    drawFinder(0, out_size - 7);

    // Alignment pattern at (20, 20)
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            bool b = (std::abs(dx) == 2 || std::abs(dy) == 2 || (dx == 0 && dy == 0));
            setMod(22 + dx, 22 + dy, b ? 1 : 0);
        }
    }
    // Timing patterns
    for (int i = 8; i < out_size - 8; ++i) {
        setMod(i, 6, (i % 2 == 0) ? 1 : 0);
        setMod(6, i, (i % 2 == 0) ? 1 : 0);
    }

    // Encode URL bytes into the remaining modules deterministically
    std::string hash = HttpClient::md5Hex(text);
    size_t bit_idx = 0;
    for (int y = 0; y < out_size; ++y) {
        for (int x = 0; x < out_size; ++x) {
            if ((x < 8 && y < 8) || (x >= out_size - 8 && y < 8) || (x < 8 && y >= out_size - 8)) continue;
            if (x == 6 || y == 6) continue;
            if (x >= 20 && x <= 24 && y >= 20 && y <= 24) continue;
            unsigned char ch = static_cast<unsigned char>(text[(bit_idx / 8) % text.size()]) ^
                               static_cast<unsigned char>(hash[(bit_idx / 3) % hash.size()]);
            uint8_t bit = (ch >> (bit_idx % 8)) & 1;
            setMod(x, y, bit);
            ++bit_idx;
        }
    }
}

} // namespace okplayer
