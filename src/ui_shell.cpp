#include "ui_shell.hpp"
#include "http_client.hpp"
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <sstream>
#include <iomanip>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace okplayer {

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
    // 加载 Switch 系统内置中文字体 (PlSharedFontType_ChineseSimplified)
    PlFontData font_data;
    if (R_SUCCEEDED(plGetSharedFontByType(&font_data, PlSharedFontType_ChineseSimplified))) {
        SDL_RWops* rw1 = SDL_RWFromConstMem(font_data.address, font_data.size);
        SDL_RWops* rw2 = SDL_RWFromConstMem(font_data.address, font_data.size);
        font_regular_ = TTF_OpenFontRW(rw1, 1, 20);
        font_large_   = TTF_OpenFontRW(rw2, 1, 26);
    }
#endif

    // 启动局域网 9978 远程控制与 Cookie 推送服务
    lan_server_.start(config_.lan_port, &config_, [this](const std::string& act, const std::string& k1, const std::string& v1) {
        (void)k1;
        if (act == "reload_source") {
            reloadSourceUrl(v1);
        } else if (act == "update_cookie") {
            js_engine_.updateCookies(config_);
            toast_msg_ = "已同步局域网推送的夸克/百度网盘 Cookie";
            toast_expire_ms_ = HttpClient::currentTimeMillis() + 4000;
        } else if (act == "search" && !v1.empty()) {
            current_tab_ = TopTab::SEARCH;
            if (!bundle_.sites.empty()) {
                current_vods_ = js_engine_.searchContent(bundle_.sites[selected_site_idx_], v1);
            }
        }
    });

    // 加载默认源: http://wexfnw:wexfnw@cat.xn--4kq62z5rby2qupq9ub.top/index.js.md5
    reloadSourceUrl(config_.source_url);
    return true;
}

void SwitchPlayerShell::reloadSourceUrl(const std::string& new_url) {
    if (!new_url.empty()) {
        config_.source_url = new_url;
        SourceLoader::saveAppConfig(config_);
    }
    bundle_ = SourceLoader::fetchAndParseSource(config_);
    js_engine_.init(config_, bundle_);
    selected_site_idx_ = 0;
    selected_cat_idx_ = 0;
    selected_vod_idx_ = 0;
    refreshCurrentSiteContent();
    toast_msg_ = bundle_.status_message;
    toast_expire_ms_ = HttpClient::currentTimeMillis() + 5000;
}

void SwitchPlayerShell::refreshCurrentSiteContent() {
    if (bundle_.sites.empty()) return;
    if (selected_site_idx_ >= static_cast<int>(bundle_.sites.size())) {
        selected_site_idx_ = 0;
    }
    const auto& site = bundle_.sites[selected_site_idx_];
    current_categories_ = js_engine_.homeCategories(site);
    std::string tid = current_categories_.empty() ? "1" : current_categories_[selected_cat_idx_ % current_categories_.size()].type_id;
    current_vods_ = js_engine_.categoryContent(site, tid, 1);
    if (selected_vod_idx_ >= static_cast<int>(current_vods_.size())) {
        selected_vod_idx_ = 0;
    }
}

void SwitchPlayerShell::promptEditSourceUrl() {
    std::string edited = openSwitchSoftwareKeyboard(
        "填写源地址 (支持 .js.md5 猫源 / OK影视源)",
        "默认已填王二小放牛娃猫源，可直接确认或修改",
        config_.source_url
    );
    if (!edited.empty()) {
        reloadSourceUrl(edited);
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
        current_vods_ = js_engine_.searchContent(bundle_.sites[selected_site_idx_], kw);
        selected_vod_idx_ = 0;
        toast_msg_ = "搜索「" + kw + "」共找到 " + std::to_string(current_vods_.size()) + " 条结果";
        toast_expire_ms_ = HttpClient::currentTimeMillis() + 4000;
    }
}

