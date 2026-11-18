#include "ui_shell.hpp"
#include "http_client.hpp"
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cmath>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace okplayer {

namespace {

// UTF-8 安全截断：绝不把多字节中文字符从中间截断（消除 □ 乱码框）
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

// 为每部影片生成稳定的 SwitchTVBox 风格渐变封面底色
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
        font_regular_ = TTF_OpenFontRW(rw1, 1, 19);
        font_large_   = TTF_OpenFontRW(rw2, 1, 25);
    }
#endif

    // 先立刻渲染第一帧主界面，彻底消除启动黑屏 1~2 秒的问题
    toast_msg_ = "正在后台加载影视源，请稍候...";
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
            std::lock_guard<std::mutex> lk(data_mutex_);
            current_tab_ = TopTab::SEARCH;
            if (!bundle_.sites.empty()) {
                current_vods_ = js_engine_.searchContent(bundle_.sites[selected_site_idx_], v1);
            }
        }
    });

    // 后台线程异步加载默认源: http://wexfnw:wexfnw@cat.xn--4kq62z5rby2qupq9ub.top/index.js.md5
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
    std::thread([this, cfg_copy]() mutable {
        LoadedSourceBundle fetched = SourceLoader::fetchAndParseSource(cfg_copy);
        {
            std::lock_guard<std::mutex> lk(data_mutex_);
            pending_bundle_ = std::move(fetched);
            pending_source_apply_ = true;
        }
        source_loading_.store(false);
    }).detach();
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
        toast_msg_ = "正在加载新源地址...";
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
    if (!kw.empty()) {
        std::lock_guard<std::mutex> lk(data_mutex_);
        if (!bundle_.sites.empty()) {
            current_tab_ = TopTab::SEARCH;
            current_vods_ = js_engine_.searchContent(bundle_.sites[selected_site_idx_], kw);
            selected_vod_idx_ = 0;
            toast_msg_ = "搜索「" + kw + "」共找到 " + std::to_string(current_vods_.size()) + " 条结果";
            toast_expire_ms_ = HttpClient::currentTimeMillis() + 4000;
        }
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
            : "正在向百度服务器申请登录二维码...";
    }
    last_qr_poll_ms_ = HttpClient::currentTimeMillis();
    modal_state_ = ModalState::QR_LOGIN;
    qr_loading_.store(true);

    std::thread([this, provider]() {
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
    }).detach();
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
            modal_state_ = ModalState::VOD_DETAIL;
            return;
        }
        if (kDown & HidNpadButton_A) player_paused_ = !player_paused_;
        if (kDown & HidNpadButton_X) player_osd_visible_ = !player_osd_visible_;
        if (kDown & HidNpadButton_Left)  player_pos_sec_ = std::max(0.0, player_pos_sec_ - 10.0);
        if (kDown & HidNpadButton_Right) player_pos_sec_ = std::min(player_dur_sec_, player_pos_sec_ + 10.0);
        if (kDown & HidNpadButton_Up)    player_volume_ = std::min(100, player_volume_ + 5);
        if (kDown & HidNpadButton_Down)  player_volume_ = std::max(0, player_volume_ - 5);
        if (kDown & HidNpadButton_Y) {
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
        refreshCurrentSiteContent();
    }
    if ((kDown & HidNpadButton_ZR) && !bundle_.sites.empty()) {
        selected_site_idx_ = (selected_site_idx_ + 1) % static_cast<int>(bundle_.sites.size());
        refreshCurrentSiteContent();
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
    // 1. 接收后台线程拉取完成的影视源
    {
        std::lock_guard<std::mutex> lk(data_mutex_);
        if (pending_source_apply_) {
            bundle_ = std::move(pending_bundle_);
            pending_source_apply_ = false;
            js_engine_.init(config_, bundle_);
            selected_site_idx_ = 0;
            selected_cat_idx_ = 0;
            selected_vod_idx_ = 0;
            refreshCurrentSiteContent();
            toast_msg_ = bundle_.status_message;
            toast_expire_ms_ = now_ms + 4500;
        }
    }

    // 2. 非阻塞后台线程轮询夸克/百度扫码状态（彻底消除界面卡死）
    if (modal_state_ == ModalState::QR_LOGIN &&
        !qr_loading_.load() &&
        !qr_polling_.load() &&
        (active_qr_.state == QRLoginState::WAITING_SCAN || active_qr_.state == QRLoginState::SCANNED_WAIT_CONFIRM) &&
        now_ms - last_qr_poll_ms_ >= 2000) {
        last_qr_poll_ms_ = now_ms;
        qr_polling_.store(true);
        QRLoginSession session_copy = active_qr_;
        AppConfig cfg_copy = config_;
        std::thread([this, session_copy, cfg_copy]() mutable {
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
                        refreshCurrentSiteContent();
                        toast_msg_ = active_qr_.status_text;
                        toast_expire_ms_ = HttpClient::currentTimeMillis() + 4000;
                    }
                }
            }
            qr_polling_.store(false);
        }).detach();
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

