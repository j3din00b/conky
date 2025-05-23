/*
 *
 * Conky, a system monitor, based on torsmo
 *
 * Any original torsmo code is licensed under the BSD license
 *
 * All code written since the fork of torsmo is licensed under the GPL
 *
 * Please see COPYING for details
 *
 * Copyright (c) 2004, Hannu Saransaari and Lauri Hakkarainen
 * Copyright (c) 2005-2024 Brenden Matthews, Philip Kovacs, et. al.
 *	(see AUTHORS)
 * All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include "../lua/x11-settings.h"
#include "x11.h"

#include <X11/X.h>
#include <X11/Xlibint.h>
#undef min
#undef max
#include <sys/types.h>

#include "../common.h"
#include "../conky.h"
#include "../geometry.h"
#include "../logging.h"
#include "gui.h"

#ifdef BUILD_XINPUT
#include "../mouse-events.h"

#include <vector>
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <string>

// #ifndef OWN_WINDOW
// #include <iostream>
// #endif

extern "C" {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wvariadic-macros"
#pragma GCC diagnostic ignored "-Wregister"
#include <X11/XKBlib.h>
#pragma GCC diagnostic pop
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xmd.h>
#include <X11/Xutil.h>

#ifdef BUILD_IMLIB2
#include "../conky-imlib2.h"
#endif /* BUILD_IMLIB2 */
#ifdef BUILD_XFT
#include <X11/Xft/Xft.h>
#endif
#ifdef BUILD_XINERAMA
#include <X11/extensions/Xinerama.h>
#endif
#ifdef BUILD_XSHAPE
#include <X11/extensions/shape.h>
#endif /* BUILD_XSHAPE */
#ifdef BUILD_XFIXES
#include <X11/extensions/Xfixes.h>
#endif /* BUILD_XFIXES */
#ifdef BUILD_XINPUT
#include <X11/extensions/XInput.h>
#include <X11/extensions/XInput2.h>
#endif /* BUILD_XINPUT */
#ifdef HAVE_XCB_ERRORS
#include <xcb/xcb.h>
#include <xcb/xcb_errors.h>
#endif
#include <X11/Xresource.h>
}

Display *display = nullptr;
int screen;

#ifdef HAVE_XCB_ERRORS
xcb_connection_t *xcb_connection;
xcb_errors_context_t *xcb_errors_ctx;
#endif

/* Window stuff */
struct conky_x11_window window;

bool have_argb_visual = false;

/* local prototypes */
static Window find_desktop_window(Window *p_root, Window *p_desktop);
static Window find_desktop_window_impl(Window win, int w, int h);

/* WARNING, this type not in Xlib spec */
static int x11_error_handler(Display *d, XErrorEvent *err) {
  char *error_name = nullptr;
  bool name_allocated = false;

  char *code_description = nullptr;
  bool code_allocated = false;

#ifdef HAVE_XCB_ERRORS
  if (xcb_errors_ctx != nullptr) {
    const char *extension;
    const char *base_name = xcb_errors_get_name_for_error(
        xcb_errors_ctx, err->error_code, &extension);
    if (extension != nullptr) {
      const std::size_t size = strlen(base_name) + strlen(extension) + 4;
      error_name = new char[size];
      snprintf(error_name, size, "%s (%s)", base_name, extension);
      name_allocated = true;
    } else {
      error_name = const_cast<char *>(base_name);
    }

    const char *major =
        xcb_errors_get_name_for_major_code(xcb_errors_ctx, err->request_code);
    const char *minor = xcb_errors_get_name_for_minor_code(
        xcb_errors_ctx, err->request_code, err->minor_code);
    if (minor != nullptr) {
      const std::size_t size = strlen(major) + strlen(minor) + 4;
      code_description = new char[size];
      snprintf(code_description, size, "%s - %s", major, minor);
      code_allocated = true;
    } else {
      code_description = const_cast<char *>(major);
    }
  }
#endif

  if (error_name == nullptr) {
    if (err->error_code > 0 && err->error_code < 17) {
      static std::array<std::string, 17> NAMES = {
          "request", "value",         "window",    "pixmap",    "atom",
          "cursor",  "font",          "match",     "drawable",  "access",
          "alloc",   "colormap",      "G context", "ID choice", "name",
          "length",  "implementation"};
      error_name = const_cast<char *>(NAMES[err->error_code].c_str());
    } else {
      static char code_name_buffer[5];
      error_name = reinterpret_cast<char *>(&code_name_buffer);
      snprintf(error_name, 4, "%d", err->error_code);
    }
  }
  if (code_description == nullptr) {
    const std::size_t size = 37;
    code_description = new char[size];
    snprintf(code_description, size, "error code: [major: %i, minor: %i]",
             err->request_code, err->minor_code);
    code_allocated = true;
  }

  DBGP(
      "X %s Error:\n"
      "Display: %lx, XID: %li, Serial: %lu\n"
      "%s",
      error_name, reinterpret_cast<uint64_t>(err->display),
      static_cast<int64_t>(err->resourceid), err->serial, code_description);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfree-nonheap-object"
  // *_allocated takes care of avoiding freeing unallocated objects
  if (name_allocated) delete[] error_name;
  if (code_allocated) delete[] code_description;
#pragma GCC diagnostic pop

  return 0;
}

__attribute__((noreturn)) static int x11_ioerror_handler(Display *d) {
  CRIT_ERR("X IO Error: Display %lx\n", reinterpret_cast<uint64_t>(d));
}

/// @brief Function to get virtual root windows of screen.
///
/// Some WMs (swm, tvtwm, amiwm, enlightenment, etc.) use virtual roots to
/// manage workspaces. These are direct descendants of root and WMs reparent all
/// children to them.
///
/// @param screen screen to get the (current) virtual root of
/// @return the virtual root window of the screen
static Window VRootWindowOfScreen(Screen *screen) {
  Window root = RootWindowOfScreen(screen);
  Display *dpy = DisplayOfScreen(screen);

  /* go look for a virtual root */
  Atom _NET_VIRTUAL_ROOTS = XInternAtom(display, "_NET_VIRTUAL_ROOTS", True);
  if (_NET_VIRTUAL_ROOTS == 0) return root;

  auto vroots = x11_atom_window_list(dpy, root, _NET_VIRTUAL_ROOTS);

  if (vroots.empty()) return root;

  Atom _NET_CURRENT_DESKTOP =
      XInternAtom(display, "_NET_CURRENT_DESKTOP", True);
  if (_NET_CURRENT_DESKTOP == 0) return root;

  Atom actual_type;
  int actual_format;
  unsigned long nitems, bytesafter;
  int *cardinal;

  XGetWindowProperty(dpy, root, _NET_CURRENT_DESKTOP, 0, 1, False, XA_CARDINAL,
                     &actual_type, &actual_format, &nitems, &bytesafter,
                     (unsigned char **)&cardinal);

  if (vroots.size() > *cardinal) { root = vroots[*cardinal]; }
  XFree(cardinal);

  return root;
}
inline Window VRootWindow(Display *display, int screen) {
  return VRootWindowOfScreen(ScreenOfDisplay(display, screen));
}
inline Window DefaultVRootWindow(Display *display) {
  return VRootWindowOfScreen(DefaultScreenOfDisplay(display));
}

