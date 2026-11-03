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
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "JarkCore.h"
#include "../jark/include/jarkUtils.h"
#include "../jark/include/MediaPlayer.h"
#include "../sdk/DirectoryOpusViewerPlugins.h"

namespace
{
    const wchar_t kViewerClass[] = L"JarkViewOpusViewerView";
    const UINT_PTR kFrameTimer = 1;   // animation frame advance
    const UINT_PTR kLiveTimer  = 2;   // poll the live-photo player

    // Viewer frames are kept at (near) native resolution; only extreme images
    // are capped so a single DIB cannot blow the process up.
    const int kViewerMaxDim = 8192;

    // Live Photo video is decoded on demand, so it does not need native
    // resolution: capping it keeps the per-frame BGR->BGRA conversion cheap
    // enough to run inside the UI thread at 30fps.
    const int kLiveMaxDim  = 2048;
    const int kLivePollMs  = 10;      // ~60Hz; the player itself paces frames

    // MediaPlayer::start() opens the audio device, and on a Bluetooth output
    // that costs 400-800ms here. Calling it on the UI thread freezes Opus, so
    // it runs on a worker while the still image stays on screen; the timer only
    // starts reading frames once the worker reports ready.
    struct LiveSession
    {
        std::shared_ptr<jark::MediaPlayer> player;
        std::shared_ptr<ImageAsset>        keepAlive;  // owns the video bytes
        std::atomic<bool>                  started{ false };
        std::atomic<bool>                  cancelled{ false };
    };

    // Workers in flight, so DVP_Uninit can wait for them before the DLL goes
    // away. Every worker touches FFmpeg, so nothing may outlive the module.
    std::mutex              g_liveMtx;
    std::condition_variable g_liveCv;
    int                     g_liveRunning = 0;

    void LiveBegin()
    {
        std::lock_guard<std::mutex> lk(g_liveMtx);
        ++g_liveRunning;
    }

    void LiveEnd()
    {
        {
            std::lock_guard<std::mutex> lk(g_liveMtx);
            --g_liveRunning;
        }
        g_liveCv.notify_all();
    }

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
        SIZE                              baseSize = { 0, 0 };// still size at rotation 0
        SIZE                              size = { 0, 0 };  // layout size, after rotation
        HBITMAP                           bmp = nullptr;     // cached DIB
        SIZE                              bmpSize = { 0, 0 };// DIB pixel size; can differ from size
        uint64_t                          frameGen = 0;      // bumped whenever the source frame changes
        uint64_t                          bmpGen = 0;        // frameGen the DIB currently holds
        int                               bmpRotation = -1;  // rotation currently in bmp
        bool                              bmpHasAlpha = false;
        int                               tickLog = 0;       // log the first few timer ticks only

        // Backing store. Painting straight onto the window DC exposed the bare
        // background between the FillRect and the (slow, stretched) frame blit,
        // which read as a white flash on every video frame.
        HDC                               backDc = nullptr;
        HBITMAP                           backBmp = nullptr;
        HGDIOBJ                           backOld = nullptr;
        int                               backW = 0;
        int                               backH = 0;
        COLORREF                          bgCol = CLR_INVALID;
        HBRUSH                            bgBrush = nullptr;

        // Live Photo / Motion Photo
        std::shared_ptr<LiveSession>      session;
        bool                              live = false;      // asset carries playable video
        bool                              playing = false;
        bool                              playedOnce = false;
        cv::Mat                           videoFrame;        // latest decoded frame; empty = show still
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
        st->bmpGen = 0;
        st->bmpRotation = -1;
        st->bmpSize = { 0, 0 };
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

    // Layout size = the still photo's size, rotated. Live-photo video keeps
    // this layout so the pane does not resize when the clip starts (the clip is
    // usually smaller than the photo and gets fitted inside).
    SIZE LayoutSize(const ViewerState* st)
    {
        SIZE s = st->baseSize;
        if (st->rotation == 90 || st->rotation == 270)
        {
            const LONG t = s.cx;
            s.cx = s.cy;
            s.cy = t;
        }
        return s;
    }

