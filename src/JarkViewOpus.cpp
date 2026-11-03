/*
   JarkViewOpus - Directory Opus Viewer Plugin
   Phase 1: decode via the JarkViewer core (jark006/JarkViewer, GPL-3.0).

   Identify decodes the image once (result cached in the ImageAsset LRU);
   LoadBitmap then hits the cache, so each file is decoded exactly once.

   Plugin API: Directory Opus Viewer Plugin SDK (c) GP Software.
   License: GPL-3.0-or-later
*/

#include <windows.h>
#include <objidl.h>
#include <cstring>
#include <algorithm>
#include <memory>

#include "../jark/include/ImageDatabase.h"
#include "../jark/include/FormatSniffer.h"

#include "../sdk/DirectoryOpusViewerPlugins.h"

// ---------------------------------------------------------------------------
// Plugin identity
// ---------------------------------------------------------------------------

// {9A7B3F42-6C1D-4E5A-8F2B-3D9E1C7A5B60}
static const GUID GUIDPlugin_JarkViewOpus =
{ 0x9a7b3f42, 0x6c1d, 0x4e5a, { 0x8f, 0x2b, 0x3d, 0x9e, 0x1c, 0x7a, 0x5b, 0x60 } };

static const wchar_t kPluginName[]        = L"JarkViewOpus";
static const wchar_t kPluginDescription[] = L"Modern image formats via JarkViewer core (HEIC/AVIF/JXL) - JarkViewOpus";
static const wchar_t kPluginCopyright[]   = L"GPL-3.0-or-later";
static const wchar_t kPluginURL[]         = L"https://github.com/Montagne76/JarkViewOpus";

// Claimed extensions. (.webp intentionally omitted: Opus 13 handles it internally.)
static const wchar_t kHandleExts[] = L".heic;.heif;.avif;.avifs;.jxl";

static HINSTANCE       g_hModuleInstance = nullptr;
static CRITICAL_SECTION g_cs;
static int             g_refCount = 0;

static void LeoStringCopy(LPWSTR dst, UINT cchMax, const wchar_t* src)
{
    if (dst && cchMax > 0)
        wcsncpy_s(dst, cchMax, src ? src : L"", _TRUNCATE);
}

// ---------------------------------------------------------------------------
// JarkViewer core singleton
// ---------------------------------------------------------------------------

// No preload worker thread (a background decoder thread inside the DOpus
// process is not worth the shutdown risk; DOpus drives file switching
// synchronously anyway). LRU budget keeps repeat views instant.
static jark::ImageDatabase* g_db = nullptr;

static jark::ImageDatabase* GetDB()
{
    EnterCriticalSection(&g_cs);
    if (!g_db)
    {
        g_db = new (std::nothrow) jark::ImageDatabase(false /*startPreloadWorker*/);
        if (g_db)
            g_db->setByteBudget(256ull * 1024 * 1024);
    }
    LeaveCriticalSection(&g_cs);
    return g_db;
}

static void DestroyDB()
{
    EnterCriticalSection(&g_cs);
    delete g_db;
    g_db = nullptr;
    LeaveCriticalSection(&g_cs);
}

// Decode (or fetch from cache) one file. Returns nullptr on failure.
static std::shared_ptr<jark::ImageAsset> LoadAsset(const std::wstring& path)
{
    jark::ImageDatabase* db = GetDB();
    if (!db)
        return nullptr;
    try
    {
        return db->getDataPtr(path);
    }
    catch (...)
    {
        return nullptr;
    }
}

