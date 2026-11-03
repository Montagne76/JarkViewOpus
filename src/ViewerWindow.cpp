/*
   JarkViewOpus - animated viewer window.

   Protocol (see DirectoryOpusViewerPlugins.h):
     DOpus creates our window via DVP_CreateViewer, then drives it with
     DVPLUGINMSG_LOAD / LOADSTREAM / RESIZE / ZOOM / ROTATE / REDRAW / CLEAR.
     We report image size back with DVPN_SIZECHANGE and paint in WM_PAINT.

   License: GPL-3.0-or-later
*/
#include "ViewerWindow.h"

#include <windowsx.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "JarkCore.h"
#include "../jark/include/jarkUtils.h"
#include "../sdk/DirectoryOpusViewerPlugins.h"

namespace
{
    const wchar_t kViewerClass[] = L"JarkViewOpusViewerView";
    const UINT_PTR kFrameTimer = 1;

    // Viewer frames are kept at (near) native resolution; only extreme images
    // are capped so a single DIB cannot blow the process up.
    const int kViewerMaxDim = 8192;

    struct ViewerState
    {
        HWND                              hwnd = nullptr;
        std::wstring                      path;
        std::shared_ptr<ImageAsset>       asset;
        std::vector<int>                  durations; // ms per frame
        int                               cur = 0;
        int                               rotation = 0;      // 0/90/180/270
        int                               zoom = ZOOM_FITPAGE;
        int                               panX = 0;          // extra offset from centre, px
        int                               panY = 0;
        bool                              dragging = false;
        POINT                             dragOrigin = {};
        int                               dragPanX = 0;
        int                               dragPanY = 0;
        bool                              animating = false;
        SIZE                              size = { 0, 0 };  // after rotation
        HBITMAP                           bmp = nullptr;     // cached DIB
        int                               bmpFrame = -1;     // frame currently in bmp
        int                               bmpRotation = -1;  // rotation currently in bmp
        bool                              bmpHasAlpha = false;
        int                               tickLog = 0;       // log the first few timer ticks only
    };

    CRITICAL_SECTION          g_listCs;
    bool                      g_listCsReady = false;
    std::vector<ViewerState*> g_viewers;
    HINSTANCE                 g_hInst = nullptr;
    bool                      g_classRegistered = false;

    void EnsureListCs()
    {
        static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
        InitOnceExecuteOnce(&once, [](PINIT_ONCE, PVOID, PVOID*) -> BOOL {
            InitializeCriticalSection(&g_listCs);
            g_listCsReady = true;
            return TRUE;
        }, nullptr, nullptr);
    }

    void FreeBmp(ViewerState* st)
    {
        if (st->bmp)
        {
            DeleteObject(st->bmp);
            st->bmp = nullptr;
        }
        st->bmpFrame = -1;
        st->bmpRotation = -1;
    }

    COLORREF QueryBgCol(HWND hwnd)
    {
        NMHDR hdr = {};
        hdr.hwndFrom = hwnd;
        hdr.idFrom = 0;
        hdr.code = DVPN_GETBGCOL;
        return static_cast<COLORREF>(SendMessageW(GetParent(hwnd), WM_NOTIFY, 0,
                                                  reinterpret_cast<LPARAM>(&hdr)));
    }

    void NotifySize(HWND hwnd, const SIZE& size)
    {
        DVPNMSIZECHANGE n = {};
        n.hdr.hwndFrom = hwnd;
        n.hdr.idFrom = 0;
        n.hdr.code = DVPN_SIZECHANGE;
        n.szSize = size;
        SendMessageW(GetParent(hwnd), WM_NOTIFY, 0, reinterpret_cast<LPARAM>(&n));
    }

    // Rebuild / refresh the cached DIB for the current frame. The DIB itself is
    // reused when the geometry still matches, so switching frames is a memcpy
    // rather than a fresh full-size allocation.
    bool EnsureBmp(ViewerState* st)
    {
        if (!st->asset || st->asset->frames.empty())
            return false;

        if (st->cur < 0 || st->cur >= static_cast<int>(st->asset->frames.size()))
            st->cur = 0;

        if (st->bmp && st->bmpFrame == st->cur && st->bmpRotation == st->rotation)
            return true;

        SIZE sz = { 0, 0 };
        bool alpha = false;
        if (!MatToDIBReuse(st->asset->frames[st->cur], st->rotation, true, kViewerMaxDim,
                           &st->bmp, &sz, &alpha))
            return false;
        st->bmpFrame = st->cur;
        st->bmpRotation = st->rotation;
        st->bmpHasAlpha = alpha;
        st->size = sz;
        return true;
    }

