#include "ui_shell.hpp"
#include "http_client.hpp"
#include <SDL2/SDL_image.h>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <pthread.h>
#include <functional>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace okplayer {

namespace {

// UTF-8 字符步进工具：绝不从中文字符中间截断
std::string utf8PopLastChar(const std::string& s) {
    if (s.empty()) return "";
    size_t i = s.size() - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) {
        --i;
    }
    return s.substr(0, i);
}

std::string utf8Truncate(const std::string& s, size_t max_chars) {
    size_t i = 0;
    size_t chars = 0;
    while (i < s.size() && chars < max_chars) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        if ((c & 0x80) == 0) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > s.size()) break;
        i += len;
        ++chars;
    }
    if (i < s.size()) {
        return s.substr(0, i) + "..";
    }
    return s;
}

SDL_Color cardAccentColor(const std::string& title) {
    static const SDL_Color PALETTE[] = {
        {34, 72, 128, 255},
        {45, 92, 118, 255},
        {78, 54, 122, 255},
        {36, 98, 86, 255},
        {108, 62, 74, 255},
        {52, 76, 134, 255}
    };
    size_t h = 0;
    for (unsigned char c : title) h = h * 31 + c;
    return PALETTE[h % (sizeof(PALETTE) / sizeof(PALETTE[0]))];
}

void spawnLargeStackThread(std::function<void()> fn) {
    auto* heap_fn = new std::function<void()>(std::move(fn));
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024 * 1024); // 1 MB stack for Switch libcurl + TLS
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    auto entry = [](void* arg) -> void* {
        auto* f = static_cast<std::function<void()>*>(arg);
        try {
            (*f)();
        } catch (...) {}
        delete f;
        return nullptr;
    };
    if (pthread_create(&tid, &attr, entry, heap_fn) != 0) {
        delete heap_fn;
    }
    pthread_attr_destroy(&attr);
}

bool pointInRect(int px, int py, int rx, int ry, int rw, int rh) {
    return px >= rx && px <= (rx + rw) && py >= ry && py <= (ry + rh);
}

} // namespace

std::string SwitchPlayerShell::openSwitchSoftwareKeyboard(
    const std::string& header_text,
    const std::string& sub_text,
    const std::string& initial_text
) {
#ifdef __SWITCH__
    SwkbdConfig kbd;
    char out_buf[1024] = {0};
    Result rc = swkbdCreate(&kbd, 0);
    if (R_SUCCEEDED(rc)) {
        swkbdConfigMakePresetDefault(&kbd);
        swkbdConfigSetHeaderText(&kbd, header_text.c_str());
        swkbdConfigSetSubText(&kbd, sub_text.c_str());
        swkbdConfigSetGuideText(&kbd, "请输入或粘贴源地址 / 搜索词...");
        swkbdConfigSetInitialText(&kbd, initial_text.c_str());
        swkbdConfigSetStringLenMax(&kbd, sizeof(out_buf) - 1);
        rc = swkbdShow(&kbd, out_buf, sizeof(out_buf));
        swkbdClose(&kbd);
        if (R_SUCCEEDED(rc) && std::strlen(out_buf) > 0) {
            return std::string(out_buf);
        }
    }
#else
    (void)header_text;
    (void)sub_text;
#endif
    return initial_text;
}

bool SwitchPlayerShell::init() {
    HttpClient::globalInit();
    SourceLoader::loadAppConfig(config_);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) < 0) {
        return false;
    }
    TTF_Init();
    IMG_Init(IMG_INIT_PNG);

    window_ = SDL_CreateWindow(
        "OK影视-Switch",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720,
        SDL_WINDOW_SHOWN
    );
    if (!window_) return false;

    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) return false;
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

#ifdef __SWITCH__
    hidInitializeTouchScreen();
    PlFontData font_data;
    if (R_SUCCEEDED(plGetSharedFontByType(&font_data, PlSharedFontType_ChineseSimplified))) {
        SDL_RWops* rw1 = SDL_RWFromConstMem(font_data.address, font_data.size);
        SDL_RWops* rw2 = SDL_RWFromConstMem(font_data.address, font_data.size);
        font_regular_ = TTF_OpenFontRW(rw1, 1, 18);
        font_large_   = TTF_OpenFontRW(rw2, 1, 24);
    }
#endif

    // 加载轻音少女 (K-ON! HTT) 主题美化纹理
    auto loadThemeTex = [&](const char* rel_path) -> SDL_Texture* {
        std::string p1 = std::string("romfs:/") + rel_path;
        SDL_Texture* t = IMG_LoadTexture(renderer_, p1.c_str());
        if (!t) {
            std::string p2 = std::string("romfs/") + rel_path;
            t = IMG_LoadTexture(renderer_, p2.c_str());
        }
        if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        return t;
    };
    kon_bg_tex_        = loadThemeTex("theme/kon_bg.png");
    kon_banner_tex_    = loadThemeTex("theme/kon_banner.png");
    kon_tonchan_tex_   = loadThemeTex("theme/kon_tonchan.png");
    kon_htt_heart_tex_ = loadThemeTex("theme/kon_htt_heart.png");
    kon_tea_pink_tex_  = loadThemeTex("theme/kon_tea_pink.png");
    for (int i = 0; i < 5; ++i) {
        std::string cp = "theme/kon_card_" + std::to_string(i) + ".png";
        kon_card_tex_[i] = loadThemeTex(cp.c_str());
    }
    static const char* CHAR_NAMES[5] = {"yui", "mio", "ritsu", "mugi", "azusa"};
    for (int i = 0; i < 5; ++i) {
        std::string idle_p = std::string("theme/kon_char_") + CHAR_NAMES[i] + "_idle.png";
        std::string act_p  = std::string("theme/kon_char_") + CHAR_NAMES[i] + "_active.png";
        kon_char_idle_[i]   = loadThemeTex(idle_p.c_str());
        kon_char_active_[i] = loadThemeTex(act_p.c_str());
    }

    // 先立刻渲染第一帧主界面，彻底消除启动黑屏
    toast_msg_ = "正在同步源站与热门片单...";
    toast_expire_ms_ = HttpClient::currentTimeMillis() + 3000;
    renderFrame();

    // 启动局域网 9978 远程控制与 Cookie 推送服务
    lan_server_.start(config_.lan_port, &config_, [this](const std::string& act, const std::string& k1, const std::string& v1) {
        (void)k1;
        if (act == "reload_source") {
            reloadSourceUrlAsync(v1);
        } else if (act == "update_cookie") {
            std::lock_guard<std::mutex> lk(data_mutex_);
            js_engine_.updateCookies(config_);
            toast_msg_ = "已同步局域网推送的夸克/百度网盘 Cookie";
            toast_expire_ms_ = HttpClient::currentTimeMillis() + 4000;
        } else if (act == "search" && !v1.empty()) {
            current_tab_ = TopTab::SEARCH;
            if (!bundle_.sites.empty()) {
                SiteConfig sc = bundle_.sites[selected_site_idx_];
                spawnLargeStackThread([this, sc, v1]() {
                    auto vods = js_engine_.searchContent(sc, v1);
                    std::lock_guard<std::mutex> lk(data_mutex_);
                    pending_vods_ = std::move(vods);
                    pending_content_apply_ = true;
                });
            }
        }
    });

    reloadSourceUrlAsync(config_.source_url);
    return true;
}

void SwitchPlayerShell::reloadSourceUrlAsync(const std::string& new_url) {
    if (source_loading_.load()) return;
    if (!new_url.empty()) {
        config_.source_url = new_url;
        SourceLoader::saveAppConfig(config_);
    }
    source_loading_.store(true);
    AppConfig cfg_copy = config_;
    spawnLargeStackThread([this, cfg_copy]() mutable {
        LoadedSourceBundle fetched = SourceLoader::fetchAndParseSource(cfg_copy);
        {
            std::lock_guard<std::mutex> lk(data_mutex_);
            pending_bundle_ = std::move(fetched);
            pending_source_apply_ = true;
        }
        source_loading_.store(false);
    });
}

