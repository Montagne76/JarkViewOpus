#pragma once

// OpenEXR 解码（tinyexr）
//
// 上游把 EXR 交给 OpenCV 兜底，但这份 OpenCV 编译时没有链接 OpenEXR，运行时报
// "OpenEXR codec is disabled"；Windows 的 WIC 也没有 EXR 编解码器。所以 EXR 得
// 自带一个解码器，否则这个格式只能留空。
//
// tinyexr 是单头文件实现（BSD-3-Clause），压缩块复用 stb_image 已有的 zlib 入口，
// 不引入新的二进制依赖。

#include <cstdint>
#include <span>
#include <string_view>

#include <opencv2/opencv.hpp>

namespace jark {

// 从内存缓冲解码单帧 EXR，输出 8 位 BGR（有有效 alpha 时为 BGRA）。
// 失败返回空 Mat。像素值按线性场景参考处理：自动曝光到 99.5% 亮度后做 sRGB 编码。
cv::Mat loadEXR(std::wstring_view path, std::span<const uint8_t> buf);

} // namespace jark
