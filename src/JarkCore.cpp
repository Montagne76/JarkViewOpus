/*
   JarkViewOpus - shared decode core (JarkViewer upstream, GPL-3.0).
   License: GPL-3.0-or-later
*/
#include "JarkCore.h"

#include <algorithm>
#include <vector>

#include "../jark/include/ImageDatabase.h"

namespace
{
    CRITICAL_SECTION g_cs;
    bool             g_csReady = false;
    ImageDatabase*   g_db = nullptr;

    void EnsureCs()
    {
        static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
        InitOnceExecuteOnce(&once, [](PINIT_ONCE, PVOID, PVOID*) -> BOOL {
            InitializeCriticalSection(&g_cs);
            g_csReady = true;
            return TRUE;
        }, nullptr, nullptr);
    }
}

ImageDatabase* JarkGetDB()
{
    EnsureCs();
    EnterCriticalSection(&g_cs);
    if (!g_db)
    {
        // startPreloadWorker MUST be true: getDataPtr()/getSafePtr() only poll
        // the cache, the worker is the one that actually decodes. Without it
        // every call spins the full 60s timeout and freezes the DOpus UI.
        g_db = new (std::nothrow) ImageDatabase(true /*startPreloadWorker*/);
        if (g_db)
            g_db->setByteBudget(256ull * 1024 * 1024);
    }
    LeaveCriticalSection(&g_cs);
    return g_db;
}

void JarkShutdownDB()
{
    if (!g_csReady)
        return;
    EnterCriticalSection(&g_cs);
    ImageDatabase* db = g_db;
    g_db = nullptr;
    LeaveCriticalSection(&g_cs);
    // ~ImageDatabase stops the preload worker before its members die.
    delete db;
}

std::shared_ptr<ImageAsset> JarkLoadAsset(const std::wstring& path)
{
    ImageDatabase* db = JarkGetDB();
    if (!db || path.empty())
        return nullptr;
    try
    {
        return db->getSafePtr(path);
    }
    catch (...)
    {
        return nullptr;
    }
}

std::shared_ptr<ImageAsset> JarkLoadAssetFromStream(LPSTREAM stream, const wchar_t* name)
{
    if (!stream)
        return nullptr;

    LARGE_INTEGER zero = {};
    ULARGE_INTEGER end = {};
    if (FAILED(stream->Seek(zero, STREAM_SEEK_END, &end)))
        return nullptr;
    const unsigned long long total = end.QuadPart;
    if (total < 16 || total > 2ull * 1024 * 1024 * 1024)
        return nullptr;
    if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr)))
        return nullptr;

    wchar_t tmpDir[MAX_PATH] = {};
    wchar_t tmpName[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, tmpDir) || !GetTempFileNameW(tmpDir, L"jvo", 0, tmpName))
        return nullptr;

    // Keep the original name's extension: some loaders route by extension.
    std::wstring realPath = tmpName;
    realPath = realPath.substr(0, realPath.size() - 4);
    if (name)
    {
        const wchar_t* slash = wcsrchr(name, L'\\');
        const wchar_t* slash2 = wcsrchr(name, L'/');
        if (slash2 > slash)
            slash = slash2;
        std::wstring base = slash ? std::wstring(slash + 1) : std::wstring(name);
        if (base.find(L'.') != std::wstring::npos)
            realPath += base;
        else
            realPath += L"stream.img";
    }

    std::shared_ptr<ImageAsset> asset;
    HANDLE hFile = CreateFileW(realPath.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        bool ok = true;
        char buf[65536];
        unsigned long long remaining = total;
        while (remaining > 0)
        {
            ULONG want = static_cast<ULONG>(std::min<unsigned long long>(remaining, sizeof(buf)));
            ULONG got = 0;
            if (FAILED(stream->Read(buf, want, &got)) || got == 0)
            {
                ok = false;
                break;
            }
            DWORD written = 0;
            if (!WriteFile(hFile, buf, got, &written, nullptr) || written != got)
            {
                ok = false;
                break;
            }
            remaining -= got;
        }
        CloseHandle(hFile);

        if (ok)
            asset = JarkLoadAsset(realPath);
    }

    DeleteFileW(tmpName);
    DeleteFileW(realPath.c_str());
    return asset;
}