void SwitchPlayerShell::refreshCurrentSiteContentAsync() {
    if (bundle_.sites.empty() || content_loading_.load()) return;
    if (selected_site_idx_ >= static_cast<int>(bundle_.sites.size())) {
        selected_site_idx_ = 0;
    }
    SiteConfig site = bundle_.sites[selected_site_idx_];
    int cat_idx = selected_cat_idx_;
    content_loading_.store(true);

    spawnLargeStackThread([this, site, cat_idx]() {
        auto cats = js_engine_.homeCategories(site);
        std::string tid = cats.empty() ? "1" : cats[cat_idx % cats.size()].type_id;
        auto vods = js_engine_.categoryContent(site, tid, 1);
        {
            std::lock_guard<std::mutex> lk(data_mutex_);
            pending_categories_ = std::move(cats);
            pending_vods_ = std::move(vods);
            pending_content_apply_ = true;
        }
        content_loading_.store(false);
    });
}

void SwitchPlayerShell::openVodDetailAsync(const VodItem& v) {
    if (v.vod_id == "quark_login_prompt") {
        openQrLoginModal(NetdiskProvider::QUARK);
        return;
    }
    if (v.vod_id == "baidu_login_prompt") {
        openQrLoginModal(NetdiskProvider::BAIDU);
        return;
    }
    if (!v.play_flags.empty()) {
        active_vod_ = v;
        selected_flag_idx_ = 0;
        selected_ep_idx_ = 0;
        modal_state_ = ModalState::VOD_DETAIL;
        return;
    }
    if (bundle_.sites.empty()) return;
    SiteConfig sc = bundle_.sites[selected_site_idx_];
    std::string vid = v.vod_id;
    toast_msg_ = "正在获取剧集选集列表...";
    toast_expire_ms_ = HttpClient::currentTimeMillis() + 3000;
    spawnLargeStackThread([this, sc, vid]() {
        VodItem det = js_engine_.detailContent(sc, vid);
        std::lock_guard<std::mutex> lk(data_mutex_);
        pending_detail_vod_ = std::move(det);
        pending_detail_apply_ = true;
    });
}

void SwitchPlayerShell::switchToTab(TopTab tab) {
    current_tab_ = tab;
    if (current_tab_ == TopTab::QUARK_DRIVE) {
        for (size_t i = 0; i < bundle_.sites.size(); ++i) {
            if (bundle_.sites[i].key == "quark_my") {
                selected_site_idx_ = static_cast<int>(i);
                refreshCurrentSiteContentAsync();
                break;
            }
        }
    } else if (current_tab_ == TopTab::BAIDU_DRIVE) {
        for (size_t i = 0; i < bundle_.sites.size(); ++i) {
            if (bundle_.sites[i].key == "baidu_my") {
                selected_site_idx_ = static_cast<int>(i);
                refreshCurrentSiteContentAsync();
                break;
            }
        }
    } else if (current_tab_ == TopTab::HOME_VOD || current_tab_ == TopTab::CATEGORIES) {
        if (!bundle_.sites.empty() &&
            (bundle_.sites[selected_site_idx_].key == "quark_my" ||
             bundle_.sites[selected_site_idx_].key == "baidu_my")) {
            selected_site_idx_ = 0;
            refreshCurrentSiteContentAsync();
        }
    }
}

void SwitchPlayerShell::promptEditSourceUrl() {
    std::string edited = openSwitchSoftwareKeyboard(
        "填写源地址 (支持 .js.md5 猫源 / OK影视源)",
        "默认已填王二小放牛娃猫源，可直接确认或修改",
        config_.source_url
    );
    if (!edited.empty()) {
        toast_msg_ = "正在后台加载新源地址...";
        toast_expire_ms_ = HttpClient::currentTimeMillis() + 3000;
        reloadSourceUrlAsync(edited);
    }
}

void SwitchPlayerShell::promptSearchKeyword() {
    std::string kw = openSwitchSoftwareKeyboard(
        "聚合搜索 / 网盘分享链接直解",
        "输入影视名称，或粘贴 pan.quark.cn/s/... 及 pan.baidu.com/s/...",
        ""
    );
    if (!kw.empty() && !bundle_.sites.empty()) {
        current_tab_ = TopTab::SEARCH;
        SiteConfig sc = bundle_.sites[selected_site_idx_];
        toast_msg_ = "正在全网聚合搜索「" + kw + "」...";
        toast_expire_ms_ = HttpClient::currentTimeMillis() + 4000;
        content_loading_.store(true);
        spawnLargeStackThread([this, sc, kw]() {
            auto vods = js_engine_.searchContent(sc, kw);
            {
                std::lock_guard<std::mutex> lk(data_mutex_);
                pending_vods_ = std::move(vods);
                pending_content_apply_ = true;
                toast_msg_ = "搜索「" + kw + "」共找到 " + std::to_string(pending_vods_.size()) + " 条结果";
                toast_expire_ms_ = HttpClient::currentTimeMillis() + 4000;
            }
            content_loading_.store(false);
        });
    }
}

void SwitchPlayerShell::openQrLoginModal(NetdiskProvider provider) {
    if (qr_loading_.load()) return;
    {
        std::lock_guard<std::mutex> lk(data_mutex_);
        active_qr_ = QRLoginSession{};
        active_qr_.provider = provider;
        active_qr_.state = QRLoginState::IDLE;
        active_qr_.status_text = (provider == NetdiskProvider::QUARK)
            ? "正在向夸克服务器申请登录二维码..."
            : "正在向百度服务器申请官方登录二维码...";
    }
    last_qr_poll_ms_ = HttpClient::currentTimeMillis();
    modal_state_ = ModalState::QR_LOGIN;
    qr_loading_.store(true);

    spawnLargeStackThread([this, provider]() {
        QRLoginSession s = (provider == NetdiskProvider::QUARK)
            ? NetdiskAuth::startQuarkQRLogin()
            : NetdiskAuth::startBaiduQRLogin();
        {
            std::lock_guard<std::mutex> lk(data_mutex_);
            if (modal_state_ == ModalState::QR_LOGIN && active_qr_.provider == provider) {
                active_qr_ = std::move(s);
            }
        }
        qr_loading_.store(false);
    });
}

void SwitchPlayerShell::startPlayback(const VodItem& vod, const std::string& flag_name, int ep_index) {
    auto it = vod.play_flags.find(flag_name);
    if (it == vod.play_flags.end() || it->second.empty()) return;
    if (ep_index < 0 || ep_index >= static_cast<int>(it->second.size())) ep_index = 0;

    const auto& ep = it->second[ep_index];
    SiteConfig sc;
    sc.key = vod.site_key;
    sc.name = vod.site_name;

    active_play_ = js_engine_.playerContent(sc, flag_name, ep.second);
    if (!active_play_.error_msg.empty() && active_play_.url.empty()) {
        toast_msg_ = active_play_.error_msg;
        toast_expire_ms_ = HttpClient::currentTimeMillis() + 5000;
        if (ep.second.find("quark") != std::string::npos && config_.quark_cookie.empty()) {
            openQrLoginModal(NetdiskProvider::QUARK);
        } else if (ep.second.find("baidu") != std::string::npos && config_.baidu_cookie.empty()) {
            openQrLoginModal(NetdiskProvider::BAIDU);
        }
        return;
    }

    selected_quality_idx_ = 0;
    player_paused_ = false;
    player_osd_visible_ = true;
    player_pos_sec_ = 0.0;
    modal_state_ = ModalState::VIDEO_PLAYING;
    video_player_.start(active_play_.url, active_play_.headers);
}

void SwitchPlayerShell::runMainLoop() {
#ifdef __SWITCH__
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
#endif
    while (running_) {
#ifdef __SWITCH__
        if (!appletMainLoop()) break;
#endif
        uint64_t now_ms = HttpClient::currentTimeMillis();
        handleGamepadInput();
        updateFrame(now_ms);
        renderFrame();
    }
}