void SwitchPlayerShell::renderTopBar() {
    drawRect(0, 0, 1280, 66, {15, 20, 30, 255});
    drawRect(0, 65, 1280, 1, {42, 54, 78, 255});

    // 品牌 Logo 胶囊
    drawRoundedRect(18, 13, 188, 40, 10, {24, 38, 64, 255});
    drawText("OK影视 Switch", 30, 20, {96, 178, 255, 255}, true);

    static const char* TAB_NAMES[6] = {
        "首页点播", "分类筛选", "聚合搜索", "夸克网盘", "百度网盘", "源与设置"
    };
    for (int i = 0; i < 6; ++i) {
        int tx = 224 + i * 116;
        bool active = (static_cast<int>(current_tab_) == i);
        if (active) {
            drawRoundedRect(tx - 10, 14, 106, 38, 12, {38, 148, 88, 255});
        }
        drawText(TAB_NAMES[i], tx, 22, active ? SDL_Color{255, 255, 255, 255} : SDL_Color{158, 174, 198, 255});
    }

    // 右上角网盘登录状态胶囊
    std::string q_badge = config_.quark_cookie.empty() ? "夸克:未登" : ("夸克:" + utf8Truncate(config_.quark_nickname, 5));
    std::string b_badge = config_.baidu_cookie.empty() ? "百度:未登" : ("百度:" + utf8Truncate(config_.baidu_username, 5));
    drawRoundedRect(934, 15, 156, 36, 10, config_.quark_cookie.empty() ? SDL_Color{56, 36, 44, 255} : SDL_Color{26, 86, 62, 255});
    drawText(q_badge, 948, 23, {235, 245, 255, 255});
    drawRoundedRect(1100, 15, 160, 36, 10, config_.baidu_cookie.empty() ? SDL_Color{56, 36, 44, 255} : SDL_Color{28, 72, 114, 255});
    drawText(b_badge, 1114, 23, {235, 245, 255, 255});
}

void SwitchPlayerShell::renderSiteAndCategorySidebar() {
    drawRect(0, 66, 236, 594, {13, 17, 26, 255});
    drawRect(235, 66, 1, 594, {34, 44, 64, 255});
    drawText("源站列表 (ZL/ZR)", 18, 82, {128, 148, 180, 255});

    int max_sites = std::min(static_cast<int>(bundle_.sites.size()), 12);
    for (int i = 0; i < max_sites; ++i) {
        int sy = 116 + i * 42;
        bool sel = (i == selected_site_idx_);
        if (sel) {
            drawRoundedRect(12, sy - 5, 212, 34, 8, {36, 84, 156, 255});
            drawRoundedRect(12, sy - 1, 4, 26, 2, {96, 198, 255, 255});
        }
        drawText(utf8Truncate(bundle_.sites[i].name, 13), 24, sy + 2,
                 sel ? SDL_Color{255, 255, 255, 255} : SDL_Color{176, 190, 210, 255});
    }
}