/* X11 initializer */
void init_x11() {
  DBGP("enter init_x11()");
  if (display == nullptr) {
    const std::string &dispstr = display_name.get(*state);
    // passing nullptr to XOpenDisplay should open the default display
    const char *disp = static_cast<unsigned int>(!dispstr.empty()) != 0u
                           ? dispstr.c_str()
                           : nullptr;
    if ((display = XOpenDisplay(disp)) == nullptr) {
      std::string err =
          std::string("can't open display: ") + XDisplayName(disp);
#ifdef BUILD_WAYLAND
      NORM_ERR(err.c_str());
      return;
#else  /* BUILD_WAYLAND */
      throw std::runtime_error(err);
#endif /* BUILD_WAYLAND */
    }
  }

  info.x11.monitor.number = 1;
  info.x11.monitor.current = 0;
  info.x11.desktop.current = 1;
  info.x11.desktop.number = 1;
  info.x11.desktop.all_names.clear();
  info.x11.desktop.name.clear();

  screen = DefaultScreen(display);

  XSetErrorHandler(&x11_error_handler);
  XSetIOErrorHandler(&x11_ioerror_handler);

  update_x11_resource_db(true);
  update_x11_workarea();

  get_x11_desktop_info(display, 0);

#ifdef HAVE_XCB_ERRORS
  auto connection = xcb_connect(NULL, NULL);
  if (!xcb_connection_has_error(connection)) {
    if (xcb_errors_context_new(connection, &xcb_errors_ctx) != 0) {
      xcb_errors_ctx = nullptr;
    }
  }
#endif /* HAVE_XCB_ERRORS */
  DBGP("leave init_x11()");
}

void deinit_x11() {
  if (display) {
    DBGP("deinit_x11()");
    XCloseDisplay(display);
    display = nullptr;
  }
}

// Source: dunst
// https://github.com/bebehei/dunst/blob/1bc3237a359f37905426012c0cca90d71c4b3b18/src/x11/x.c#L463
void update_x11_resource_db(bool first_run) {
  XrmDatabase db;
  XTextProperty prop;
  Window root;

  XFlush(display);

  root = RootWindow(display, screen);

  XLockDisplay(display);
  if (XGetTextProperty(display, root, &prop, XA_RESOURCE_MANAGER)) {
    if (!first_run) {
      db = XrmGetDatabase(display);
      XrmDestroyDatabase(db);
    }

    // https://github.com/dunst-project/dunst/blob/master/src/x11/x.c#L499
    display->db = NULL; // should be new or deleted
    db = XrmGetStringDatabase((const char *)prop.value);
    XrmSetDatabase(display, db);
  }
  XUnlockDisplay(display);

  XFlush(display);
  XSync(display, false);
}

void update_x11_workarea() {
  /* default work area is display */
  workarea = conky::absolute_rect<int>(
      conky::vec2i::Zero(), conky::vec2i(DisplayWidth(display, screen),
                                         DisplayHeight(display, screen)));

#ifdef BUILD_XINERAMA
  /* if xinerama is being used, adjust workarea to the head's area */
  int useless1, useless2;
  if (XineramaQueryExtension(display, &useless1, &useless2) == 0) {
    return; /* doesn't even have xinerama */
  }

  if (XineramaIsActive(display) == 0) {
    return; /* has xinerama but isn't using it */
  }

  int heads = 0;
  XineramaScreenInfo *si = XineramaQueryScreens(display, &heads);
  if (si == nullptr) {
    NORM_ERR(
        "warning: XineramaQueryScreen returned nullptr, ignoring head "
        "settings");
    return; /* queryscreens failed? */
  }

  int i = head_index.get(*state);
  if (i < 0 || i >= heads) {
    NORM_ERR("warning: invalid head index, ignoring head settings");
    return;
  }

  XineramaScreenInfo *ps = &si[i];
  workarea.set_pos(ps->x_org, ps->y_org);
  workarea.set_size(ps->width, ps->height);
  XFree(si);

  DBGP("Fixed xinerama area to: %d %d %d %d", workarea[0], workarea[1],
       workarea[2], workarea[3]);
#endif
}

/* Find root window and desktop window.
 * Return desktop window on success,
 * and set root and desktop byref return values.
 * Return 0 on failure. */
static Window find_desktop_window(Window root) {
  Window desktop = root;

  /* get subwindows from root */
  int display_width = DisplayWidth(display, screen);
  int display_height = DisplayHeight(display, screen);
  desktop = find_desktop_window_impl(root, display_width, display_height);
  update_x11_workarea();
  desktop =
      find_desktop_window_impl(desktop, workarea.width(), workarea.height());

  if (desktop != root) {
    NORM_ERR("desktop window (0x%lx) is subwindow of root window (0x%lx)",
             desktop, root);
  } else {
    NORM_ERR("desktop window (0x%lx) is root window", desktop);
  }
  return desktop;
}

#ifdef OWN_WINDOW
#ifdef BUILD_ARGB
namespace {
/* helper function for set_transparent_background() */
void do_set_background(Window win, uint8_t alpha) {
  Colour colour = background_colour.get(*state);
  colour.alpha = alpha;
  unsigned long xcolor =
      colour.to_x11_color(display, screen, have_argb_visual, true);
  XSetWindowBackground(display, win, xcolor);
}
}  // namespace
#endif /* BUILD_ARGB */

/* if no argb visual is configured sets background to ParentRelative for the
   Window and all parents, else real transparency is used */
void set_transparent_background(Window win) {
#ifdef BUILD_ARGB
  if (have_argb_visual) {
    // real transparency
    do_set_background(win, set_transparent.get(*state)
                               ? 0
                               : own_window_argb_value.get(*state));
    return;
  }
#endif /* BUILD_ARGB */

  // pseudo transparency
  if (set_transparent.get(*state)) {
    Window parent = win;
    unsigned int i;

    for (i = 0; i < 50 && parent != RootWindow(display, screen); i++) {
      Window r, *children;
      unsigned int n;

      XSetWindowBackgroundPixmap(display, parent, ParentRelative);

      XQueryTree(display, parent, &r, &parent, &children, &n);
      XFree(children);
    }
    return;
  }

#ifdef BUILD_ARGB
  do_set_background(win, 0);
#endif /* BUILD_ARGB */
}
#endif /* OWN_WINDOW */

#ifdef BUILD_ARGB
static int get_argb_visual(Visual **visual, int *depth) {
  /* code from gtk project, gdk_screen_get_rgba_visual */
  XVisualInfo visual_template;
  XVisualInfo *visual_list;
  int nxvisuals = 0, i;

  visual_template.screen = screen;
  visual_list =
      XGetVisualInfo(display, VisualScreenMask, &visual_template, &nxvisuals);
  for (i = 0; i < nxvisuals; i++) {
    if (visual_list[i].depth == 32 && (visual_list[i].red_mask == 0xff0000 &&
                                       visual_list[i].green_mask == 0x00ff00 &&
                                       visual_list[i].blue_mask == 0x0000ff)) {
      *visual = visual_list[i].visual;
      *depth = visual_list[i].depth;
      DBGP("Found ARGB Visual");
      XFree(visual_list);
      return 1;
    }
  }

  // no argb visual available
  DBGP("No ARGB Visual found");
  XFree(visual_list);

  return 0;
}
#endif /* BUILD_ARGB */

void destroy_window() {
#ifdef BUILD_XFT
  if (window.xftdraw != nullptr) { XftDrawDestroy(window.xftdraw); }
#endif /* BUILD_XFT */
  if (window.gc != nullptr) { XFreeGC(display, window.gc); }
  memset(&window, 0, sizeof(struct conky_x11_window));
}

