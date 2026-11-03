# JarkViewOpus

Directory Opus 查看器插件：在 DOpus 预览窗格内直接渲染 HEIC/HEIF、AVIF、JPEG XL 等现代图像格式。

解码核心移植自 [JarkViewer](https://github.com/jark006/JarkViewer)（GPL-3.0）。

## 当前状态（Phase 0 骨架）

- [x] 插件框架：`DVP_InitEx / DVP_IdentifyW / DVP_IdentifyFileW / DVP_LoadBitmapW` 全套接口
- [x] 魔数嗅探（BMFF brand / JXL / WebP，逻辑对齐 JarkViewer FormatSniffer）
- [x] 解码：Phase 0 暂用 **Windows WIC**（需系统装 HEIF/AVIF 编解码器扩展），识别时验证编解码器可用，解不了的文件自动放行给其他查看器
- [ ] Phase 1：接入 JarkViewer 解码核心（libheif/libavif/libjxl），摆脱 WIC 依赖
- [ ] Phase 2：动图循环播放（APNG / 动图 WebP / JXL / AVIF）
- [ ] Phase 3：Motion Photo / Live Photo 窗格内播放（MediaDecoder + FFmpeg）
- [ ] Phase 4：配置界面、打包、发布

## 安装（测试）

1. 从 Actions 构建产物下载 `JarkViewOpus.dll`
2. 复制到 `...\Directory Opus\Viewers\`
3. 重启 DOpus，`Preferences / Plugins / Viewers` 中可见 **JarkViewOpus**
4. 预览一张 `.heic` / `.avif` / `.jxl` 验证

Phase 0 WIC 依赖：Windows 设置 → 应用 → 可选功能 → 已安装的扩展，需要「HEIF 图像扩展」「HEVC 视频扩展」「AV1/AVIF 扩展」等。Phase 1 接入 JarkViewer 核心后不再需要。

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
