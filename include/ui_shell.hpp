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
#include <unordered_map>
#include <mutex>
#include <atomic>

namespace okplayer {

enum class TopTab {
    HOME_VOD = 0,     // 发现·首页 (平泽唯 Yui)
    CATEGORIES = 1,   // 影视·分类 (秋山澪 Mio)
    SEARCH = 2,       // 聚合·搜索 (田井中律 Ritsu)
    QUARK_DRIVE = 3,  // 夸克·网盘 (琴吹紬 Mugi)
    BAIDU_DRIVE = 4,  // 百度·网盘 (中野梓 Azusa)
    SETTINGS = 5      // 配置·中心 (HTT 放课后茶会)
};

enum class ModalState {
    NONE = 0,
    VOD_DETAIL = 1,       // 剧集详情与选集弹窗 (夸克4K / 百度网盘 / 直连线路切换)
    QR_LOGIN = 2,         // 夸克/百度网盘 屏幕二维码扫码弹窗
    VIDEO_PLAYING = 3     // 全屏视频播放界面 (支持触屏 + 摇杆快进/音量/倍速/清晰度切换)
};

struct CachedTextTex {
    SDL_Texture* tex = nullptr;
    int w = 0;
    int h = 0;
};

struct HitZone {
    int id = 0;
    SDL_Rect rect{0, 0, 0, 0};
};

class SwitchPlayerShell {
public:
    bool init();
    void runMainLoop();
    void shutdown();

    static std::string openSwitchSoftwareKeyboard(
        const std::string& header_text,
        const std::string& sub_text,
        const std::string& initial_text
    );

private:
    void handleGamepadInput();
    void handleTouchOrClick(int tx, int ty);
    void updateFrame(uint64_t now_ms);
    void renderFrame();

    // 渲染子模块
    void renderTopBar();
    void renderSiteAndCategorySidebar();
    void renderVodGrid();
    void renderSettingsPanel();
    void renderDetailModal();
    void renderQrLoginModal();
    void renderFullscreenPlayer();
    void renderBottomGamepadLegend();

    void drawRect(int x, int y, int w, int h, SDL_Color color, bool filled = true);
    void drawRoundedRect(int x, int y, int w, int h, int radius, SDL_Color color);
    void drawRoundedOutline(int x, int y, int w, int h, int radius, int thickness, SDL_Color color);
    void drawText(const std::string& text, int x, int y, SDL_Color color, bool large = false);
    void drawTextCentered(const std::string& text, int cx, int y, SDL_Color color, bool large = false);
    void drawTextClipped(const std::string& text, int x, int y, int max_px_width, SDL_Color color, bool large = false);
    void clearTextCache();

    // 业务动作
    void reloadSourceUrlAsync(const std::string& new_url);
    void refreshCurrentSiteContentAsync();
    void openVodDetailAsync(const VodItem& v);
    void promptEditSourceUrl();
    void promptSearchKeyword();
    void openQrLoginModal(NetdiskProvider provider);
    void startPlayback(const VodItem& vod, const std::string& flag_name, int ep_index);
    void switchToTab(TopTab tab);

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    TTF_Font* font_regular_ = nullptr;
    TTF_Font* font_large_ = nullptr;
    std::unordered_map<std::string, CachedTextTex> text_cache_;

    // 轻音少女 (K-ON! HTT) 主题纹理缓存
    SDL_Texture* kon_bg_tex_ = nullptr;
    SDL_Texture* kon_banner_tex_ = nullptr;
    SDL_Texture* kon_tonchan_tex_ = nullptr;
    SDL_Texture* kon_htt_heart_tex_ = nullptr;
    SDL_Texture* kon_tea_pink_tex_ = nullptr;
    SDL_Texture* kon_card_tex_[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    SDL_Texture* kon_char_idle_[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    SDL_Texture* kon_char_active_[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    SDL_Texture* active_qr_png_tex_ = nullptr;
    std::string active_qr_png_token_;

    bool running_ = true;
    AppConfig config_;
    LoadedSourceBundle bundle_;
    JsSpiderEngine js_engine_;
    LanWebServer lan_server_;

    // 异步加载与线程安全锁
    std::mutex data_mutex_;
    std::atomic<bool> source_loading_{false};
    std::atomic<bool> content_loading_{false};
    std::atomic<bool> qr_polling_{false};
    std::atomic<bool> qr_loading_{false};
    bool pending_source_apply_ = false;
    LoadedSourceBundle pending_bundle_;
    bool pending_content_apply_ = false;
    std::vector<CategoryItem> pending_categories_;
    std::vector<VodItem> pending_vods_;
    bool pending_detail_apply_ = false;
    VodItem pending_detail_vod_;

    TopTab current_tab_ = TopTab::HOME_VOD;
    ModalState modal_state_ = ModalState::NONE;

    int selected_site_idx_ = 0;
    int selected_cat_idx_ = 0;
    int selected_vod_idx_ = 0;
    int selected_flag_idx_ = 0;
    int selected_ep_idx_ = 0;
    int selected_quality_idx_ = 0;
    int settings_focus_idx_ = 0;
    bool touch_down_prev_ = false;

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