void SwitchPlayerShell::renderVodGrid() {
    // 顶部紧凑源状态胶囊（按 Y 随时改源）
    drawRoundedRect(256, 76, 1004, 38, 10, {20, 27, 42, 255});
    std::string src_label = source_loading_.load()
        ? "正在后台同步源地址: " + utf8Truncate(config_.source_url, 64)
        : "[Y键改源] 当前源: " + utf8Truncate(config_.source_url, 68);
    drawText(src_label, 272, 85, source_loading_.load() ? SDL_Color{255, 210, 96, 255} : SDL_Color{156, 208, 255, 255});

    const int COLS = 5;
    int start_x = 256;
    int start_y = 126;
    int card_w = 190;
    int card_h = 252;
    int gap_x = 13;
    int gap_y = 14;

    int count = std::min(static_cast<int>(current_vods_.size()), 10);
    if (count == 0 && source_loading_.load()) {
        drawTextCentered("正在加载王二小放牛娃 .js.md5 猫源，请稍候...", 758, 350, {160, 195, 240, 255}, true);
        return;
    }

    for (int i = 0; i < count; ++i) {
        int row = i / COLS;
        int col = i % COLS;
        int cx = start_x + col * (card_w + gap_x);
        int cy = start_y + row * (card_h + gap_y);
        bool focused = (i == selected_vod_idx_);

        // 卡片底板
        drawRoundedRect(cx, cy, card_w, card_h, 12,
                        focused ? SDL_Color{34, 52, 82, 255} : SDL_Color{22, 28, 42, 255});
        if (focused) {
            drawRoundedOutline(cx, cy, card_w, card_h, 12, 3, {88, 178, 255, 255});
        }

        // 海报封面区（多彩渐变艺术底 + 居中片名大字）
        SDL_Color art = cardAccentColor(current_vods_[i].vod_name);
        drawRoundedRect(cx + 8, cy + 8, card_w - 16, 166, 9, art);
        drawRect(cx + 8, cy + 138, card_w - 16, 36, {14, 18, 28, 120});

        // 清晰度角标胶囊
        if (!current_vods_[i].vod_remarks.empty()) {
            drawRoundedRect(cx + 14, cy + 14, 126, 24, 7, {36, 146, 78, 240});
            drawText(utf8Truncate(current_vods_[i].vod_remarks, 12), cx + 22, cy + 17, {255, 255, 255, 255});
        }

        // 封面中心片名水印
        drawTextCentered(utf8Truncate(current_vods_[i].vod_name, 8), cx + card_w / 2, cy + 84, {240, 248, 255, 235});

        // 底部剧名与年份/来源（UTF-8 安全截断，无乱码）
        drawText(utf8Truncate(current_vods_[i].vod_name, 12), cx + 10, cy + 184, {245, 250, 255, 255});
        std::string sub = current_vods_[i].vod_year + " · " + current_vods_[i].site_name;
        drawText(utf8Truncate(sub, 15), cx + 10, cy + 214, {146, 164, 190, 255});
    }
}

void SwitchPlayerShell::renderSettingsPanel() {
    drawRoundedRect(256, 78, 1004, 566, 14, {18, 24, 36, 255});
    drawText("接口源地址与网盘账号配置中心 (适配摇杆按键 + 局域网同步)", 284, 96, {96, 178, 255, 255}, true);

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
        int ry = 148 + i * 110;
        bool sel = (settings_focus_idx_ == i);
        drawRoundedRect(284, ry, 948, 96, 10, sel ? SDL_Color{34, 54, 88, 255} : SDL_Color{24, 32, 48, 255});
        if (sel) drawRoundedOutline(284, ry, 948, 96, 10, 2, {88, 178, 255, 255});
        drawText(rows[i].title, 304, ry + 10, {255, 255, 255, 255});
        drawText(utf8Truncate(rows[i].value, 76), 304, ry + 38, {121, 192, 255, 255});
        drawText(utf8Truncate(rows[i].hint, 78), 304, ry + 64, {150, 165, 185, 255});
    }

    drawText("手机/电脑浏览器局域网快捷管理地址: " + lan_server_.getLanUrl(), 284, 598, {63, 185, 80, 255});
}

void SwitchPlayerShell::renderDetailModal() {
    drawRect(0, 0, 1280, 720, {6, 10, 18, 180});
    drawRoundedRect(110, 72, 1060, 560, 16, {18, 24, 38, 255});
    drawRoundedOutline(110, 72, 1060, 560, 16, 2, {88, 178, 255, 255});

    drawText(active_vod_.vod_name, 144, 98, {255, 255, 255, 255}, true);
    drawText(active_vod_.vod_year + " | " + active_vod_.vod_area + " | " + active_vod_.vod_remarks,
             144, 138, {121, 192, 255, 255});
    drawText(utf8Truncate(active_vod_.vod_content, 62), 144, 174, {190, 205, 225, 255});

    drawText("播放线路 (按 L 或 Y 切换夸克网盘 / 百度网盘 / 直连路线):", 144, 230, {255, 215, 100, 255});
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
        drawRoundedRect(fx, 264, 224, 42, 10, sel ? SDL_Color{38, 148, 88, 255} : SDL_Color{30, 40, 60, 255});
        drawText(kv.first, fx + 14, 274, {255, 255, 255, 255});
        fx += 240;
        ++idx;
    }

    drawText("剧集选集 (左摇杆/十字键左右选择，按 A 立即播放):", 144, 330, {200, 215, 235, 255});
    for (size_t i = 0; i < active_eps.size() && i < 8; ++i) {
        int ex = 144 + static_cast<int>(i % 4) * 244;
        int ey = 370 + static_cast<int>(i / 4) * 62;
        bool ep_sel = (static_cast<int>(i) == selected_ep_idx_);
        drawRoundedRect(ex, ey, 228, 48, 10, ep_sel ? SDL_Color{40, 100, 195, 255} : SDL_Color{28, 36, 54, 255});
        if (ep_sel) drawRoundedOutline(ex, ey, 228, 48, 10, 2, {121, 192, 255, 255});
        drawText(utf8Truncate(active_eps[i].first, 14), ex + 14, ey + 13, {255, 255, 255, 255});
    }

    drawText("[A] 立即播放当前选集    [L/Y] 切换夸克/百度线路    [-] 网盘扫码授权    [B] 返回列表",
             144, 578, {150, 175, 205, 255});
}

