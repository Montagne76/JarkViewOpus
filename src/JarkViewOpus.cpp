/*
   JarkViewOpus - Directory Opus Viewer Plugin
   Decodes via the JarkViewer core (jark006/JarkViewer, GPL-3.0).

   Two rendering paths:
     - Static images  -> DVP_LoadBitmapW returns an HBITMAP, DOpus's internal
                         viewer handles zoom/pan/rotation.
     - Animations     -> DVPFIF_CanReturnViewer makes DOpus create a viewer
                         window (DVP_CreateViewer); we paint and drive frames
                         with a timer (same model as the official GIF plugin).

   Plugin API: Directory Opus Viewer Plugin SDK (c) GP Software.
   License: GPL-3.0-or-later
*/

#include <windows.h>
#include <objidl.h>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <algorithm>
#include <memory>
#include <string>

#include "../jark/include/jarkUtils.h"
#include "../jark/include/FormatSniffer.h"

#include "JarkCore.h"
#include "ViewerWindow.h"

#include "../sdk/DirectoryOpusViewerPlugins.h"

// ---------------------------------------------------------------------------
// Plugin identity
// ---------------------------------------------------------------------------

// {9A7B3F42-6C1D-4E5A-8F2B-3D9E1C7A5B60}
static const GUID GUIDPlugin_JarkViewOpus =
{ 0x9a7b3f42, 0x6c1d, 0x4e5a, { 0x8f, 0x2b, 0x3d, 0x9e, 0x1c, 0x7a, 0x5b, 0x60 } };

static const wchar_t kPluginName[]        = L"JarkViewOpus";
static const wchar_t kPluginDescription[] = L"Modern image formats via JarkViewer core (HEIC/AVIF/JXL/APNG) - JarkViewOpus";
static const wchar_t kPluginCopyright[]   = L"GPL-3.0-or-later";
static const wchar_t kPluginURL[]         = L"https://github.com/Montagne76/JarkViewOpus";

// Claimed extensions. (.webp/.gif intentionally omitted: Opus 13 handles them.)
static const wchar_t kHandleExts[] = L".heic;.heif;.avif;.avifs;.jxl;.apng";

static HINSTANCE        g_hModuleInstance = nullptr;
static CRITICAL_SECTION g_cs;
static int              g_refCount = 0;

static void LeoStringCopy(LPWSTR dst, UINT cchMax, const wchar_t* src)
{
    if (dst && cchMax > 0)
        wcsncpy_s(dst, cchMax, src ? src : L"", _TRUNCATE);
}

// ---------------------------------------------------------------------------
// Format gate
// ---------------------------------------------------------------------------

static bool IsApngExt(const wchar_t* path)
{
    const wchar_t* dot = path ? wcsrchr(path, L'.') : nullptr;
    return dot && _wcsicmp(dot, L".apng") == 0;
}

static bool SniffSupported(const uint8_t* d, size_t n, bool allowPng)
{
    if (n < 12)
        return false;
    try
    {
        switch (jark::sniffFileFormat(std::span<const uint8_t>(d, n)))
        {
        case jark::FileFormat::Heif:
        case jark::FileFormat::Avif:
        case jark::FileFormat::Jxl:
            return true;
        case jark::FileFormat::Png:
            return allowPng; // APNG only; plain .png is left to Opus
        default:
            return false;
        }
    }
    catch (...)
    {
        return false;
    }
}

static void FillFileInfo(LPVIEWERPLUGINFILEINFOW lpVPFileInfo, const SIZE& size, bool animated,
                         bool hasAlpha)
{
    DWORD flags = DVPFIF_CanReturnBitmap | DVPFIF_CanReturnThumbnail;
    if (animated)
        flags |= DVPFIF_CanReturnViewer;
    if (hasAlpha)
        flags |= DVPFIF_HasAlphaChannel;

    lpVPFileInfo->dwFlags    = flags;
    lpVPFileInfo->wMajorType = DVPMajorType_Image;
    lpVPFileInfo->wMinorType = 0;
    lpVPFileInfo->szImageSize = size;
    lpVPFileInfo->iNumBits   = 32;
}

