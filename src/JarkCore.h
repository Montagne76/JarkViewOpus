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

// The frame to display for a still viewer.
const cv::Mat& CurrentFrameOf(const ImageAsset& asset);

// cv::Mat -> 32bpp top-down DIB.
//   rotation   0/90/180/270 (clockwise)
//   premultiply  convert straight alpha to premultiplied (needed by AlphaBlend)
//   maxDim     >0 downscales so the longest side fits maxDim
HBITMAP MatToDIB(const cv::Mat& src, int rotation, bool premultiply, int maxDim,
                 LPSIZE outSize, bool* outHasAlpha);