void x11_init_window(lua::state &l, bool own) {
  DBGP("enter x11_init_window()");
  // own is unused if OWN_WINDOW is not defined
  (void)own;

  window.root = VRootWindow(display, screen);
  if (window.root == None) {
    DBGP2("no desktop window found");
    return;
  }
  window.desktop = find_desktop_window(window.root);

  window.visual = DefaultVisual(display, screen);
  window.colourmap = DefaultColormap(display, screen);

#ifdef OWN_WINDOW
  if (own) {
    int depth = 0, flags = CWOverrideRedirect | CWBackingStore;
    Visual *visual = nullptr;

    depth = CopyFromParent;
    visual = CopyFromParent;
#ifdef BUILD_ARGB
    if (use_argb_visual.get(l) && (get_argb_visual(&visual, &depth) != 0)) {
      have_argb_visual = true;
      window.visual = visual;
      window.colourmap = XCreateColormap(display, DefaultRootWindow(display),
                                         window.visual, AllocNone);
    }
#endif /* BUILD_ARGB */

    int b = border_inner_margin.get(l) + border_width.get(l) +
            border_outer_margin.get(l);

    /* Sanity check to avoid making an invalid 0x0 window */
    if (b == 0) { b = 1; }

    XClassHint classHint;

    // class_name must be a named local variable, so that c_str() remains
    // valid until we call XmbSetWMProperties() or XSetClassHint. We use
    // const_cast because, for whatever reason, res_name is not declared as
    // const char *. XmbSetWMProperties hopefully doesn't modify the value
    // (hell, even their own example app assigns a literal string constant to
    // the field)
    const std::string &class_name = own_window_class.get(l);

    classHint.res_name = const_cast<char *>(class_name.c_str());
    classHint.res_class = classHint.res_name;

    if (own_window_type.get(l) == window_type::OVERRIDE) {
      /* An override_redirect True window.
       * No WM hints or button processing needed. */
      XSetWindowAttributes attrs = {ParentRelative,
                                    0L,
                                    0,
                                    0L,
                                    0,
                                    0,
                                    Always,
                                    0L,
                                    0L,
                                    False,
                                    StructureNotifyMask | ExposureMask,
                                    0L,
                                    True,
                                    0,
                                    0};
      flags |= CWBackPixel;
      if (have_argb_visual) {
        attrs.colormap = window.colourmap;
        flags &= ~CWBackPixel;
        flags |= CWBorderPixel | CWColormap;
      }

      /* Parent is desktop window (which might be a child of root) */
      window.window = XCreateWindow(
          display, window.desktop, window.geometry.x(), window.geometry.y(), b,
          b, 0, depth, InputOutput, visual, flags, &attrs);

      XLowerWindow(display, window.window);
      XSetClassHint(display, window.window, &classHint);

      NORM_ERR("window type - override");
    } else { /* own_window_type.get(l) != TYPE_OVERRIDE */

      /* A window managed by the window manager.
       * Process hints and buttons. */
      XSetWindowAttributes attrs = {
          ParentRelative,
          0L,
          0,
          0L,
          0,
          0,
          Always,
          0L,
          0L,
          False,
          StructureNotifyMask | ExposureMask | ButtonPressMask |
              ButtonReleaseMask,
          0L,
          own_window_type.get(l) == window_type::UTILITY ? True : False,
          0,
          0};

      XWMHints wmHint;
      Atom xa;

      flags |= CWBackPixel;
      if (have_argb_visual) {
        attrs.colormap = window.colourmap;
        flags &= ~CWBackPixel;
        flags |= CWBorderPixel | CWColormap;
      }

      if (own_window_type.get(l) == window_type::DOCK) {
        window.geometry.set_pos(conky::vec2i::Zero());
      }
      /* Parent is root window so WM can take control */
      window.window = XCreateWindow(display, window.root, window.geometry.x(),
                                    window.geometry.y(), b, b, 0, depth,
                                    InputOutput, visual, flags, &attrs);

      uint16_t hints = own_window_hints.get(l);

      wmHint.flags = InputHint | StateHint;
      /* allow decorated windows to be given input focus by WM */
      wmHint.input = TEST_HINT(hints, window_hints::UNDECORATED) ? False : True;
#ifdef BUILD_XSHAPE
#ifdef BUILD_XFIXES
      if (own_window_type.get(l) == window_type::UTILITY) {
        XRectangle rect;
        XserverRegion region = XFixesCreateRegion(display, &rect, 1);
        XFixesSetWindowShapeRegion(display, window.window, ShapeInput, 0, 0,
                                   region);
        XFixesDestroyRegion(display, region);
      }
#endif /* BUILD_XFIXES */
      if (!wmHint.input) {
        /* allow only decorated windows to be given mouse input */
        int major_version;
        int minor_version;
        if (XShapeQueryVersion(display, &major_version, &minor_version) == 0) {
          NORM_ERR("Input shapes are not supported");
        } else {
          if (own_window.get(*state) &&
              (own_window_type.get(*state) != window_type::NORMAL ||
               ((TEST_HINT(own_window_hints.get(*state),
                           window_hints::UNDECORATED)) != 0))) {
            XShapeCombineRectangles(display, window.window, ShapeInput, 0, 0,
                                    nullptr, 0, ShapeSet, Unsorted);
          }
        }
      }
#endif /* BUILD_XSHAPE */
      wmHint.initial_state = NormalState;
      if (own_window_type.get(l) == window_type::DOCK ||
          own_window_type.get(l) == window_type::PANEL) {
        // Docks and panels MUST have WithdrawnState initially for Fluxbox to
        // move the window into the slit area.
        // See: https://github.com/brndnmtthws/conky/issues/2046
        // But most other WMs will explicitly ignore windows in WithdrawnState
        // See: https://github.com/brndnmtthws/conky/issues/2112
        // So we must resort to checking for WM at runtime
        if (info.system.wm == conky::info::window_manager::fluxbox) {
          wmHint.initial_state = WithdrawnState;
        }
      }

      XmbSetWMProperties(display, window.window, nullptr, nullptr, argv_copy,
                         argc_copy, nullptr, &wmHint, &classHint);
      XStoreName(display, window.window, own_window_title.get(l).c_str());

      /* Sets an empty WM_PROTOCOLS property */
      XSetWMProtocols(display, window.window, nullptr, 0);

      /* Set window type */
      if ((xa = ATOM(_NET_WM_WINDOW_TYPE)) != None) {
        Atom prop;

        switch (own_window_type.get(l)) {
          case window_type::DESKTOP:
            prop = ATOM(_NET_WM_WINDOW_TYPE_DESKTOP);
            NORM_ERR("window type - desktop");
            break;
          case window_type::DOCK:
            prop = ATOM(_NET_WM_WINDOW_TYPE_DOCK);
            NORM_ERR("window type - dock");
            break;
          case window_type::PANEL:
            prop = ATOM(_NET_WM_WINDOW_TYPE_DOCK);
            NORM_ERR("window type - panel");
            break;
          case window_type::UTILITY:
            prop = ATOM(_NET_WM_WINDOW_TYPE_UTILITY);
            NORM_ERR("window type - utility");
            break;
          case window_type::NORMAL:
          default:
            prop = ATOM(_NET_WM_WINDOW_TYPE_NORMAL);
            NORM_ERR("window type - normal");
            break;
        }
        XChangeProperty(display, window.window, xa, XA_ATOM, 32,
                        PropModeReplace,
                        reinterpret_cast<unsigned char *>(&prop), 1);
      }

      /* Set desired hints */

      /* Window decorations */
      if (TEST_HINT(hints, window_hints::UNDECORATED)) {
        DBGP("hint - undecorated");
        xa = ATOM(_MOTIF_WM_HINTS);
        if (xa != None) {
          long prop[5] = {2, 0, 0, 0, 0};
          XChangeProperty(display, window.window, xa, xa, 32, PropModeReplace,
                          reinterpret_cast<unsigned char *>(prop), 5);
        }
      }

      /* Below other windows */
      if (TEST_HINT(hints, window_hints::BELOW)) {
        DBGP("hint - below");
        xa = ATOM(_WIN_LAYER);
        if (xa != None) {
          long prop = 0;

          XChangeProperty(display, window.window, xa, XA_CARDINAL, 32,
                          PropModeAppend,
                          reinterpret_cast<unsigned char *>(&prop), 1);
        }

        xa = ATOM(_NET_WM_STATE);
        if (xa != None) {
          Atom xa_prop = ATOM(_NET_WM_STATE_BELOW);

          XChangeProperty(display, window.window, xa, XA_ATOM, 32,
                          PropModeAppend,
                          reinterpret_cast<unsigned char *>(&xa_prop), 1);
        }
      }

      /* Above other windows */
      if (TEST_HINT(hints, window_hints::ABOVE)) {
        DBGP("hint - above");
        xa = ATOM(_WIN_LAYER);
        if (xa != None) {
          long prop = 6;

          XChangeProperty(display, window.window, xa, XA_CARDINAL, 32,
                          PropModeAppend,
                          reinterpret_cast<unsigned char *>(&prop), 1);
        }

        xa = ATOM(_NET_WM_STATE);
        if (xa != None) {
          Atom xa_prop = ATOM(_NET_WM_STATE_ABOVE);

          XChangeProperty(display, window.window, xa, XA_ATOM, 32,
                          PropModeAppend,
                          reinterpret_cast<unsigned char *>(&xa_prop), 1);
        }
      }

      /* Sticky */
      if (TEST_HINT(hints, window_hints::STICKY)) {
        DBGP("hint - sticky");
        xa = ATOM(_NET_WM_DESKTOP);
        if (xa != None) {
          CARD32 xa_prop = 0xFFFFFFFF;

          XChangeProperty(display, window.window, xa, XA_CARDINAL, 32,
                          PropModeAppend,
                          reinterpret_cast<unsigned char *>(&xa_prop), 1);
        }

        xa = ATOM(_NET_WM_STATE);
        if (xa != None) {
          Atom xa_prop = ATOM(_NET_WM_STATE_STICKY);

          XChangeProperty(display, window.window, xa, XA_ATOM, 32,
                          PropModeAppend,
                          reinterpret_cast<unsigned char *>(&xa_prop), 1);
        }
      }

      /* Skip taskbar */
      if (TEST_HINT(hints, window_hints::SKIP_TASKBAR)) {
        DBGP("hint - skip taskbar");
        xa = ATOM(_NET_WM_STATE);
        if (xa != None) {
          Atom xa_prop = ATOM(_NET_WM_STATE_SKIP_TASKBAR);

          XChangeProperty(display, window.window, xa, XA_ATOM, 32,
                          PropModeAppend,
                          reinterpret_cast<unsigned char *>(&xa_prop), 1);
        }
      }

      /* Skip pager */
      if (TEST_HINT(hints, window_hints::SKIP_PAGER)) {
        DBGP("hint - skip pager");
        xa = ATOM(_NET_WM_STATE);
        if (xa != None) {
          Atom xa_prop = ATOM(_NET_WM_STATE_SKIP_PAGER);

          XChangeProperty(display, window.window, xa, XA_ATOM, 32,
                          PropModeAppend,
                          reinterpret_cast<unsigned char *>(&xa_prop), 1);
        }
      }
    }

    NORM_ERR("drawing to created window (0x%lx)", window.window);
    XMapWindow(display, window.window);
  } else
#endif /* OWN_WINDOW */
  {
    XWindowAttributes attrs;

    if (window.window == None) { window.window = window.desktop; }

    if (XGetWindowAttributes(display, window.window, &attrs) != 0) {
      window.geometry.set_size(attrs.width, attrs.height);
    }

    NORM_ERR("drawing to desktop window");
  }

  /* Drawable is same as window. This may be changed by double buffering. */
  window.drawable = window.window;

  XFlush(display);

  int64_t input_mask = ExposureMask | PropertyChangeMask;
#ifdef OWN_WINDOW
  if (own_window.get(l)) {
    input_mask |= StructureNotifyMask;
#if !defined(BUILD_XINPUT)
    input_mask |= ButtonPressMask | ButtonReleaseMask;
#endif
  }
#if defined(BUILD_MOUSE_EVENTS) || defined(BUILD_XINPUT)
  bool xinput_ok = false;
#ifdef BUILD_XINPUT
  // not a loop; substitutes goto with break - if checks fail
  do {
    int _ignored;  // segfault if NULL
    if (!XQueryExtension(display, "XInputExtension", &window.xi_opcode,
                         &_ignored, &_ignored)) {
      // events will still ~work but let the user know why they're buggy
      NORM_ERR("XInput extension is not supported by X11!");
      break;
    }

    int major = 2, minor = 0;
    int retval = XIQueryVersion(display, &major, &minor);
    if (retval != 0) {
      NORM_ERR("Error: XInput 2.0 is not supported!");
      break;
    }

    const std::size_t mask_size = (XI_LASTEVENT + 7) / 8;
    unsigned char mask_bytes[mask_size] = {0}; /* must be zeroed! */
    XISetMask(mask_bytes, XI_HierarchyChanged);
#ifdef BUILD_MOUSE_EVENTS
    XISetMask(mask_bytes, XI_Motion);
#endif /* BUILD_MOUSE_EVENTS */
    // Capture click events for "override" window type
    if (!own) {
      XISetMask(mask_bytes, XI_ButtonPress);
      XISetMask(mask_bytes, XI_ButtonRelease);
    }

    XIEventMask ev_masks[1];
    ev_masks[0].deviceid = XIAllDevices;
    ev_masks[0].mask_len = sizeof(mask_bytes);
    ev_masks[0].mask = mask_bytes;
    XISelectEvents(display, window.root, ev_masks, 1);

    if (own) {
#ifdef BUILD_MOUSE_EVENTS
      XIClearMask(mask_bytes, XI_Motion);
#endif /* BUILD_MOUSE_EVENTS */
      XISetMask(mask_bytes, XI_ButtonPress);
      XISetMask(mask_bytes, XI_ButtonRelease);

      ev_masks[0].deviceid = XIAllDevices;
      ev_masks[0].mask_len = sizeof(mask_bytes);
      ev_masks[0].mask = mask_bytes;
      XISelectEvents(display, window.window, ev_masks, 1);
    }

    // setup cache
    int num_devices;
    XDeviceInfo *info = XListInputDevices(display, &num_devices);
    for (int i = 0; i < num_devices; i++) {
      if (info[i].use == IsXPointer || info[i].use == IsXExtensionPointer) {
        conky::device_info::from_xi_id(info[i].id, display);
      }
    }
    XFreeDeviceList(info);

    xinput_ok = true;
  } while (false);
#endif /* BUILD_XINPUT */
  // Fallback to basic X11 enter/leave events if xinput fails to init.
  // It's not recommended to add event masks to special windows in X; causes a
  // crash (thus own_window_type != TYPE_DESKTOP)
#ifdef BUILD_MOUSE_EVENTS
  if (!xinput_ok && own && own_window_type.get(l) != window_type::DESKTOP) {
    input_mask |= PointerMotionMask | EnterWindowMask | LeaveWindowMask;
  }
#endif /* BUILD_MOUSE_EVENTS */
#endif /* BUILD_MOUSE_EVENTS || BUILD_XINPUT */
#endif /* OWN_WINDOW */
  window.event_mask = input_mask;
  XSelectInput(display, window.window, input_mask);

  window_created = 1;
  DBGP("leave x11_init_window()");
}

