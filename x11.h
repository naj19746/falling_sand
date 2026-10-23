#ifndef X11_H
#define X11_H

#include <SDL.h>

/* The X window behind an SDL window, or 0. */
unsigned long x11_window_id(SDL_Window *win);

/* True when a fullscreen or maximized window on the current workspace
 * hides the whole desktop, so there's no point drawing. */
int x11_desktop_covered(void);

/* Size of the X root window (spans all monitors). Returns 0 on success. */
int x11_root_size(int *w, int *h);

/* Make the next SDL_CreateWindow produce a DESKTOP-type window. */
void x11_desktop_hint(void);

/* Once the window is shown: keep it below, on all workspaces, out of the
 * taskbar and pager. */
int x11_make_desktop(SDL_Window *win);

/* Number of other desktop-type windows (e.g. Caja drawing icons), which
 * will fight ours for the bottom layer. Returns -1 if it can't tell. */
int x11_count_other_desktops(SDL_Window *win);

#endif
