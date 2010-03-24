// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/logging.h"
#include "main.h"
#include "xlib_window.h"


Display *g_xlib_display = NULL;
Window g_xlib_window = 0;

GLint g_width = 512;
GLint g_height = 512;
bool g_override_redirect = true;


bool XlibInit() {
  g_xlib_display = XOpenDisplay(0);
  if (!g_xlib_display)
    return false;

  int screen = DefaultScreen(g_xlib_display);
  Window root_window = RootWindow(g_xlib_display, screen);

  XWindowAttributes attributes;
  XGetWindowAttributes(g_xlib_display, root_window, &attributes);

  g_width = g_width == -1 ? attributes.width : g_width;
  g_height = g_height == -1 ? attributes.height : g_height;
  XVisualInfo* xlib_visinfo = GetXVisual();

  unsigned long mask = CWBackPixel | CWBorderPixel | CWColormap | CWEventMask |
    CWOverrideRedirect;
  XSetWindowAttributes attr;
  attr.background_pixel = 0;
  attr.border_pixel = 0;
  attr.colormap = XCreateColormap(g_xlib_display, root_window,
                                  xlib_visinfo->visual, AllocNone);
  attr.event_mask = StructureNotifyMask | ExposureMask | KeyPressMask;
  attr.override_redirect = g_override_redirect ? True : False;
  g_xlib_window = XCreateWindow(g_xlib_display, root_window,
                                0, 0, g_width, g_height, 0,
                                xlib_visinfo->depth, InputOutput,
                                xlib_visinfo->visual, mask, &attr);

  XMapWindow(g_xlib_display, g_xlib_window);
  XSync(g_xlib_display, True);

  XGetWindowAttributes(g_xlib_display, g_xlib_window, &attributes);
  g_width = attributes.width;
  g_height = attributes.height;

  XFree(xlib_visinfo);

  return true;
}
