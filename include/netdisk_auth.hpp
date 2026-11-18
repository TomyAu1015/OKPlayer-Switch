#pragma once
#include "types.hpp"
#include <string>
#include <vector>

namespace okplayer {

class NetdiskAuth {
public:
    // =========================================================================
    // 1. 夸克网盘 (pan.quark.cn) 扫码登录、Cookie 校验、个人网盘与分享链接解析
    // =========================================================================
    static QRLoginSession startQuarkQRLogin();
    static bool pollQuarkQRLogin(QRLoginSession& session, AppConfig& cfg);
    static bool verifyQuarkCookie(AppConfig& cfg);
    static std::vector<NetdiskFileItem> listQuarkPersonalFiles(
        const AppConfig& cfg,
        const std::string& pdir_fid = "0"
    );
    static std::vector<NetdiskFileItem> listQuarkShareFiles(
        const AppConfig& cfg,
        const std::string& share_url,
        std::string& out_pwd_id,
        std::string& out_stoken,
        const std::string& pdir_fid = "0"
    );
    static PlayResult resolveQuarkPlayUrl(
        const AppConfig& cfg,
        const std::string& pwd_id,
        const std::string& stoken,
        const std::string& fid,
        const std::string& share_fid_token
    );

    // =========================================================================
    // 2. 百度网盘 (pan.baidu.com) 扫码登录、Cookie 校验、个人网盘与分享链接解析
    // =========================================================================
    static QRLoginSession startBaiduQRLogin();
    static bool pollBaiduQRLogin(QRLoginSession& session, AppConfig& cfg);
    static bool verifyBaiduCookie(AppConfig& cfg);
    static std::vector<NetdiskFileItem> listBaiduPersonalFiles(
        const AppConfig& cfg,
        const std::string& dir_path = "/"
    );
    static std::vector<NetdiskFileItem> listBaiduShareFiles(
        const AppConfig& cfg,
        const std::string& share_url,
        std::string& out_surl,
        std::string& out_uk,
        std::string& out_shareid
    );
    static PlayResult resolveBaiduPlayUrl(
        const AppConfig& cfg,
        const std::string& fs_id,
        const std::string& path_in_drive
    );

    // =========================================================================
    // 3. 纯 C++ 离线二维码矩阵生成器 (用于 Switch 屏幕直接渲染夸克/百度扫码图)
    // =========================================================================
    static void generateQrMatrix(const std::string& text, int& out_size, std::vector<uint8_t>& out_modules);
};

} // namespace okplayer