static bool IsSupportedSniff(jark::FileFormat ff)
{
    switch (ff)
    {
    case jark::FileFormat::Heif:
    case jark::FileFormat::Avif:
    case jark::FileFormat::Jxl:
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// cv::Mat -> 32bpp top-down DIB (BGRA)
// ---------------------------------------------------------------------------

// Preview cap: images larger than this are downscaled during the Mat->DIB
// conversion so DOpus's fit-to-window scaling has far fewer pixels to stretch.
static const int kMaxDecodeDim = 4096;

static HBITMAP MatToDIB(const cv::Mat& srcIn, LPSIZE lpszSize, LPVIEWERPLUGINFILEINFOW lpVPFileInfo)
{
    if (srcIn.empty())
        return nullptr;

    cv::Mat src = srcIn;

    if (std::max(src.cols, src.rows) > kMaxDecodeDim)
    {
        const double s = static_cast<double>(kMaxDecodeDim) /
                         static_cast<double>(std::max(src.cols, src.rows));
        cv::resize(src, src, cv::Size(), s, s, cv::INTER_AREA);
    }

    // Normalize to 8-bit BGRA.
    cv::Mat bgra;
    try
    {
        if (src.depth() != CV_8U)
        {
            cv::Mat tmp;
            double scale = 1.0;
            if (src.depth() == CV_16U)      scale = 1.0 / 257.0;
            else if (src.depth() == CV_32F || src.depth() == CV_64F) scale = 255.0;
            src.convertTo(tmp, CV_8U, scale);
            src = tmp;
        }
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
    }
    catch (...)
    {
        return nullptr;
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

    if (lpszSize)
    {
        lpszSize->cx = bgra.cols;
        lpszSize->cy = bgra.rows;
    }
    if (lpVPFileInfo && srcIn.channels() == 4)
        lpVPFileInfo->dwFlags |= DVPFIF_HasAlphaChannel;
    return hBitmap;
}

// ---------------------------------------------------------------------------
// DOpus viewer plugin exports
// ---------------------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        InitializeCriticalSection(&g_cs);
        g_hModuleInstance = hModule;
        break;
    case DLL_PROCESS_DETACH:
        DeleteCriticalSection(&g_cs);
        break;
    default:
        break;
    }
    return TRUE;
}

extern "C" {

__declspec(dllexport) BOOL DVP_InitEx(LPDVPINITEXDATA /*pInitExData*/)
{
    EnterCriticalSection(&g_cs);
    ++g_refCount;
    LeaveCriticalSection(&g_cs);
    return TRUE;
}

__declspec(dllexport) void DVP_Uninit(void)
{
    EnterCriticalSection(&g_cs);
    if (g_refCount > 0)
        --g_refCount;
    LeaveCriticalSection(&g_cs);
    if (g_refCount == 0)
        DestroyDB();
}

__declspec(dllexport) BOOL DVP_USBSafe(LPOPUSUSBSAFEDATA /*pUSBSafeData*/)
{
    return TRUE;
}

// Identify the viewer plugin to DOpus.
__declspec(dllexport) BOOL DVP_IdentifyW(LPVIEWERPLUGININFOW lpVPInfo)
{
    if (!lpVPInfo || lpVPInfo->cbSize < VIEWERPLUGININFO_V4_SIZE)
        return FALSE;

    lpVPInfo->dwFlags = DVPFIF_CanHandleStreams            // files inside Zip/FTP
                      | DVPFIF_CanHandleBytes              // identify from header bytes
                      | DVPFIF_ExtensionsOnlyIfSlow        // slow/non-seek: match by extension
                      | DVPFIF_ExtensionsOnlyIfNoRndSeek;

    LeoStringCopy(lpVPInfo->lpszHandleExts,  lpVPInfo->cchHandleExtsMax,  kHandleExts);
    LeoStringCopy(lpVPInfo->lpszName,        lpVPInfo->cchNameMax,        kPluginName);
    LeoStringCopy(lpVPInfo->lpszDescription, lpVPInfo->cchDescriptionMax, kPluginDescription);
    LeoStringCopy(lpVPInfo->lpszCopyright,   lpVPInfo->cchCopyrightMax,   kPluginCopyright);
    LeoStringCopy(lpVPInfo->lpszURL,         lpVPInfo->cchURLMax,         kPluginURL);

    lpVPInfo->dwlMinFileSize    = 16;
    lpVPInfo->uiMajorFileType   = DVPMajorType_Image;
    lpVPInfo->idPlugin          = GUIDPlugin_JarkViewOpus;
    return TRUE;
}

static void FillFileInfo(LPVIEWERPLUGINFILEINFOW lpVPFileInfo, const SIZE& size, int bits)
{
    lpVPFileInfo->dwFlags    = DVPFIF_CanReturnBitmap | DVPFIF_CanReturnThumbnail;
    lpVPFileInfo->wMajorType = DVPMajorType_Image;
    lpVPFileInfo->wMinorType = 0;
    lpVPFileInfo->szImageSize.cx = size.cx;
    lpVPFileInfo->szImageSize.cy = size.cy;
    lpVPFileInfo->iNumBits   = bits;
}

// Sniff a header buffer; returns true when the bytes belong to a format we claim.
static bool SniffSupported(const uint8_t* d, size_t n)
{
    if (n < 12)
        return false;
    try
    {
        return IsSupportedSniff(jark::sniffFileFormat(std::span<const uint8_t>(d, n)));
    }
    catch (...)
    {
        return false;
    }
}

// Identify a local file: sniff magic bytes, then decode once (result lands in
// the LRU cache so the following LoadBitmap is free). This gives DOpus the
// real image size without paying a second decode.
__declspec(dllexport) BOOL DVP_IdentifyFileW(HWND /*hWnd*/, LPWSTR lpszName,
                                             LPVIEWERPLUGINFILEINFOW lpVPFileInfo,
                                             HANDLE /*hAbortEvent*/)
{
    if (g_refCount == 0 || !lpVPFileInfo || !lpszName)
        return FALSE;

    uint8_t header[96] = {};
    size_t got = 0;
    {
        HANDLE hFile = CreateFileW(lpszName, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
            return FALSE;
        DWORD read = 0;
        ReadFile(hFile, header, sizeof(header), &read, nullptr);
        CloseHandle(hFile);
        got = read;
    }

    if (!SniffSupported(header, got))
        return FALSE;

    auto asset = LoadAsset(lpszName);
    if (!asset)
        return FALSE;

    const cv::Mat& m = asset->frames.empty() ? asset->primaryFrame
                                             : (asset->primaryFrame.empty() ? asset->frames.front()
                                                                            : asset->primaryFrame);
    if (m.empty())
        return FALSE;

    FillFileInfo(lpVPFileInfo, SIZE{ m.cols, m.rows }, 32);
    return TRUE;
}

// Stream identify (files inside archives/FTP): sniff only, no size probe —
// full decode happens in LoadBitmapStreamW.
__declspec(dllexport) BOOL DVP_IdentifyFileStreamW(HWND /*hWnd*/, LPSTREAM lpStream,
                                                   LPWSTR /*lpszName*/,
                                                   LPVIEWERPLUGINFILEINFOW lpVPFileInfo,
                                                   DWORD /*dwStreamFlags*/)
{
    if (g_refCount == 0 || !lpStream || !lpVPFileInfo)
        return FALSE;

    LARGE_INTEGER zero = {};
    ULARGE_INTEGER cur = {};
    if (FAILED(lpStream->Seek(zero, STREAM_SEEK_CUR, &cur)))
        return FALSE;
    LARGE_INTEGER restorePos = {};
    restorePos.QuadPart = static_cast<LONGLONG>(cur.QuadPart);

    uint8_t header[96] = {};
    ULONG read = 0;
    HRESULT hr = lpStream->Read(header, sizeof(header), &read);
    lpStream->Seek(restorePos, STREAM_SEEK_SET, nullptr);
    if (FAILED(hr) || !SniffSupported(header, read))
        return FALSE;

    // Size unknown without a decode; report the header box can't give cheaply.
    // DOpus will get the real size from the returned bitmap.
    FillFileInfo(lpVPFileInfo, SIZE{ 0, 0 }, 32);
    return TRUE;
}

// First frame -> HBITMAP. Decode is served from the LRU cache filled at identify.
__declspec(dllexport) HBITMAP DVP_LoadBitmapW(HWND /*hWnd*/, LPWSTR lpszName,
                                              LPVIEWERPLUGINFILEINFOW lpVPFileInfo,
                                              LPSIZE lpszSize, HANDLE /*hAbortEvent*/)
{
    if (g_refCount == 0 || !lpszName)
        return nullptr;

    auto asset = LoadAsset(lpszName);
    if (!asset)
        return nullptr;

    const cv::Mat& m = asset->frames.empty() ? asset->primaryFrame
                                             : (asset->primaryFrame.empty() ? asset->frames.front()
                                                                            : asset->primaryFrame);
    return MatToDIB(m, lpszSize, lpVPFileInfo);
}

// Stream load: materialize the stream into a temp file (JarkViewer's loader is
// path-based), decode, then remove the temp file.
__declspec(dllexport) HBITMAP DVP_LoadBitmapStreamW(HWND /*hWnd*/, LPSTREAM lpStream,
                                                    LPWSTR lpszName,
                                                    LPVIEWERPLUGINFILEINFOW lpVPFileInfo,
                                                    LPSIZE lpszSize, DWORD /*dwStreamFlags*/)
{
    if (g_refCount == 0 || !lpStream)
        return nullptr;

    // Stream size.
    LARGE_INTEGER zero = {}, end = {};
    ULARGE_INTEGER cur = {};
    if (FAILED(lpStream->Seek(zero, STREAM_SEEK_CUR, &cur)) ||
        FAILED(lpStream->Seek(zero, STREAM_SEEK_END, &end)))
        return nullptr;
    const unsigned long long total = end.QuadPart;
    LARGE_INTEGER back = {};
    back.QuadPart = 0;
    if (FAILED(lpStream->Seek(back, STREAM_SEEK_SET, nullptr)))
        return nullptr;

    if (total < 16 || total > 2ull * 1024 * 1024 * 1024)
        return nullptr;

    wchar_t tmpPath[MAX_PATH * 2] = {};
    wchar_t tmpDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmpDir);
    const wchar_t* baseName = lpszName ? wcsrchr(lpszName, L'\\') : nullptr;
    baseName = baseName ? baseName + 1 : L"stream.heic";
    if (!GetTempFileNameW(tmpDir, L"jvo", 0, tmpPath))
        return nullptr;
    // GetTempFileName gives ".tmp"; rename to the real extension so the
    // loader routes by extension where the header is not authoritative.
    std::wstring realPath = tmpPath;
    realPath = realPath.substr(0, realPath.size() - 4);
    realPath += baseName;

    bool ok = false;
    HBITMAP hbmp = nullptr;
    HANDLE hFile = CreateFileW(realPath.c_str(), GENERIC_WRITE, 0,
                               nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        char buf[65536];
        unsigned long long remaining = total;
        ok = true;
        while (remaining > 0)
        {
            ULONG want = static_cast<ULONG>(std::min<unsigned long long>(remaining, sizeof(buf)));
            ULONG got = 0;
            if (FAILED(lpStream->Read(buf, want, &got)) || got == 0)
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
        {
            auto asset = LoadAsset(realPath);
            if (asset)
            {
                const cv::Mat& m = asset->frames.empty() ? asset->primaryFrame
                                                         : (asset->primaryFrame.empty() ? asset->frames.front()
                                                                                        : asset->primaryFrame);
                hbmp = MatToDIB(m, lpszSize, lpVPFileInfo);
            }
        }
    }

    DeleteFileW(tmpPath);
    DeleteFileW(realPath.c_str());
    return hbmp;
}

} // extern "C"
