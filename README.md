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
| 安卓 / 三星 | `.jpg` 内嵌 `ftyp` MP4（尾部或 XMP 声明长度） | 先看 4 KiB 探测头：里面出现 MotionPhoto 标记、或存在同名侧车就认领。JPEG 的段表走到 SOS 还没见到标记就不再读；只有标记可能还在后面（APP 段超过 4 KiB）时才把窗口放大到 128 KiB。普通 JPEG 仍交给 Opus 内置快路径 |
| 苹果导出包 | `.livp`（zip 内含 heic + mov） | 认领 `.livp` 扩展名 |

行为：加载即自动播放一次（**静音**，音轨只用来驱动时钟），播完回到全分辨率静图；**双击重播**。

### PSD / PSB 的认领条件

Opus 内置对 PSD 只能画出文件里**嵌入的预览缩略图**（Photoshop 关掉「最大兼容性」时根本没有，且分辨率通常只有几百像素）。我们改读真正的合成图：上游 `decodePsdComposite` 支持 8/16 位与 Raw / PackBits / ZIP（含 prediction），遇到明确标记「未保存合成图」的文档会退回按图层合成；它解不了的情况（例如非 RGB 色彩模式）识别阶段返回 FALSE，文件仍然落回 Opus 的预览图，不会显示成空白。

保护性上限：画布超过 **1 亿像素** 的不认领——合成图会是数百 MB 的 BGRA 缓冲，加上 OpenCV 的拷贝，全部发生在 Opus 进程里。

### 新接的非主流格式（Opus 侧完全无支持）

这几类格式 Opus 既没有查看器插件、也没有内置解码（判定方法见文末「要不要再接一种格式：怎么查」）。认领一律要求**扩展名 + 魔数双重命中**，因为其中几个签名极短（PFM 三个字节、PNM 两个字节），只靠嗅探会把普通文本文件吃进来。

| 扩展名 | 格式 | 解码路径 | 说明 |
|---|---|---|---|
| `.exr` | OpenEXR | **tinyexr**（本项目新增） | 这份 OpenCV 编译时没链接 OpenEXR（运行时只会答 "OpenEXR codec is disabled"），WIC 也没有 EXR 解码器，所以自带一个。覆盖 tinyexr 支持的 NONE/RLE/ZIP/ZIPS/PIZ/PXR24 等压缩。线性场景参考值先按 99.5% 亮度定白点（避开太阳/高光把整幅拉黑），再走 sRGB 编码，否则 HDR 内容不是全白就是全黑 |
| `.hdr` | Radiance RGBE | stb_image | 上游 `loadSTB`，stb 内部做 gamma 展开 |
| `.pic` | Softimage PIC | stb_image | 同上 |
| `.qoi` | Quite OK Image | 上游 `loadQOI` | — |
| `.pnm .ppm .pgm .pbm .pxm` | Netpbm | OpenCV `PxMDecoder` | 16 位样本线性缩放到 8 位 |
| `.pfm` | Portable FloatMap | 上游 `loadPFM` | 浮点，`convertMatToCV_8U` 动态定标 |
| `.blp` | Blizzard BLP1/BLP2 | 上游 `loadBLP` | 调色板 / DXT1·3·5 / RawBGRA |
| `.svg` | SVG | lunasvg + 上游 `VectorImage` | 真矢量：按当前缩放**重新光栅化**（首帧 1024px，上限 4096px），不是固定分辨率位图。Opus 自己用 Edge 预览（相当于开个网页），所以接过来 |

`.svg` 另有两个细节：`baseSize` 取文档声明的 intrinsic 尺寸而非首帧光栅尺寸，所以 1:1 与适应窗口都按矢量边界算；`<switch>`、CSS `var()`、`light-dark()` 由上游 `SVGPreprocessor` 折叠（lunasvg 不认这些，遇到会把整个图元丢掉不画）。

故意**不接**：`.dds`（Opus 13 原生 + opuswic）、`.jp2/.j2k`（自带 `j2k.dll`）、`.tga`（`targa.dll`）、`.pcx`（`pcx.dll`）、`.ras/.sr`（上游没有解码器、OpenCV 也不支持，接了必然白屏）、`.wp2`（实验格式，只能按扩展名判定，收益接近零）。

### 我们改过上游哪几处（vendor patch）

除 `src/` 下自己的代码，`jark/` 里有三处改动，升级上游时需要重新打：

| 文件 | 改动 |
|---|---|
| `src/ImageDatabase.cpp` | `case FileFormat::Exr` 从「交给 OpenCV 兜底」改成调 `jark::loadEXR` |
| `src/ImageDatabase.cpp` | 顶部加 `#include "ExrLoader.h"` |
| `include/tinyexr.h` | `TINYEXR_USE_STB_ZLIB` 段的两行函数声明去掉 `extern "C"`（stb 的实现是 C++ 链接，否则链接期找不到符号） |

## 安装（测试）

