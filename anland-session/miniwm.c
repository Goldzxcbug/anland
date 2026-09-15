/* mini-wm.c — Xwayland rootless bootstrap WM (no decoration / no reparent,
 * #32)
 *
 * Xwayland 24.1 rootless window surfacing condition
 * (hw/xwayland/xwayland-window.c): ensure_surface_for_window requires
 * redirectDraw == Manual — this WM does XCompositeRedirectWindow(Manual) at
 * MapRequest (after redirect, SetWindowPixmap triggers ensure; already-
 * mapped windows can be adopted this way too). No reparenting: client
 * windows stay children of root, exactly one wl_surface per window; O-R
 * popup windows never hit MapRequest → they don't surface.
 * Duties: SubstructureRedirect takes over MapRequest/ConfigureRequest
 * (pass-through) + WM_STATE + input focus (the minimal ICCCM set).
 *
 * Control channel v2 (host daemon → X-side operations):
 *   every surfaced Xwayland window sends a WL_SURFACE_SERIAL ClientMessage
 *   to root (data.l[0]=lo, l[1]=hi; this WM always receives it thanks to
 *   SubstructureRedirectMask) — building the serial ↔ X window pair table;
 *   listening for text-line commands on $ANLAND_WM_SOCK, else
 *   $XDG_RUNTIME_DIR/anland-wm.sock — the awl runtime dir, where Xwayland
 *   also finds wayland-0 (convention: container /run/anland, a droidspaces
 *   bind mount of the host runtime_dir /data/local/tmp/awl; the daemon
 *   derives the same path from its config "runtime_dir"), else the
 *   historical /host/data/local/tmp/anland-wm.sock (root only: that dir is
 *   0771 shell, the awl dir 0777):
 *     S <serial> <w> <h>   resize that X window (Android window size sync)
 *     C <serial>           request close (WM_DELETE_WINDOW; XKillClient if
 *                          the protocol is absent)
 *     R <serial>           raise (pointer entered / touch landed on its
 *                          Android window)
 *     F <serial>           raise + X input focus (its Activity got focus)
 *   The host daemon only knows surface↔serial (set_serial), speaks no X
 *   protocol — a two-level pairing.
 *
 * X-space geometry (2026-09-15, "Xwayland unclickable"): every toplevel is
 * its own Android window, but input still travels through ONE X screen —
 * rootless Xwayland dispatches wl_pointer/wl_touch at drawable.x/y + the
 * surface-local position and lets the DIX hit-test the X stack
 * (xwayland-input.c dispatch_absolute_motion / xwl_touch_send_event /
 * xwl_xy_to_window); the pointer is clamped to the screen (mipointer
 * limits). So this WM pins every managed toplevel at (0,0) (MapRequest,
 * ConfigureRequest, the S command — a client's own placement has no visual
 * meaning here anyway) so the whole window lies inside the screen the host
 * daemon grows to the largest Android window, and R/F mirror the Android
 * side onto the X stacking order so the DIX finds the window the event was
 * actually aimed at (what mutter/kwin do as XWM by restacking).
 *
 * Single-threaded select: X connection fd + listen fd; X events batched via
 * XPending.
 */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>

#define MAX_PAIRS 128

/* Control socket path (see the header): resolved once, sized for sun_path. */
static char g_sock_path[108];
static const char* wm_sock_path(void) {
    if (g_sock_path[0]) return g_sock_path;
    const char* p = getenv("ANLAND_WM_SOCK");
    const char* rt = getenv("XDG_RUNTIME_DIR");
    if (p && *p)
        snprintf(g_sock_path, sizeof g_sock_path, "%s", p);
    else if (rt && *rt)
        snprintf(g_sock_path, sizeof g_sock_path, "%s/anland-wm.sock", rt);
    else
        snprintf(g_sock_path, sizeof g_sock_path, "/host/data/local/tmp/anland-wm.sock");
    return g_sock_path;
}

/* A second MapRequest for the same window (unmap/remap) redirects again →
 * BadAccess; other stray errors shouldn't kill the WM either — log and
 * continue */
static int on_x_error(Display* d, XErrorEvent* e) {
    fprintf(stderr, "mini-wm: x error opcode=%d minor=%d rid=0x%lx\n",
            e->request_code, e->minor_code, e->resourceid);
    return 0;
}

static void set_wm_state(Display* d, Window w, unsigned long state) {
    static Atom wm_state = None;
    if (wm_state == None) wm_state = XInternAtom(d, "WM_STATE", False);
    unsigned long data[2] = { state, None };
    XChangeProperty(d, w, wm_state, wm_state, 32, PropModeReplace,
                    (unsigned char*)data, 2);
}