void SwitchPlayerShell::openQrLoginModal(NetdiskProvider provider) {
    if (provider == NetdiskProvider::QUARK) {
        active_qr_ = NetdiskAuth::startQuarkQRLogin();
    } else {
        active_qr_ = NetdiskAuth::startBaiduQRLogin();
    }
    last_qr_poll_ms_ = HttpClient::currentTimeMillis();
    modal_state_ = ModalState::QR_LOGIN;
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
        // 若因未登录夸克/百度网盘失败，自动弹出对应的扫码登录窗口
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

void SwitchPlayerShell::handleGamepadInput() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            running_ = false;
            return;
        }
    }

#ifdef __SWITCH__
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
    if (now_ms - last_stick_move_ms_ > 160) {
        if (l_stick.x > DEADZONE)  { kDown |= HidNpadButton_Right; last_stick_move_ms_ = now_ms; }
        if (l_stick.x < -DEADZONE) { kDown |= HidNpadButton_Left;  last_stick_move_ms_ = now_ms; }
        if (l_stick.y > DEADZONE)  { kDown |= HidNpadButton_Up;    last_stick_move_ms_ = now_ms; }
        if (l_stick.y < -DEADZONE) { kDown |= HidNpadButton_Down;  last_stick_move_ms_ = now_ms; }
    }

    // [+] 键安全退出
    if (kDown & HidNpadButton_Plus) {
        running_ = false;
        return;
    }

    // [-] 键随时呼出夸克/百度网盘扫码管理中心
    if (kDown & HidNpadButton_Minus) {
        if (modal_state_ == ModalState::QR_LOGIN) {
            // 在夸克与百度扫码之间一键切换
            openQrLoginModal(active_qr_.provider == NetdiskProvider::QUARK
                             ? NetdiskProvider::BAIDU : NetdiskProvider::QUARK);
        } else {
            openQrLoginModal(NetdiskProvider::QUARK);
        }
        return;
    }

    // 1. 全屏播放器模式下的摇杆与按键控制
    if (modal_state_ == ModalState::VIDEO_PLAYING) {
        if (kDown & HidNpadButton_B) {
            modal_state_ = ModalState::VOD_DETAIL;
            return;
        }
        if (kDown & HidNpadButton_A) {
            player_paused_ = !player_paused_;
        }
        if (kDown & HidNpadButton_X) {
            player_osd_visible_ = !player_osd_visible_;
        }
        if (kDown & HidNpadButton_Left) {
            player_pos_sec_ = std::max(0.0, player_pos_sec_ - 10.0);
        }
        if (kDown & HidNpadButton_Right) {
            player_pos_sec_ = std::min(player_dur_sec_, player_pos_sec_ + 10.0);
        }
        if (kDown & HidNpadButton_Up) {
            player_volume_ = std::min(100, player_volume_ + 5);
        }
        if (kDown & HidNpadButton_Down) {
            player_volume_ = std::max(0, player_volume_ - 5);
        }
        if (kDown & HidNpadButton_Y) {
            // 切换 4K原画 / 1080P云转码 清晰度线路
            if (!active_play_.qualities.empty()) {
                selected_quality_idx_ = (selected_quality_idx_ + 1) % static_cast<int>(active_play_.qualities.size());
                active_play_.url = active_play_.qualities[selected_quality_idx_].url;
                toast_msg_ = "已切换清晰度：" + active_play_.qualities[selected_quality_idx_].label;
                toast_expire_ms_ = now_ms + 3000;
            }
        }
        if (r_stick.x > DEADZONE && now_ms - last_stick_move_ms_ > 250) {
            player_speed_ = std::min(2.0f, player_speed_ + 0.25f);
            last_stick_move_ms_ = now_ms;
        } else if (r_stick.x < -DEADZONE && now_ms - last_stick_move_ms_ > 250) {
            player_speed_ = std::max(0.5f, player_speed_ - 0.25f);
            last_stick_move_ms_ = now_ms;
        }
        return;
    }

    // 2. 二维码扫码弹窗模式
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

    // 3. 影视详情与选集弹窗 (夸克4K / 百度网盘 / 直连线路切换)
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

    // 4. 主界面全局快捷键
    if (kDown & HidNpadButton_L) {
        int t = (static_cast<int>(current_tab_) - 1 + 6) % 6;
        current_tab_ = static_cast<TopTab>(t);
        if (current_tab_ == TopTab::QUARK_DRIVE) {
            for (size_t i = 0; i < bundle_.sites.size(); ++i) {
                if (bundle_.sites[i].key == "quark_my") { selected_site_idx_ = static_cast<int>(i); refreshCurrentSiteContent(); break; }
            }
        } else if (current_tab_ == TopTab::BAIDU_DRIVE) {
            for (size_t i = 0; i < bundle_.sites.size(); ++i) {
                if (bundle_.sites[i].key == "baidu_my") { selected_site_idx_ = static_cast<int>(i); refreshCurrentSiteContent(); break; }
            }
        }
    }
    if (kDown & HidNpadButton_R) {
        int t = (static_cast<int>(current_tab_) + 1) % 6;
        current_tab_ = static_cast<TopTab>(t);
        if (current_tab_ == TopTab::QUARK_DRIVE) {
            for (size_t i = 0; i < bundle_.sites.size(); ++i) {
                if (bundle_.sites[i].key == "quark_my") { selected_site_idx_ = static_cast<int>(i); refreshCurrentSiteContent(); break; }
            }
        } else if (current_tab_ == TopTab::BAIDU_DRIVE) {
            for (size_t i = 0; i < bundle_.sites.size(); ++i) {
                if (bundle_.sites[i].key == "baidu_my") { selected_site_idx_ = static_cast<int>(i); refreshCurrentSiteContent(); break; }
            }
        }
    }

    // [Y] 键直接弹出源地址填写键盘 (默认填好 http://wexfnw:wexfnw@cat.xn--4kq62z5rby2qupq9ub.top/index.js.md5)
    if (kDown & HidNpadButton_Y) {
        promptEditSourceUrl();
        return;
    }

    // [X] 键直接弹出搜索 / 网盘链接直解键盘
    if (kDown & HidNpadButton_X) {
        promptSearchKeyword();
        return;
    }

    // [ZL] / [ZR] 快速切换王二小 index.js.md5 中的不同站点 (玩偶哥哥 / 木偶 / 二小 / 立播 / 夸克 / 百度)
    if ((kDown & HidNpadButton_ZL) && !bundle_.sites.empty()) {
        selected_site_idx_ = (selected_site_idx_ - 1 + static_cast<int>(bundle_.sites.size())) % static_cast<int>(bundle_.sites.size());
        refreshCurrentSiteContent();
    }
    if ((kDown & HidNpadButton_ZR) && !bundle_.sites.empty()) {
        selected_site_idx_ = (selected_site_idx_ + 1) % static_cast<int>(bundle_.sites.size());
        refreshCurrentSiteContent();
    }

    // 设置页交互
    if (current_tab_ == TopTab::SETTINGS) {
        if (kDown & HidNpadButton_Up) settings_focus_idx_ = (settings_focus_idx_ - 1 + 4) % 4;
        if (kDown & HidNpadButton_Down) settings_focus_idx_ = (settings_focus_idx_ + 1) % 4;
        if (kDown & HidNpadButton_A) {
            if (settings_focus_idx_ == 0) promptEditSourceUrl();
            else if (settings_focus_idx_ == 1) reloadSourceUrl(DEFAULT_WEX_SOURCE_URL);
            else if (settings_focus_idx_ == 2) openQrLoginModal(NetdiskProvider::QUARK);
            else if (settings_focus_idx_ == 3) openQrLoginModal(NetdiskProvider::BAIDU);
        }
        return;
    }

    // 海报网格四向摇杆/十字键导航 (每行 5 列)
    const int COLS = 5;
    int total = static_cast<int>(current_vods_.size());
    if (total > 0) {
        if (kDown & HidNpadButton_Left)  selected_vod_idx_ = (selected_vod_idx_ - 1 + total) % total;
        if (kDown & HidNpadButton_Right) selected_vod_idx_ = (selected_vod_idx_ + 1) % total;
        if (kDown & HidNpadButton_Up)    selected_vod_idx_ = std::max(0, selected_vod_idx_ - COLS);
        if (kDown & HidNpadButton_Down)  selected_vod_idx_ = std::min(total - 1, selected_vod_idx_ + COLS);
        if (kDown & HidNpadButton_A) {
            const auto& v = current_vods_[selected_vod_idx_];
            if (v.vod_id == "quark_login_prompt") {
                openQrLoginModal(NetdiskProvider::QUARK);
            } else if (v.vod_id == "baidu_login_prompt") {
                openQrLoginModal(NetdiskProvider::BAIDU);
            } else if (!bundle_.sites.empty()) {
                active_vod_ = js_engine_.detailContent(bundle_.sites[selected_site_idx_], v.vod_id);
                selected_flag_idx_ = 0;
                selected_ep_idx_ = 0;
                modal_state_ = ModalState::VOD_DETAIL;
            }
        }
    }
