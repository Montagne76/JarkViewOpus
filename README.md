# JarkViewOpus

Directory Opus 查看器插件：在 DOpus 预览窗格内直接渲染 HEIC/HEIF、AVIF、JPEG XL 等现代图像格式。

解码核心移植自 [JarkViewer](https://github.com/jark006/JarkViewer)（GPL-3.0）。

## 当前状态

- [x] 插件框架：`DVP_InitEx / DVP_IdentifyW / DVP_IdentifyFileW / DVP_LoadBitmapW` + `DVP_CreateViewer`
- [x] 魔数嗅探（BMFF brand / JXL / PNG，逻辑对齐 JarkViewer FormatSniffer）
- [x] **Phase 1**：接入 JarkViewer 解码核心（libheif / libavif / libjxl / OpenCV 静态库），零 WIC 依赖
- [x] **Phase 2**：动图循环播放（APNG、动图 AVIF、动图 JXL）——自定义 viewer 窗口 + 定时器推帧
- [ ] Phase 3：Motion Photo / Live Photo 窗格内播放（MediaDecoder + FFmpeg）
- [ ] Phase 4：配置界面、打包、发布

### 两条渲染路径

| 文件类型 | 路径 | 缩放/平移/旋转 |
|---|---|---|
| 静态 HEIC / AVIF / JXL | `DVP_LoadBitmapW` 返回 HBITMAP | 由 DOpus 内置查看器负责 |
| 动图（帧数 > 1） | `DVPFIF_CanReturnViewer` → `DVP_CreateViewer` 自绘窗口 + `SetTimer` 循环 | 插件自绘（适应窗口 / 缩放 / 90° 旋转） |

## 安装（测试）

1. 从 Actions 构建产物下载 `JarkViewOpus.dll`
2. 复制到 `...\Directory Opus\Viewers\`
3. 重启 DOpus，`Preferences / Plugins / Viewers` 中可见 **JarkViewOpus**
4. 预览 `.heic` / `.avif` / `.jxl` / `.apng`

## 注意

- 解码走 JarkViewer 预读线程（`ImageDatabase(true)` + `getSafePtr`）。**不要**改成 `ImageDatabase(false)` 或 `getDataPtr`——后者只轮询缓存，会空转 60 秒超时并卡死 DOpus 主线程。
- 动图无播放控制条（v1 语义：打开即循环播放）。

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
