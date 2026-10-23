#include "x11.h"

#include <SDL_syswm.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>

static int get_x11(SDL_Window *win, Display **dpy, Window *xw)
{
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_X11)
        return -1;
    *dpy = info.info.x11.display;
    *xw  = info.info.x11.window;
    return 0;
}

unsigned long x11_window_id(SDL_Window *win)
{
    Display *dpy;
    Window xw;
    return get_x11(win, &dpy, &xw) == 0 ? xw : 0;
}

static int get_atoms(Display *dpy, Window w, Atom prop, Atom type,
                     unsigned long max, unsigned char **data, unsigned long *n)
{
    Atom actual;
    int fmt;
    unsigned long after;
    *data = NULL;
    if (XGetWindowProperty(dpy, w, prop, 0, max, False, type, &actual, &fmt,
                           n, &after, data) != Success || !*data) {
        *n = 0;
        return -1;
    }
    return 0;
}

static int has_atom(Display *dpy, Window w, Atom prop, Atom want)
{
    unsigned char *data;
    unsigned long n;
    int found = 0;
    if (get_atoms(dpy, w, prop, XA_ATOM, 32, &data, &n) == 0) {
        for (unsigned long i = 0; i < n; i++)
            if (((Atom *)data)[i] == want)
                found = 1;
        XFree(data);
    }
    return found;
}

static long get_cardinal(Display *dpy, Window w, Atom prop, long fallback)
{
    unsigned char *data;
    unsigned long n;
    long v = fallback;
    if (get_atoms(dpy, w, prop, XA_CARDINAL, 1, &data, &n) == 0) {
        if (n == 1)
            v = (long)((unsigned long *)data)[0];
        XFree(data);
    }
    return v;
}

static int ignore_errors(Display *d, XErrorEvent *e)
{
    /* A window can close between listing and inspecting it. */
    (void)d;
    (void)e;
    return 0;
}

/* Whether the atom list (as from get_atoms) holds want. */
static int list_has(const unsigned char *data, unsigned long n, Atom want)
{
    for (unsigned long i = 0; i < n; i++)
        if (((const Atom *)data)[i] == want)
            return 1;
    return 0;
}

int x11_desktop_covered(void)
{
    static Display *dpy;
    static Atom stacking, cur_desk, wm_desk, state, hidden, fullscreen,
                max_h, max_v, type, normal;
    if (!dpy) {
        if (!(dpy = XOpenDisplay(NULL)))
            return 0;
        stacking   = XInternAtom(dpy, "_NET_CLIENT_LIST_STACKING", False);
        cur_desk   = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
        wm_desk    = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
        state      = XInternAtom(dpy, "_NET_WM_STATE", False);
        hidden     = XInternAtom(dpy, "_NET_WM_STATE_HIDDEN", False);
        fullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
        max_h      = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
        max_v      = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_VERT", False);
        type       = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
        normal     = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    }

    Window root = DefaultRootWindow(dpy);
    int root_w = DisplayWidth(dpy, DefaultScreen(dpy));

    long desk = get_cardinal(dpy, root, cur_desk, 0);

    unsigned char *data;
    unsigned long n;
    if (get_atoms(dpy, root, stacking, XA_WINDOW, 4096, &data, &n) != 0)
        return 0;

    /* Each property read is a round trip to the server, so the window's
     * state is read once and checked for everything. */
    XErrorHandler old = XSetErrorHandler(ignore_errors);
    int covered = 0;
    for (unsigned long i = 0; i < n && !covered; i++) {
        Window w = ((Window *)data)[i];
        if (!has_atom(dpy, w, type, normal))
            continue;
        unsigned char *st;
        unsigned long ns;
        if (get_atoms(dpy, w, state, XA_ATOM, 32, &st, &ns) != 0)
            continue;               /* no state: neither maximized nor full */
        int big = !list_has(st, ns, hidden) &&
                  (list_has(st, ns, fullscreen) ||
                   (list_has(st, ns, max_h) && list_has(st, ns, max_v)));
        XFree(st);
        if (!big)
            continue;
        long d = get_cardinal(dpy, w, wm_desk, desk);
        if (d != desk && d != 0xFFFFFFFFl)
            continue;

        /* Only counts if it spans the whole screen; a maximized window on
         * one of several monitors leaves the others showing. */
        XWindowAttributes a;
        if (XGetWindowAttributes(dpy, w, &a) && a.width >= root_w * 9 / 10)
            covered = 1;
    }
    XSync(dpy, False);
    XSetErrorHandler(old);
    XFree(data);
    return covered;
}