bool IsAnimatedAsset(const ImageAsset& asset)
{
    return asset.frames.size() > 1;
}

const cv::Mat& CurrentFrameOf(const ImageAsset& asset)
{
    if (!asset.frames.empty())
        return asset.frames.front();
    return asset.primaryFrame;
}

HBITMAP MatToDIB(const cv::Mat& srcIn, int rotation, bool premultiply, int maxDim,
                 LPSIZE outSize, bool* outHasAlpha)
{
    if (outHasAlpha)
        *outHasAlpha = false;
    if (srcIn.empty())
        return nullptr;

    cv::Mat src = srcIn;

    if (maxDim > 0 && std::max(src.cols, src.rows) > maxDim)
    {
        const double s = static_cast<double>(maxDim) /
                         static_cast<double>(std::max(src.cols, src.rows));
        cv::resize(src, src, cv::Size(), s, s, cv::INTER_AREA);
    }

    try
    {
        if (src.depth() != CV_8U)
        {
            cv::Mat tmp;
            double scale = 1.0;
            if (src.depth() == CV_16U)
                scale = 1.0 / 257.0;
            else if (src.depth() == CV_32F || src.depth() == CV_64F)
                scale = 255.0;
            src.convertTo(tmp, CV_8U, scale);
            src = tmp;
        }

        cv::Mat bgra;
        switch (src.channels())
        {
        case 4: bgra = src; break;
        case 3: cv::cvtColor(src, bgra, cv::COLOR_BGR2BGRA); break;
        case 1: cv::cvtColor(src, bgra, cv::COLOR_GRAY2BGRA); break;
        default:
        {
            cv::Mat tmp;
            cv::cvtColor(src, tmp, cv::COLOR_BGR2BGRA);
            bgra = tmp;
            break;
        }
        }

        const int rot = ((rotation % 360) + 360) % 360;
        if (rot == 90)
            cv::rotate(bgra, bgra, cv::ROTATE_90_CLOCKWISE);
        else if (rot == 180)
            cv::rotate(bgra, bgra, cv::ROTATE_180);
        else if (rot == 270)
            cv::rotate(bgra, bgra, cv::ROTATE_90_COUNTERCLOCKWISE);

        if (premultiply && bgra.channels() == 4)
        {
            for (int y = 0; y < bgra.rows; ++y)
            {
                uint8_t* p = bgra.ptr<uint8_t>(y);
                for (int x = 0; x < bgra.cols; ++x, p += 4)
                {
                    const unsigned a = p[3];
                    if (a == 255)
                        continue;
                    p[0] = static_cast<uint8_t>((p[0] * a + 127) / 255);
                    p[1] = static_cast<uint8_t>((p[1] * a + 127) / 255);
                    p[2] = static_cast<uint8_t>((p[2] * a + 127) / 255);
                }
            }
        }

        if (outHasAlpha)
            *outHasAlpha = (bgra.channels() == 4);

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = bgra.cols;
        bmi.bmiHeader.biHeight      = -bgra.rows; // top-down
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        HDC hdc = CreateCompatibleDC(nullptr);
        if (!hdc)
            return nullptr;
        void* bits = nullptr;
        HBITMAP hBitmap = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        DeleteDC(hdc);
        if (!hBitmap || !bits)
        {
            DeleteObject(hBitmap);
            return nullptr;
        }

        const size_t dstStride = static_cast<size_t>(bgra.cols) * 4;
        if (bgra.isContinuous() && static_cast<size_t>(bgra.step) == dstStride)
        {
            memcpy(bits, bgra.data, dstStride * static_cast<size_t>(bgra.rows));
        }
        else
        {
            uint8_t* dst = static_cast<uint8_t*>(bits);
            for (int y = 0; y < bgra.rows; ++y)
                memcpy(dst + dstStride * y, bgra.ptr(y), dstStride);
        }

        if (outSize)
        {
            outSize->cx = bgra.cols;
            outSize->cy = bgra.rows;
        }
        return hBitmap;
    }
    catch (...)
    {
        return nullptr;
    }
}