static Window find_desktop_window_impl(Window win, int w, int h) {
  unsigned int i, j;
  Window troot, parent, *children;
  unsigned int n;

  /* search subwindows with same size as display or work area */

  for (i = 0; i < 10; i++) {
    XQueryTree(display, win, &troot, &parent, &children, &n);

    for (j = 0; j < n; j++) {
      XWindowAttributes attrs;
      if (XGetWindowAttributes(display, children[j], &attrs) != 0) {
        /* Window must be mapped and same size as display or
         * work space */
        if (attrs.map_state == IsViewable && attrs.override_redirect == false &&
            ((attrs.width == w && attrs.height == h))) {
          win = children[j];
          break;
        }
      }
    }

    XFree(children);
    if (j == n) { break; }
  }

  return win;
}

void create_gc() {
  XGCValues values;

  values.graphics_exposures = 0;
  values.function = GXcopy;
  window.gc = XCreateGC(display, window.drawable,
                        GCFunction | GCGraphicsExposures, &values);
}

// Get current desktop number
static inline void get_x11_desktop_current(Display *current_display,
                                           Window root, Atom atom) {
  Atom actual_type;
  int actual_format;
  unsigned long nitems;
  unsigned long bytes_after;
  unsigned char *prop = nullptr;
  struct information *current_info = &info;

  if (atom == None) { return; }

  if ((XGetWindowProperty(current_display, root, atom, 0, 1L, False,
                          XA_CARDINAL, &actual_type, &actual_format, &nitems,
                          &bytes_after, &prop) == 0) &&
      (actual_type == XA_CARDINAL) && (nitems == 1L) && (actual_format == 32)) {
    current_info->x11.desktop.current = prop[0] + 1;
  }
  if (prop != nullptr) { XFree(prop); }
}

