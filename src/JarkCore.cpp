/*
   JarkViewOpus - shared decode core (JarkViewer upstream, GPL-3.0).
   License: GPL-3.0-or-later
*/
#include "JarkCore.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
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

void JarkLog(const wchar_t* fmt, ...)
{
    static INIT_ONCE         once = INIT_ONCE_STATIC_INIT;
    static CRITICAL_SECTION  cs;
    static bool              enabled = true;

    InitOnceExecuteOnce(&once, [](PINIT_ONCE, PVOID, PVOID*) -> BOOL {
        InitializeCriticalSection(&cs);
        // Opt-in only: normal use should not leave a log file behind.
        wchar_t v[8] = {};
        enabled = (GetEnvironmentVariableW(L"JVO_LOG", v, 8) > 0);
        return TRUE;
    }, nullptr, nullptr);
    if (!enabled)
        return;

    wchar_t msg[1024] = {};
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st = {};
    GetLocalTime(&st);

    // %LOCALAPPDATA% first: TEMP is redirected to a read-only location in some
    // sandboxes, and this file also needs to be easy for a human to find.
    std::wstring file;
    wchar_t base[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH) > 0)
    {
        const std::wstring dir = std::wstring(base) + L"\\JarkViewOpus";
        CreateDirectoryW(dir.c_str(), nullptr);
        file = dir + L"\\JarkViewOpus.log";
    }
    else
    {
        wchar_t dir[MAX_PATH] = {};
        if (GetTempPathW(MAX_PATH, dir) == 0)
            return;
        file = std::wstring(dir) + L"JarkViewOpus.log";
    }

    wchar_t line[1200] = {};
    const int chars = _snwprintf_s(line, _countof(line), _TRUNCATE,
                                   L"%02u:%02u:%02u.%03u [%lu] %s\r\n",
                                   st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                                   GetCurrentThreadId(), msg);

    char utf8[2400] = {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line, chars > 0 ? chars : 0,
                                          utf8, sizeof(utf8) - 1, nullptr, nullptr);
    if (bytes <= 0)
        return;

    EnterCriticalSection(&cs);
    HANDLE h = CreateFileW(file.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(h, utf8, static_cast<DWORD>(bytes), &written, nullptr);
        CloseHandle(h);
    }
    LeaveCriticalSection(&cs);
}

const cv::Mat& CurrentFrameOf(const ImageAsset& asset)
{
    if (!asset.frames.empty())
        return asset.frames.front();
    return asset.primaryFrame;
}