    int DurationOf(const ViewerState* st, int index)
    {
        if (index < 0 || index >= static_cast<int>(st->durations.size()))
            return 100;
        int d = st->durations[index];
        if (d < 20)
            d = 100; // 0/negative durations -> sane default
        return d;
    }

    // ZOOM_FITPAGE / ZOOM_TILED fit the window, ZOOM_ORIGINAL (0) means 1:1,
    // anything else is a percentage. Getting this wrong makes Opus's "original
    // size" button behave like "fit" - which is what it used to do here.
    double ScaleOf(const ViewerState* st, int cw, int ch)
    {
        if (st->size.cx <= 0 || st->size.cy <= 0 || cw <= 0 || ch <= 0)
            return 1.0;
        if (st->zoom == ZOOM_FITPAGE || st->zoom == ZOOM_TILED)
        {
            const double s = std::min(static_cast<double>(cw) / st->size.cx,
                                      static_cast<double>(ch) / st->size.cy);
            return std::min(s, 4.0); // never blow tiny images up past 400%
        }
        if (st->zoom == ZOOM_ORIGINAL || st->zoom < 0)
            return 1.0;
        return st->zoom / 100.0;
    }

    // Keep the image from being dragged off its own centre: when it is smaller
    // than the view it stays centred, otherwise pan is bounded to the overflow.
    void ClampPan(ViewerState* st, int cw, int ch)
    {
        const double scale = ScaleOf(st, cw, ch);
        const int dw = static_cast<int>(st->size.cx * scale + 0.5);
        const int dh = static_cast<int>(st->size.cy * scale + 0.5);
        const int mx = dw > cw ? (dw - cw + 1) / 2 : 0;
        const int my = dh > ch ? (dh - ch + 1) / 2 : 0;
        st->panX = std::clamp(st->panX, -mx, mx);
        st->panY = std::clamp(st->panY, -my, my);
    }

    bool CanDrag(const ViewerState* st, int cw, int ch)
    {
        const double scale = ScaleOf(st, cw, ch);
        const int dw = static_cast<int>(st->size.cx * scale + 0.5);
        const int dh = static_cast<int>(st->size.cy * scale + 0.5);
        return dw > cw || dh > ch;
    }

    void ScheduleFrame(ViewerState* st)
    {
        if (!st->asset || st->asset->frames.size() < 2)
        {
            st->animating = false;
            return;
        }
        st->animating = true;
        const int ms = DurationOf(st, st->cur);
        const UINT_PTR tok = SetTimer(st->hwnd, kFrameTimer, static_cast<UINT>(ms), nullptr);
        JarkLog(L"ScheduleFrame: set timer %d ms -> %llu (hwnd=%p)", ms,
                (unsigned long long)tok, (void*)st->hwnd);
    }

    void StopAnimation(ViewerState* st)
    {
        if (st->hwnd)
            KillTimer(st->hwnd, kFrameTimer);
        st->animating = false;
    }

    void SetAsset(ViewerState* st, std::shared_ptr<ImageAsset> asset, const std::wstring& path)
    {
        StopAnimation(st);
        FreeBmp(st);
        st->asset = std::move(asset);
        st->path = path;
        st->cur = 0;
        st->durations.clear();

        if (!st->asset || st->asset->frames.empty())
        {
            JarkLog(L"SetAsset: empty asset for %s", path.c_str());
            return;
        }

        st->durations.assign(st->asset->frameDurations.begin(), st->asset->frameDurations.end());
        const bool dib = EnsureBmp(st);
        JarkLog(L"SetAsset: %s frames=%zu durations=%zu dib=%d size=%dx%d",
                path.c_str(), st->asset->frames.size(), st->durations.size(),
                dib ? 1 : 0, st->size.cx, st->size.cy);
        if (!dib)
            return;

        NotifySize(st->hwnd, st->size);
        ScheduleFrame(st);
        InvalidateRect(st->hwnd, nullptr, FALSE);
    }