int x11_root_size(int *w, int *h)
{
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy)
        return -1;
    int scr = DefaultScreen(dpy);
    *w = DisplayWidth(dpy, scr);
    *h = DisplayHeight(dpy, scr);
    XCloseDisplay(dpy);
    return 0;
}

void x11_desktop_hint(void)
{
    /* SDL writes _NET_WM_WINDOW_TYPE itself during SDL_CreateWindow and
     * again when showing, so the type has to come through its hint. */
    SDL_SetHint(SDL_HINT_X11_WINDOW_TYPE, "_NET_WM_WINDOW_TYPE_DESKTOP");
}

static void send_wm_message(Display *dpy, Window xw, Atom msg,
                            long d0, long d1, long d2)
{
    XEvent ev = {0};
    ev.xclient.type         = ClientMessage;
    ev.xclient.window       = xw;
    ev.xclient.message_type = msg;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = d0;
    ev.xclient.data.l[1]    = d1;
    ev.xclient.data.l[2]    = d2;
    ev.xclient.data.l[3]    = 1;   /* source: normal application */
    XSendEvent(dpy, DefaultRootWindow(dpy), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
}

int x11_make_desktop(SDL_Window *win)
{
    Display *dpy;
    Window xw;
    if (get_x11(win, &dpy, &xw) != 0)
        return -1;

    /* The window is mapped now, so state changes go through the WM as
     * EWMH client messages (1 = _NET_WM_STATE_ADD). */
    Atom state = XInternAtom(dpy, "_NET_WM_STATE", False);
    send_wm_message(dpy, xw, state, 1,
                    XInternAtom(dpy, "_NET_WM_STATE_BELOW", False),
                    XInternAtom(dpy, "_NET_WM_STATE_STICKY", False));
    send_wm_message(dpy, xw, state, 1,
                    XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False),
                    XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False));

    /* 0xFFFFFFFF = show on all workspaces. */
    send_wm_message(dpy, xw, XInternAtom(dpy, "_NET_WM_DESKTOP", False),
                    0xFFFFFFFFl, 0, 0);

    XLowerWindow(dpy, xw);
    XFlush(dpy);
    return 0;
}

static int is_desktop_window(Display *dpy, Window w, Atom type, Atom desktop)
{
    Atom actual;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    int found = 0;

    if (XGetWindowProperty(dpy, w, type, 0, 16, False, XA_ATOM, &actual, &fmt,
                           &n, &after, &data) == Success && data) {
        Atom *atoms = (Atom *)data;
        for (unsigned long i = 0; i < n; i++)
            if (atoms[i] == desktop)
                found = 1;
    }
    if (data)
        XFree(data);
    return found;
}

int x11_count_other_desktops(SDL_Window *win)
{
    Display *dpy;
    Window self;
    if (get_x11(win, &dpy, &self) != 0)
        return -1;

    Atom list    = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    Atom type    = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom desktop = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);

    Atom actual;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, DefaultRootWindow(dpy), list, 0, 4096, False,
                           XA_WINDOW, &actual, &fmt, &n, &after, &data) != Success
        || !data)
        return -1;

    int count = 0;
    Window *wins = (Window *)data;
    for (unsigned long i = 0; i < n; i++)
        if (wins[i] != self && is_desktop_window(dpy, wins[i], type, desktop))
            count++;

    XFree(data);
    return count;
}