namespace
{
    // Shared conversion tail: any input depth / channel count -> 8-bit BGRA with
    // the requested rotation and (optionally) premultiplied alpha.
    bool ToBGRA(const cv::Mat& srcIn, int rotation, bool premultiply, int maxDim,
                cv::Mat& out, bool* outHasAlpha)
    {
        if (outHasAlpha)
            *outHasAlpha = false;
        if (srcIn.empty())
            return false;

        cv::Mat src = srcIn;
        try
        {
            if (maxDim > 0 && std::max(src.cols, src.rows) > maxDim)
            {
                const double s = static_cast<double>(maxDim) /
                                 static_cast<double>(std::max(src.cols, src.rows));
                cv::resize(src, src, cv::Size(), s, s, cv::INTER_AREA);
            }

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

            switch (src.channels())
            {
            case 4:  out = src; break;
            case 3:  cv::cvtColor(src, out, cv::COLOR_BGR2BGRA); break;
            case 1:  cv::cvtColor(src, out, cv::COLOR_GRAY2BGRA); break;
            default: cv::cvtColor(src, out, cv::COLOR_BGR2BGRA); break;
            }

            const int rot = ((rotation % 360) + 360) % 360;
            if (rot == 90)
                cv::rotate(out, out, cv::ROTATE_90_CLOCKWISE);
            else if (rot == 180)
                cv::rotate(out, out, cv::ROTATE_180);
            else if (rot == 270)
                cv::rotate(out, out, cv::ROTATE_90_COUNTERCLOCKWISE);

            if (premultiply && out.channels() == 4)
            {
                for (int y = 0; y < out.rows; ++y)
                {
                    uint8_t* p = out.ptr<uint8_t>(y);
                    for (int x = 0; x < out.cols; ++x, p += 4)
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
                *outHasAlpha = (out.channels() == 4);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    // Copy BGRA pixels into *inOut, reusing the DIB when its geometry matches.
    // Animation frames are almost always the same size, and allocating a fresh
    // full-size DIB for every frame is pure waste on large animations.
    bool CopyToDIB(const cv::Mat& bgra, HBITMAP* inOut, SIZE* outSize)
    {
        if (bgra.empty())
            return false;

        const size_t stride = static_cast<size_t>(bgra.cols) * 4;
        const size_t rows   = static_cast<size_t>(bgra.rows);

        if (inOut && *inOut)
        {
            DIBSECTION ds = {};
            if (GetObjectW(*inOut, sizeof(ds), &ds) == sizeof(ds) && ds.dsBm.bmBits &&
                ds.dsBm.bmWidth == bgra.cols &&
                static_cast<size_t>(ds.dsBm.bmWidthBytes) == stride &&
                static_cast<size_t>(ds.dsBm.bmHeight) == rows)
            {
                uint8_t* dst = static_cast<uint8_t*>(ds.dsBm.bmBits);
                if (bgra.isContinuous() && bgra.step == stride)
                {
                    memcpy(dst, bgra.data, stride * rows);
                }
                else
                {
                    for (int y = 0; y < bgra.rows; ++y)
                        memcpy(dst + stride * y, bgra.ptr(y), stride);
                }
                if (outSize)
                {
                    outSize->cx = bgra.cols;
                    outSize->cy = bgra.rows;
                }
                return true;
            }

            DeleteObject(*inOut);
            *inOut = nullptr;
        }

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = bgra.cols;
        bmi.bmiHeader.biHeight      = -bgra.rows; // top-down
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        HDC hdc = CreateCompatibleDC(nullptr);
        if (!hdc)
            return false;
        void* bits = nullptr;
        HBITMAP hBitmap = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        DeleteDC(hdc);
        if (!hBitmap || !bits)
        {
            DeleteObject(hBitmap);
            return false;
        }

        uint8_t* dst = static_cast<uint8_t*>(bits);
        if (bgra.isContinuous() && bgra.step == stride)
        {
            memcpy(dst, bgra.data, stride * rows);
        }
        else
        {
            for (int y = 0; y < bgra.rows; ++y)
                memcpy(dst + stride * y, bgra.ptr(y), stride);
        }

        if (inOut)
            *inOut = hBitmap;
        if (outSize)
        {
            outSize->cx = bgra.cols;
            outSize->cy = bgra.rows;
        }
        return true;
    }
}

HBITMAP MatToDIB(const cv::Mat& src, int rotation, bool premultiply, int maxDim,
                 LPSIZE outSize, bool* outHasAlpha)
{
    cv::Mat bgra;
    bool alpha = false;
    if (!ToBGRA(src, rotation, premultiply, maxDim, bgra, &alpha))
    {
        if (outHasAlpha)
            *outHasAlpha = false;
        return nullptr;
    }
    if (outHasAlpha)
        *outHasAlpha = alpha;

    HBITMAP hbmp = nullptr;
    if (!CopyToDIB(bgra, &hbmp, outSize))
        return nullptr;
    return hbmp;
}

bool MatToDIBReuse(const cv::Mat& src, int rotation, bool premultiply, int maxDim,
                   HBITMAP* inOut, LPSIZE outSize, bool* outHasAlpha)
{
    cv::Mat bgra;
    bool alpha = false;
    if (!ToBGRA(src, rotation, premultiply, maxDim, bgra, &alpha))
    {
        if (outHasAlpha)
            *outHasAlpha = false;
        return false;
    }
    if (outHasAlpha)
        *outHasAlpha = alpha;
    return CopyToDIB(bgra, inOut, outSize);
}