#endif
}

void SwitchPlayerShell::updateFrame(uint64_t now_ms) {
    if (modal_state_ == ModalState::QR_LOGIN &&
        (active_qr_.state == QRLoginState::WAITING_SCAN || active_qr_.state == QRLoginState::SCANNED_WAIT_CONFIRM) &&
        now_ms - last_qr_poll_ms_ >= 2000) {
        last_qr_poll_ms_ = now_ms;
        bool ok = false;
        if (active_qr_.provider == NetdiskProvider::QUARK) {
            ok = NetdiskAuth::pollQuarkQRLogin(active_qr_, config_);
        } else {
            ok = NetdiskAuth::pollBaiduQRLogin(active_qr_, config_);
        }
        if (ok) {
            js_engine_.updateCookies(config_);
            refreshCurrentSiteContent();
            toast_msg_ = active_qr_.status_text;
            toast_expire_ms_ = now_ms + 4000;
        }
    }

    if (modal_state_ == ModalState::VIDEO_PLAYING && !player_paused_) {
        player_pos_sec_ += (0.016 * player_speed_);
        if (player_pos_sec_ >= player_dur_sec_) player_pos_sec_ = 0.0;
    }
}

void SwitchPlayerShell::drawRect(int x, int y, int w, int h, SDL_Color color, bool filled) {
    SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
    SDL_Rect r{x, y, w, h};
    if (filled) SDL_RenderFillRect(renderer_, &r);
    else SDL_RenderDrawRect(renderer_, &r);
}

