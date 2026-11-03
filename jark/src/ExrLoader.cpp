/*
   JarkViewOpus - OpenEXR decoding through tinyexr.

   Why this file exists: upstream routes EXR to the OpenCV fallback, and this
   build's OpenCV answers "OpenEXR codec is disabled" (it was not linked against
   OpenEXR, and Windows' WIC has no EXR decoder either). Without a decoder here
   the format can only ever show a blank viewer.

   tinyexr is header-only (BSD-3-Clause). Its zlib work is delegated to the
   stb_image / stb_image_write implementations that upstream already compiles,
   so nothing new has to be linked.

   License: GPL-3.0-or-later
*/
#include "ExrLoader.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "jarkUtils.h"

// tinyexr's zlib work is delegated to stb, but the copy of stb_image compiled in
// ImageDatabase.cpp does not export the symbol we need (it is compiled for the
// loaders only and the zlib entry point is not pulled in). Rather than depend on
// another translation unit's symbol visibility, compile a private static copy of
// both stb headers right here: static linkage means no clash with the existing
// implementations and no second look at the linker.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
// stb drops its whole zlib section when STBI_NO_PNG is set without this, which
// is exactly the entry point tinyexr needs.
#define STBI_SUPPORT_ZLIB
#include "stb_image.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define TINYEXR_USE_MINIZ 0
#define TINYEXR_USE_STB_ZLIB 1
#define TINYEXR_USE_OPENMP 0
#define TINYEXR_IMPLEMENTATION
#include "tinyexr.h"

namespace jark {
namespace {

    // Scene-linear floats shown as-is would either clip to white (HDR skies) or
    // come out nearly black (values living in 0..1), so normalise against a high
    // luminance percentile and then apply an sRGB-ish encode. The percentile
    // ignores the handful of sun/specular samples that would otherwise drag the
    // whole frame down.
    float whitePointOf(const cv::Mat& luminance)
    {
        const int total = luminance.cols * luminance.rows;
        if (total <= 0)
            return 1.0f;

        std::vector<float> samples;
        samples.reserve(8192);
        const int step = (std::max)(1, total / 8192);
        const float* p = luminance.ptr<float>(0);
        for (int i = 0; i < total; i += step)
        {
            const float v = p[i];
            if (std::isfinite(v) && v > 0.0f)
                samples.push_back(v);
        }
        if (samples.empty())
            return 1.0f;

        const size_t idx = (samples.size() - 1) * 995 / 1000;
        std::nth_element(samples.begin(), samples.begin() + idx, samples.end());
        const float white = samples[idx];
        return (std::isfinite(white) && white > 1e-6f) ? white : 1.0f;
    }

    cv::Mat gammaLut()
    {
        static const cv::Mat lut = [] {
            cv::Mat table(1, 256, CV_8U);
            for (int i = 0; i < 256; ++i)
            {
                const double v = std::pow(i / 255.0, 1.0 / 2.2) * 255.0;
                table.at<uchar>(i) = static_cast<uchar>(std::clamp(v, 0.0, 255.0) + 0.5);
            }
            return table;
        }();
        return lut;
    }
}

cv::Mat loadEXR(std::wstring_view path, std::span<const uint8_t> buf)
{
    if (buf.empty() || buf.size() > static_cast<size_t>(INT_MAX))
        return {};

    float* rgba = nullptr;
    int width = 0;
    int height = 0;
    const char* err = nullptr;

    const int ret = LoadEXRFromMemory(&rgba, &width, &height, buf.data(), buf.size(), &err);
    if (ret != TINYEXR_SUCCESS || !rgba || width <= 0 || height <= 0)
    {
        JARK_LOG("EXR decode failed: {} [{}]",
                 jarkUtils::wstringToUtf8(path),
                 err ? err : "unknown");
        if (err)
            FreeEXRErrorMessage(err);
        if (rgba)
            free(rgba);
        return {};
    }
    // Guard against a header that claims an absurd canvas: LoadEXRFromMemory has
    // already allocated, so this is a post-hoc sanity check, not a protection.
    if (static_cast<int64_t>(width) * height > 400ll * 1000 * 1000)
    {
        JARK_LOG("EXR too large: {}x{} ({})", width, height, jarkUtils::wstringToUtf8(path));
        free(rgba);
        return {};
    }

    // Wrap without copying; rgba stays owned by us and is freed on every exit.
    const cv::Mat src(height, width, CV_32FC4, rgba);

    std::vector<cv::Mat> channels;
    cv::split(src, channels); // R, G, B, A

    const cv::Mat luminance = 0.2126f * channels[0] + 0.7152f * channels[1] + 0.0722f * channels[2];
    const float white = whitePointOf(luminance);
    // Only pull highlights down when there actually are any: a document whose
    // values already sit in 0..1 is shown at its authored brightness, and one
    // with a sky at 20.0 gets normalised instead of blowing out to white.
    const float scale = white > 1.0f ? 1.0f / white : 1.0f;

    // Scale only the colour channels: alpha is already 0..1 and must not be
    // re-exposed. Order is B, G, R for OpenCV's default layout.
    std::vector<cv::Mat> ordered = { channels[2] * scale, channels[1] * scale, channels[0] * scale };

    double alphaMin = 1.0, alphaMax = 1.0;
    cv::minMaxLoc(channels[3], &alphaMin, &alphaMax);
    const bool hasAlpha = alphaMax > 0.0 && alphaMin < 0.999;

    cv::Mat floatMat;
    if (hasAlpha)
        ordered.push_back(channels[3]);
    cv::merge(ordered, floatMat);

    // Over-range values clip here; the white point already put almost everything
    // below 1.0.
    cv::Mat eightBit;
    floatMat.convertTo(eightBit, CV_8U, 255.0);

    // convertTo saturates, so the LUT can assume 0..255 input. src and dst must
    // not alias here, hence the separate destination.
    cv::Mat gammaCorrected;
    cv::LUT(eightBit, gammaLut(), gammaCorrected);

    free(rgba);

    JARK_LOG("EXR decoded: {}x{} white={:.4f} alpha={} ({})",
             width, height, white, hasAlpha ? 1 : 0, jarkUtils::wstringToUtf8(path));
    return gammaCorrected;
}

} // namespace jark