/* ---- serial ↔ X window pair table ---- */
static struct { unsigned long long serial; Window win; } s_pairs[MAX_PAIRS];
static int s_n_pairs = 0;
static Atom g_serial_atom = None;   /* "WL_SURFACE_SERIAL" (only created by
                                      * Xwayland's first window, so we create
                                      * it at startup with makeit — same name,
                                      * same XID, no side effect) */

static void pair_remember(unsigned long long serial, Window win) {
    if (!serial || win == None) return;
    for (int i = 0; i < s_n_pairs; i++)
        if (s_pairs[i].serial == serial) { s_pairs[i].win = win; return; }
    if (s_n_pairs < MAX_PAIRS) {
        s_pairs[s_n_pairs].serial = serial;
        s_pairs[s_n_pairs].win = win;
        s_n_pairs++;
        fprintf(stderr, "mini-wm: serial %llu -> window 0x%lx\n", serial, win);
    }
}
static Window pair_lookup(unsigned long long serial) {
    for (int i = 0; i < s_n_pairs; i++)
        if (s_pairs[i].serial == serial) return s_pairs[i].win;
    return None;
}
static void pair_forget_window(Window win) {
    for (int i = 0; i < s_n_pairs; i++)
        if (s_pairs[i].win == win) {
            memmove(&s_pairs[i], &s_pairs[i + 1],
                    (size_t)(s_n_pairs - i - 1) * sizeof(s_pairs[0]));
            s_n_pairs--;
            return;
        }
}

/* Ask an X window to close: WM_DELETE_WINDOW (ICCCM); force-kill if the
 * protocol is absent */
static void request_close(Display* d, Window w) {
    static Atom protocols = None, wm_delete = None;
    if (protocols == None) {
        protocols = XInternAtom(d, "WM_PROTOCOLS", False);
        wm_delete = XInternAtom(d, "WM_DELETE_WINDOW", False);
    }
    Atom* protos = NULL;
    int n = 0;
    bool found = false;
    if (XGetWMProtocols(d, w, &protos, &n)) {
        for (int i = 0; i < n; i++)
            if (protos[i] == wm_delete) { found = true; break; }
        if (protos) XFree(protos);
    }
    if (found) {
        XEvent ev = { 0 };
        ev.type = ClientMessage;
        ev.xclient.window = w;
        ev.xclient.message_type = protocols;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = (long)wm_delete;
        ev.xclient.data.l[1] = CurrentTime;
        XSendEvent(d, w, False, NoEventMask, &ev);
        XFlush(d);
        fprintf(stderr, "mini-wm: WM_DELETE_WINDOW -> 0x%lx\n", w);
    } else {
        XKillClient(d, w);
        fprintf(stderr, "mini-wm: kill client (window 0x%lx)\n", w);
    }
}

/* ---- Control-channel commands ---- */
static void handle_command(Display* d, char* line) {
    unsigned long long serial = 0;
    int w = 0, h = 0;
    if (sscanf(line, "S %llu %d %d", &serial, &w, &h) == 3) {
        Window win = pair_lookup(serial);
        if (win != None && w > 0 && h > 0) {
            /* (0,0) pin: the Android window shows the whole X window, so its
             * X position only matters for input — inside the screen */
            XMoveResizeWindow(d, win, 0, 0, (unsigned)w, (unsigned)h);
            XFlush(d);
            fprintf(stderr, "mini-wm: resize 0x%lx -> %dx%d\n", win, w, h);
        } else {
            fprintf(stderr, "mini-wm: resize serial %llu has no paired window\n", serial);
        }
    } else if (sscanf(line, "C %llu", &serial) == 1) {
        Window win = pair_lookup(serial);
        if (win != None) request_close(d, win);
        else fprintf(stderr, "mini-wm: close serial %llu has no paired window\n", serial);
    } else if (sscanf(line, "R %llu", &serial) == 1 ||
               sscanf(line, "F %llu", &serial) == 1) {
        /* raise (R) / raise + focus (F): the Android side tells us which X
         * window input is about to reach — put it on top of the X stack so
         * the DIX hit-test agrees (see the header) */
        Window win = pair_lookup(serial);
        if (win != None) {
            XRaiseWindow(d, win);
            if (line[0] == 'F')
                XSetInputFocus(d, win, RevertToPointerRoot, CurrentTime);
            XFlush(d);
            fprintf(stderr, "mini-wm: %s 0x%lx\n",
                    line[0] == 'F' ? "raise+focus" : "raise", win);
        } else {
            fprintf(stderr, "mini-wm: %c serial %llu has no paired window\n",
                    line[0], serial);
        }
    } else {
        fprintf(stderr, "mini-wm: unknown command '%s'\n", line);
    }
}