static bool ReadHeader(const wchar_t* path, uint8_t* buf, size_t cap, size_t* got)
{
    *got = 0;
    HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;
    DWORD read = 0;
    const BOOL ok = ReadFile(hFile, buf, static_cast<DWORD>(cap), &read, nullptr);
    CloseHandle(hFile);
    *got = read;
    return ok && read > 0;
}

// ---------------------------------------------------------------------------
// Exports
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
    const int left = g_refCount;
    LeaveCriticalSection(&g_cs);

    if (left == 0)
    {
        // Order matters: windows hold assets that live in the DB cache, and the
        // DB owns the preload thread that must be joined before unload.
        JarkViewerDestroyAll();
        JarkShutdownDB();
    }
}

__declspec(dllexport) BOOL DVP_USBSafe(LPOPUSUSBSAFEDATA /*pUSBSafeData*/)
{
    return TRUE;
}

__declspec(dllexport) BOOL DVP_IdentifyW(LPVIEWERPLUGININFOW lpVPInfo)
{
    if (!lpVPInfo || lpVPInfo->cbSize < VIEWERPLUGININFO_V4_SIZE)
        return FALSE;

    lpVPInfo->dwFlags = DVPFIF_CanHandleStreams
                      | DVPFIF_CanHandleBytes
                      | DVPFIF_ExtensionsOnlyIfSlow
                      | DVPFIF_ExtensionsOnlyIfNoRndSeek;

    LeoStringCopy(lpVPInfo->lpszHandleExts,  lpVPInfo->cchHandleExtsMax,  kHandleExts);
    LeoStringCopy(lpVPInfo->lpszName,        lpVPInfo->cchNameMax,        kPluginName);
    LeoStringCopy(lpVPInfo->lpszDescription, lpVPInfo->cchDescriptionMax, kPluginDescription);
    LeoStringCopy(lpVPInfo->lpszCopyright,   lpVPInfo->cchCopyrightMax,   kPluginCopyright);
    LeoStringCopy(lpVPInfo->lpszURL,         lpVPInfo->cchURLMax,         kPluginURL);

    lpVPInfo->dwlMinFileSize    = 16;
    lpVPInfo->uiMajorFileType   = DVPMajorType_Image;
    lpVPInfo->idPlugin          = GUID_JarkViewOpus;
    return TRUE;
}

// Identify a local file: gate by magic bytes, decode once (result lands in the
// LRU so LoadBitmap / the viewer window reuse it) and report the real size.
// Animations additionally advertise DVPFIF_CanReturnViewer so DOpus creates
// our painting window instead of its static image view.
__declspec(dllexport) BOOL DVP_IdentifyFileW(HWND /*hWnd*/, LPWSTR lpszName,
                                             LPVIEWERPLUGINFILEINFOW lpVPFileInfo,
                                             HANDLE /*hAbortEvent*/)
{
    if (g_refCount == 0 || !lpVPFileInfo || !lpszName)
        return FALSE;

    uint8_t header[96] = {};
    size_t got = 0;
    if (!ReadHeader(lpszName, header, sizeof(header), &got))
        return FALSE;
    if (!SniffSupported(header, got, IsApngExt(lpszName)))
        return FALSE;

    auto asset = JarkLoadAsset(lpszName);
    if (!asset)
        return FALSE;

    const cv::Mat& m = CurrentFrameOf(*asset);
    if (m.empty())
        return FALSE;

    FillFileInfo(lpVPFileInfo, SIZE{ m.cols, m.rows }, IsAnimatedAsset(*asset), m.channels() == 4);
    return TRUE;
}

