/*
   JarkViewOpus - Directory Opus Viewer Plugin
   Phase 0 skeleton: HEIC/HEIF/AVIF/JXL identification + WIC-based decode.

   Decode pipeline will be replaced by the JarkViewer core (GPL-3.0) in Phase 1.
   Plugin API: Directory Opus Viewer Plugin SDK (c) GP Software.

   License: GPL-3.0-or-later
*/

#include <windows.h>
#include <objidl.h>
#include <wincodec.h>
#include <cstring>
#include <algorithm>

#include "../sdk/DirectoryOpusViewerPlugins.h"

// ---------------------------------------------------------------------------
// Plugin identity
// ---------------------------------------------------------------------------

// {9A7B3F42-6C1D-4E5A-8F2B-3D9E1C7A5B60}
static const GUID GUIDPlugin_JarkViewOpus =
{ 0x9a7b3f42, 0x6c1d, 0x4e5a, { 0x8f, 0x2b, 0x3d, 0x9e, 0x1c, 0x7a, 0x5b, 0x60 } };

static const wchar_t kPluginName[]        = L"JarkViewOpus";
static const wchar_t kPluginDescription[] = L"Modern image formats (HEIC/AVIF/JPEG XL) - JarkViewOpus";
static const wchar_t kPluginCopyright[]   = L"GPL-3.0-or-later";
static const wchar_t kPluginURL[]         = L"https://github.com/Montagne76/JarkViewOpus";

// Phase 0: WIC-only formats. (.webp intentionally omitted: Opus 13 handles
// WebP internally and WIC WebP needs extra video extensions anyway.)
static const wchar_t kHandleExts[] = L".heic;.heif;.avif;.avifs;.jxl";

static HINSTANCE       g_hModuleInstance = nullptr;
static CRITICAL_SECTION g_cs;
static int             g_refCount = 0;

// Minimal local helpers (no dependency on Leo's common code).
struct LeoScopeLock
{
    LeoScopeLock()  { EnterCriticalSection(&g_cs); }
    ~LeoScopeLock() { LeaveCriticalSection(&g_cs); }
};

static void LeoStringCopy(LPWSTR dst, UINT cchMax, const wchar_t* src)
{
    if (dst && cchMax > 0)
    {
        wcsncpy_s(dst, cchMax, src ? src : L"", _TRUNCATE);
    }
}

// ---------------------------------------------------------------------------
// Format sniffing (magic bytes; logic mirrors JarkViewer's FormatSniffer)
// ---------------------------------------------------------------------------

enum class Fmt { None, Heif, Avif, Jxl, WebP };

static Fmt SniffFormat(const uint8_t* d, size_t n)
{
    // WebP: RIFF....WEBP
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4))
        return Fmt::WebP;

    // JPEG XL naked codestream
    if (n >= 2 && d[0] == 0xFF && d[1] == 0x0A)
        return Fmt::Jxl;

    // ISO BMFF family: scan for the 'ftyp' box within the first 96 bytes,
    // then match compatible brands (vendors may add extra leading boxes).
    const size_t limit = std::min<size_t>(n, 96);
    for (size_t off = 0; off + 12 <= limit; ++off)
    {
        if (memcmp(d + off + 4, "ftyp", 4) != 0)
            continue;

        uint32_t boxSize = (static_cast<uint32_t>(d[off]) << 24) |
                           (static_cast<uint32_t>(d[off + 1]) << 16) |
                           (static_cast<uint32_t>(d[off + 2]) << 8) |
                            static_cast<uint32_t>(d[off + 3]);
        if (boxSize < 16)
            boxSize = 16;

        const size_t brandEnd = std::min({ off + boxSize, n, limit });
        for (size_t b = off + 8; b + 4 <= brandEnd; b += 4)
        {
            if (!memcmp(d + b, "avif", 4) || !memcmp(d + b, "avis", 4))
                return Fmt::Avif;
            if (!memcmp(d + b, "jxl ", 4))
                return Fmt::Jxl;
            static const char* kHeifBrands[] = {
                "heic", "heix", "hevc", "hevx", "heim", "heis",
                "hevm", "hevs", "heif", "mif1", "msf1" };
            for (const char* brand : kHeifBrands)
                if (!memcmp(d + b, brand, 4))
                    return Fmt::Heif;
        }
        break; // only the leading 'ftyp' box is relevant
    }
    return Fmt::None;
}