void SwitchPlayerShell::handleTouchOrClick(int tx, int ty) {
    // 1. 全屏播放界面触屏操控
    if (modal_state_ == ModalState::VIDEO_PLAYING) {
        // 左上角关闭返回按钮 (32, 24, 110, 44)
        if (pointInRect(tx, ty, 24, 16, 130, 56)) {
            video_player_.stop();
            modal_state_ = ModalState::VOD_DETAIL;
            return;
        }
        // 进度条拖动/点按跳进度 (48, 576, 1184, 36)
        if (pointInRect(tx, ty, 48, 572, 1184, 40)) {
            double ratio = std::clamp((tx - 48) / 1184.0, 0.0, 1.0);
            video_player_.seekTo(ratio * video_player_.getDurationSec());
            return;
        }
        // 底部播放控制按钮栏
        if (ty >= 615) {
            if (pointInRect(tx, ty, 48, 620, 140, 50)) {
                video_player_.togglePause();
                player_paused_ = video_player_.isPaused();
            } else if (pointInRect(tx, ty, 200, 620, 220, 50) && !active_play_.qualities.empty()) {
                selected_quality_idx_ = (selected_quality_idx_ + 1) % static_cast<int>(active_play_.qualities.size());
                active_play_.url = active_play_.qualities[selected_quality_idx_].url;
                active_play_.headers = active_play_.qualities[selected_quality_idx_].headers;
                video_player_.start(active_play_.url, active_play_.headers);
                toast_msg_ = "已切换清晰度：" + active_play_.qualities[selected_quality_idx_].label;
                toast_expire_ms_ = HttpClient::currentTimeMillis() + 3000;
            } else if (pointInRect(tx, ty, 440, 620, 150, 50)) {
                player_speed_ = (player_speed_ >= 2.0f) ? 1.0f : (player_speed_ + 0.25f);
                video_player_.setSpeed(player_speed_);
            }
            return;
        }
        // 点击屏幕中央暂停/显隐 OSD
        player_osd_visible_ = !player_osd_visible_;
        return;
    }

    // 2. 二维码扫码弹窗触屏操控
    if (modal_state_ == ModalState::QR_LOGIN) {
        // 弹窗外部点击关闭
        if (!pointInRect(tx, ty, 210, 58, 860, 594)) {
            modal_state_ = ModalState::NONE;
            return;
        }
        // 底部 3 个触屏按钮: [刷新二维码] [切换夸克/百度] [关闭窗口]
        if (pointInRect(tx, ty, 270, 582, 220, 46)) {
            openQrLoginModal(active_qr_.provider);
        } else if (pointInRect(tx, ty, 520, 582, 240, 46)) {
            openQrLoginModal(active_qr_.provider == NetdiskProvider::QUARK
                             ? NetdiskProvider::BAIDU : NetdiskProvider::QUARK);
        } else if (pointInRect(tx, ty, 790, 582, 200, 46)) {
            modal_state_ = ModalState::NONE;
        }
        return;
    }

    // 3. 剧集详情与选集弹窗触屏操控
    if (modal_state_ == ModalState::VOD_DETAIL) {
        if (!pointInRect(tx, ty, 110, 68, 1060, 570)) {
            modal_state_ = ModalState::NONE;
            return;
        }
        // 右上角 [返回列表] 按钮
        if (pointInRect(tx, ty, 1020, 84, 126, 40)) {
            modal_state_ = ModalState::NONE;
            return;
        }
        // 播放线路胶囊点击 (y: 260..306)
        int fx = 144;
        int idx = 0;
        for (const auto& kv : active_vod_.play_flags) {
            (void)kv;
            if (pointInRect(tx, ty, fx, 260, 216, 44)) {
                selected_flag_idx_ = idx;
                selected_ep_idx_ = 0;
                return;
            }
            fx += 230;
            ++idx;
        }
        // 剧集选集按钮点击 (y: 366..540)
        int num_flags = static_cast<int>(active_vod_.play_flags.size());
        if (num_flags > 0) {
            auto it = active_vod_.play_flags.begin();
            std::advance(it, selected_flag_idx_ % num_flags);
            for (size_t i = 0; i < it->second.size() && i < 8; ++i) {
                int ex = 144 + static_cast<int>(i % 4) * 244;
                int ey = 368 + static_cast<int>(i / 4) * 62;
                if (pointInRect(tx, ty, ex, ey, 228, 48)) {
                    selected_ep_idx_ = static_cast<int>(i);
                    startPlayback(active_vod_, it->first, selected_ep_idx_);
                    return;
                }
            }
        }
        return;
    }

    // 4. 顶部导航栏触屏点击 (y: 0..66)
    if (ty <= 66) {
        for (int i = 0; i < 6; ++i) {
            int tab_x = 210 + i * 116;
            if (pointInRect(tx, ty, tab_x, 8, 112, 50)) {
                switchToTab(static_cast<TopTab>(i));
                return;
            }
        }
        // 右上角夸克扫码徽章
        if (pointInRect(tx, ty, 922, 12, 168, 42)) {
            openQrLoginModal(NetdiskProvider::QUARK);
            return;
        }
        // 右上角百度扫码徽章
        if (pointInRect(tx, ty, 1098, 12, 168, 42)) {
            openQrLoginModal(NetdiskProvider::BAIDU);
            return;
        }
        return;
    }

    // 5. 左侧源站侧边栏触屏点击 (x: 0..226, y: 108..572)
    if (tx <= 226 && ty >= 104 && ty <= 576) {
        int max_sites = std::min(static_cast<int>(bundle_.sites.size()), 11);
        for (int i = 0; i < max_sites; ++i) {
            int sy = 112 + i * 40;
            if (pointInRect(tx, ty, 8, sy - 4, 210, 36)) {
                selected_site_idx_ = i;
                selected_cat_idx_ = 0;
                refreshCurrentSiteContentAsync();
                return;
            }
        }
        return;
    }

    // 6. 设置页 4 行卡片触屏点击
    if (current_tab_ == TopTab::SETTINGS) {
        for (int i = 0; i < 4; ++i) {
            int ry = 146 + i * 108;
            if (pointInRect(tx, ty, 248, ry, 1008, 94)) {
                settings_focus_idx_ = i;
                if (i == 0) promptEditSourceUrl();
                else if (i == 1) reloadSourceUrlAsync(DEFAULT_WEX_SOURCE_URL);
                else if (i == 2) openQrLoginModal(NetdiskProvider::QUARK);
                else if (i == 3) openQrLoginModal(NetdiskProvider::BAIDU);
                return;
            }
        }
        return;
    }

    // 7. 主内容区顶部：分类胶囊 & 搜索框 & 源地址栏触屏点击 (y: 72..120)
    if (ty >= 72 && ty <= 122 && tx >= 236) {
        // 分类胶囊 (4 个)
        int cat_count = std::min(static_cast<int>(current_categories_.size()), 5);
        for (int c = 0; c < cat_count; ++c) {
            int cx = 244 + c * 108;
            if (pointInRect(tx, ty, cx, 74, 100, 40)) {
                selected_cat_idx_ = c;
                refreshCurrentSiteContentAsync();
                return;
            }
        }
        // 搜索框按钮 (x: 794..974)
        if (pointInRect(tx, ty, 794, 74, 180, 40)) {
            promptSearchKeyword();
            return;
        }
        // 改源按钮 (x: 984..1264)
        if (pointInRect(tx, ty, 984, 74, 280, 40)) {
            promptEditSourceUrl();
            return;
        }
    }

    // 8. 5x2 影视海报网格触屏点击
    const int COLS = 5;
    int start_x = 244;
    int start_y = 126;
    int card_w = 192;
    int card_h = 252;
    int gap_x = 14;
    int gap_y = 14;
    int count = std::min(static_cast<int>(current_vods_.size()), 10);
    for (int i = 0; i < count; ++i) {
        int row = i / COLS;
        int col = i % COLS;
        int cx = start_x + col * (card_w + gap_x);
        int cy = start_y + row * (card_h + gap_y);
        if (pointInRect(tx, ty, cx, cy, card_w, card_h)) {
            selected_vod_idx_ = i;
            openVodDetailAsync(current_vods_[i]);
            return;
        }
    }
}

void SwitchPlayerShell::handleGamepadInput() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            running_ = false;
            return;
        }
        if (ev.type == SDL_MOUSEBUTTONDOWN) {
            handleTouchOrClick(ev.button.x, ev.button.y);
        } else if (ev.type == SDL_FINGERDOWN) {
            int tx = static_cast<int>(ev.tfinger.x * 1280.0f);
            int ty = static_cast<int>(ev.tfinger.y * 720.0f);
            handleTouchOrClick(tx, ty);
        }
    }

