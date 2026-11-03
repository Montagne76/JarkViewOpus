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
#include <vector>

#include "../jark/include/jarkUtils.h"
#include "../jark/include/FormatSniffer.h"
#include "../jark/include/MediaPlayer.h" // complete type of jark::VideoSource

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
static const wchar_t kPluginDescription[] = L"Modern image formats via JarkViewer core (HEIC/AVIF/JXL/APNG/WebP/Live Photo) - JarkViewOpus";
static const wchar_t kPluginCopyright[]   = L"GPL-3.0-or-later";
static const wchar_t kPluginURL[]         = L"https://github.com/Montagne76/JarkViewOpus";

// Claimed extensions. .gif stays out entirely: Opus's gifanim plugin already
// handles it. .webp / .jpg are listed but gated in SniffSupported - any file a
// plugin claims must be decoded by us, so only the variants Opus cannot show
// correctly (animated WebP, Motion Photo JPEG) are claimed; stills stay on
// Opus's faster internal path.
static const wchar_t kHandleExts[] =
    L".heic;.heif;.avif;.avifs;.jxl;.apng;.livp;.webp;.jpg;.jpeg";

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

static bool HasExt(const wchar_t* path, const wchar_t* ext)
{
    const wchar_t* dot = path ? wcsrchr(path, L'.') : nullptr;
    return dot && _wcsicmp(dot, ext) == 0;
}

// APNG carries an "acTL" (animation control) chunk before IDAT. A lot of APNGs
// in the wild are saved as plain .png, and Opus's internal decoding would then
// silently show frame 1 only - so claim PNGs that carry acTL regardless of name.
static bool HasAnimChunk(const uint8_t* d, size_t n)
{
    static const char kActl[4] = { 'a', 'c', 'T', 'L' };
    if (n < 41)
        return false;
    for (size_t i = 8; i + 4 <= n; ++i)
    {
        if (memcmp(d + i, kActl, 4) == 0)
            return true;
    }
    return false;
}

// Animated WebP: RIFF/WEBP container whose first chunk is VP8X and whose
// extended header sets the animation flag (bit 1). Static WebP files use VP8/
// VP8L instead and have no VP8X at all.
static bool IsAnimatedWebp(const uint8_t* d, size_t n)
{
    if (n < 30 || memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "WEBP", 4) != 0)
        return false;
    if (memcmp(d + 12, "VP8X", 4) != 0)
        return false;
    if (d[20] & 0x02) // ANIMATION flag in the VP8X header
        return true;

    // Some muxers leave the flag clear; fall back to the chunk list.
    bool anim = false;
    bool frame = false;
    for (size_t i = 12; i + 4 <= n; ++i)
    {
        if (memcmp(d + i, "ANIM", 4) == 0)
            anim = true;
        else if (memcmp(d + i, "ANMF", 4) == 0)
            frame = true;
    }
    return anim && frame;
}

// Same-name sidecar video (Apple / VIVO export the clip next to the photo).
static bool HasSidecarVideo(const wchar_t* path)
{
    if (!path || !*path)
        return false;

    std::wstring base(path);
    const size_t slash = base.find_last_of(L"\\/");
    const size_t dot   = base.find_last_of(L'.');
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
        base.resize(dot);

    static const wchar_t* kVideoExts[] = { L".mov", L".MOV", L".mp4", L".MP4" };
    for (const wchar_t* ext : kVideoExts)
    {
        const DWORD attr = GetFileAttributesW((base + ext).c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
            return true;
    }
    return false;
}

// Android / Samsung Motion Photo markers. The XMP packet sits in an APP1
// segment, i.e. within the first kilobytes of the file, so the head buffer is
// enough to decide without scanning the whole JPEG.
static bool HasMotionMarker(const uint8_t* d, size_t n)
{
    static const char* kMarks[] = {
        "MotionPhoto=\"1\"",                 // Android / Samsung XMP
        "Item:Semantic=\"MotionPhoto\"",     // Android XMP container directory
        "MicroVideo=\"1\"",                  // older Android (MicroVideo)
        "MicroVideoOffset",                  // older Android (MicroVideo)
        "MotionPhoto_Data",                  // Samsung SEFT trailer
    };
    for (const char* mark : kMarks)
    {
        const size_t len = strlen(mark);
        if (len == 0 || n < len)
            continue;
        for (size_t i = 0; i + len <= n; ++i)
        {
            if (memcmp(d + i, mark, len) == 0)
                return true;
        }
    }
    return false;
}

static bool IsExt(const wchar_t* path, const wchar_t* ext)
{
    const wchar_t* dot = path ? wcsrchr(path, L'.') : nullptr;
    return dot && _wcsicmp(dot, ext) == 0;
}

static bool SniffSupported(const uint8_t* d, size_t n, const wchar_t* path)
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
            // .apng: trust the extension. .png (or anything else): only when the
            // animation control chunk proves it is an APNG; plain PNGs stay with
            // Opus's faster internal path.
            return HasExt(path, L".apng") || HasAnimChunk(d, n);
        case jark::FileFormat::WebP:
            // Opus decodes static WebP itself and shows frame 1 only for
            // animated ones, so claim the animation exclusively.
            return IsAnimatedWebp(d, n);
        case jark::FileFormat::Jpeg:
            // A plain JPEG decodes faster inside Opus than through our core, so
            // only claim files that prove they are a Motion Photo.
            if (!IsExt(path, L".jpg") && !IsExt(path, L".jpeg"))
                return false;
            return HasMotionMarker(d, n) || HasSidecarVideo(path);
        case jark::FileFormat::Livp:
            // Zip container, but only .livp files are Live Photo packages.
            return IsExt(path, L".livp");
        default:
            return false;
        }
    }
    catch (...)
    {
        return false;
    }
}