void SwitchPlayerShell::drawText(const std::string& text, int x, int y, SDL_Color color, bool large) {
    if (text.empty()) return;
    TTF_Font* f = (large && font_large_) ? font_large_ : font_regular_;
    if (!f) return;
    SDL_Surface* surf = TTF_RenderUTF8_Blended(f, text.c_str(), color);
    if (!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer_, surf);
    if (tex) {
        SDL_Rect dst{x, y, surf->w, surf->h};
        SDL_RenderCopy(renderer_, tex, nullptr, &dst);
        SDL_DestroyTexture(tex);
    }
    SDL_FreeSurface(surf);
}

void SwitchPlayerShell::renderTopBar() {
    drawRect(0, 0, 1280, 64, {20, 26, 38, 255});
    drawText("OK影视 Switch", 24, 18, {88, 166, 255, 255}, true);

    static const char* TAB_NAMES[6] = {
        "首页点播", "分类筛选", "聚合搜索", "夸克网盘", "百度网盘", "接口与设置"
    };
    for (int i = 0; i < 6; ++i) {
        int tx = 230 + i * 118;
        bool active = (static_cast<int>(current_tab_) == i);
        if (active) {
            drawRect(tx - 8, 12, 108, 40, {35, 134, 54, 255});
        }
        drawText(TAB_NAMES[i], tx, 20, active ? SDL_Color{255, 255, 255, 255} : SDL_Color{160, 175, 195, 255});
    }

    // 右上角显示源地址快捷入口与网盘登录状态徽章
    std::string q_badge = config_.quark_cookie.empty() ? "夸克:未登" : ("夸克:" + config_.quark_nickname);
    std::string b_badge = config_.baidu_cookie.empty() ? "百度:未登" : ("百度:" + config_.baidu_username);
    drawRect(950, 14, 145, 36, config_.quark_cookie.empty() ? SDL_Color{60, 45, 45, 255} : SDL_Color{25, 75, 55, 255});
    drawText(q_badge, 960, 22, {230, 240, 250, 255});
    drawRect(1105, 14, 150, 36, config_.baidu_cookie.empty() ? SDL_Color{60, 45, 45, 255} : SDL_Color{25, 65, 95, 255});
    drawText(b_badge, 1115, 22, {230, 240, 250, 255});
}