#ifdef __SWITCH__
    // 原生 libnx 电容触屏轮询（确保在所有 Switch 固件与 SDL2 配置下 100% 灵敏响应触控）
    HidTouchScreenState touch_state{};
    if (hidGetTouchScreenStates(&touch_state, 1) > 0) {
        if (touch_state.count > 0) {
            if (!touch_down_prev_) {
                touch_down_prev_ = true;
                handleTouchOrClick(touch_state.touches[0].x, touch_state.touches[0].y);
            }
        } else {
            touch_down_prev_ = false;
        }
    }

    static PadState pad;
    static bool pad_inited = false;
    if (!pad_inited) {
        padInitializeDefault(&pad);
        pad_inited = true;
    }
    padUpdate(&pad);
    u64 kDown = padGetButtonsDown(&pad);
    HidAnalogStickState l_stick = padGetStickPos(&pad, 0);
    HidAnalogStickState r_stick = padGetStickPos(&pad, 1);

    uint64_t now_ms = HttpClient::currentTimeMillis();
    const int DEADZONE = 10000;
    if (now_ms - last_stick_move_ms_ > 140) {
        if (l_stick.x > DEADZONE)  { kDown |= HidNpadButton_Right; last_stick_move_ms_ = now_ms; }
        if (l_stick.x < -DEADZONE) { kDown |= HidNpadButton_Left;  last_stick_move_ms_ = now_ms; }
        if (l_stick.y > DEADZONE)  { kDown |= HidNpadButton_Up;    last_stick_move_ms_ = now_ms; }
        if (l_stick.y < -DEADZONE) { kDown |= HidNpadButton_Down;  last_stick_move_ms_ = now_ms; }
    }

    if (kDown & HidNpadButton_Plus) {
        running_ = false;
        return;
    }

    if (kDown & HidNpadButton_Minus) {
        if (modal_state_ == ModalState::QR_LOGIN) {
            openQrLoginModal(active_qr_.provider == NetdiskProvider::QUARK
                             ? NetdiskProvider::BAIDU : NetdiskProvider::QUARK);
        } else {
            openQrLoginModal(NetdiskProvider::QUARK);
        }
        return;
    }

    if (modal_state_ == ModalState::VIDEO_PLAYING) {
        if (kDown & HidNpadButton_B) {
            video_player_.stop();
            modal_state_ = ModalState::VOD_DETAIL;
            return;
        }
        if (kDown & HidNpadButton_A) {
            video_player_.togglePause();
            player_paused_ = video_player_.isPaused();
        }
        if (kDown & HidNpadButton_X) player_osd_visible_ = !player_osd_visible_;
        if (kDown & HidNpadButton_Left)  video_player_.seekRelative(-10.0);
        if (kDown & HidNpadButton_Right) video_player_.seekRelative(10.0);
        if (kDown & HidNpadButton_Up)    { player_volume_ = std::min(100, player_volume_ + 5); video_player_.setVolume(player_volume_); }
        if (kDown & HidNpadButton_Down)  { player_volume_ = std::max(0, player_volume_ - 5); video_player_.setVolume(player_volume_); }
        if (kDown & HidNpadButton_Y) {
            if (!active_play_.qualities.empty()) {
                selected_quality_idx_ = (selected_quality_idx_ + 1) % static_cast<int>(active_play_.qualities.size());
                active_play_.url = active_play_.qualities[selected_quality_idx_].url;
                active_play_.headers = active_play_.qualities[selected_quality_idx_].headers;
                video_player_.start(active_play_.url, active_play_.headers);
                toast_msg_ = "已切换清晰度：" + active_play_.qualities[selected_quality_idx_].label;
                toast_expire_ms_ = now_ms + 3000;
            }
        }
        if (r_stick.x > DEADZONE && now_ms - last_stick_move_ms_ > 250) {
            player_speed_ = std::min(2.0f, player_speed_ + 0.25f);
            video_player_.setSpeed(player_speed_);
            last_stick_move_ms_ = now_ms;
        } else if (r_stick.x < -DEADZONE && now_ms - last_stick_move_ms_ > 250) {
            player_speed_ = std::max(0.5f, player_speed_ - 0.25f);
            video_player_.setSpeed(player_speed_);
            last_stick_move_ms_ = now_ms;
        }
        return;
    }

    if (modal_state_ == ModalState::QR_LOGIN) {
        if (kDown & HidNpadButton_B) {
            modal_state_ = ModalState::NONE;
        } else if (kDown & HidNpadButton_A) {
            openQrLoginModal(active_qr_.provider);
        } else if (kDown & (HidNpadButton_L | HidNpadButton_R | HidNpadButton_Y)) {
            openQrLoginModal(active_qr_.provider == NetdiskProvider::QUARK
                             ? NetdiskProvider::BAIDU : NetdiskProvider::QUARK);
        }
        return;
    }

    if (modal_state_ == ModalState::VOD_DETAIL) {
        if (kDown & HidNpadButton_B) {
            modal_state_ = ModalState::NONE;
            return;
        }
        int num_flags = static_cast<int>(active_vod_.play_flags.size());
        if (num_flags == 0) return;

        auto it = active_vod_.play_flags.begin();
        std::advance(it, selected_flag_idx_ % num_flags);
        int num_eps = static_cast<int>(it->second.size());

        if (kDown & (HidNpadButton_L | HidNpadButton_Y)) {
            selected_flag_idx_ = (selected_flag_idx_ + 1) % num_flags;
            selected_ep_idx_ = 0;
        } else if (kDown & HidNpadButton_Left) {
            selected_ep_idx_ = (selected_ep_idx_ - 1 + std::max(1, num_eps)) % std::max(1, num_eps);
        } else if (kDown & HidNpadButton_Right) {
            selected_ep_idx_ = (selected_ep_idx_ + 1) % std::max(1, num_eps);
        } else if (kDown & HidNpadButton_A) {
            startPlayback(active_vod_, it->first, selected_ep_idx_);
        }
        return;
    }

    if (kDown & HidNpadButton_L) {
        int t = (static_cast<int>(current_tab_) - 1 + 6) % 6;
        switchToTab(static_cast<TopTab>(t));
    }
    if (kDown & HidNpadButton_R) {
        int t = (static_cast<int>(current_tab_) + 1) % 6;
        switchToTab(static_cast<TopTab>(t));
    }

    if (kDown & HidNpadButton_Y) {
        promptEditSourceUrl();
        return;
    }

    if (kDown & HidNpadButton_X) {
        promptSearchKeyword();
        return;
    }

    if ((kDown & HidNpadButton_ZL) && !bundle_.sites.empty()) {
        selected_site_idx_ = (selected_site_idx_ - 1 + static_cast<int>(bundle_.sites.size())) % static_cast<int>(bundle_.sites.size());
        refreshCurrentSiteContentAsync();
    }
    if ((kDown & HidNpadButton_ZR) && !bundle_.sites.empty()) {
        selected_site_idx_ = (selected_site_idx_ + 1) % static_cast<int>(bundle_.sites.size());
        refreshCurrentSiteContentAsync();
    }

    if (current_tab_ == TopTab::SETTINGS) {
        if (kDown & HidNpadButton_Up) settings_focus_idx_ = (settings_focus_idx_ - 1 + 4) % 4;
        if (kDown & HidNpadButton_Down) settings_focus_idx_ = (settings_focus_idx_ + 1) % 4;
        if (kDown & HidNpadButton_A) {
            if (settings_focus_idx_ == 0) promptEditSourceUrl();
            else if (settings_focus_idx_ == 1) reloadSourceUrlAsync(DEFAULT_WEX_SOURCE_URL);
            else if (settings_focus_idx_ == 2) openQrLoginModal(NetdiskProvider::QUARK);
            else if (settings_focus_idx_ == 3) openQrLoginModal(NetdiskProvider::BAIDU);
        }
        return;
    }

    const int COLS = 5;
    int total = static_cast<int>(current_vods_.size());
    if (total > 0) {
        if (kDown & HidNpadButton_Left)  selected_vod_idx_ = (selected_vod_idx_ - 1 + total) % total;
        if (kDown & HidNpadButton_Right) selected_vod_idx_ = (selected_vod_idx_ + 1) % total;
        if (kDown & HidNpadButton_Up)    selected_vod_idx_ = std::max(0, selected_vod_idx_ - COLS);
        if (kDown & HidNpadButton_Down)  selected_vod_idx_ = std::min(total - 1, selected_vod_idx_ + COLS);
        if (kDown & HidNpadButton_A) {
            openVodDetailAsync(current_vods_[selected_vod_idx_]);
        }
    }
#endif
}