int main(void) {
    Display* d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "mini-wm: cannot open display\n"); return 1; }
    int ev = 0, err = 0;
    if (!XCompositeQueryExtension(d, &ev, &err)) {
        fprintf(stderr, "mini-wm: no composite extension\n");
        return 1;
    }
    Window root = DefaultRootWindow(d);
    XSetErrorHandler(on_x_error);
    XSelectInput(d, root, SubstructureRedirectMask | SubstructureNotifyMask);
    g_serial_atom = XInternAtom(d, "WL_SURFACE_SERIAL", False);

    /* Adopt already-mapped normal windows (WM started after the clients;
     * fresh starts see none). Redirecting an already-viewable window
     * surfaces it too, via the SetWindowPixmap path. */
    {
        Window rr, pr, *kids = NULL;
        unsigned n = 0;
        if (XQueryTree(d, root, &rr, &pr, &kids, &n)) {
            for (unsigned i = 0; i < n; i++) {
                XWindowAttributes wa;
                if (kids[i] != root &&
                    XGetWindowAttributes(d, kids[i], &wa) &&
                    wa.map_state == IsViewable && !wa.override_redirect) {
                    XCompositeRedirectWindow(d, kids[i],
                                             CompositeRedirectManual);
                    set_wm_state(d, kids[i], NormalState);
                }
            }
        }
        if (kids) XFree(kids);
    }

    /* Control channel */
    unlink(wm_sock_path());
    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0) { perror("mini-wm: socket"); return 1; }
    struct sockaddr_un sa = { 0 };
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", wm_sock_path());
    if (bind(lfd, (struct sockaddr*)&sa, sizeof(sa)) != 0 ||
        listen(lfd, 4) != 0) {
        perror("mini-wm: bind/listen");
        return 1;
    }

    XSync(d, False);
    fprintf(stderr, "mini-wm: running (root 0x%lx, sock %s)\n",
            root, wm_sock_path());

    int xfd = XConnectionNumber(d);
    int maxfd = xfd > lfd ? xfd : lfd;
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        FD_SET(lfd, &fds);
        if (select(maxfd + 1, &fds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            perror("mini-wm: select");
            break;
        }
        if (FD_ISSET(lfd, &fds)) {
            int cfd = accept(lfd, NULL, NULL);
            if (cfd >= 0) {
                char line[256] = { 0 };
                ssize_t got = read(cfd, line, sizeof(line) - 1);
                if (got > 0) {
                    line[got] = '\0';
                    line[strcspn(line, "\r\n")] = '\0';
                    handle_command(d, line);
                }
                close(cfd);
            }
        }
        while (XPending(d)) {
            XEvent e;
            XNextEvent(d, &e);
            switch (e.type) {
            case MapRequest: {
                Window w = e.xmaprequest.window;
                XMoveWindow(d, w, 0, 0);   /* (0,0) pin, see the header (covers a CreateWindow position) */
                XCompositeRedirectWindow(d, w, CompositeRedirectManual);
                XMapWindow(d, w);
                set_wm_state(d, w, NormalState);
                XSetInputFocus(d, w, RevertToPointerRoot, CurrentTime);
                fprintf(stderr, "mini-wm: mapped 0x%lx\n", w);
                break;
            }
            case ConfigureRequest: {
                /* pass-through for size/border/stacking; the position is
                 * always (0,0) — a client re-centering itself on the (now
                 * larger) screen would push part of its window outside it */
                XConfigureRequestEvent* c = &e.xconfigurerequest;
                XWindowChanges wc;
                memset(&wc, 0, sizeof wc);
                wc.x = 0; wc.y = 0;
                wc.width = c->width; wc.height = c->height;
                wc.border_width = c->border_width;
                wc.sibling = c->above; wc.stack_mode = c->detail;
                XConfigureWindow(d, c->window,
                                 (c->value_mask & ~(unsigned)(CWX | CWY)) | CWX | CWY,
                                 &wc);
                break;
            }
            case CirculateRequest:
                XCirculateSubwindows(d, e.xcirculaterequest.window,
                                     e.xcirculaterequest.place);
                break;
            case ClientMessage: {
                /* Xwayland window association: WL_SURFACE_SERIAL (l[0]=lo, l[1]=hi) */
                if (e.xclient.message_type == g_serial_atom &&
                    e.xclient.format == 32) {
                    unsigned long long serial =
                        ((unsigned long long)(uint32_t)e.xclient.data.l[1]
                         << 32) | (uint32_t)e.xclient.data.l[0];
                    pair_remember(serial, e.xclient.window);
                }
                break;
            }
            case DestroyNotify:
                pair_forget_window(e.xdestroywindow.window);
                break;
            default:
                break;   /* Create/Unmap/Configure Notify etc.: Xwayland's own business */
            }
            XSync(d, False);
        }
    }
    close(lfd);
    unlink(wm_sock_path());
    return 0;
}