// ---------------------------------------------------------------------------
// WIC helpers
// ---------------------------------------------------------------------------

struct ScopedCOM
{
    HRESULT hr;
    explicit ScopedCOM(DWORD model = COINIT_MULTITHREADED)
        : hr(CoInitializeEx(nullptr, model)) {} // RPC_E_CHANGED_MODE is tolerated
    ~ScopedCOM() { if (SUCCEEDED(hr)) CoUninitialize(); }
    bool ok() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

static bool PixelFormatHasAlpha(REFGUID fmt)
{
    return fmt == GUID_WICPixelFormat32bppBGRA || fmt == GUID_WICPixelFormat32bppRGBA ||
           fmt == GUID_WICPixelFormat32bppPRGBA || fmt == GUID_WICPixelFormat32bppPBGRA ||
           fmt == GUID_WICPixelFormat64bppRGBA || fmt == GUID_WICPixelFormat64bppBGRA ||
           fmt == GUID_WICPixelFormat64bppPRGBA || fmt == GUID_WICPixelFormat64bppPBGRA ||
           fmt == GUID_WICPixelFormat40bppCMYKAlpha || fmt == GUID_WICPixelFormat80bppCMYKAlpha;
}

// Decode the first frame of a decoder into a 32bpp top-down DIB.
static HBITMAP DecodeFirstFrame(IWICImagingFactory* factory, IWICBitmapDecoder* decoder,
                                LPSIZE lpszSize, LPVIEWERPLUGINFILEINFOW lpVPFileInfo)
{
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    HBITMAP hBitmap = nullptr;
    HDC hdc = nullptr;
    void* bits = nullptr;

    do
    {
        if (FAILED(decoder->GetFrame(0, &frame)))
            break;

        GUID srcFmt = {};
        BOOL hasAlpha = FALSE;
        if (SUCCEEDED(frame->GetPixelFormat(&srcFmt)))
            hasAlpha = PixelFormatHasAlpha(srcFmt);

        UINT w = 0, h = 0;
        if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0)
            break;

        if (FAILED(factory->CreateFormatConverter(&converter)))
            break;
        if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom)))
            break;

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = static_cast<LONG>(w);
        bmi.bmiHeader.biHeight      = -static_cast<LONG>(h); // top-down
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        hdc = CreateCompatibleDC(nullptr);
        if (!hdc)
            break;
        hBitmap = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!hBitmap || !bits)
            break;

        const size_t rowBytes = static_cast<size_t>(w) * 4;
        if (FAILED(converter->CopyPixels(nullptr, static_cast<UINT>(rowBytes),
                                         static_cast<UINT>(rowBytes * h),
                                         static_cast<BYTE*>(bits))))
        {
            DeleteObject(hBitmap);
            hBitmap = nullptr;
            break;
        }

        if (lpszSize)
        {
            lpszSize->cx = static_cast<LONG>(w);
            lpszSize->cy = static_cast<LONG>(h);
        }
        if (lpVPFileInfo && hasAlpha)
            lpVPFileInfo->dwFlags |= DVPFIF_HasAlphaChannel;
    } while (false);

    if (hdc)
        DeleteDC(hdc);
    if (converter)
        converter->Release();
    if (frame)
        frame->Release();
    return hBitmap;
}

static bool WicCanDecodeFile(LPCWSTR path, IWICImagingFactory* factory, SIZE* sizeOut)
{
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    bool ok = false;
    if (SUCCEEDED(factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
                                                     WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)))
    {
        UINT w = 0, h = 0;
        if (SUCCEEDED(frame->GetSize(&w, &h)) && w > 0 && h > 0)
        {
            if (sizeOut) { sizeOut->cx = static_cast<LONG>(w); sizeOut->cy = static_cast<LONG>(h); }
            ok = true;
        }
    }
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    return ok;
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
    LeoScopeLock lock;
    ++g_refCount;
    return TRUE;
}