void SwitchPlayerShell::renderQrLoginModal() {
    drawRect(0, 0, 1280, 720, {6, 10, 18, 190});
    drawRoundedRect(210, 62, 860, 580, 16, {16, 22, 34, 255});
    drawRoundedOutline(210, 62, 860, 580, 16, 2, {88, 178, 255, 255});

    std::string title = (active_qr_.provider == NetdiskProvider::QUARK)
        ? "夸克网盘 (pan.quark.cn) 手机扫码登录"
        : "百度网盘 (pan.baidu.com) 手机扫码登录";
    drawTextCentered(title, 640, 88, {96, 178, 255, 255}, true);
    drawTextCentered(utf8Truncate(active_qr_.status_text, 48), 640, 130, {235, 245, 255, 255});

    // 标准 4 模块白边 (Quiet Zone) + 整像素对齐，保证手机 1 秒识别扫码
    const int box_size = 320;
    const int box_x = (1280 - box_size) / 2;
    const int box_y = 174;
    drawRoundedRect(box_x, box_y, box_size, box_size, 12, {255, 255, 255, 255});

    if (active_qr_.qr_size > 0 && !active_qr_.qr_modules.empty()) {
        int total_mods = active_qr_.qr_size + 8; // 两侧各 4 模块 Quiet Zone
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
        drawTextCentered(qr_loading_.load() ? "正在生成真实二维码..." : "二维码未就绪，请按 [A] 重试",
                         640, 320, {40, 48, 60, 255});
    }

    drawTextCentered("方式二：手机/电脑浏览器打开 " + lan_server_.getLanUrl() + " 可直接粘贴 Cookie 同步",
                     640, 520, {63, 185, 80, 255});
    drawTextCentered("[A] 刷新二维码    [L/R/Y] 切换夸克/百度网盘    [B] 关闭窗口",
                     640, 574, {170, 185, 205, 255});
}

void SwitchPlayerShell::renderFullscreenPlayer() {
    drawRect(0, 0, 1280, 720, {8, 10, 15, 255});
    drawText("正在播放: " + active_vod_.vod_name, 48, 280, {220, 235, 255, 255}, true);
    drawText("视频流地址: " + utf8Truncate(active_play_.url, 80), 48, 330, {120, 175, 235, 255});

    if (player_osd_visible_) {
        drawRect(0, 560, 1280, 160, {14, 18, 28, 230});
        drawRoundedRect(48, 590, 1184, 12, 6, {45, 55, 75, 255});
        int fill_w = static_cast<int>((player_pos_sec_ / std::max(1.0, player_dur_sec_)) * 1184.0);
        drawRoundedRect(48, 590, std::clamp(fill_w, 0, 1184), 12, 6, {56, 139, 253, 255});

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
    drawRect(0, 660, 1280, 60, {14, 18, 28, 255});
    drawRect(0, 660, 1280, 1, {36, 46, 68, 255});
    drawText("[左摇杆/十字键] 移动   [A] 确认/选集   [Y] 填写源地址   [X] 搜索/网盘链   [L/R] 标签   [ZL/ZR] 站点   [-] 夸克/百度扫码   [+] 退出",
             24, 680, {175, 195, 220, 255});

    if (HttpClient::currentTimeMillis() < toast_expire_ms_ && !toast_msg_.empty()) {
        drawRoundedRect(300, 604, 680, 42, 10, {32, 134, 78, 240});
        drawTextCentered(utf8Truncate(toast_msg_, 50), 640, 615, {255, 255, 255, 255});
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
    clearTextCache();
    if (font_regular_) TTF_CloseFont(font_regular_);
    if (font_large_) TTF_CloseFont(font_large_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    TTF_Quit();
    SDL_Quit();
    HttpClient::globalCleanup();
}

} // namespace okplayer