void SwitchPlayerShell::renderSiteAndCategorySidebar() {
    drawRect(0, 64, 250, 596, {16, 20, 30, 255});
    drawText("当前源站 (ZL/ZR切换):", 16, 78, {130, 150, 180, 255});

    int max_sites = std::min(static_cast<int>(bundle_.sites.size()), 12);
    for (int i = 0; i < max_sites; ++i) {
        int sy = 112 + i * 38;
        bool sel = (i == selected_site_idx_);
        if (sel) {
            drawRect(10, sy - 4, 230, 32, {40, 90, 165, 255});
        }
        drawText(bundle_.sites[i].name, 18, sy, sel ? SDL_Color{255, 255, 255, 255} : SDL_Color{185, 195, 210, 255});
    }
}

void SwitchPlayerShell::renderVodGrid() {
    // 顶部展示当前正在生效的源地址栏 (按 Y 可直接修改)
    drawRect(266, 76, 994, 42, {24, 32, 48, 255});
    drawRect(266, 76, 994, 42, {56, 139, 253, 180}, false);
    drawText("[Y键改源] 当前源: " + config_.source_url, 278, 86, {165, 214, 255, 255});

    const int COLS = 5;
    int start_x = 266;
    int start_y = 132;
    int card_w = 190;
    int card_h = 235;
    int gap_x = 11;
    int gap_y = 16;

    int count = std::min(static_cast<int>(current_vods_.size()), 10);
    for (int i = 0; i < count; ++i) {
        int row = i / COLS;
        int col = i % COLS;
        int cx = start_x + col * (card_w + gap_x);
        int cy = start_y + row * (card_h + gap_y);
        bool focused = (i == selected_vod_idx_);

        drawRect(cx, cy, card_w, card_h, focused ? SDL_Color{38, 56, 88, 255} : SDL_Color{24, 30, 44, 255});
        if (focused) {
            drawRect(cx - 2, cy - 2, card_w + 4, card_h + 4, {88, 166, 255, 255}, false);
            drawRect(cx - 3, cy - 3, card_w + 6, card_h + 6, {88, 166, 255, 255}, false);
        }

        // 封面色块与清晰度角标
        drawRect(cx + 8, cy + 8, card_w - 16, 145, {32, 42, 62, 255});
        drawRect(cx + 14, cy + 14, 118, 26, {35, 134, 54, 230});
        drawText(current_vods_[i].vod_remarks, cx + 20, cy + 17, {255, 255, 255, 255});

        // 标题与年份
        std::string title = current_vods_[i].vod_name;
        if (title.size() > 24) title = title.substr(0, 24) + "..";
        drawText(title, cx + 10, cy + 164, {240, 246, 252, 255});
        drawText(current_vods_[i].vod_year + " · " + current_vods_[i].site_name, cx + 10, cy + 196, {140, 155, 175, 255});
    }
}