    void Paint(HWND hwnd, ViewerState* st)
    {
        PAINTSTRUCT ps = {};
        HDC dc = BeginPaint(hwnd, &ps);
        if (!dc)
            return;

        RECT client = {};
        GetClientRect(hwnd, &client);

        HBRUSH bg = CreateSolidBrush(QueryBgCol(hwnd));
        FillRect(dc, &client, bg);
        DeleteObject(bg);

        if (!st || !EnsureBmp(st) || st->size.cx <= 0 || st->size.cy <= 0)
        {
            EndPaint(hwnd, &ps);
            return;
        }

        const int cw = client.right - client.left;
        const int ch = client.bottom - client.top;
        if (cw <= 0 || ch <= 0)
        {
            EndPaint(hwnd, &ps);
            return;
        }

        const double scale = ScaleOf(st, cw, ch);

        int dw = static_cast<int>(st->size.cx * scale + 0.5);
        int dh = static_cast<int>(st->size.cy * scale + 0.5);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        const int dx = client.left + (cw - dw) / 2 + st->panX;
        const int dy = client.top + (ch - dh) / 2 + st->panY;

        HDC mem = CreateCompatibleDC(dc);
        if (!mem)
        {
            EndPaint(hwnd, &ps);
            return;
        }
        HGDIOBJ old = SelectObject(mem, st->bmp);

        if (st->bmpHasAlpha)
        {
            BLENDFUNCTION bf = {};
            bf.BlendOp = AC_SRC_OVER;
            bf.SourceConstantAlpha = 255;
            bf.AlphaFormat = AC_SRC_ALPHA; // source is premultiplied
            AlphaBlend(dc, dx, dy, dw, dh, mem, 0, 0, st->size.cx, st->size.cy, bf);
        }
        else
        {
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchBlt(dc, dx, dy, dw, dh, mem, 0, 0, st->size.cx, st->size.cy, SRCCOPY);
        }

        SelectObject(mem, old);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
    }