void SwitchPlayerShell::updateFrame(uint64_t now_ms) {
    {
        std::lock_guard<std::mutex> lk(data_mutex_);
        if (pending_source_apply_) {
            bundle_ = std::move(pending_bundle_);
            pending_source_apply_ = false;
            js_engine_.init(config_, bundle_);
            selected_site_idx_ = 0;
            selected_cat_idx_ = 0;
            selected_vod_idx_ = 0;
            toast_msg_ = bundle_.status_message;
            toast_expire_ms_ = now_ms + 4000;
            refreshCurrentSiteContentAsync();
        }
        if (pending_content_apply_) {
            if (!pending_categories_.empty()) {
                current_categories_ = std::move(pending_categories_);
            }
            current_vods_ = std::move(pending_vods_);
            if (selected_vod_idx_ >= static_cast<int>(current_vods_.size())) {
                selected_vod_idx_ = 0;
            }
            pending_content_apply_ = false;
        }
        if (pending_detail_apply_) {
            active_vod_ = std::move(pending_detail_vod_);
            pending_detail_apply_ = false;
            selected_flag_idx_ = 0;
            selected_ep_idx_ = 0;
            modal_state_ = ModalState::VOD_DETAIL;
        }
    }

    // 非阻塞后台线程轮询夸克/百度扫码状态
    if (modal_state_ == ModalState::QR_LOGIN &&
        !qr_loading_.load() &&
        !qr_polling_.load() &&
        (active_qr_.state == QRLoginState::WAITING_SCAN || active_qr_.state == QRLoginState::SCANNED_WAIT_CONFIRM) &&
        now_ms - last_qr_poll_ms_ >= 2000) {
        last_qr_poll_ms_ = now_ms;
        qr_polling_.store(true);
        QRLoginSession session_copy = active_qr_;
        AppConfig cfg_copy = config_;
        spawnLargeStackThread([this, session_copy, cfg_copy]() mutable {
            bool ok = (session_copy.provider == NetdiskProvider::QUARK)
                ? NetdiskAuth::pollQuarkQRLogin(session_copy, cfg_copy)
                : NetdiskAuth::pollBaiduQRLogin(session_copy, cfg_copy);
            {
                std::lock_guard<std::mutex> lk(data_mutex_);
                if (modal_state_ == ModalState::QR_LOGIN &&
                    active_qr_.qr_token == session_copy.qr_token) {
                    active_qr_.state = session_copy.state;
                    active_qr_.status_text = session_copy.status_text;
                    if (ok) {
                        config_ = cfg_copy;
                        js_engine_.updateCookies(config_);
                        toast_msg_ = active_qr_.status_text;
                        toast_expire_ms_ = HttpClient::currentTimeMillis() + 4500;
                        modal_state_ = ModalState::NONE;
                        refreshCurrentSiteContentAsync();
                    }
                }
            }
            qr_polling_.store(false);
        });
    }

    if (modal_state_ == ModalState::VIDEO_PLAYING) {
        player_pos_sec_ = video_player_.getPositionSec();
        player_dur_sec_ = std::max(1.0, video_player_.getDurationSec());
        player_paused_ = video_player_.isPaused();
    }
}

void SwitchPlayerShell::drawRect(int x, int y, int w, int h, SDL_Color color, bool filled) {
    SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
    SDL_Rect r{x, y, w, h};
    if (filled) SDL_RenderFillRect(renderer_, &r);
    else SDL_RenderDrawRect(renderer_, &r);
}

void SwitchPlayerShell::drawRoundedRect(int x, int y, int w, int h, int radius, SDL_Color color) {
    if (w <= 0 || h <= 0) return;
    int r = std::min(radius, std::min(w / 2, h / 2));
    if (r <= 1) {
        drawRect(x, y, w, h, color, true);
        return;
    }
    SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
    SDL_Rect mid{x + r, y, w - 2 * r, h};
    SDL_RenderFillRect(renderer_, &mid);
    SDL_Rect left_strip{x, y + r, r, h - 2 * r};
    SDL_RenderFillRect(renderer_, &left_strip);
    SDL_Rect right_strip{x + w - r, y + r, r, h - 2 * r};
    SDL_RenderFillRect(renderer_, &right_strip);

    for (int dy = 0; dy < r; ++dy) {
        int dx = static_cast<int>(std::sqrt(static_cast<double>(r * r - dy * dy)));
        SDL_RenderDrawLine(renderer_, x + r - dx, y + r - 1 - dy, x + r - 1, y + r - 1 - dy);
        SDL_RenderDrawLine(renderer_, x + w - r, y + r - 1 - dy, x + w - r + dx - 1, y + r - 1 - dy);
        SDL_RenderDrawLine(renderer_, x + r - dx, y + h - r + dy, x + r - 1, y + h - r + dy);
        SDL_RenderDrawLine(renderer_, x + w - r, y + h - r + dy, x + w - r + dx - 1, y + h - r + dy);
    }
}

void SwitchPlayerShell::drawRoundedOutline(int x, int y, int w, int h, int radius, int thickness, SDL_Color color) {
    for (int t = 0; t < thickness; ++t) {
        drawRect(x - t, y - t, w + 2 * t, h + 2 * t, color, false);
    }
    (void)radius;
}


SDL_Texture* SwitchPlayerShell::getOrFetchPosterTex(const std::string& pic_url) {
    if (pic_url.empty() || pic_url.rfind("http", 0) != 0) return nullptr;
    auto it = poster_tex_cache_.find(pic_url);
    if (it != poster_tex_cache_.end()) {
        return it->second;
    }
    {
        std::lock_guard<std::mutex> lk(data_mutex_);
        auto bit = pending_poster_bytes_.find(pic_url);
        if (bit != pending_poster_bytes_.end()) {
            SDL_Texture* tex = nullptr;
            if (!bit->second.empty()) {
                SDL_RWops* rw = SDL_RWFromConstMem(bit->second.data(), static_cast<int>(bit->second.size()));
                if (rw) {
                    tex = IMG_LoadTexture_RW(renderer_, rw, 1);
                    if (tex) SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
                }
            }
            poster_tex_cache_[pic_url] = tex;
            pending_poster_bytes_.erase(bit);
            return tex;
        }
        if (poster_fetching_[pic_url]) return nullptr;
        poster_fetching_[pic_url] = true;
    }

    spawnLargeStackThread([this, pic_url]() {
        HttpResponse r = HttpClient::get(pic_url, {{"Referer", "https://movie.douban.com/"}}, 6000, true);
        std::lock_guard<std::mutex> lk(data_mutex_);
        if (r.ok() && r.body.size() > 64) {
            pending_poster_bytes_[pic_url] = std::move(r.body);
        } else {
            pending_poster_bytes_[pic_url] = "";
        }
    });
    return nullptr;
}

void SwitchPlayerShell::clearTextCache() {
    for (auto& kv : text_cache_) {
        if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    }
    text_cache_.clear();
}

void SwitchPlayerShell::drawText(const std::string& text, int x, int y, SDL_Color color, bool large) {
    if (text.empty()) return;
    TTF_Font* f = (large && font_large_) ? font_large_ : font_regular_;
    if (!f) return;

    char key_buf[64];
    std::snprintf(key_buf, sizeof(key_buf), "%d|%02x%02x%02x%02x|", large ? 1 : 0, color.r, color.g, color.b, color.a);
    std::string key = std::string(key_buf) + text;

    auto it = text_cache_.find(key);
    if (it == text_cache_.end()) {
        if (text_cache_.size() >= 512) {
            clearTextCache();
        }
        SDL_Surface* surf = TTF_RenderUTF8_Blended(f, text.c_str(), color);
        if (!surf) return;
        SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer_, surf);
        CachedTextTex entry{tex, surf->w, surf->h};
        SDL_FreeSurface(surf);
        if (!tex) return;
        it = text_cache_.emplace(std::move(key), entry).first;
    }

    SDL_Rect dst{x, y, it->second.w, it->second.h};
    SDL_RenderCopy(renderer_, it->second.tex, nullptr, &dst);
}

void SwitchPlayerShell::drawTextCentered(const std::string& text, int cx, int y, SDL_Color color, bool large) {
    if (text.empty()) return;
    TTF_Font* f = (large && font_large_) ? font_large_ : font_regular_;
    if (!f) return;
    int w = 0, h = 0;
    if (TTF_SizeUTF8(f, text.c_str(), &w, &h) == 0) {
        drawText(text, cx - w / 2, y, color, large);
    } else {
        drawText(text, cx, y, color, large);
    }
}

void SwitchPlayerShell::drawTextClipped(const std::string& text, int x, int y, int max_px_width, SDL_Color color, bool large) {
    if (text.empty() || max_px_width <= 16) return;
    TTF_Font* f = (large && font_large_) ? font_large_ : font_regular_;
    if (!f) return;

    int w = 0, h = 0;
    if (TTF_SizeUTF8(f, text.c_str(), &w, &h) == 0 && w <= max_px_width) {
        drawText(text, x, y, color, large);
        return;
    }
    std::string cur = text;
    while (!cur.empty()) {
        cur = utf8PopLastChar(cur);
        std::string candidate = cur + "..";
        if (TTF_SizeUTF8(f, candidate.c_str(), &w, &h) == 0 && w <= max_px_width) {
            drawText(candidate, x, y, color, large);
            return;
        }
    }
}

