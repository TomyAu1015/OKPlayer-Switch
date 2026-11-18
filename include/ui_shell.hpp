#pragma once
#include "types.hpp"
#include "source_loader.hpp"
#include "js_engine.hpp"
#include "netdisk_auth.hpp"
#include "lan_server.hpp"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string>
#include <vector>

namespace okplayer {

enum class TopTab {
    HOME_VOD = 0,     // 首页点播 (王二小放牛娃 index.js.md5 站点)
    CATEGORIES = 1,   // 分类筛选
    SEARCH = 2,       // 聚合搜索 / 分享链接直解
    QUARK_DRIVE = 3,  // 夸克网盘 (扫码登录 + 个人云盘直连)
    BAIDU_DRIVE = 4,  // 百度网盘 (扫码登录 + 个人云盘直连)
    SETTINGS = 5      // 接口源地址直接填写 + 局域网远程服务
};

enum class ModalState {
    NONE = 0,
    VOD_DETAIL = 1,       // 剧集详情与选集弹窗 (夸克4K / 百度网盘 / 直连线路切换)
    QR_LOGIN = 2,         // 夸克/百度网盘 屏幕二维码扫码弹窗
    VIDEO_PLAYING = 3     // 全屏视频播放界面 (支持摇杆快进/音量/倍速/清晰度切换)
};

class SwitchPlayerShell {
public:
    bool init();
    void runMainLoop();
    void shutdown();

    // 调用 Switch 系统原生软键盘 (swkbd) 直接输入源地址或搜索词
    static std::string openSwitchSoftwareKeyboard(
        const std::string& header_text,
        const std::string& sub_text,
        const std::string& initial_text
    );

private:
    void handleGamepadInput();
    void updateFrame(uint64_t now_ms);
    void renderFrame();

    // 渲染子模块
    void renderTopBar();
    void renderSiteAndCategorySidebar();
    void renderVodGrid();
    void renderNetdiskPanel(NetdiskProvider provider);
    void renderSettingsPanel();
    void renderDetailModal();
    void renderQrLoginModal();
    void renderFullscreenPlayer();
    void renderBottomGamepadLegend();

    void drawRect(int x, int y, int w, int h, SDL_Color color, bool filled = true);
    void drawText(const std::string& text, int x, int y, SDL_Color color, bool large = false);

    // 业务动作
    void reloadSourceUrl(const std::string& new_url);
    void refreshCurrentSiteContent();
    void promptEditSourceUrl();
    void promptSearchKeyword();
    void openQrLoginModal(NetdiskProvider provider);
    void startPlayback(const VodItem& vod, const std::string& flag_name, int ep_index);

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    TTF_Font* font_regular_ = nullptr;
    TTF_Font* font_large_ = nullptr;

    bool running_ = true;
    AppConfig config_;
    LoadedSourceBundle bundle_;
    JsSpiderEngine js_engine_;
    LanWebServer lan_server_;

    TopTab current_tab_ = TopTab::HOME_VOD;
    ModalState modal_state_ = ModalState::NONE;

    int selected_site_idx_ = 0;
    int selected_cat_idx_ = 0;
    int selected_vod_idx_ = 0;
    int selected_flag_idx_ = 0;
    int selected_ep_idx_ = 0;
    int selected_quality_idx_ = 0;
    int settings_focus_idx_ = 0;

    std::vector<CategoryItem> current_categories_;
    std::vector<VodItem> current_vods_;
    VodItem active_vod_;
    PlayResult active_play_;
    QRLoginSession active_qr_;

    // 播放器状态
    bool player_paused_ = false;
    bool player_osd_visible_ = true;
    double player_pos_sec_ = 0.0;
    double player_dur_sec_ = 1440.0;
    int player_volume_ = 85;
    float player_speed_ = 1.0f;
    uint64_t last_stick_move_ms_ = 0;
    uint64_t last_qr_poll_ms_ = 0;
    std::string toast_msg_;
    uint64_t toast_expire_ms_ = 0;
};

} // namespace okplayer