__declspec(dllexport) void DVP_Uninit(void)
{
    LeoScopeLock lock;
    if (g_refCount > 0)
        --g_refCount;
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

// Identify a local file: magic bytes first, then require that WIC really has
// a codec for it (so files we cannot decode pass through to other viewers).
__declspec(dllexport) BOOL DVP_IdentifyFileW(HWND /*hWnd*/, LPWSTR lpszName,
                                             LPVIEWERPLUGINFILEINFOW lpVPFileInfo,
                                             HANDLE /*hAbortEvent*/)
{
    if (g_refCount == 0 || !lpVPFileInfo)
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

    if (SniffFormat(header, got) == Fmt::None)
        return FALSE;

    ScopedCOM com;
    if (!com.ok())
        return FALSE;
    IWICImagingFactory* factory = nullptr;
    BOOL result = FALSE;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&factory))))
    {
        SIZE size = {};
        if (WicCanDecodeFile(lpszName, factory, &size))
        {
            FillFileInfo(lpVPFileInfo, size, 32);
            result = TRUE;
        }
        factory->Release();
    }
    return result;
}

__declspec(dllexport) BOOL DVP_IdentifyFileStreamW(HWND /*hWnd*/, LPSTREAM lpStream,
                                                   LPWSTR /*lpszName*/,
                                                   LPVIEWERPLUGINFILEINFOW lpVPFileInfo,
                                                   DWORD /*dwStreamFlags*/)
{
    if (g_refCount == 0 || !lpStream || !lpVPFileInfo)
        return FALSE;

    // Remember position, read the header, restore position.
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
    if (FAILED(hr) || SniffFormat(header, read) == Fmt::None)
        return FALSE;

    ScopedCOM com;
    if (!com.ok())
        return FALSE;
    IWICImagingFactory* factory = nullptr;
    BOOL result = FALSE;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&factory))))
    {
        IWICBitmapDecoder* decoder = nullptr;
        if (SUCCEEDED(factory->CreateDecoderFromStream(lpStream, nullptr,
                                                       WICDecodeMetadataCacheOnDemand, &decoder)))
        {
            IWICBitmapFrameDecode* frame = nullptr;
            if (SUCCEEDED(decoder->GetFrame(0, &frame)))
            {
                UINT w = 0, h = 0;
                if (SUCCEEDED(frame->GetSize(&w, &h)) && w > 0 && h > 0)
                {
                    FillFileInfo(lpVPFileInfo, SIZE{ static_cast<LONG>(w), static_cast<LONG>(h) }, 32);
                    result = TRUE;
                }
                frame->Release();
            }
            decoder->Release();
        }
        factory->Release();
    }
    return result;
}

__declspec(dllexport) HBITMAP DVP_LoadBitmapW(HWND /*hWnd*/, LPWSTR lpszName,
                                              LPVIEWERPLUGINFILEINFOW lpVPFileInfo,
                                              LPSIZE lpszSize, HANDLE /*hAbortEvent*/)
{
    if (g_refCount == 0)
        return nullptr;

    ScopedCOM com;
    if (!com.ok())
        return nullptr;
    IWICImagingFactory* factory = nullptr;
    HBITMAP hbmp = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&factory))))
    {
        IWICBitmapDecoder* decoder = nullptr;
        if (SUCCEEDED(factory->CreateDecoderFromFilename(lpszName, nullptr, GENERIC_READ,
                                                         WICDecodeMetadataCacheOnDemand, &decoder)))
        {
            hbmp = DecodeFirstFrame(factory, decoder, lpszSize, lpVPFileInfo);
            decoder->Release();
        }
        factory->Release();
    }
    return hbmp;
}

__declspec(dllexport) HBITMAP DVP_LoadBitmapStreamW(HWND /*hWnd*/, LPSTREAM lpStream,
                                                    LPWSTR /*lpszName*/,
                                                    LPVIEWERPLUGINFILEINFOW lpVPFileInfo,
                                                    LPSIZE lpszSize, DWORD /*dwStreamFlags*/)
{
    if (g_refCount == 0)
        return nullptr;

    ScopedCOM com;
    if (!com.ok())
        return nullptr;
    IWICImagingFactory* factory = nullptr;
    HBITMAP hbmp = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&factory))))
    {
        IWICBitmapDecoder* decoder = nullptr;
        if (SUCCEEDED(factory->CreateDecoderFromStream(lpStream, nullptr,
                                                       WICDecodeMetadataCacheOnDemand, &decoder)))
        {
            hbmp = DecodeFirstFrame(factory, decoder, lpszSize, lpVPFileInfo);
            decoder->Release();
        }
        factory->Release();
    }
    return hbmp;
}

} // extern "C"