void SwitchPlayerShell::renderTopBar() {
    drawRect(0, 0, 1280, 66, {15, 19, 29, 255});
    drawRect(0, 65, 1280, 1, {40, 52, 76, 255});

    // 品牌 Logo + HTT 爱心图标
    drawRoundedRect(14, 12, 188, 42, 10, {26, 38, 64, 255});
    if (kon_htt_heart_tex_) {
        SDL_Rect hdst{22, 17, 32, 32};
        SDL_RenderCopy(renderer_, kon_htt_heart_tex_, nullptr, &hdst);
        drawText("OK影视 HTT", 60, 21, {255, 196, 224, 255}, true);
    } else {
        drawText("OK影视 Switch", 26, 21, {96, 178, 255, 255}, true);
    }

    static const char* TAB_NAMES[6] = {
        "发现·首页", "影视·分类", "聚合·搜索", "夸克·网盘", "百度·网盘", "配置·中心"
    };
    for (int i = 0; i < 6; ++i) {
        int tx = 212 + i * 116;
        bool active = (static_cast<int>(current_tab_) == i);
        if (active) {
            drawRoundedRect(tx, 12, 110, 42, 11, {218, 78, 136, 255});
        } else {
            drawRoundedRect(tx, 14, 110, 38, 10, {22, 28, 42, 200});
        }
        // 绘制轻音少女 5 位主角 Q 版立绘头像 (Yui/Mio/Ritsu/Mugi/Azusa) + 茶杯图标
        SDL_Texture* icon_tex = nullptr;
        if (i < 5) {
            icon_tex = active ? kon_char_active_[i] : kon_char_idle_[i];
        } else {
            icon_tex = kon_tea_pink_tex_;
        }
        if (icon_tex) {
            SDL_Rect idst{tx + 6, 19, 26, 26};
            SDL_RenderCopy(renderer_, icon_tex, nullptr, &idst);
            drawTextClipped(TAB_NAMES[i], tx + 35, 23, 72,
                            active ? SDL_Color{255, 255, 255, 255} : SDL_Color{170, 184, 206, 255});
        } else {
            drawTextCentered(TAB_NAMES[i], tx + 55, 23,
                             active ? SDL_Color{255, 255, 255, 255} : SDL_Color{170, 184, 206, 255});
        }
    }

    // 右上角夸克/百度登录状态触屏按钮
    std::string q_badge = config_.quark_cookie.empty() ? "夸克:扫码登录" : ("夸克:" + utf8Truncate(config_.quark_nickname, 5));
    std::string b_badge = config_.baidu_cookie.empty() ? "百度:扫码登录" : ("百度:" + utf8Truncate(config_.baidu_username, 5));
    drawRoundedRect(920, 14, 168, 38, 10, config_.quark_cookie.empty() ? SDL_Color{56, 36, 48, 255} : SDL_Color{26, 92, 66, 255});
    drawTextCentered(q_badge, 1004, 23, {240, 248, 255, 255});
    drawRoundedRect(1096, 14, 168, 38, 10, config_.baidu_cookie.empty() ? SDL_Color{56, 36, 48, 255} : SDL_Color{28, 76, 122, 255});
    drawTextCentered(b_badge, 1180, 23, {240, 248, 255, 255});
}

void SwitchPlayerShell::renderSiteAndCategorySidebar() {
    drawRect(0, 66, 226, 594, {13, 17, 26, 245});
    drawRect(225, 66, 1, 594, {36, 48, 70, 255});
    drawText("站点切换 (触屏/ZL/ZR)", 14, 78, {255, 182, 214, 255});

    int max_sites = std::min(static_cast<int>(bundle_.sites.size()), 11);
    for (int i = 0; i < max_sites; ++i) {
        int sy = 110 + i * 40;
        bool sel = (i == selected_site_idx_);
        if (sel) {
            drawRoundedRect(10, sy - 4, 206, 32, 8, {44, 88, 168, 255});
            drawRoundedRect(10, sy, 4, 24, 2, {255, 182, 214, 255});
        }
        drawTextClipped(bundle_.sites[i].name, 22, sy + 2, 186,
                        sel ? SDL_Color{255, 255, 255, 255} : SDL_Color{178, 192, 214, 255});
    }

    if (kon_tonchan_tex_) {
        drawRoundedRect(10, 566, 206, 82, 10, {22, 28, 44, 235});
        drawRoundedOutline(10, 566, 206, 82, 10, 1, {218, 78, 136, 180});
        SDL_Rect dst{18, 575, 64, 64};
        SDL_RenderCopy(renderer_, kon_tonchan_tex_, nullptr, &dst);
        drawText("放课后茶会 HTT", 88, 582, {255, 196, 224, 255});
        drawText("触屏+手柄双控", 88, 610, {165, 195, 230, 255});
    }
}

void SwitchPlayerShell::renderVodGrid() {
    // 顶部二级导航栏：真实分类胶囊 (热门电影/热门剧集/综艺/动漫) + 搜索按钮 + 改源按钮
    int cat_count = std::min(static_cast<int>(current_categories_.size()), 5);
    for (int c = 0; c < cat_count; ++c) {
        int cx = 244 + c * 108;
        bool c_sel = (c == (selected_cat_idx_ % std::max(1, cat_count)));
        drawRoundedRect(cx, 76, 100, 36, 9,
                        c_sel ? SDL_Color{40, 102, 196, 255} : SDL_Color{22, 30, 46, 240});
        drawTextCentered(utf8Truncate(current_categories_[c].type_name, 5), cx + 50, 84,
                         c_sel ? SDL_Color{255, 255, 255, 255} : SDL_Color{175, 190, 215, 255});
    }

    // [X 搜索影片] 触屏按钮
    drawRoundedRect(794, 76, 180, 36, 9, {28, 64, 96, 255});
    drawTextCentered("[X] 搜索影片/网盘链", 884, 84, {165, 224, 255, 255});

    // [Y 修改源地址] 触屏按钮（像素裁剪，绝不溢出）
    drawRoundedRect(984, 76, 280, 36, 9, {26, 36, 56, 255});
    std::string src_short = source_loading_.load()
        ? "同步源中..."
        : ("[Y改源] " + config_.source_url);
    drawTextClipped(src_short, 996, 84, 256, {195, 215, 245, 255});

    const int COLS = 5;
    int start_x = 244;
    int start_y = 124;
    int card_w = 192;
    int card_h = 254;
    int gap_x = 14;
    int gap_y = 14;

    int count = std::min(static_cast<int>(current_vods_.size()), 10);
    if (count == 0 && (source_loading_.load() || content_loading_.load())) {
        drawTextCentered("正在加载实时影视片单，请稍候...", 754, 350, {180, 210, 250, 255}, true);
        return;
    }

    for (int i = 0; i < count; ++i) {
        int row = i / COLS;
        int col = i % COLS;
        int cx = start_x + col * (card_w + gap_x);
        int cy = start_y + row * (card_h + gap_y);
        bool focused = (i == selected_vod_idx_);

        // 卡片外框与底板
        drawRoundedRect(cx, cy, card_w, card_h, 12,
                        focused ? SDL_Color{36, 54, 86, 255} : SDL_Color{22, 28, 42, 245});
        if (focused) {
            drawRoundedOutline(cx, cy, card_w, card_h, 12, 3, {255, 156, 204, 255});
        }

        // 海报封面区（轻音少女主题封面底图 + 暗色渐变叠层）
        SDL_Color art = cardAccentColor(current_vods_[i].vod_name);
        drawRoundedRect(cx + 8, cy + 8, card_w - 16, 168, 9, art);
        SDL_Texture* poster_tex = getOrFetchPosterTex(current_vods_[i].vod_pic);
        SDL_Texture* card_tex = poster_tex ? poster_tex : kon_card_tex_[i % 5];
        if (card_tex) {
            SDL_SetTextureAlphaMod(card_tex, poster_tex ? 255 : (focused ? 225 : 170));
            SDL_Rect art_dst{cx + 9, cy + 9, card_w - 18, 166};
            SDL_RenderCopy(renderer_, card_tex, nullptr, &art_dst);
            drawRect(cx + 9, cy + 132, card_w - 18, 43, {12, 16, 26, 175});
        }

        // 左上角橙色评分/状态胶囊角标（对齐参考视频样式，动态自适应宽度）
        if (!current_vods_[i].vod_remarks.empty()) {
            drawRoundedRect(cx + 14, cy + 14, 112, 24, 7, {230, 126, 34, 245});
            drawTextClipped(current_vods_[i].vod_remarks, cx + 20, cy + 16, 100, {255, 255, 255, 255});
        }

        // 封面底部小字（站点名）
        drawTextClipped(current_vods_[i].site_name, cx + 14, cy + 146, card_w - 28, {220, 232, 250, 240});

        // 底部剧名与年份/地区（像素级限宽裁剪，绝不超框或乱码）
        drawTextClipped(current_vods_[i].vod_name, cx + 10, cy + 186, card_w - 20, {248, 252, 255, 255});
        std::string sub = (current_vods_[i].vod_year.empty() ? "2026" : current_vods_[i].vod_year) +
                          " · " + (current_vods_[i].vod_area.empty() ? current_vods_[i].site_name : current_vods_[i].vod_area);
        drawTextClipped(sub, cx + 10, cy + 218, card_w - 20, {150, 168, 196, 255});
    }
}