    void OnLoad(HWND hwnd, ViewerState* st, const wchar_t* path, LPSTREAM stream)
    {
        std::shared_ptr<ImageAsset> asset;
        std::wstring name = path ? path : L"";

        if (stream)
            asset = JarkLoadAssetFromStream(stream, path);
        else if (path && *path)
            asset = JarkLoadAsset(path);

        if (!asset)
        {
            StopAnimation(st);
            FreeBmp(st);
            st->asset.reset();
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        SetAsset(st, std::move(asset), name);
    }

    void FillImageInfo(ViewerState* st, LPVIEWERPLUGINFILEINFOW info)
    {
        if (!info)
            return;
        info->dwFlags = DVPFIF_CanReturnViewer | DVPFIF_CanReturnThumbnail;
        info->wMajorType = DVPMajorType_Image;
        info->wMinorType = 0;
        if (st && st->asset && !st->asset->frames.empty())
        {
            const cv::Mat& m = st->asset->frames.front();
            info->szImageSize.cx = m.cols;
            info->szImageSize.cy = m.rows;
        }
        else
        {
            info->szImageSize.cx = 0;
            info->szImageSize.cy = 0;
        }
        info->iNumBits = 32;
        if (st && st->bmpHasAlpha)
            info->dwFlags |= DVPFIF_HasAlphaChannel;
    }

    void OnRotate(ViewerState* st, int amount)
    {
        if (amount == 0)
            return;
        st->rotation = ((st->rotation + amount) % 360 + 360) % 360;
        st->panX = 0;
        st->panY = 0;
        FreeBmp(st);
        if (EnsureBmp(st))
            NotifySize(st->hwnd, st->size);
        InvalidateRect(st->hwnd, nullptr, FALSE);
    }

    HBITMAP CopyCurrent(ViewerState* st)
    {
        if (!st || !st->asset || st->asset->frames.empty())
            return nullptr;
        SIZE sz = { 0, 0 };
        bool alpha = false;
        return MatToDIB(st->asset->frames[st->cur], st->rotation, true, kViewerMaxDim, &sz, &alpha);
    }

    LRESULT CALLBACK ViewerProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        ViewerState* st = reinterpret_cast<ViewerState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (msg)
        {
        case WM_NCCREATE:
        {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ViewerState* state = static_cast<ViewerState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            JarkLog(L"ViewerWnd: WM_NCCREATE hwnd=%p parent=%p", (void*)hwnd, (void*)cs->hwndParent);
            return TRUE;
        }

        case WM_CREATE:
            JarkLog(L"ViewerWnd: WM_CREATE hwnd=%p", (void*)hwnd);
            return 0;

        case WM_SIZE:
        case WM_WINDOWPOSCHANGED:
            if (st && msg == WM_SIZE)
            {
                RECT rc = {};
                GetClientRect(hwnd, &rc);
                ClampPan(st, rc.right, rc.bottom);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_ERASEBKGND:
            return 1; // we paint the whole client area

        case WM_LBUTTONDOWN:
        {
            if (!st)
                return 0;
            RECT rc = {};
            GetClientRect(hwnd, &rc);
            if (CanDrag(st, rc.right, rc.bottom))
            {
                st->dragging = true;
                st->dragOrigin.x = GET_X_LPARAM(lParam);
                st->dragOrigin.y = GET_Y_LPARAM(lParam);
                st->dragPanX = st->panX;
                st->dragPanY = st->panY;
                SetCapture(hwnd);
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            }
            return 0;
        }

        case WM_MOUSEMOVE:
        {
            if (!st || !st->dragging)
                return 0;
            RECT rc = {};
            GetClientRect(hwnd, &rc);
            st->panX = st->dragPanX + (GET_X_LPARAM(lParam) - st->dragOrigin.x);
            st->panY = st->dragPanY + (GET_Y_LPARAM(lParam) - st->dragOrigin.y);
            ClampPan(st, rc.right, rc.bottom);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONUP:
            if (st && st->dragging)
            {
                st->dragging = false;
                ReleaseCapture();
            }
            return 0;

        case WM_CAPTURECHANGED:
            if (st)
                st->dragging = false;
            return 0;

        case WM_LBUTTONDBLCLK:
            if (st)
            {
                // Toggle between fit-to-window and 1:1. Opus's own double click
                // (usually "close viewer") is not forwarded from here.
                st->zoom = (st->zoom == ZOOM_FITPAGE || st->zoom == ZOOM_TILED)
                               ? ZOOM_ORIGINAL
                               : ZOOM_FITPAGE;
                st->panX = 0;
                st->panY = 0;
                JarkLog(L"dblclk: zoom -> %d", st->zoom);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_SETCURSOR:
            if (st && LOWORD(lParam) == HTCLIENT)
            {
                if (st->dragging)
                {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                    return TRUE;
                }
                RECT rc = {};
                GetClientRect(hwnd, &rc);
                SetCursor(LoadCursorW(nullptr, CanDrag(st, rc.right, rc.bottom) ? IDC_HAND
                                                                                : IDC_ARROW));
                return TRUE;
            }
            break;

        case WM_SETFOCUS:
            // Do not steal focus from Opus (keyboard shortcuts / wheel paging).
            if (GetParent(hwnd) && GetParent(hwnd) != hwnd)
                SetFocus(GetParent(hwnd));
            return 0;

        case WM_PAINT:
            Paint(hwnd, st);
            return 0;

        case WM_TIMER:
            if (wParam == kFrameTimer && st)
            {
                KillTimer(hwnd, kFrameTimer);
                if (st->asset && st->asset->frames.size() > 1)
                {
                    st->cur = (st->cur + 1) % static_cast<int>(st->asset->frames.size());
                    if (st->tickLog < 5)
                    {
                        JarkLog(L"WM_TIMER: frame -> %d/%zu", st->cur, st->asset->frames.size());
                        ++st->tickLog;
                    }
                    // No FreeBmp: EnsureBmp notices cur changed and rewrites the
                    // existing DIB when the geometry matches.
                    InvalidateRect(hwnd, nullptr, FALSE);
                    ScheduleFrame(st);
                }
                else
                {
                    st->animating = false;
                }
            }
            return 0;

        case DVPLUGINMSG_LOADW:
            JarkLog(L"ViewerWnd: LOADW %s", lParam ? reinterpret_cast<const wchar_t*>(lParam) : L"(null)");
            if (st)
                OnLoad(hwnd, st, reinterpret_cast<const wchar_t*>(lParam), nullptr);
            return TRUE;

        case DVPLUGINMSG_LOADSTREAMW:
            if (st)
                OnLoad(hwnd, st, reinterpret_cast<const wchar_t*>(wParam),
                       reinterpret_cast<LPSTREAM>(lParam));
            return TRUE;

        case DVPLUGINMSG_GETIMAGEINFOW:
            if (st)
                FillImageInfo(st, reinterpret_cast<LPVIEWERPLUGINFILEINFOW>(lParam));
            return TRUE;

        case DVPLUGINMSG_GETCAPABILITIES:
            return VPCAPABILITY_RESIZE_FIT | VPCAPABILITY_RESIZE_ANY |
                   VPCAPABILITY_ROTATE_RIGHTANGLE | VPCAPABILITY_SUPPLYBITMAP;

        case DVPLUGINMSG_GETPICSIZE:
        case DVPLUGINMSG_GETORIGINALPICSIZE:
            if (wParam)
                *reinterpret_cast<LPINT>(wParam) = 32;
            if (lParam && st)
                *reinterpret_cast<LPSIZE>(lParam) = st->size;
            return TRUE;

        case DVPLUGINMSG_GETAUTOBGCOL:
            return FALSE;

        case DVPLUGINMSG_REDRAW:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case DVPLUGINMSG_RESIZE:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case DVPLUGINMSG_SETZOOM:
            if (st)
            {
                st->zoom = static_cast<int>(static_cast<INT_PTR>(wParam));
                st->panX = 0;
                st->panY = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return TRUE;

        case DVPLUGINMSG_ZOOM:
            if (st)
            {
                st->zoom = static_cast<int>(static_cast<INT_PTR>(wParam));
                st->panX = 0;
                st->panY = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return TRUE; // Opus treats anything else as "zoom not supported"

        case DVPLUGINMSG_GETZOOMFACTOR:
            return st ? st->zoom : ZOOM_FITPAGE;

        case DVPLUGINMSG_GETZOOMLIMITS:
            return MAKELONG(10, 1000); // min 10%, max 1000%

        case DVPLUGINMSG_SETROTATION:
            if (st)
            {
                st->rotation = ((static_cast<int>(wParam) % 360) + 360) % 360;
                st->panX = 0;
                st->panY = 0;
                FreeBmp(st);
                if (EnsureBmp(st))
                    NotifySize(hwnd, st->size);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case DVPLUGINMSG_ROTATE:
            if (st)
                OnRotate(st, static_cast<int>(wParam));
            return 0;

        case DVPLUGINMSG_GETBITMAP:
            return reinterpret_cast<LRESULT>(CopyCurrent(st));

        case DVPLUGINMSG_CLEAR:
            if (st)
            {
                StopAnimation(st);
                FreeBmp(st);
                st->asset.reset();
                st->cur = 0;
                st->panX = 0;
                st->panY = 0;
                st->dragging = false;
                st->size = { 0, 0 };
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case DVPLUGINMSG_REINITIALIZE:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_NCDESTROY:
        {
            ViewerState* doomed = st;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            if (doomed)
            {
                StopAnimation(doomed);
                FreeBmp(doomed);
                if (g_listCsReady)
                {
                    EnterCriticalSection(&g_listCs);
                    g_viewers.erase(std::remove(g_viewers.begin(), g_viewers.end(), doomed),
                                    g_viewers.end());
                    LeaveCriticalSection(&g_listCs);
                }
                delete doomed;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        default:
            break;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    bool EnsureClass()
    {
        if (g_classRegistered)
            return true;

        HINSTANCE inst = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&ViewerProc), &inst))
            return false;
        g_hInst = inst;

        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc   = ViewerProc;
        wc.hInstance     = inst;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr; // WM_ERASEBKGND handled
        wc.lpszClassName = kViewerClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;

        g_classRegistered = true;
        return true;
    }
}

HWND JarkViewerCreate(HWND hwndParent, const RECT* rc, DWORD /*flags*/)
{
    if (!hwndParent || !EnsureClass())
        return nullptr;

    EnsureListCs();

    ViewerState* st = new (std::nothrow) ViewerState();
    if (!st)
        return nullptr;

    RECT r = {};
    if (rc)
        r = *rc;
    else
        r = RECT{ 0, 0, 320, 240 };

    HWND hwnd = CreateWindowExW(0, kViewerClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                r.left, r.top, std::max<LONG>(1, r.right - r.left),
                                std::max<LONG>(1, r.bottom - r.top),
                                hwndParent, nullptr, g_hInst, st);
    if (!hwnd)
    {
        delete st;
        return nullptr;
    }

    st->hwnd = hwnd;
    if (g_listCsReady)
    {
        EnterCriticalSection(&g_listCs);
        g_viewers.push_back(st);
        LeaveCriticalSection(&g_listCs);
    }
    return hwnd;
}

void JarkViewerDestroyAll()
{
    if (!g_listCsReady)
        return;

    std::vector<HWND> windows;
    EnterCriticalSection(&g_listCs);
    for (ViewerState* st : g_viewers)
    {
        if (st && st->hwnd)
            windows.push_back(st->hwnd);
    }
    LeaveCriticalSection(&g_listCs);

    for (HWND hwnd : windows)
    {
        if (IsWindow(hwnd))
            DestroyWindow(hwnd);
    }
}
