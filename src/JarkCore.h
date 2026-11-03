/*
   JarkViewOpus - shared decode core (JarkViewer upstream, GPL-3.0).
   Used by both the static bitmap path and the animated viewer window.
   License: GPL-3.0-or-later
*/
#pragma once

#include <windows.h>
#include <objidl.h>
#include <memory>
#include <string>

#include "../jark/include/ImageDatabase.h"  // also brings in jarkUtils.h (ImageAsset)

// Process-wide decode database. Starts the upstream preload worker thread;
// without it the cache API would poll forever (see JarkCore.cpp).
ImageDatabase* JarkGetDB();

// Decode, or fetch from the LRU cache. Blocks for one decode at most.
std::shared_ptr<ImageAsset> JarkLoadAsset(const std::wstring& path);

// Same for an IStream (files inside archives / FTP). Upstream's loader is
// path-based, so the stream is spooled to a temp file first.
std::shared_ptr<ImageAsset> JarkLoadAssetFromStream(LPSTREAM stream, const wchar_t* name);

// Stop the preload worker and drop the cache. Call before DLL unload.
void JarkShutdownDB();

// True when the asset is a real animation (more than one frame).
bool IsAnimatedAsset(const ImageAsset& asset);

// True for Live Photo / Motion Photo: a still (primaryFrame) plus playable video
// bytes. Upstream hands us the video either from a trailer MP4 (Android/Samsung
// JPEG) or a sidecar .MOV/.MP4 with the same name (Apple/VIVO HEIC).
// The bytes are owned by the asset, so the asset must outlive playback.
bool IsLivePhotoAsset(const ImageAsset& asset);

// The frame to display for a still viewer.
const cv::Mat& CurrentFrameOf(const ImageAsset& asset);

// cv::Mat -> 32bpp top-down DIB.
//   rotation   0/90/180/270 (clockwise)
//   premultiply  convert straight alpha to premultiplied (needed by AlphaBlend)
//   maxDim     >0 downscales so the longest side fits maxDim
HBITMAP MatToDIB(const cv::Mat& src, int rotation, bool premultiply, int maxDim,
                 LPSIZE outSize, bool* outHasAlpha);

// Same conversion, but reuses *inOut when its geometry still matches. Animation
// frames keep their size, so this avoids allocating a DIB per frame.
bool MatToDIBReuse(const cv::Mat& src, int rotation, bool premultiply, int maxDim,
                   HBITMAP* inOut, LPSIZE outSize, bool* outHasAlpha);

// Append one line to %TEMP%\JarkViewOpus.log. Cheap, thread-safe, and useful
// because a viewer plugin has no other way to talk back while inside Opus.
// Setting JVO_NOLOG=1 in the environment disables it.
void JarkLog(const wchar_t* fmt, ...);