// Get total number of available desktops
static inline void get_x11_desktop_number(Display *current_display, Window root,
                                          Atom atom) {
  Atom actual_type;
  int actual_format;
  unsigned long nitems;
  unsigned long bytes_after;
  unsigned char *prop = nullptr;
  struct information *current_info = &info;

  if (atom == None) { return; }

  if ((XGetWindowProperty(current_display, root, atom, 0, 1L, False,
                          XA_CARDINAL, &actual_type, &actual_format, &nitems,
                          &bytes_after, &prop) == 0) &&
      (actual_type == XA_CARDINAL) && (nitems == 1L) && (actual_format == 32)) {
    current_info->x11.desktop.number = prop[0];
  }
  if (prop != nullptr) { XFree(prop); }
}

// Get all desktop names
static inline void get_x11_desktop_names(Display *current_display, Window root,
                                         Atom atom) {
  Atom actual_type;
  int actual_format;
  unsigned long nitems;
  unsigned long bytes_after;
  unsigned char *prop = nullptr;
  struct information *current_info = &info;

  if (atom == None) { return; }

  if ((XGetWindowProperty(current_display, root, atom, 0, (~0L), False,
                          ATOM(UTF8_STRING), &actual_type, &actual_format,
                          &nitems, &bytes_after, &prop) == 0) &&
      (actual_type == ATOM(UTF8_STRING)) && (nitems > 0L) &&
      (actual_format == 8)) {
    current_info->x11.desktop.all_names.assign(
        reinterpret_cast<const char *>(prop), nitems);
  }
  if (prop != nullptr) { XFree(prop); }
}

// Get current desktop name
static inline void get_x11_desktop_current_name(const std::string &names) {
  struct information *current_info = &info;
  unsigned int i = 0, j = 0;
  int k = 0;

  while (i < names.size()) {
    if (names[i++] == '\0') {
      if (++k == current_info->x11.desktop.current) {
        current_info->x11.desktop.name.assign(names.c_str() + j);
        break;
      }
      j = i;
    }
  }
}

void get_x11_desktop_info(Display *current_display, Atom atom) {
  Window root;
  static Atom atom_current, atom_number, atom_names;
  struct information *current_info = &info;
  XWindowAttributes window_attributes;

  root = RootWindow(current_display, current_info->x11.monitor.current);

  /* Check if we initialise else retrieve changed property */
  if (atom == 0) {
    atom_current = XInternAtom(current_display, "_NET_CURRENT_DESKTOP", True);
    atom_number = XInternAtom(current_display, "_NET_NUMBER_OF_DESKTOPS", True);
    atom_names = XInternAtom(current_display, "_NET_DESKTOP_NAMES", True);
    get_x11_desktop_current(current_display, root, atom_current);
    get_x11_desktop_number(current_display, root, atom_number);
    get_x11_desktop_names(current_display, root, atom_names);
    get_x11_desktop_current_name(current_info->x11.desktop.all_names);

    /* Set the PropertyChangeMask on the root window, if not set */
    XGetWindowAttributes(display, root, &window_attributes);
    if ((window_attributes.your_event_mask & PropertyChangeMask) == 0) {
      XSetWindowAttributes attributes;
      attributes.event_mask =
          window_attributes.your_event_mask | PropertyChangeMask;
      XChangeWindowAttributes(display, root, CWEventMask, &attributes);
      XGetWindowAttributes(display, root, &window_attributes);
    }
  } else {
    if (atom == atom_current) {
      get_x11_desktop_current(current_display, root, atom_current);
      get_x11_desktop_current_name(current_info->x11.desktop.all_names);
    } else if (atom == atom_number) {
      get_x11_desktop_number(current_display, root, atom_number);
    } else if (atom == atom_names) {
      get_x11_desktop_names(current_display, root, atom_names);
      get_x11_desktop_current_name(current_info->x11.desktop.all_names);
    }
  }
}

static const char NOT_IN_X[] = "Not running in X";

void print_monitor(struct text_object *obj, char *p, unsigned int p_max_size) {
  (void)obj;

  if (!out_to_x.get(*state)) {
    strncpy(p, NOT_IN_X, p_max_size);
    return;
  }
  snprintf(p, p_max_size, "%d", XDefaultScreen(display));
}

void print_monitor_number(struct text_object *obj, char *p,
                          unsigned int p_max_size) {
  (void)obj;

  if (!out_to_x.get(*state)) {
    strncpy(p, NOT_IN_X, p_max_size);
    return;
  }
  snprintf(p, p_max_size, "%d", XScreenCount(display));
}

void print_desktop(struct text_object *obj, char *p, unsigned int p_max_size) {
  (void)obj;

  if (!out_to_x.get(*state)) {
    strncpy(p, NOT_IN_X, p_max_size);
    return;
  }
  snprintf(p, p_max_size, "%d", info.x11.desktop.current);
}

void print_desktop_number(struct text_object *obj, char *p,
                          unsigned int p_max_size) {
  (void)obj;

  if (!out_to_x.get(*state)) {
    strncpy(p, NOT_IN_X, p_max_size);
    return;
  }
  snprintf(p, p_max_size, "%d", info.x11.desktop.number);
}

void print_desktop_name(struct text_object *obj, char *p,
                        unsigned int p_max_size) {
  (void)obj;

  if (!out_to_x.get(*state)) {
    strncpy(p, NOT_IN_X, p_max_size);
  } else {
    strncpy(p, info.x11.desktop.name.c_str(), p_max_size);
  }
}