// Stream identify (archive / FTP members): sniff only, size unknown until load.
__declspec(dllexport) BOOL DVP_IdentifyFileStreamW(HWND /*hWnd*/, LPSTREAM lpStream,
                                                   LPWSTR lpszName,
                                                   LPVIEWERPLUGINFILEINFOW lpVPFileInfo,
                                                   DWORD /*dwStreamFlags*/)
{
    if (g_refCount == 0 || !lpStream || !lpVPFileInfo)
        return FALSE;

    LARGE_INTEGER zero = {};
    ULARGE_INTEGER cur = {};
    if (FAILED(lpStream->Seek(zero, STREAM_SEEK_CUR, &cur)))
        return FALSE;
    LARGE_INTEGER restore = {};
    restore.QuadPart = static_cast<LONGLONG>(cur.QuadPart);

    uint8_t header[96] = {};
    ULONG read = 0;
    HRESULT hr = lpStream->Read(header, sizeof(header), &read);
    lpStream->Seek(restore, STREAM_SEEK_SET, nullptr);
    if (FAILED(hr) || !SniffSupported(header, read, IsApngExt(lpszName)))
        return FALSE;

    // We cannot know the frame count without a full decode, and streaming a
    // whole animation just to identify it is wasteful. Advertise the viewer so
    // animation works for archives too; stills simply paint one frame.
    FillFileInfo(lpVPFileInfo, SIZE{ 0, 0 }, true, false);
    return TRUE;
}

// Static path: first frame as a 32bpp DIB (DOpus scales/pans it).
__declspec(dllexport) HBITMAP DVP_LoadBitmapW(HWND /*hWnd*/, LPWSTR lpszName,
                                              LPVIEWERPLUGINFILEINFOW lpVPFileInfo,
                                              LPSIZE lpszSize, HANDLE /*hAbortEvent*/)
{
    if (g_refCount == 0 || !lpszName)
        return nullptr;

    auto asset = JarkLoadAsset(lpszName);
    if (!asset)
        return nullptr;

    SIZE sz = { 0, 0 };
    bool alpha = false;
    HBITMAP hbmp = MatToDIB(CurrentFrameOf(*asset), 0, false, 4096, &sz, &alpha);
    if (!hbmp)
        return nullptr;

    if (lpVPFileInfo && alpha)
        lpVPFileInfo->dwFlags |= DVPFIF_HasAlphaChannel;
    if (lpszSize)
        *lpszSize = sz;
    return hbmp;
}

__declspec(dllexport) HBITMAP DVP_LoadBitmapStreamW(HWND /*hWnd*/, LPSTREAM lpStream,
                                                    LPWSTR lpszName,
                                                    LPVIEWERPLUGINFILEINFOW lpVPFileInfo,
                                                    LPSIZE lpszSize, DWORD /*dwStreamFlags*/)
{
    if (g_refCount == 0 || !lpStream)
        return nullptr;

    auto asset = JarkLoadAssetFromStream(lpStream, lpszName);
    if (!asset)
        return nullptr;

    SIZE sz = { 0, 0 };
    bool alpha = false;
    HBITMAP hbmp = MatToDIB(CurrentFrameOf(*asset), 0, false, 4096, &sz, &alpha);
    if (!hbmp)
        return nullptr;

    if (lpVPFileInfo && alpha)
        lpVPFileInfo->dwFlags |= DVPFIF_HasAlphaChannel;
    if (lpszSize)
        *lpszSize = sz;
    return hbmp;
}

// Animated path: DOpus creates this window for files where identify set
// DVPFIF_CanReturnViewer. All frame handling lives in ViewerWindow.cpp.
__declspec(dllexport) HWND DVP_CreateViewer(HWND hWnd, LPRECT lpRc, DWORD dwFlags)
{
    if (g_refCount == 0)
        return nullptr;
    return JarkViewerCreate(hWnd, lpRc, dwFlags);
}

} // extern "C"
