# JarkViewOpus

Directory Opus 查看器插件：在 DOpus 预览窗格内直接渲染 HEIC/HEIF、AVIF、JPEG XL 等现代图像格式。

解码核心移植自 [JarkViewer](https://github.com/jark006/JarkViewer)（GPL-3.0）。

## 当前状态

- [x] 插件框架：`DVP_InitEx / DVP_IdentifyW / DVP_IdentifyFileW / DVP_LoadBitmapW` + `DVP_CreateViewer`
- [x] 魔数嗅探（BMFF brand / JXL / PNG，逻辑对齐 JarkViewer FormatSniffer）
- [x] **Phase 1**：接入 JarkViewer 解码核心（libheif / libavif / libjxl / OpenCV 静态库），零 WIC 依赖
- [x] **Phase 2**：动图循环播放（APNG、动图 WebP、动图 AVIF、动图 JXL）——自定义 viewer 窗口 + 定时器推帧
- [x] **Phase 3**：Motion Photo / Live Photo 窗格内播放（上游 MediaPlayer，复用已链接的 FFmpeg）
- [ ] Phase 4：配置界面、打包、发布

### 两条渲染路径

| 文件类型 | 路径 | 缩放/平移/旋转 |
|---|---|---|
| 静态 HEIC / AVIF / JXL | `DVP_LoadBitmapW` 返回 HBITMAP | 由 DOpus 内置查看器负责 |
| 动图（帧数 > 1） | `DVPFIF_CanReturnViewer` → `DVP_CreateViewer` 自绘窗口 + `SetTimer` 循环 | 插件自绘（适应窗口 / 缩放 / 90° 旋转） |
| 实况照片（静图 + 视频） | 同上，但帧由 `jark::MediaPlayer` 实时解码 | 同上；视频帧等比内嵌在照片版面内 |

### 动图 WebP 的认领条件

不用扩展名判断，只看容器：`RIFF....WEBP` 的第一块是 `VP8X` 且扩展头里的 **animation 位（bit 1）**置位，或者同时出现 `ANIM` / `ANMF` 块。静态 WebP（含以 `VP8X` 开头的无损/带 alpha 变体，其 flag 只有 ALPHA/ICC 位）一律返回 FALSE，继续走 Opus 内置解码——那边更快，而且静态图本来就不需要自绘窗口。

### 实况照片支持的三种封装

| 来源 | 封装 | 插件如何认领 |
|---|---|---|
| 苹果 / vivo | `.heic` + 同名 `.mov` 侧车 | `.heic` 本来就被认领，识别时发现侧车即标记为实况 |
| 安卓 / 三星 | `.jpg` 内嵌 `ftyp` MP4（尾部或 XMP 声明长度） | 只有文件头 128 KiB 内出现 MotionPhoto 标记、或存在同名侧车时才认领；普通 JPEG 仍交给 Opus 内置快路径 |
| 苹果导出包 | `.livp`（zip 内含 heic + mov） | 认领 `.livp` 扩展名 |

行为：加载即自动播放一次（**静音**，音轨只用来驱动时钟），播完回到全分辨率静图；**双击重播**。

## 安装（测试）

1. 从 Actions 构建产物下载 `JarkViewOpus.dll`
2. 复制到 `...\Directory Opus\Viewers\`
3. 重启 DOpus，`Preferences / Plugins / Viewers` 中可见 **JarkViewOpus**
4. 预览 `.heic` / `.avif` / `.jxl` / `.apng` / `.livp` / 动图 `.webp`（`.png` 只要含 APNG 的 `acTL` 块也会被接管）
5. 实况照片：`.heic` + 同名 `.mov`、安卓 Motion Photo `.jpg`、或 `.livp` 都会在窗格里自动播放一次

## 坑：插件列表置顶并不够

Opus 的 **内置 WIC 图片例程优先于所有第三方查看器插件**，`Preferences / Plugins / Viewers` 里的顺序只决定第三方之间的先后。`.png` / `.gif` 这类内置支持的格式，插件必须声明 `DVPFIF_OverrideInternal`（`DVP_IdentifyW` 的 `dwFlags`）才能抢到文件——官方 `gifanim.dll` 就是靠这一条接管 `.gif` 的。少了它，`.apng` 会被 WIC 解码成静态第一帧。

## 注意

- 解码走 JarkViewer 预读线程（`ImageDatabase(true)` + `getSafePtr`）。**不要**改成 `ImageDatabase(false)` 或 `getDataPtr`——后者只轮询缓存，会空转 60 秒超时并卡死 DOpus 主线程。
- 动图无播放控制条（v1 语义：打开即循环播放）。
- 实况照片的播放启动在**工作线程**上（`MediaPlayer::start()` 要激活音频端点，蓝牙输出实测 400–800 ms，放在 UI 线程会卡住 Opus）。窗格在启动期间显示静图，`DVP_Uninit` 会等这些工作线程收尾后再卸载模块。
- 诊断日志：设环境变量 `JVO_LOG=1` 后写到 `%LOCALAPPDATA%\JarkViewOpus\JarkViewOpus.log`，默认关闭。

## 构建

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
# 产物: build/Release/JarkViewOpus.dll
```

或直接使用 GitHub Actions（push 即构建，产物在 Actions 页面 Artifacts）。

## 许可证

GPL-3.0-or-later。解码核心源自 JarkViewer（GPL-3.0，版权归 jark006）。
DOpus 插件接口头文件版权归 GP Software，随官方 SDK 分发。

<!-- TODO: 发布公开版前补入完整 GPL-3.0 许可证全文 -->
