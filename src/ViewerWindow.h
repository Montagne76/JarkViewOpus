/*
   JarkViewOpus - animated viewer window.

   DOpus hands the plugin a child window and drives it with DVPLUGINMSG_*
   messages (same model as the official Animated GIF plugin). We paint frames
   ourselves, which is the only way to animate inside the viewer / preview pane.

   License: GPL-3.0-or-later
*/
#pragma once

#include <windows.h>

// Creates the viewer window inside hwndParent using the rect DOpus supplied.
HWND JarkViewerCreate(HWND hwndParent, const RECT* rc, DWORD flags);

// Destroys every live viewer window (called before the DLL unloads).
void JarkViewerDestroyAll();