1. 从 Actions 构建产物下载 `JarkViewOpus.dll`
2. 复制到 `...\Directory Opus\Viewers\`
3. 重启 DOpus，`Preferences / Plugins / Viewers` 中可见 **JarkViewOpus**
4. 预览 `.heic` / `.avif` / `.jxl` / `.apng` / `.livp` / 动图 `.webp` / `.psd` / `.psb` / `.exr` / `.hdr` / `.qoi` / `.pnm` `.ppm` `.pgm` / `.pfm` / `.blp` / `.svg`（`.png` 只要含 APNG 的 `acTL` 块也会被接管）
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

## 上游版本（vendor pin）

| 项 | 值 |
|---|---|
| 仓库 | [jark006/JarkViewer](https://github.com/jark006/JarkViewer) |
| 上游 release | **v2.0**（2026-11-01） |
| 源码 pin | `6252738`（v2.0 之后的 main tip，仅文档改动） |
| 静态库 | `static_lib` release 的 `JarkViewer_static_lib_v2.0+.7z`（265,032,797 字节，2026-10-09 更新） |
| 已 vendor | `jark/src/` 18 个 cpp + `jark/include/`（含 `resource.h`）；与 pin 逐字节一致 |
| 未 vendor | `JarkViewer/src/main.cpp` 等 UI 层、ImGui、`ImageResampler.cpp`（UI-only） |

升级步骤：

1. 拉上游 main，确认 `JarkViewer/src/*.cpp` 与 `JarkViewer/include/` 的改动；
2. 只把解码链路用到的文件同步进 `jark/`（UI 层不搬）——对照 `CMakeLists.txt` 的 `JARK_SRC` 列表；
3. 若上游发了新的 `static_lib` 资产，同步改 `.github/workflows/build.yml` 里的下载 URL 与缓存 key（`jark-static-lib-v2.0-2026-10`），否则 CI 会继续用旧缓存；
4. 跑 `test-identify.py` + `test-viewer.py` 回归（样本见 `test-samples/`）。

## 要不要再接一种格式：怎么查

决定接管某个格式前，先证明 Opus 那边确实没有。三步走，前两步可以自动化：

1. **问插件自己**。`Viewers\` 下每个 DLL 都实现了 `DVP_IdentifyW`，它会把自己负责的扩展名回填到 `VIEWERPLUGININFOW.lpszHandleExts`。注意 `DVP_InitEx` 收的是 `DVPINITEXDATA*` 结构指针，传 `NULL` 会让多数第三方插件直接访问违例——而且 `DVP_IdentifyW` 在未 Init 的情况下本来就允许调用。工作目录下的 `probe-viewer.py` 干这件事，每个 DLL 跑在独立子进程里，崩溃不会带崩整轮扫描。
2. **拿样本问插件**。有了扩展名清单还不够——`Animated GIF`、`Raw Digital Camera`、`movie.dll` 声明的扩展名是空的，靠内容嗅探。所以要用真实文件逐个调 `DVP_IdentifyFileW`。`probe-all.py` 会把 `Viewers\*.dll` × `test-samples/opus-probe/probe-*` 全交叉一遍，打印认领矩阵。样本由 `make-probe-samples.py` 按规范手写（本机没有 PIL/OpenCV），其中 `.tga` / `.pcx` 是**对照组**：这两个已知有插件，如果它们没被认领，是探测方法坏了，而不是格式没人支持。
3. **剩下的才查文档**。前两步只覆盖第三方插件；Opus 内置于查看器的能力（BMP/PNG/JPEG/GIF/TIFF/PSD 预览图，以及 13 版新增的 DDS、AVIF，SVG 走 Edge）不在 `Viewers\` 里。旁证有三处：本机 `%APPDATA%\GPSoftware\Directory Opus\FileTypes\Groups\groups.oxr` 的 `Images` 群组成员、Opus 13 更新说明、以及官方论坛的格式请求帖。

2026-11-03 的实测结论（Opus 13.23）：

| 格式 | 第三方插件 | Opus 内置 | 结论 |
|---|---|---|---|
| `.tga` / `.pcx` | targa.dll / pcx.dll | — | 已有 |
| `.jp2 .j2k` | j2k.dll | — | 已有 |
| `.dds` | opuswic 声明 | 13 版原生新增 | 已有 |
| `.webp` | WebP.dll（静态）| — | 静态已有，动图由本插件接管 |
| `.psd` | — | 仅嵌入的预览缩略图 | 本插件接管 |
| `.svg` | docs.dll(`*.*`) 转 Edge | Edge 预览 | 有，但是矢量交互式，接管反而降级 |
| `.exr` `.hdr` `.qoi` `.pnm/.ppm/.pgm/.pbm` `.pfm` | **无** | **无** | 真正的空缺 |

## 许可证

GPL-3.0-or-later。解码核心源自 JarkViewer（GPL-3.0，版权归 jark006）。
DOpus 插件接口头文件版权归 GP Software，随官方 SDK 分发。

<!-- TODO: 发布公开版前补入完整 GPL-3.0 许可证全文 -->