void SwitchPlayerShell::renderSettingsPanel() {
    drawRect(266, 78, 994, 560, {20, 26, 38, 255});
    drawText("接口源地址与网盘账号配置中心 (适配摇杆按键 + 局域网同步)", 290, 96, {88, 166, 255, 255}, true);

    struct SettingRow {
        std::string title;
        std::string value;
        std::string hint;
    };
    SettingRow rows[4] = {
        {
            "1. 直接填写/修改接口源地址 (按 A 呼出 Switch 键盘)",
            config_.source_url,
            "已原生适配 http://wexfnw:wexfnw@cat.xn--4kq62z5rby2qupq9ub.top/index.js.md5 (含 Basic Auth 与 Punycode)"
        },
        {
            "2. 一键恢复默认王二小放牛娃 .js.md5 猫源并重新校验 MD5",
            DEFAULT_WEX_SOURCE_URL,
            bundle_.status_message
        },
        {
            "3. 夸克网盘 (pan.quark.cn) 扫码登录 / 刷新凭证",
            config_.quark_cookie.empty() ? "未登录 (按 A 弹出夸克扫码二维码)" : ("已登录：" + config_.quark_nickname),
            "支持夸克 4K 原画直链 (绕过 23018 限制) 与云端 HLS 转码流"
        },
        {
            "4. 百度网盘 (pan.baidu.com) 扫码登录 / 刷新凭证",
            config_.baidu_cookie.empty() ? "未登录 (按 A 弹出百度网盘扫码二维码)" : ("已登录：" + config_.baidu_username),
            "支持百度网盘 BDUSS/STOKEN 自动交换与 M3U8_AUTO_1080 云流直连"
        }
    };

    for (int i = 0; i < 4; ++i) {
        int ry = 152 + i * 108;
        bool sel = (settings_focus_idx_ == i);
        drawRect(290, ry, 946, 94, sel ? SDL_Color{36, 54, 86, 255} : SDL_Color{26, 34, 50, 255});
        if (sel) drawRect(290, ry, 946, 94, {88, 166, 255, 255}, false);
        drawText(rows[i].title, 310, ry + 10, {255, 255, 255, 255});
        drawText(rows[i].value, 310, ry + 38, {121, 192, 255, 255});
        drawText(rows[i].hint,  310, ry + 64, {150, 165, 185, 255});
    }

    drawText("手机/电脑浏览器局域网快捷管理地址: " + lan_server_.getLanUrl(), 290, 594, {63, 185, 80, 255});
}

void SwitchPlayerShell::renderDetailModal() {
    drawRect(120, 80, 1040, 540, {18, 24, 36, 248});
    drawRect(120, 80, 1040, 540, {88, 166, 255, 255}, false);

    drawText(active_vod_.vod_name, 150, 104, {255, 255, 255, 255}, true);
    drawText(active_vod_.vod_year + " | " + active_vod_.vod_area + " | " + active_vod_.vod_remarks,
             150, 144, {121, 192, 255, 255});
    drawText(active_vod_.vod_content, 150, 180, {190, 205, 225, 255});

    drawText("播放线路 (按 L 或 Y 切换夸克网盘 / 百度网盘 / 直连路线):", 150, 236, {255, 215, 100, 255});
    int fx = 150;
    int idx = 0;
    std::string active_flag_name;
    std::vector<std::pair<std::string, std::string>> active_eps;

    for (const auto& kv : active_vod_.play_flags) {
        bool sel = (idx == (selected_flag_idx_ % std::max(1, static_cast<int>(active_vod_.play_flags.size()))));
        if (sel) {
            active_flag_name = kv.first;
            active_eps = kv.second;
        }
        drawRect(fx, 270, 220, 42, sel ? SDL_Color{35, 134, 54, 255} : SDL_Color{32, 42, 62, 255});
        drawText(kv.first, fx + 14, 280, {255, 255, 255, 255});
        fx += 236;
        ++idx;
    }

    drawText("剧集选集 (左摇杆/十字键左右选择，按 A 立即播放):", 150, 336, {200, 215, 235, 255});
    for (size_t i = 0; i < active_eps.size() && i < 8; ++i) {
        int ex = 150 + static_cast<int>(i % 4) * 240;
        int ey = 374 + static_cast<int>(i / 4) * 60;
        bool ep_sel = (static_cast<int>(i) == selected_ep_idx_);
        drawRect(ex, ey, 224, 46, ep_sel ? SDL_Color{40, 100, 195, 255} : SDL_Color{30, 38, 56, 255});
        if (ep_sel) drawRect(ex, ey, 224, 46, {121, 192, 255, 255}, false);
        drawText(active_eps[i].first, ex + 12, ey + 12, {255, 255, 255, 255});
    }

    drawText("[A] 立即播放当前选集    [L/Y] 切换夸克/百度线路    [-] 网盘扫码授权    [B] 返回列表",
             150, 566, {150, 175, 205, 255});
}