void SwitchPlayerShell::renderSettingsPanel() {
    drawRoundedRect(244, 78, 1016, 566, 14, {18, 24, 36, 250});
    drawText("接口源地址与网盘账号配置中心 (支持触屏点击 + 手柄按键)", 272, 94, {255, 182, 214, 255}, true);

    struct SettingRow {
        std::string title;
        std::string value;
        std::string hint;
    };
    SettingRow rows[4] = {
        {
            "1. 点击或按 [A] 直接填写/修改接口源地址 (弹出 Switch 系统键盘)",
            config_.source_url,
            "已原生适配 http://wexfnw:wexfnw@cat.xn--4kq62z5rby2qupq9ub.top/index.js.md5"
        },
        {
            "2. 点击或按 [A] 一键恢复默认王二小放牛娃 .js.md5 猫源并刷新片单",
            DEFAULT_WEX_SOURCE_URL,
            bundle_.status_message
        },
        {
            "3. 夸克网盘 (pan.quark.cn) 手机扫码登录 / 刷新凭证",
            config_.quark_cookie.empty() ? "未登录 (点击此行或按 A 弹出夸克官方登录码)" : ("已登录：" + config_.quark_nickname),
            "使用 su.quark.cn 官方扫码授权协议，支持夸克 4K 原画直链与转码"
        },
        {
            "4. 百度网盘 (pan.baidu.com) 手机扫码登录 / 刷新凭证",
            config_.baidu_cookie.empty() ? "未登录 (点击此行或按 A 弹出百度官方登录二维码)" : ("已登录：" + config_.baidu_username),
            "直接拉取 passport.baidu.com 官方二维码图片，扫码即弹确认登录"
        }
    };

    for (int i = 0; i < 4; ++i) {
        int ry = 146 + i * 108;
        bool sel = (settings_focus_idx_ == i);
        drawRoundedRect(268, ry, 968, 94, 10, sel ? SDL_Color{38, 56, 92, 255} : SDL_Color{24, 32, 48, 255});
        if (sel) drawRoundedOutline(268, ry, 968, 94, 10, 2, {255, 166, 210, 255});
        drawTextClipped(rows[i].title, 288, ry + 10, 928, {255, 255, 255, 255});
        drawTextClipped(rows[i].value, 288, ry + 38, 928, {121, 192, 255, 255});
        drawTextClipped(rows[i].hint,  288, ry + 64, 928, {155, 172, 195, 255});
    }

    drawTextClipped("手机/电脑浏览器局域网管理地址: " + lan_server_.getLanUrl(), 268, 596, 780, {63, 185, 80, 255});
    if (kon_banner_tex_) {
        SDL_Rect bdst{1076, 584, 160, 52};
        SDL_RenderCopy(renderer_, kon_banner_tex_, nullptr, &bdst);
    }
}

void SwitchPlayerShell::renderDetailModal() {
    drawRect(0, 0, 1280, 720, {6, 10, 18, 190});
    drawRoundedRect(110, 68, 1060, 570, 16, {18, 24, 38, 255});
    drawRoundedOutline(110, 68, 1060, 570, 16, 2, {255, 166, 210, 255});

    drawTextClipped(active_vod_.vod_name, 144, 94, 850, {255, 255, 255, 255}, true);
    // 右上角触屏返回按钮
    drawRoundedRect(1020, 88, 122, 38, 9, {52, 64, 88, 255});
    drawTextCentered("[B] 返回列表", 1081, 97, {235, 245, 255, 255});

    drawTextClipped(active_vod_.vod_year + " | " + active_vod_.vod_area + " | " + active_vod_.vod_remarks,
                    144, 136, 980, {121, 192, 255, 255});
    drawTextClipped(active_vod_.vod_content, 144, 172, 980, {190, 205, 225, 255});

    drawText("播放线路 (点击线路或按 L/Y 切换直连 / 夸克原画 / 百度网盘):", 144, 224, {255, 215, 100, 255});
    int fx = 144;
    int idx = 0;
    std::string active_flag_name;
    std::vector<std::pair<std::string, std::string>> active_eps;

    for (const auto& kv : active_vod_.play_flags) {
        bool sel = (idx == (selected_flag_idx_ % std::max(1, static_cast<int>(active_vod_.play_flags.size()))));
        if (sel) {
            active_flag_name = kv.first;
            active_eps = kv.second;
        }
        drawRoundedRect(fx, 260, 216, 42, 10, sel ? SDL_Color{218, 78, 136, 255} : SDL_Color{30, 40, 60, 255});
        drawTextCentered(utf8Truncate(kv.first, 10), fx + 108, 271, {255, 255, 255, 255});
        fx += 230;
        ++idx;
    }

    drawText("剧集选集 (直接触屏点击剧集，或用摇杆左右选择后按 A 播放):", 144, 326, {200, 215, 235, 255});
    for (size_t i = 0; i < active_eps.size() && i < 8; ++i) {
        int ex = 144 + static_cast<int>(i % 4) * 244;
        int ey = 368 + static_cast<int>(i / 4) * 62;
        bool ep_sel = (static_cast<int>(i) == selected_ep_idx_);
        drawRoundedRect(ex, ey, 228, 48, 10, ep_sel ? SDL_Color{40, 102, 196, 255} : SDL_Color{28, 36, 54, 255});
        if (ep_sel) drawRoundedOutline(ex, ey, 228, 48, 10, 2, {121, 192, 255, 255});
        drawTextCentered(utf8Truncate(active_eps[i].first, 12), ex + 114, ey + 14, {255, 255, 255, 255});
    }

    drawText("[触屏点击/A键] 立即播放选集    [L/Y] 切换播放源    [-] 网盘扫码授权    [B] 返回",
             144, 584, {160, 182, 210, 255});
}