#ifdef OWN_WINDOW
namespace x11_strut {
enum value : size_t {
  LEFT,
  RIGHT,
  TOP,
  BOTTOM,
  LEFT_START_Y,
  LEFT_END_Y,
  RIGHT_START_Y,
  RIGHT_END_Y,
  TOP_START_X,
  TOP_END_X,
  BOTTOM_START_X,
  BOTTOM_END_X,
  COUNT,
};

inline std::array<long, x11_strut::COUNT> array() {
  std::array<long, COUNT> sizes;
  std::memset(sizes.data(), 0, sizeof(long) * COUNT);
  return sizes;
}
}  // namespace x11_strut

void set_struts() {
  /* clang-format on */
  static bool warn_once = true;
  if (warn_once) {
    // Before adding new sessions to the unsupported list, please check whether
    // it's at all possible to support them by re-arranging values provided in
    // this function
    /* clang-format off */
    const bool unsupported = wm_is({
      /* clang-format off */
      conky::info::window_manager::enlightenment  // has its own gadgets system; requires a custom output and additional libraries
        /* clang-format on */
    });

    if (unsupported) {
      // feel free to add any special support
      NORM_ERR(
          "WM/DE you're using (%s) doesn't support WM_STRUT hints (well); "
          "reserved area functionality might not work correctly",
          info.system.wm_name);
    }
    warn_once = false;
  }

  // Most WMs simply subtract the primary strut side from workarea where windows
  // will be placed. e.g. TOP_LEFT will cause all windows to be shifted down
  // even if conky is a thin and tall window. It's our responsibility to set the
  // primary strut side to the value that's going to eat up least available
  // space.
  //
  // compiz:
  // https://github.com/compiz-reloaded/compiz/blob/155c201ec62c289c5a44b6dd23eddec3e3c20e26/src/window.c#L1843
  // Fluxbox:
  // https://github.com/fluxbox/fluxbox/blob/88bbf811dade299ca2dac66cb81e4b3d96cfe741/src/Ewmh.cc#L1397
  // i3:
  // https://github.com/i3/i3/blob/cfa4cf16bea809c7c715a86c428757e577c85254/src/manage.c#L260
  // Mutter:
  // https://gitlab.gnome.org/GNOME/mutter/-/blob/ea8b65d0c92ebf84060ea0b5c61d02fa9004e1e9/src/core/boxes.c#L564
  // Openbox:
  // https://github.com/Mikachu/openbox/blob/dac6e2f6f8f2e0c5586a9e19f18508a03db639cb/openbox/screen.c#L1428
  // xfwm:
  // https://github.com/xfce-mirror/xfwm4/blob/37636f55bcbca064e62489d7fe183ef7b9371b4c/src/placement.c#L220
  //
  // The EWMH spec doesn't handle placement of panels/docks in middle of the
  // screen (e.g. left side of the right monitor with Xinerama). Submissions are
  // closed and it won't be fixed.
  // See: https://gitlab.freedesktop.org/xdg/xdg-specs/-/merge_requests/22

  Atom atom = ATOM(_NET_WM_STRUT);
  if (atom == None) return;

  auto sizes = x11_strut::array();

  const int display_width = DisplayWidth(display, screen);
  const int display_height = DisplayHeight(display, screen);

  bool supports_cutout =
      ENABLE_RUNTIME_TWEAKS &&
      wm_is({
          /* clang-format off */
      conky::info::window_manager::compiz,
      conky::info::window_manager::fluxbox,
      conky::info::window_manager::i3,  // only uses WM_STRUT_PARTIAL to determine top/bottom dock placement
      conky::info::window_manager::kwin,
          /* clang-format on */
      });

  if (supports_cutout) {
    alignment align = text_alignment.get(*state);
    // Middle and none align don't have least significant bit set.
    // Ensures either vertical or horizontal axis are start/end
    if ((*align & 0b0101) == 0) return;

    // Compute larger dimension only once; so we don't jump between axes for
    // corner alignments.
    // If window is wider than it's tall, top/bottom placement is preferred.
    // It's also preferred for WMs that completely ignore horizontal docks.
    static bool is_wide_window =
        window.geometry.width() > window.geometry.height() ||
        wm_is(conky::info::window_manager::i3);
    if (is_wide_window) {
      switch (align) {
        case alignment::TOP_LEFT:
        case alignment::TOP_RIGHT:
        case alignment::TOP_MIDDLE:
          sizes[x11_strut::TOP] =
              std::clamp(window.geometry.end_y(), 0, display_height);
          sizes[x11_strut::TOP_START_X] =
              std::clamp(window.geometry.x(), 0, display_width);
          sizes[x11_strut::TOP_END_X] =
              std::clamp(window.geometry.end_x(), 0, display_width);
          break;
        case alignment::BOTTOM_LEFT:
        case alignment::BOTTOM_RIGHT:
        case alignment::BOTTOM_MIDDLE:
          sizes[x11_strut::BOTTOM] =
              display_height -
              std::clamp(window.geometry.y(), 0, display_height);
          sizes[x11_strut::BOTTOM_START_X] =
              std::clamp(window.geometry.x(), 0, display_width);
          sizes[x11_strut::BOTTOM_END_X] =
              std::clamp(window.geometry.end_x(), 0, display_width);
          break;
        case alignment::MIDDLE_LEFT:
          sizes[x11_strut::LEFT] =
              std::clamp(window.geometry.end_x(), 0, display_width);
          sizes[x11_strut::LEFT_START_Y] =
              std::clamp(window.geometry.y(), 0, display_height);
          sizes[x11_strut::LEFT_END_Y] =
              std::clamp(window.geometry.end_y(), 0, display_height);
          break;
        case alignment::MIDDLE_RIGHT:
          sizes[x11_strut::RIGHT] =
              display_width - std::clamp(window.geometry.x(), 0, display_width);
          sizes[x11_strut::RIGHT_START_Y] =
              std::clamp(window.geometry.y(), 0, display_height);
          sizes[x11_strut::RIGHT_END_Y] =
              std::clamp(window.geometry.end_y(), 0, display_height);
          break;
        default:
          // can't reserve space in middle of the screen
          break;
      }
    } else {
      // if window is thin, prefer left/right placement
      switch (align) {
        case alignment::TOP_LEFT:
        case alignment::MIDDLE_LEFT:
        case alignment::BOTTOM_LEFT:
          sizes[x11_strut::LEFT] =
              std::clamp(window.geometry.end_x(), 0, display_width);
          sizes[x11_strut::LEFT_START_Y] =
              std::clamp(window.geometry.y(), 0, display_height);
          sizes[x11_strut::LEFT_END_Y] =
              std::clamp(window.geometry.end_y(), 0, display_height);
          break;
        case alignment::TOP_RIGHT:
        case alignment::MIDDLE_RIGHT:
        case alignment::BOTTOM_RIGHT:
          sizes[x11_strut::RIGHT] =
              display_width - std::clamp(window.geometry.x(), 0, display_width);
          sizes[x11_strut::RIGHT_START_Y] =
              std::clamp(window.geometry.y(), 0, display_height);
          sizes[x11_strut::RIGHT_END_Y] =
              std::clamp(window.geometry.end_y(), 0, display_height);
          break;
        case alignment::TOP_MIDDLE:
          sizes[x11_strut::TOP] =
              std::clamp(window.geometry.end_y(), 0, display_height);
          sizes[x11_strut::TOP_START_X] =
              std::clamp(window.geometry.x(), 0, display_width);
          sizes[x11_strut::TOP_END_X] =
              std::clamp(window.geometry.end_x(), 0, display_width);
          break;
        case alignment::BOTTOM_MIDDLE:
          sizes[x11_strut::BOTTOM] =
              display_height -
              std::clamp(window.geometry.y(), 0, display_height);
          sizes[x11_strut::BOTTOM_START_X] =
              std::clamp(window.geometry.x(), 0, display_width);
          sizes[x11_strut::BOTTOM_END_X] =
              std::clamp(window.geometry.end_x(), 0, display_width);
          break;
        default:
          // can't reserve space in middle of the screen
          break;
      }
    }
  } else {
    // This approach works better for fully spec-compliant WMs
    if (window.geometry.width() < window.geometry.height()) {
      const int space_left = window.geometry.end_x();
      const int space_right =
          display_width - window.geometry.end_x() + window.geometry.width();
      if (space_left < space_right) {
        sizes[x11_strut::LEFT] =
            std::clamp(window.geometry.end_x(), 0, display_width);
        sizes[x11_strut::LEFT_START_Y] =
            std::clamp(window.geometry.y(), 0, display_height);
        sizes[x11_strut::LEFT_END_Y] =
            std::clamp(window.geometry.end_y(), 0, display_height);
      } else {
        // we subtract x from display_width in case conky isn't flush with the
        // right screen side; i.e. there's a gap between conky and the right
        // side of the screen
        sizes[x11_strut::RIGHT] =
            display_width - std::clamp(window.geometry.x(), 0, display_width);
        sizes[x11_strut::RIGHT_START_Y] =
            std::clamp(window.geometry.y(), 0, display_height);
        sizes[x11_strut::RIGHT_END_Y] =
            std::clamp(window.geometry.end_y(), 0, display_height);
      }
    } else {
      const int space_top = window.geometry.end_y();
      const int space_bottom =
          display_height - window.geometry.end_y() + window.geometry.height();
      if (space_top < space_bottom) {
        sizes[x11_strut::TOP] =
            std::clamp(window.geometry.end_y(), 0, display_height);
        sizes[x11_strut::TOP_START_X] =
            std::clamp(window.geometry.x(), 0, display_width);
        sizes[x11_strut::TOP_END_X] =
            std::clamp(window.geometry.end_x(), 0, display_width);
      } else {
        // we subtract y from display_height in case conky isn't flush with the
        // bottom screen side; i.e. there's a gap between conky and the bottom
        // of the screen
        sizes[x11_strut::BOTTOM] =
            display_height - std::clamp(window.geometry.y(), 0, display_height);
        sizes[x11_strut::BOTTOM_START_X] =
            std::clamp(window.geometry.x(), 0, display_width);
        sizes[x11_strut::BOTTOM_END_X] =
            std::clamp(window.geometry.end_x(), 0, display_width);
      }
    }
  }

  DBGP(
      "Reserved space: left=%d, right=%d, top=%d, "
      "bottom=%d",
      sizes[0], sizes[1], sizes[2], sizes[3]);

  XChangeProperty(display, window.window, atom, XA_CARDINAL, 32,
                  PropModeReplace, reinterpret_cast<unsigned char *>(&sizes),
                  4);

  atom = ATOM(_NET_WM_STRUT_PARTIAL);
  if (atom == None) return;

  DBGP(
      "Reserved space edges: left_start_y=%d, left_end_y=%d, "
      "right_start_y=%d, right_end_y=%d, top_start_x=%d, "
      "top_end_x=%d, bottom_start_x=%d, bottom_end_x=%d",
      sizes[4], sizes[5], sizes[6], sizes[7], sizes[8], sizes[9], sizes[10],
      sizes[11]);

  XChangeProperty(display, window.window, atom, XA_CARDINAL, 32,
                  PropModeReplace, reinterpret_cast<unsigned char *>(&sizes),
                  12);
}
#endif /* OWN_WINDOW */