static void FillFileInfo(LPVIEWERPLUGINFILEINFOW lpVPFileInfo, const SIZE& size, bool needViewer,
                         bool hasAlpha)
{
    // Animations and Live Photos must NOT advertise CanReturnBitmap: if both are
    // offered Opus takes the cheap static path (DVP_LoadBitmapW) and never asks
    // for a viewer window, so the picture stays on frame 1 forever. The official
    // gifanim plugin advertises CanReturnThumbnail|CanReturnViewer only, for
    // exactly this reason.
    DWORD flags = DVPFIF_CanReturnThumbnail;
    if (needViewer)
        flags |= DVPFIF_CanReturnViewer;
    else
        flags |= DVPFIF_CanReturnBitmap;
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
        // DB owns the preload thread that must be joined before unload. Live
        // photo players start on workers, so wait for those in between.
        JarkViewerDestroyAll();
        JarkViewerFlushLive();
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
                      | DVPFIF_OverrideInternal
                      | DVPFIF_ExtensionsOnlyIfSlow
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

// Identify a local file: gate by magic bytes, decode once (result lands in the
// LRU so LoadBitmap / the viewer window reuse it) and report the real size.
// Animations and Live Photos additionally advertise DVPFIF_CanReturnViewer so
// DOpus creates our painting window instead of its static image view.
__declspec(dllexport) BOOL DVP_IdentifyFileW(HWND /*hWnd*/, LPWSTR lpszName,
                                             LPVIEWERPLUGINFILEINFOW lpVPFileInfo,
                                             HANDLE /*hAbortEvent*/)
{
    if (g_refCount == 0 || !lpVPFileInfo || !lpszName)
        return FALSE;

    // 128 KiB: enough for a JPEG's XMP packet, where the Motion Photo markers
    // live. Header sniffing alone would not see them in the first 96 bytes.
    std::vector<uint8_t> header(128 * 1024);
    size_t got = 0;
    if (!ReadHeader(lpszName, header.data(), header.size(), &got))
    {
        JarkLog(L"IdentifyFileW: cannot read header (%s)", lpszName);
        return FALSE;
    }
    if (!SniffSupported(header.data(), got, lpszName))
    {
        JarkLog(L"IdentifyFileW: sniff rejected (%s)", lpszName);
        return FALSE;
    }

    auto asset = JarkLoadAsset(lpszName);
    if (!asset)
    {
        JarkLog(L"IdentifyFileW: decode failed (%s)", lpszName);
        return FALSE;
    }

    const cv::Mat& m = CurrentFrameOf(*asset);
    if (m.empty())
        return FALSE;

    const bool animated = IsAnimatedAsset(*asset);
    const bool live     = IsLivePhotoAsset(*asset);
    // Both need a viewer window: an animation has frames to push, a live photo
    // has a clip to stream.
    FillFileInfo(lpVPFileInfo, SIZE{ m.cols, m.rows }, animated || live,
                 m.channels() == 4);
    JarkLog(L"IdentifyFileW: OK %s frames=%zu live=%d video=%zu bytes size=%dx%d flags=0x%08X",
            lpszName, asset->frames.size(), live ? 1 : 0,
            asset->videoSource ? asset->videoSource->data.size() : 0,
            m.cols, m.rows, lpVPFileInfo->dwFlags);
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
    if (FAILED(hr) || !SniffSupported(header, read, lpszName))
        return FALSE;

    // We cannot know the frame count without a full decode, and streaming a
    // whole animation just to identify it is wasteful. Advertise the viewer so
    // animation works for archives too; stills simply paint one frame.
    // Motion Photo JPEGs are not detected here on purpose: they need a head
    // scan for the XMP marker, and moving the stream position around for a
    // member inside an archive is not worth it.
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

    JarkLog(L"LoadBitmapW: %s %dx%d (frames=%zu)", lpszName, sz.cx, sz.cy,
            asset->frames.size());

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
    HWND h = JarkViewerCreate(hWnd, lpRc, dwFlags);
    JarkLog(L"CreateViewer: parent=%p rect=(%ld,%ld,%ld,%ld) flags=0x%lX -> %p",
            (void*)hWnd, lpRc ? lpRc->left : 0, lpRc ? lpRc->top : 0,
            lpRc ? lpRc->right : 0, lpRc ? lpRc->bottom : 0,
            (unsigned long)dwFlags, (void*)h);
    return h;
}

} // extern "C"