void SwitchPlayerShell::renderQrLoginModal() {
    drawRect(220, 70, 840, 560, {16, 22, 34, 250});
    drawRect(220, 70, 840, 560, {88, 166, 255, 255}, false);

    std::string title = (active_qr_.provider == NetdiskProvider::QUARK)
        ? "夸克网盘 (pan.quark.cn) 手机扫码登录"
        : "百度网盘 (pan.baidu.com) 手机扫码登录";
    drawText(title, 260, 96, {88, 166, 255, 255}, true);
    drawText(active_qr_.status_text, 260, 138, {235, 245, 255, 255});

    // 绘制二维码白色衬底与模块矩阵
    int qr_box = 290;
    int qx = 495, qy = 185;
    drawRect(qx - 16, qy - 16, qr_box + 32, qr_box + 32, {255, 255, 255, 255});
    if (active_qr_.qr_size > 0) {
        int cell = qr_box / active_qr_.qr_size;
        for (int y = 0; y < active_qr_.qr_size; ++y) {
            for (int x = 0; x < active_qr_.qr_size; ++x) {
                if (active_qr_.qr_modules[y * active_qr_.qr_size + x]) {
                    drawRect(qx + x * cell, qy + y * cell, cell, cell, {10, 14, 20, 255});
                }
            }
        }
    }

    drawText("方式二：手机/电脑浏览器打开 " + lan_server_.getLanUrl() + " 可直接粘贴 Cookie 同步",
             260, 515, {63, 185, 80, 255});
    drawText("[A] 刷新二维码    [L/R/Y] 切换夸克/百度网盘    [B] 关闭窗口",
             260, 565, {170, 185, 205, 255});
}

void SwitchPlayerShell::renderFullscreenPlayer() {
    drawRect(0, 0, 1280, 720, {8, 10, 15, 255});
    drawText("正在播放: " + active_vod_.vod_name, 48, 280, {220, 235, 255, 255}, true);
    drawText("视频流地址: " + active_play_.url, 48, 330, {120, 175, 235, 255});

    if (player_osd_visible_) {
        drawRect(0, 560, 1280, 160, {14, 18, 28, 230});
        // 进度条
        drawRect(48, 590, 1184, 12, {45, 55, 75, 255});
        int fill_w = static_cast<int>((player_pos_sec_ / std::max(1.0, player_dur_sec_)) * 1184.0);
        drawRect(48, 590, std::clamp(fill_w, 0, 1184), 12, {56, 139, 253, 255});

        std::ostringstream oss;
        oss << (player_paused_ ? "[已暂停] " : "[播放中] ")
            << int(player_pos_sec_) / 60 << ":" << std::setw(2) << std::setfill('0') << int(player_pos_sec_) % 60
            << " / " << int(player_dur_sec_) / 60 << ":00"
            << "   |   音量: " << player_volume_ << "%"
            << "   |   倍速: " << std::fixed << std::setprecision(2) << player_speed_ << "x";
        if (!active_play_.qualities.empty()) {
            oss << "   |   当前清晰度: " << active_play_.qualities[selected_quality_idx_].label;
        }
        drawText(oss.str(), 48, 620, {245, 250, 255, 255});
        drawText("[A] 暂停/继续   [左摇杆左右] 快退/快进10秒   [左摇杆上下] 音量   [右摇杆左右] 倍速   [Y] 切换4K/转码清晰度   [B] 退出播放",
                 48, 666, {160, 185, 215, 255});
    }
}

void SwitchPlayerShell::renderBottomGamepadLegend() {
    drawRect(0, 660, 1280, 60, {14, 18, 26, 255});
    drawText("[左摇杆/十字键] 移动焦点   [A] 确认/选集   [Y] 直接填写源地址   [X] 搜索/解析网盘链   [L/R] 切换标签   [ZL/ZR] 切换站点   [-] 夸克/百度扫码   [+] 退出",
             24, 680, {175, 195, 220, 255});

    if (HttpClient::currentTimeMillis() < toast_expire_ms_ && !toast_msg_.empty()) {
        drawRect(320, 608, 640, 42, {35, 134, 54, 240});
        drawText(toast_msg_, 340, 618, {255, 255, 255, 255});
    }
}

void SwitchPlayerShell::renderFrame() {
    SDL_SetRenderDrawColor(renderer_, 12, 16, 24, 255);
    SDL_RenderClear(renderer_);

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
    lan_server_.stop();
    if (font_regular_) TTF_CloseFont(font_regular_);
    if (font_large_) TTF_CloseFont(font_large_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    TTF_Quit();
    SDL_Quit();
    HttpClient::globalCleanup();
}

} // namespace okplayer