#ifdef BUILD_XDBE
void xdbe_swap_buffers() {
  if (use_xdbe.get(*state)) {
    XdbeSwapInfo swap;

    swap.swap_window = window.window;
    swap.swap_action = XdbeBackground;
    XdbeSwapBuffers(display, &swap, 1);
  }
}
#else
void xpmdb_swap_buffers(void) {
  if (use_xpmdb.get(*state)) {
    XCopyArea(display, window.back_buffer, window.window, window.gc, 0, 0,
              window.geometry.width(), window.geometry.height(), 0, 0);
    XSetForeground(display, window.gc, 0);
    XFillRectangle(display, window.drawable, window.gc, 0, 0, window.geometry.width(),
                   window.geometry.height());
    XFlush(display);
  }
}
#endif /* BUILD_XDBE */

void print_kdb_led(const int keybit, char *p, unsigned int p_max_size) {
  XKeyboardState x;
  XGetKeyboardControl(display, &x);
  snprintf(p, p_max_size, "%s", (x.led_mask & keybit ? "On" : "Off"));
}
void print_key_caps_lock(struct text_object *obj, char *p,
                         unsigned int p_max_size) {
  (void)obj;
  print_kdb_led(1, p, p_max_size);
}

void print_key_num_lock(struct text_object *obj, char *p,
                        unsigned int p_max_size) {
  (void)obj;
  print_kdb_led(2, p, p_max_size);
}

void print_key_scroll_lock(struct text_object *obj, char *p,
                           unsigned int p_max_size) {
  (void)obj;
  print_kdb_led(4, p, p_max_size);
}

void print_keyboard_layout(struct text_object *obj, char *p,
                           unsigned int p_max_size) {
  (void)obj;

  char *group = NULL;
  XkbStateRec state;
  XkbDescPtr desc;

  XkbGetState(display, XkbUseCoreKbd, &state);
  desc = XkbGetKeyboard(display, XkbAllComponentsMask, XkbUseCoreKbd);
  group = XGetAtomName(display, desc->names->groups[state.group]);

  snprintf(p, p_max_size, "%s", (group != NULL ? group : "unknown"));
  XFree(group);
  XkbFreeKeyboard(desc, XkbGBN_AllComponentsMask, True);
}

void print_mouse_speed(struct text_object *obj, char *p,
                       unsigned int p_max_size) {
  (void)obj;
  int acc_num = 0;
  int acc_denom = 0;
  int threshold = 0;

  XGetPointerControl(display, &acc_num, &acc_denom, &threshold);
  snprintf(p, p_max_size, "%d%%", (110 - threshold));
}

/// @brief Returns a mask for the event_type
/// @param event_type Xlib event type
/// @return Xlib event mask
int ev_to_mask(int event_type, int button) {
  switch (event_type) {
    case KeyPress:
      return KeyPressMask;
    case KeyRelease:
      return KeyReleaseMask;
    case ButtonPress:
      return ButtonPressMask;
    case ButtonRelease:
      switch (button) {
        case 1:
          return ButtonReleaseMask | Button1MotionMask;
        case 2:
          return ButtonReleaseMask | Button2MotionMask;
        case 3:
          return ButtonReleaseMask | Button3MotionMask;
        case 4:
          return ButtonReleaseMask | Button4MotionMask;
        case 5:
          return ButtonReleaseMask | Button5MotionMask;
        default:
          return ButtonReleaseMask;
      }
    case EnterNotify:
      return EnterWindowMask;
    case LeaveNotify:
      return LeaveWindowMask;
    case MotionNotify:
      return PointerMotionMask;
    default:
      return NoEventMask;
  }
}

