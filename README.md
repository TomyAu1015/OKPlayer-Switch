# OK影视-Switch (`OKPlayer-Switch`) — Nintendo Switch 摇杆按键适配版 (NSP + NRO)

基于参考包 **《安卓5专版-OK影视-64位-3.7.2.apk》 (`com.fongmi.android.tv`)** 架构移植的 Nintendo Switch 原生自制播放器壳（适配 Atmosphère `1.12.2+` / 固件 `22.5.0+` / `Sphaira`）。

## 1. 核心特性

1. **源地址直接填写 + 原生适配 `.js.md5` 猫源协议**：
   - 默认内置并自动加载：`http://wexfnw:wexfnw@cat.xn--4kq62z5rby2qupq9ub.top/index.js.md5`（王二小放牛娃猫源）。
   - 原生支持 URL 内嵌 **HTTP Basic Auth (`wexfnw:wexfnw`)** 与 **Punycode 国际域名 (`cat.xn--4kq62z5rby2qupq9ub.top`)** 解析。
   - 自动拉取同级目录下的 `index.js.md5`（校验 32 位 MD5）、`index.config.js` 与 `index.js` 并缓存至 `sdmc:/switch/OKPlayer-Switch/cache/`，同时兼容标准 OK影视 JSON、`**` Base64 与 `2423...2324` AES-CBC 加密接口。
   - 在任意界面按手柄 **`[Y]` 键** 即可呼出 Switch 原生软键盘 (`swkbd`) 直接填写或更换源地址；也可通过手机浏览器访问局域网 `http://<Switch-IP>:9978` 一键推送源地址。

2. **夸克网盘 (`pan.quark.cn`) + 百度网盘 (`pan.baidu.com`) 扫码登录与直连播放**：
   - **夸克网盘**：
     - 按 **`[-]` 键** 呼出屏幕二维码，使用手机【夸克APP】扫码登录（调用 `uop.quark.cn/cas/ajax/getTokenForQrcodeLogin` 与 `getServiceTicketByQrcodeToken`），自动持久化 `__puus` / `__pus` Cookie 至 `sdmc:/switch/OKPlayer-Switch/cookies/quark_cookie.txt`。
     - 支持浏览个人夸克网盘文件，以及自动转存并解析猫源中的夸克分享链接（提供 **4K/原画直链** `quark-cloud-drive/2.5.56` 绕过 `23018` 限制 + **云端 HLS 多码率转码流**）。
   - **百度网盘**：
     - 在扫码弹窗按 **`[L/R]` 键** 切换至百度网盘二维码，使用手机【百度网盘APP】扫码登录（调用 `passport.baidu.com/v2/api/getqrcode` -> `channel/unicast` -> `qrbdusslogin`），自动提取 `BDUSS` 与 `netdisk#STOKEN` 并持久化至 `sdmc:/switch/OKPlayer-Switch/cookies/baidu_cookie.txt`。
     - 支持浏览个人百度网盘文件，以及自动解析百度分享链接（提供 **`M3U8_AUTO_1080` / `720` 云转码实时流** 与 **PCS 原画直链**）。

3. **Switch 摇杆与按键完整映射**：

| 按键 / 摇杆 | 主界面 / 选集弹窗功能 | 全屏视频播放功能 |
| :--- | :--- | :--- |
| **左摇杆 / 十字键** | 四向移动焦点（海报网格 / 选集按钮 / 设置项） | 左右快退/快进 `10秒`，上下调节音量 `±5%` |
| **右摇杆** | 上下快速翻页 | 左右调节播放倍速 (`0.5x ~ 2.0x`) |
| **`A` 键** | 确认 / 进入剧集详情 / 立即播放选集 | 暂停 / 继续播放 |
| **`B` 键** | 返回上一级 / 关闭弹窗 | 退出全屏播放返回选集页 |
| **`Y` 键** | **直接填写/修改源地址**（呼出 Switch 软键盘，默认填好王二小 `.js.md5` 源）；详情页切换播放线路 | 切换清晰度路线（`夸克4K原画` / `云转码` / `百度1080P`） |
| **`X` 键** | **搜索片名 / 粘贴夸克或百度分享链接直解** | 显示 / 隐藏底部播放进度条 (OSD) |
| **`L` / `R` 键** | 切换顶部导航栏（`首页点播` / `分类` / `搜索` / `夸克网盘` / `百度网盘` / `设置`） | 详情页切换播放线路 |
| **`ZL` / `ZR` 键** | 快速切换 `.js.md5` 猫源中的不同站点（玩偶哥哥 / 木偶 / 二小 / 立播等） | 切换上一集 / 下一集 |
| **`-` (Minus) 键** | 随时打开 **夸克网盘 / 百度网盘 扫码登录弹窗** | — |
| **`+` (Plus) 键** | 安全退出返回 Switch 桌面 / Sphaira | 安全退出 |

## 2. 安装与编译说明

### 方式 A：GitHub Actions 一键云编译 (推荐)
1. 解压交付的全量包，将解压后的全部文件**直接拖拽上传至 GitHub 仓库根目录**。
2. GitHub Actions 将自动运行 `.github/workflows/build-switch-nsp.yml`，产出：
   - `OKPlayer_Switch_01058800A9E20000.nsp`（Switch 桌面图标安装包，Title ID: `01058800A9E20000`）
   - `OKPlayer-Switch.nro`（Switch 主程序，放置于 SD 卡 `sdmc:/switch/OKPlayer-Switch/OKPlayer-Switch.nro`）

### 方式 B：Switch 端安装步骤
1. 将编译产出的 `OKPlayer-Switch.nro` 放入内存卡 `/switch/OKPlayer-Switch/OKPlayer-Switch.nro`。
2. 使用 `Sphaira`、`DBI` 或 `Goldleaf` 安装 `OKPlayer_Switch_01058800A9E20000.nsp` 即可在 Switch 主屏幕直接启动，也可直接在 `Sphaira` / 相册 Homebrew Menu 中运行 `OKPlayer-Switch.nro`。