void SwitchPlayerShell::renderQrLoginModal() {
    drawRect(0, 0, 1280, 720, {6, 10, 18, 195});
    drawRoundedRect(210, 58, 860, 594, 16, {16, 22, 34, 255});
    drawRoundedOutline(210, 58, 860, 594, 16, 2, {255, 166, 210, 255});

    std::string title = (active_qr_.provider == NetdiskProvider::QUARK)
        ? "夸克网盘 (pan.quark.cn) 手机扫码登录"
        : "百度网盘 (pan.baidu.com) 手机扫码登录";
    drawTextCentered(title, 640, 82, {255, 186, 220, 255}, true);
    drawTextCentered(utf8Truncate(active_qr_.status_text, 46), 640, 124, {235, 245, 255, 255});

    const int box_size = 310;
    const int box_x = (1280 - box_size) / 2;
    const int box_y = 166;
    drawRoundedRect(box_x, box_y, box_size, box_size, 12, {255, 255, 255, 255});

    // 若百度网盘返回了官方二维码 PNG 图片，直接渲染官方图片以确保 100% 识别登录
    if (!active_qr_.qr_png_bytes.empty()) {
        if (active_qr_png_token_ != active_qr_.qr_token || !active_qr_png_tex_) {
            if (active_qr_png_tex_) {
                SDL_DestroyTexture(active_qr_png_tex_);
                active_qr_png_tex_ = nullptr;
            }
            SDL_RWops* rw = SDL_RWFromConstMem(active_qr_.qr_png_bytes.data(),
                                               static_cast<int>(active_qr_.qr_png_bytes.size()));
            if (rw) {
                active_qr_png_tex_ = IMG_LoadTexture_RW(renderer_, rw, 1);
                active_qr_png_token_ = active_qr_.qr_token;
            }
        }
    } else if (active_qr_png_tex_) {
        SDL_DestroyTexture(active_qr_png_tex_);
        active_qr_png_tex_ = nullptr;
        active_qr_png_token_.clear();
    }

    if (active_qr_png_tex_) {
        SDL_Rect dst{box_x + 15, box_y + 15, box_size - 30, box_size - 30};
        SDL_RenderCopy(renderer_, active_qr_png_tex_, nullptr, &dst);
    } else if (active_qr_.qr_size > 0 && !active_qr_.qr_modules.empty()) {
        int total_mods = active_qr_.qr_size + 8;
        int cell = std::max(2, box_size / total_mods);
        int qr_px = active_qr_.qr_size * cell;
        int qx = box_x + (box_size - qr_px) / 2;
        int qy = box_y + (box_size - qr_px) / 2;
        for (int y = 0; y < active_qr_.qr_size; ++y) {
            for (int x = 0; x < active_qr_.qr_size; ++x) {
                if (active_qr_.qr_modules[y * active_qr_.qr_size + x]) {
                    drawRect(qx + x * cell, qy + y * cell, cell, cell, {0, 0, 0, 255});
                }
            }
        }
    } else {
        drawTextCentered(qr_loading_.load() ? "正在获取官方登录二维码..." : "二维码未就绪，请点下方刷新",
                         640, 310, {40, 48, 60, 255});
    }

    drawTextCentered("备用方式：手机/电脑浏览器打开 " + lan_server_.getLanUrl() + " 可直接粘贴 Cookie 同步",
                     640, 504, {63, 185, 80, 255});

    // 底部 3 个可触屏点击的大按钮
    drawRoundedRect(270, 582, 220, 44, 10, {40, 102, 196, 255});
    drawTextCentered("[A] 刷新二维码", 380, 594, {255, 255, 255, 255});

    drawRoundedRect(520, 582, 240, 44, 10, {218, 78, 136, 255});
    drawTextCentered("[L/R] 切换夸克/百度", 640, 594, {255, 255, 255, 255});

    drawRoundedRect(790, 582, 200, 44, 10, {52, 64, 88, 255});
    drawTextCentered("[B] 关闭窗口", 890, 594, {255, 255, 255, 255});
}

void SwitchPlayerShell::renderFullscreenPlayer() {
    drawRect(0, 0, 1280, 720, {8, 10, 16, 255});

    // 渲染真实 FFmpeg 解码出的视频画面帧
    SDL_Rect full_dst{0, 0, 1280, 720};
    video_player_.renderVideoFrame(renderer_, full_dst);

    if (player_osd_visible_) {
        // 顶部状态栏（左上关闭按钮 + 实时解码分辨率状态 + 右上剧集名）
        drawRect(0, 0, 1280, 68, {12, 16, 26, 210});
        drawRoundedRect(28, 14, 116, 40, 9, {48, 60, 84, 240});
        drawTextCentered("X 退出播放", 86, 24, {255, 255, 255, 255});
        drawTextClipped(video_player_.getVideoStats(), 164, 24, 600, {121, 210, 255, 255});
        drawTextClipped(active_vod_.vod_name, 780, 22, 470, {255, 255, 255, 255}, true);

        // 底部 OSD 控制面板
        drawRect(0, 548, 1280, 172, {12, 16, 26, 225});
        drawRoundedRect(48, 578, 1184, 12, 6, {45, 55, 75, 255});
        int fill_w = static_cast<int>((player_pos_sec_ / std::max(1.0, player_dur_sec_)) * 1184.0);
        drawRoundedRect(48, 578, std::clamp(fill_w, 0, 1184), 12, 6, {218, 78, 136, 255});

        // 触屏按钮组
        drawRoundedRect(48, 616, 136, 44, 9, {40, 102, 196, 255});
        drawTextCentered(player_paused_ ? "继续播放" : "暂停播放", 116, 628, {255, 255, 255, 255});

        std::string q_label = active_play_.qualities.empty()
            ? "高清直连流"
            : active_play_.qualities[selected_quality_idx_].label;
        drawRoundedRect(200, 616, 224, 44, 9, {36, 138, 86, 255});
        drawTextCentered(utf8Truncate(q_label, 11), 312, 628, {255, 255, 255, 255});

        std::ostringstream sp_ss;
        sp_ss << "倍速 " << std::fixed << std::setprecision(2) << player_speed_ << "x";
        drawRoundedRect(440, 616, 144, 44, 9, {52, 64, 88, 255});
        drawTextCentered(sp_ss.str(), 512, 628, {255, 255, 255, 255});

        std::ostringstream time_ss;
        time_ss << int(player_pos_sec_) / 60 << ":" << std::setw(2) << std::setfill('0') << int(player_pos_sec_) % 60
                << " / " << int(player_dur_sec_) / 60 << ":00   |   音量: " << player_volume_ << "%";
        drawText(time_ss.str(), 612, 628, {235, 245, 255, 255});

        drawTextClipped("流地址: " + active_play_.url, 48, 676, 1184, {145, 175, 215, 255});
    }
}

void SwitchPlayerShell::renderBottomGamepadLegend() {
    drawRect(0, 660, 1280, 60, {14, 18, 28, 255});
    drawRect(0, 660, 1280, 1, {38, 50, 74, 255});
    drawTextClipped("[触屏/摇杆] 自由点选   [A] 确认   [Y] 填写源地址   [X] 聚合搜片   [L/R] 顶栏切换   [ZL/ZR] 换站点   [-] 夸克/百度扫码   [+] 退出",
                    20, 680, 1240, {180, 198, 225, 255});

    if (HttpClient::currentTimeMillis() < toast_expire_ms_ && !toast_msg_.empty()) {
        drawRoundedRect(290, 602, 700, 44, 11, {218, 78, 136, 242});
        drawTextCentered(utf8Truncate(toast_msg_, 46), 640, 614, {255, 255, 255, 255});
    }
}

void SwitchPlayerShell::renderFrame() {
    SDL_SetRenderDrawColor(renderer_, 12, 16, 24, 255);
    SDL_RenderClear(renderer_);
    if (kon_bg_tex_ && modal_state_ != ModalState::VIDEO_PLAYING) {
        SDL_SetTextureAlphaMod(kon_bg_tex_, 42);
        SDL_Rect full{0, 0, 1280, 720};
        SDL_RenderCopy(renderer_, kon_bg_tex_, nullptr, &full);
    }

    if (modal_state_ == ModalState::VIDEO_PLAYING) {
        renderFullscreenPlayer();
    } else {
        renderTopBar();
        renderSiteAndCategorySidebar();
        if (current_tab_ == TopTab::SETTINGS) {
            renderSettingsPanel();
        } else {
            renderVodGrid();
        }
        renderBottomGamepadLegend();

        if (modal_state_ == ModalState::VOD_DETAIL) {
            renderDetailModal();
        } else if (modal_state_ == ModalState::QR_LOGIN) {
            renderQrLoginModal();
        }
    }

    SDL_RenderPresent(renderer_);
}

void SwitchPlayerShell::shutdown() {
    video_player_.stop();
    lan_server_.stop();
    clearTextCache();
    for (auto& kv : poster_tex_cache_) {
        if (kv.second) SDL_DestroyTexture(kv.second);
    }
    poster_tex_cache_.clear();
    if (active_qr_png_tex_) SDL_DestroyTexture(active_qr_png_tex_);
    if (kon_bg_tex_) SDL_DestroyTexture(kon_bg_tex_);
    if (kon_banner_tex_) SDL_DestroyTexture(kon_banner_tex_);
    if (kon_tonchan_tex_) SDL_DestroyTexture(kon_tonchan_tex_);
    if (kon_htt_heart_tex_) SDL_DestroyTexture(kon_htt_heart_tex_);
    if (kon_tea_pink_tex_) SDL_DestroyTexture(kon_tea_pink_tex_);
    for (int i = 0; i < 5; ++i) {
        if (kon_card_tex_[i]) SDL_DestroyTexture(kon_card_tex_[i]);
        if (kon_char_idle_[i]) SDL_DestroyTexture(kon_char_idle_[i]);
        if (kon_char_active_[i]) SDL_DestroyTexture(kon_char_active_[i]);
    }
    IMG_Quit();
    if (font_regular_) TTF_CloseFont(font_regular_);
    if (font_large_) TTF_CloseFont(font_large_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    TTF_Quit();
    SDL_Quit();
    HttpClient::globalCleanup();
}

} // namespace okplayer