    // The picture to show right now, in priority order: the live-photo video
    // frame that is currently playing -> the current animation frame -> the
    // still (Motion Photo assets carry primaryFrame, not frames).
    const cv::Mat* FrameSource(ViewerState* st)
    {
        if (!st)
            return nullptr;
        if (!st->videoFrame.empty())
            return &st->videoFrame;
        if (!st->asset)
            return nullptr;
        if (!st->asset->frames.empty())
        {
            if (st->cur < 0 || st->cur >= static_cast<int>(st->asset->frames.size()))
                st->cur = 0;
            return &st->asset->frames[st->cur];
        }
        if (!st->asset->primaryFrame.empty())
            return &st->asset->primaryFrame;
        return nullptr;
    }

    // Rebuild / refresh the cached DIB for the current source frame. The DIB is
    // reused when the geometry still matches, so switching frames is a memcpy
    // rather than a fresh full-size allocation.
    bool EnsureBmp(ViewerState* st)
    {
        const cv::Mat* src = FrameSource(st);
        if (!src || src->empty())
            return false;

        if (st->bmp && st->bmpGen == st->frameGen && st->bmpRotation == st->rotation)
            return true;

        // Video frames are opaque, so skip the premultiply pass.
        const bool isVideo = !st->videoFrame.empty();
        SIZE sz = { 0, 0 };
        bool alpha = false;
        if (!MatToDIBReuse(*src, st->rotation, !isVideo,
                           isVideo ? kLiveMaxDim : kViewerMaxDim,
                           &st->bmp, &sz, &alpha))
            return false;
        st->bmpGen = st->frameGen;
        st->bmpRotation = st->rotation;
        st->bmpHasAlpha = alpha;
        st->bmpSize = sz;
        // During playback the layout keeps the still's size, so the pane never
        // jumps between the photo and its clip; Paint fits the frame inside.
        if (!isVideo)
            st->size = sz;
        else if (st->size.cx <= 0 || st->size.cy <= 0)
            st->size = LayoutSize(st);
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

    // ---------------------------------------------------------------------
    // Live Photo / Motion Photo
    //
    // Upstream hands the video to us as plain bytes on the asset (trailer MP4
    // for Android/Samsung JPEGs, sidecar .MOV for Apple/VIVO HEIC, or the
    // members of an .livp zip). MediaPlayer streams those bytes with its own
    // decode threads and an audio-master clock, so we only poll for frames
    // instead of ever holding a decoded clip in memory.
    // ---------------------------------------------------------------------

    void StopLive(ViewerState* st)
    {
        if (st->hwnd)
            KillTimer(st->hwnd, kLiveTimer);
        st->playing = false;
        if (st->session)
        {
            // The worker owns its own reference, so dropping ours is enough: it
            // finishes start(), notices the cancellation, and tears the player
            // down on its own thread. Nothing here ever blocks the UI.
            st->session->cancelled.store(true);
            st->session.reset();
        }
    }

    void StartLive(ViewerState* st)
    {
        if (!st->asset || !st->live || !st->asset->videoSource)
            return;

        const std::vector<uint8_t>& data = st->asset->videoSource->data;
        if (data.empty())
            return;

        auto session = std::make_shared<LiveSession>();
        session->player = jark::MediaPlayer::create();
        if (!session->player)
        {
            JarkLog(L"Live: MediaPlayer::create failed");
            return;
        }
        session->keepAlive = st->asset; // the player reads the asset's bytes
        st->session = session;
        st->playedOnce = false;

        LiveBegin();
        std::thread([session, bytes = data.size()]() {
            // Muted on purpose: the preview pane must stay silent while the user
            // walks through a folder. The audio track still drives the clock.
            const std::vector<uint8_t>& buf = session->keepAlive->videoSource->data;
            const bool ok = session->player->start(
                std::span<const uint8_t>(buf.data(), buf.size()), 0.0f);
            if (!ok)
            {
                JarkLog(L"Live: start failed (%zu bytes)", bytes);
            }
            else if (session->cancelled.load())
            {
                session->player.reset(); // drops the decoder threads right here
            }
            else
            {
                JarkLog(L"Live: playing %zu bytes, duration=%lldms audio=%d",
                        bytes, static_cast<long long>(session->player->durationMs()),
                        session->player->hasAudio() ? 1 : 0);
                session->started.store(true);
            }
            LiveEnd();
        }).detach();

        SetTimer(st->hwnd, kLiveTimer, kLivePollMs, nullptr);
    }

    // Playback reached the end: drop back to the still. The photo is the
    // full-resolution frame, and freezing on the last video frame would look
    // like a rendering glitch.
    void FinishLive(ViewerState* st)
    {
        if (st->hwnd)
            KillTimer(st->hwnd, kLiveTimer);
        st->playing = false;
        st->playedOnce = true;
        if (st->session)
        {
            st->session->cancelled.store(true);
            st->session.reset(); // player already finished; this just frees it
        }
        if (!st->videoFrame.empty())
        {
            st->videoFrame.release();
            ++st->frameGen;
        }
    }

    void SetAsset(ViewerState* st, std::shared_ptr<ImageAsset> asset, const std::wstring& path)
    {
        StopAnimation(st);
        StopLive(st);
        st->videoFrame.release();
        st->playedOnce = false;
        ++st->frameGen;
        FreeBmp(st);
        st->asset = std::move(asset);
        st->path = path;
        st->cur = 0;
        st->durations.clear();
        st->live = false;

        if (!st->asset)
        {
            st->size = { 0, 0 };
            JarkLog(L"SetAsset: empty asset for %s", path.c_str());
            return;
        }

        st->live = IsLivePhotoAsset(*st->asset);
        st->durations.assign(st->asset->frameDurations.begin(), st->asset->frameDurations.end());

        const cv::Mat& first = CurrentFrameOf(*st->asset);
        st->baseSize = SIZE{ first.cols, first.rows };
        st->size = LayoutSize(st);

        const bool dib = EnsureBmp(st);
        JarkLog(L"SetAsset: %s frames=%zu live=%d durations=%zu dib=%d size=%dx%d",
                path.c_str(), st->asset->frames.size(), st->live ? 1 : 0, st->durations.size(),
                dib ? 1 : 0, st->size.cx, st->size.cy);
        if (!dib)
            return;

        NotifySize(st->hwnd, st->size);
        if (st->live)
            StartLive(st);
        else
            ScheduleFrame(st);
        InvalidateRect(st->hwnd, nullptr, FALSE);
    }

    void ReleaseBack(ViewerState* st)
    {
        if (!st)
            return;
        if (st->backDc)
        {
            if (st->backOld)
                SelectObject(st->backDc, st->backOld);
            st->backOld = nullptr;
            DeleteDC(st->backDc);
            st->backDc = nullptr;
        }
        if (st->backBmp)
        {
            DeleteObject(st->backBmp);
            st->backBmp = nullptr;
        }
        st->backW = 0;
        st->backH = 0;
    }

    // Backing store is a plain compatible bitmap: it has the screen's format, so
    // the final BitBlt is a straight copy and AlphaBlend does not have to mix
    // into a destination alpha channel.
    bool EnsureBack(ViewerState* st, int w, int h)
    {
        if (!st)
            return false;
        if (st->backDc && st->backW == w && st->backH == h)
            return true;

        ReleaseBack(st);
        if (w <= 0 || h <= 0)
            return false;

        HDC screen = GetDC(nullptr);
        if (!screen)
            return false;
        HDC mem = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
        ReleaseDC(nullptr, screen);

        if (!mem || !bmp)
        {
            if (mem)
                DeleteDC(mem);
            if (bmp)
                DeleteObject(bmp);
            return false;
        }

        st->backDc = mem;
        st->backBmp = bmp;
        st->backOld = SelectObject(mem, bmp);
        st->backW = w;
        st->backH = h;
        return true;
    }

    // Background brush follows the host theme (DVPN_GETBGCOL), cached so a 30fps
    // repaint does not create and destroy a GDI object per frame.
    HBRUSH BgBrush(ViewerState* st, HWND hwnd, COLORREF* outCol)
    {
        const COLORREF col = QueryBgCol(hwnd);
        if (outCol)
            *outCol = col;
        if (!st)
            return nullptr;
        if (!st->bgBrush || st->bgCol != col)
        {
            if (st->bgBrush)
                DeleteObject(st->bgBrush);
            st->bgCol = col;
            st->bgBrush = CreateSolidBrush(col);
        }
        return st->bgBrush;
    }

    void Paint(HWND hwnd, ViewerState* st)
    {
        PAINTSTRUCT ps = {};
        HDC dc = BeginPaint(hwnd, &ps);
        if (!dc)
            return;

        RECT client = {};
        GetClientRect(hwnd, &client);
        const int cw = client.right - client.left;
        const int ch = client.bottom - client.top;

        // Compose the whole frame off-screen, then blit once. Drawing the
        // background and the picture separately straight onto the window DC let
        // the background show through between the two calls - visible as a white
        // flicker at video frame rates.
        COLORREF bgCol = 0;
        HBRUSH bg = BgBrush(st, hwnd, &bgCol);
        HBRUSH localBg = nullptr;
        if (!bg)
        {
            localBg = CreateSolidBrush(bgCol);
            bg = localBg;
        }

        const bool buffered = EnsureBack(st, cw, ch);
        HDC target = buffered ? st->backDc : dc;

        FillRect(target, &client, bg);
        if (localBg)
            DeleteObject(localBg);

        bool drew = false;
        if (st && EnsureBmp(st) && st->size.cx > 0 && st->size.cy > 0 && cw > 0 && ch > 0)
        {
            const double scale = ScaleOf(st, cw, ch);

            int dw = static_cast<int>(st->size.cx * scale + 0.5);
            int dh = static_cast<int>(st->size.cy * scale + 0.5);
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;
            const int dx = client.left + (cw - dw) / 2 + st->panX;
            const int dy = client.top + (ch - dh) / 2 + st->panY;

            // The layout box comes from the still image. For stills and animations
            // the DIB fills it exactly; live-photo video is fitted inside it so a
            // different aspect ratio letterboxes instead of distorting the face.
            const int bw = st->bmpSize.cx > 0 ? st->bmpSize.cx : st->size.cx;
            const int bh = st->bmpSize.cy > 0 ? st->bmpSize.cy : st->size.cy;
            const double fit = std::min(static_cast<double>(dw) / bw,
                                        static_cast<double>(dh) / bh);
            int iw = static_cast<int>(bw * fit + 0.5);
            int ih = static_cast<int>(bh * fit + 0.5);
            if (iw < 1) iw = 1;
            if (ih < 1) ih = 1;
            const int ix = dx + (dw - iw) / 2;
            const int iy = dy + (dh - ih) / 2;

            HDC mem = CreateCompatibleDC(target);
            if (mem)
            {
                HGDIOBJ old = SelectObject(mem, st->bmp);

                if (st->bmpHasAlpha)
                {
                    BLENDFUNCTION bf = {};
                    bf.BlendOp = AC_SRC_OVER;
                    bf.SourceConstantAlpha = 255;
                    bf.AlphaFormat = AC_SRC_ALPHA; // source is premultiplied
                    drew = AlphaBlend(target, ix, iy, iw, ih, mem, 0, 0, bw, bh, bf) != FALSE;
                }
                else
                {
                    SetStretchBltMode(target, HALFTONE);
                    SetBrushOrgEx(target, 0, 0, nullptr);
                    drew = StretchBlt(target, ix, iy, iw, ih, mem, 0, 0, bw, bh, SRCCOPY) != FALSE;
                }

                SelectObject(mem, old);
                DeleteDC(mem);
            }
        }

        if (buffered)
            BitBlt(dc, 0, 0, cw, ch, st->backDc, 0, 0, SRCCOPY);

        (void)drew;
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
            StopLive(st);
            st->videoFrame.release();
            st->live = false;
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
        info->szImageSize = st ? st->size : SIZE{ 0, 0 };
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
        if (st->live && st->baseSize.cx > 0)
            st->size = LayoutSize(st); // video frames do not update the layout
        if (EnsureBmp(st))
            NotifySize(st->hwnd, st->size);
        InvalidateRect(st->hwnd, nullptr, FALSE);
    }

    HBITMAP CopyCurrent(ViewerState* st)
    {
        const cv::Mat* src = FrameSource(st);
        if (!src || src->empty())
            return nullptr;
        SIZE sz = { 0, 0 };
        bool alpha = false;
        return MatToDIB(*src, st->rotation, st->videoFrame.empty(), kViewerMaxDim, &sz, &alpha);
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
                // A finished live photo replays on double click; everything else
                // toggles fit-to-window <-> 1:1. Opus's own double click
                // (usually "close viewer") is not forwarded from here.
                if (st->live && st->playedOnce && !st->playing)
                {
                    JarkLog(L"dblclk: replay live photo");
                    st->videoFrame.release();
                    ++st->frameGen;
                    StartLive(st);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
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
                    ++st->frameGen;
                    if (st->tickLog < 5)
                    {
                        JarkLog(L"WM_TIMER: frame -> %d/%zu", st->cur, st->asset->frames.size());
                        ++st->tickLog;
                    }
                    // No FreeBmp: EnsureBmp sees the new generation and rewrites
                    // the existing DIB when the geometry matches.
                    InvalidateRect(hwnd, nullptr, FALSE);
                    ScheduleFrame(st);
                }
                else
                {
                    st->animating = false;
                }
                return 0;
            }

            if (wParam == kLiveTimer && st)
            {
                LiveSession* s = st->session.get();
                if (!s)
                {
                    KillTimer(hwnd, kLiveTimer);
                    st->playing = false;
                    return 0;
                }
                // Still opening the decoders / audio device on the worker.
                if (!s->started.load())
                    return 0;

                if (!st->playing)
                {
                    st->playing = true;
                    st->tickLog = 0;
                    JarkLog(L"Live: first poll after start");
                }

                cv::Mat frame;
                if (s->player->acquireFrame(frame) && !frame.empty())
                {
                    st->videoFrame = std::move(frame);
                    ++st->frameGen;
                    if (st->tickLog < 5)
                    {
                        JarkLog(L"Live: frame %dx%d at %lldms", st->videoFrame.cols,
                                st->videoFrame.rows, static_cast<long long>(s->player->positionMs()));
                        ++st->tickLog;
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                }

                if (s->player->hasFinished())
                {
                    JarkLog(L"Live: finished at %lldms",
                            static_cast<long long>(s->player->positionMs()));
                    FinishLive(st);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
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
                if (st->live && st->baseSize.cx > 0)
                    st->size = LayoutSize(st);
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
                StopLive(st);
                st->videoFrame.release();
                ++st->frameGen;
                FreeBmp(st);
                st->live = false;
                st->playedOnce = false;
                st->asset.reset();
                st->cur = 0;
                st->panX = 0;
                st->panY = 0;
                st->dragging = false;
                st->baseSize = { 0, 0 };
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
                // Stop playback before the asset (which owns the video bytes the
                // player was handed) dies with the state.
                StopLive(doomed);
                doomed->videoFrame.release();
                FreeBmp(doomed);
                ReleaseBack(doomed);
                if (doomed->bgBrush)
                {
                    DeleteObject(doomed->bgBrush);
                    doomed->bgBrush = nullptr;
                }
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

void JarkViewerFlushLive()
{
    // Live-photo playback starts on a detached worker (see LiveSession). Those
    // workers sit inside FFmpeg, so the module must not unload under them.
    // Bounded: a wedged decoder must not hang Opus on shutdown.
    std::unique_lock<std::mutex> lk(g_liveMtx);
    if (!g_liveCv.wait_for(lk, std::chrono::seconds(5), [] { return g_liveRunning == 0; }))
    {
        JarkLog(L"FlushLive: %d player start(s) still running after 5s", g_liveRunning);
    }
}