#ifdef BUILD_XINPUT
void propagate_xinput_event(const conky::xi_event_data *ev) {
  if (ev->evtype != XI_Motion && ev->evtype != XI_ButtonPress &&
      ev->evtype != XI_ButtonRelease) {
    return;
  }

  Window target = window.root;
  Window child = None;
  conky::vec2i target_pos = ev->pos;
  {
    std::vector<Window> below = query_x11_windows_at_pos(
        display, ev->pos_absolute,
        [](XWindowAttributes &a) { return a.map_state == IsViewable; });
    auto it = std::remove_if(below.begin(), below.end(),
                             [](Window w) { return w == window.window; });
    below.erase(it, below.end());
    if (!below.empty()) {
      target = below.back();

      int read_x, read_y;
      // Update event x and y coordinates to be target window relative
      XTranslateCoordinates(display, window.desktop, ev->event,
                            ev->pos_absolute.x(), ev->pos_absolute.y(), &read_x,
                            &read_y, &child);
      target_pos = conky::vec2i(read_x, read_y);
    }
  }

  auto events = ev->generate_events(target, child, target_pos);

  XUngrabPointer(display, CurrentTime);
  for (auto it : events) {
    auto ev = std::get<1>(it);
    XSendEvent(display, target, True, std::get<0>(it), ev);
    free(ev);
  }

  XFlush(display);
}
#endif

void propagate_x11_event(XEvent &ev, const void *cookie) {
  bool focus = ev.type == ButtonPress;

  // cookie must be allocated before propagation, and freed after
#ifdef BUILD_XINPUT
  if (ev.type == GenericEvent && ev.xgeneric.extension == window.xi_opcode) {
    if (cookie == nullptr) { return; }
    return propagate_xinput_event(
        reinterpret_cast<const conky::xi_event_data *>(cookie));
  }
#endif

  if (!(ev.type == KeyPress || ev.type == KeyRelease ||
        ev.type == ButtonPress || ev.type == ButtonRelease ||
        ev.type == MotionNotify || ev.type == EnterNotify ||
        ev.type == LeaveNotify)) {
    // Not a known input event; blindly propagating them causes loops and all
    // sorts of other evil.
    return;
  }
  // Note that using ev.xbutton is the same as using any of the above events.
  // It's only important we don't access fields that are not common to all of
  // them.

  ev.xbutton.window = window.desktop;
  ev.xbutton.x = ev.xbutton.x_root;
  ev.xbutton.y = ev.xbutton.y_root;
  ev.xbutton.time = CurrentTime;

  /* forward the event to the window below conky (e.g. caja) or desktop */
  {
    std::vector<Window> below = query_x11_windows_at_pos(
        display, conky::vec2i(ev.xbutton.x_root, ev.xbutton.y_root),
        [](XWindowAttributes &a) { return a.map_state == IsViewable; });
    auto it = std::remove_if(below.begin(), below.end(),
                             [](Window w) { return w == window.window; });
    below.erase(it, below.end());
    if (!below.empty()) {
      ev.xbutton.window = below.back();

      Window _ignore;
      // Update event x and y coordinates to be target window relative
      XTranslateCoordinates(display, window.root, ev.xbutton.window,
                            ev.xbutton.x_root, ev.xbutton.y_root, &ev.xbutton.x,
                            &ev.xbutton.y, &_ignore);
    }
    // drop below vector
  }

  int mask =
      ev_to_mask(ev.type, ev.type == ButtonRelease ? ev.xbutton.button : 0);
  XUngrabPointer(display, CurrentTime);
  XSendEvent(display, ev.xbutton.window, True, mask, &ev);
  if (focus) {
    XSetInputFocus(display, ev.xbutton.window, RevertToParent, CurrentTime);
  }
}

Window query_x11_top_parent(Display *display, Window child) {
  Window root = DefaultVRootWindow(display);

  if (child == None || child == root) return child;

  Window ret_root, parent, *children;
  std::uint32_t child_count;

  Window current = child;
  int i;
  do {
    if (XQueryTree(display, current, &ret_root, &parent, &children,
                   &child_count) == 0) {
      break;
    }
    if (child_count != 0) XFree(children);
    if (parent == root) break;
    current = parent;
  } while (true);

  return current;
}

std::vector<Window> x11_atom_window_list(Display *display, Window window,
                                         Atom atom) {
  Atom actual_type;
  int actual_format;
  unsigned long nitems;
  unsigned long bytes_after;
  unsigned char *data = nullptr;

  if (XGetWindowProperty(display, window, atom, 0, (~0L), False, XA_WINDOW,
                         &actual_type, &actual_format, &nitems, &bytes_after,
                         &data) == 0) {
    if (actual_format == XA_WINDOW && nitems > 0) {
      Window *wdata = reinterpret_cast<Window *>(data);
      std::vector<Window> result(wdata, wdata + nitems);
      XFree(data);
      return result;
    }
  }

  return std::vector<Window>{};
}

std::vector<Window> query_x11_windows(Display *display, bool eager) {
  Window root = DefaultRootWindow(display);

  std::vector<Window> result;

  Atom clients_atom = XInternAtom(display, "_NET_CLIENT_LIST_STACKING", True);
  if (clients_atom != 0) {
    result = x11_atom_window_list(display, root, clients_atom);
    if (!result.empty()) { return result; }
  }

  clients_atom = XInternAtom(display, "_NET_CLIENT_LIST", True);
  if (clients_atom != 0) {
    result = x11_atom_window_list(display, root, clients_atom);
    if (!result.empty()) { return result; }
  }

  // slowest method

  if (eager) {
    std::vector<Window> queue = {DefaultVRootWindow(display)};

    Window _ignored, *children;
    std::uint32_t count;

    const auto has_wm_hints = [&](Window window) {
      auto hints = XGetWMHints(display, window);
      bool result = hints != NULL;
      if (result) XFree(hints);
      return result;
    };

    while (!queue.empty()) {
      Window current = queue.back();
      queue.pop_back();
      if (XQueryTree(display, current, &_ignored, &_ignored, &children,
                     &count)) {
        for (size_t i = 0; i < count; i++) queue.push_back(children[i]);
        if (has_wm_hints(current)) result.push_back(current);
        if (count > 0) XFree(children);
      }
    }
  }

  return result;
}

Window query_x11_window_at_pos(Display *display, conky::vec2i pos, int device_id) {
  (void) device_id;
  Window root = DefaultVRootWindow(display);

  
  Window root_return;
  Window last = None;

  #ifdef BUILD_XINPUT
  // these values are ignored but NULL can't be passed to XIQueryPointer.
  double root_x_return, root_y_return, win_x_return, win_y_return;
  XIButtonState buttons_return;
  XIModifierState modifiers_return;
  XIGroupState group_return;

  
  XIQueryPointer(display,device_id, window.root, &root_return, &last, &root_x_return,
                &root_y_return, &win_x_return, &win_y_return, &buttons_return, &modifiers_return, &group_return);
  #else
  // these values are ignored but NULL can't be passed to XQueryPointer.
  int root_x_return, root_y_return, win_x_return, win_y_return;
  unsigned int mask_return;

  XQueryPointer(display, window.root, &root_return, &last, &root_x_return,
                &root_y_return, &win_x_return, &win_y_return, &mask_return);
  #endif

  if (last == 0) return root;
  return last;
}

std::vector<Window> query_x11_windows_at_pos(
    Display *display, conky::vec2i pos,
    std::function<bool(XWindowAttributes &)> predicate, bool eager) {
  std::vector<Window> result;

  Window root = DefaultVRootWindow(display);
  XWindowAttributes attr;

  for (Window current : query_x11_windows(display, eager)) {
    int pos_x, pos_y;
    Window _ignore;
    // Doesn't account for decorations. There's no sane way to do that.
    XTranslateCoordinates(display, current, root, 0, 0, &pos_x, &pos_y,
                          &_ignore);
    XGetWindowAttributes(display, current, &attr);

    if (pos_x <= pos.x() && pos_y <= pos.y() && pos_x + attr.width >= pos.x() &&
        pos_y + attr.height >= pos.y() && predicate(attr)) {
      result.push_back(current);
    }
  }

  return result;
}
