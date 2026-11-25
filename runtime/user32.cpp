// user32.cpp - USER32 shims: window classes and windows, the message queue,
// paint/DC stubs and the input helpers the host layer feeds.
//
// Windows are bookkeeping only: no pixels are produced here. The host layer
// (Task 7) pushes real events in with host_post_message() and the guest pulls
// them out through PeekMessageA/GetMessageA exactly as it would on Win32.
#include "user32_internal.h"
#include "wsock_internal.h"
#include "gdi_image.h"
#include "resources.h"
#include "win32.h"
#include "../platform/os.h"
#include "memory.h"

#include <array>
#include <deque>
#include <map>
#include <string>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Defined in kernel32.cpp with the scheduler.
void sched_checkpoint();

namespace user32 {

std::map<std::string, WndClass> &classes() {
    static std::map<std::string, WndClass> m;
    return m;
}
std::map<uint32_t, Window> &windows() {
    static std::map<uint32_t, Window> m;
    return m;
}
std::deque<Msg> &queue() {
    static std::deque<Msg> q;
    return q;
}

uint32_t g_next_hwnd = 0x00020004;
uint32_t g_next_gdi = 0x00028004;
uint32_t g_main_hwnd = 0;
uint32_t g_cursor = 0;
int32_t g_cursor_x = 0, g_cursor_y = 0;
int32_t g_cursor_show = 0;
bool g_cursor_warp_active = false;
int32_t g_clip[4] = {0, 0, 0, 0};
bool g_clipped = false;
bool (*g_message_waiter)() = nullptr;
void (*g_window_shown)(uint32_t) = nullptr;
uint8_t g_key_state[256] = {0};
static uint8_t g_async_pressed[256] = {0};

// WS_VISIBLE: a window created with it is shown as part of creation.
const uint32_t WS_VISIBLE = 0x10000000u;

std::string lower(std::string s) {
    for (char &ch : s)
        ch = (char)tolower((unsigned char)ch);
    return s;
}

// Class names arrive either as a pointer to a string or as an atom (< 0x10000).
std::string class_key(uint32_t p, bool wide) {
    if (p && p < 0x10000) {
        char buf[32];
        snprintf(buf, sizeof buf, "#atom%u", p);
        return buf;
    }
    return lower(wide ? gm_wstr(p, 256) : gm_str(p, 256));
}

Window *find_window(uint32_t hwnd) {
    if (hwnd == desktop_handle) {
        static Window desktop;
        uint32_t w = 1024, h = 768, bpp = 32;
        win32_display_mode(&w, &h, &bpp);
        desktop.hwnd = desktop_handle;
        desktop.w = w;
        desktop.h = h;
        desktop.visible = true;
        desktop.unicode = true;
        return &desktop;
    }
    auto it = windows().find(hwnd);
    return it == windows().end() ? nullptr : &it->second;
}

// Windows tells a window where it is and how big it is as soon as it exists,
// and again whenever that changes; a game sizes its blit rectangle from those
// two messages and never asks again. No non-client area is modelled here.
static void send_geometry(X86 *c, uint32_t hwnd, const Window *w, bool moved, bool sized) {
    if (sized)
        host_dispatch_to_wndproc(c, hwnd, 0x0005 /* WM_SIZE */, 0 /* SIZE_RESTORED */,
                                 ((uint32_t)(uint16_t)w->h << 16) | (uint16_t)w->w);
    if (moved)
        host_dispatch_to_wndproc(c, hwnd, 0x0003 /* WM_MOVE */, 0,
                                 ((uint32_t)(uint16_t)w->y << 16) | (uint16_t)w->x);
}

void store_msg(uint32_t p, const Msg &m) {
    if (!p)
        return;
    wr32(p + 0, m.hwnd);
    wr32(p + 4, m.message);
    wr32(p + 8, m.wparam);
    wr32(p + 12, m.lparam);
    wr32(p + 16, m.time);
    wr32(p + 20, m.ptx);
    wr32(p + 24, m.pty);
}

} // namespace user32

using namespace user32;

// ---------------------------------------------------------------------------
// Host bridge
// ---------------------------------------------------------------------------
void host_post_message(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam) {
    // Record timer delivery on the same cadence seam as other host messages.
    if (msg == 0x0113)
        host_note_cadence("WM_TIMER");
    Msg m{hwnd, msg, wparam, lparam, host_millis(), (uint32_t)g_cursor_x, (uint32_t)g_cursor_y};
    m.thread = hwnd && find_window(hwnd) ? find_window(hwnd)->thread : guest_current_thread_id();
    queue().push_back(m);
}
uint32_t host_main_window() {
    return g_main_hwnd;
}
uint32_t host_close_target() {
    // A framework's first HWND can be an invisible message helper. Close the
    // foremost form instead, walking visible owners past modal dialogs so
    // the application's normal form-shutdown path stops its workers first.
    auto eligible = [](const Window *w) {
        return w && w->hwnd != desktop_handle && !w->parent && w->visible && w->w > 0 && w->h > 0 &&
               !(w->exstyle & 0x08000080u); // WS_EX_NOACTIVATE / WS_EX_TOOLWINDOW
    };
    auto order = window_z_order(0);
    for (auto i = order.rbegin(); i != order.rend(); ++i) {
        Window *w = find_window(*i);
        if (!eligible(w))
            continue;
        for (size_t n = 0; n < windows().size(); ++n) {
            Window *owner = find_window(w->owner);
            if (!eligible(owner) || owner == w)
                break;
            w = owner;
        }
        return w->hwnd;
    }
    return host_main_window();
}
uint32_t host_window_proc(uint32_t hwnd) {
    Window *w = find_window(hwnd);
    return w ? w->wndproc : 0;
}
bool host_window_rect(uint32_t hwnd, int32_t *x, int32_t *y, int32_t *w, int32_t *h) {
    Window *win = find_window(hwnd);
    if (!win)
        return false;
    if (x)
        *x = win->x;
    if (y)
        *y = win->y;
    if (w)
        *w = win->w;
    if (h)
        *h = win->h;
    return true;
}
void host_set_client_size(uint32_t hwnd, int32_t w, int32_t h) {
    if (Window *win = find_window(hwnd)) {
        win->w = w;
        win->h = h;
        win->update_pending = true;
    }
}
void host_set_key_state(int vk, bool down) {
    if (vk >= 0 && vk < 256) {
        // A press survives release until this key's next asynchronous poll.
        if (down && !(g_key_state[vk] & 0x80))
            g_async_pressed[vk] = 1;
        g_key_state[vk] = down ? 0x80 : 0x00;
    }
}
void host_set_cursor_pos(int32_t x, int32_t y) {
    g_cursor_x = x;
    g_cursor_y = y;
}
void host_set_client_cursor_pos(uint32_t hwnd, int32_t x, int32_t y) {
    int32_t origin_x = 0, origin_y = 0;
    client_origin(hwnd, &origin_x, &origin_y);
    host_set_cursor_pos(int32_t(uint32_t(x) + uint32_t(origin_x)),
                        int32_t(uint32_t(y) + uint32_t(origin_y)));
}
void host_set_message_waiter(bool (*fn)()) {
    g_message_waiter = fn;
}
void host_set_window_shown_callback(void (*fn)(uint32_t)) {
    g_window_shown = fn;
}
bool host_messages_pending() {
    return !queue().empty();
}
bool host_window_visible(uint32_t hwnd) {
    Window *w = find_window(hwnd);
    return w && w->visible;
}
bool host_cursor_visible() {
    return g_cursor_show >= 0;
}
bool host_cursor_clip(int32_t *out) {
    if (!g_clipped)
        return false;
    for (int i = 0; i < 4; ++i)
        out[i] = g_clip[i];
    return true;
}
bool host_cursor_warp_active() {
    return g_cursor_warp_active;
}
void host_get_cursor_pos(int32_t *x, int32_t *y) {
    if (x)
        *x = g_cursor_x;
    if (y)
        *y = g_cursor_y;
}

uint32_t host_dispatch_to_wndproc(X86 *c, uint32_t hwnd, uint32_t msg, uint32_t wparam,
                                  uint32_t lparam) {
    Window *w = find_window(hwnd);
    uint32_t proc = w ? w->wndproc : 0;
    if (!proc) {
        LOGV("message %04x for window %08x has no WNDPROC", msg, hwnd);
        return 0;
    }
    return guest_call(c, proc, hwnd, msg, wparam, lparam);
}

namespace user32 {

// ---------------------------------------------------------------------------
// Classes and windows
// ---------------------------------------------------------------------------
// WNDCLASSA and WNDCLASSEXA hold the same fields in the same order; the Ex
// form puts cbSize in front of them and hIconSm behind, so one reader with a
// field offset serves both. `shift` is 4 for the Ex form.
void register_class_named(X86 *c, bool wide, uint32_t shift) {
    uint32_t p = arg(c, 0);
    if (!p) {
        set_eax(c, 0);
        return;
    }
    p += shift;
    WndClass wc;
    wc.style = rd32(p + 0);
    wc.wndproc = rd32(p + 4);
    wc.cls_extra = rd32(p + 8);
    wc.wnd_extra = rd32(p + 12);
    wc.hinstance = rd32(p + 16);
    wc.hicon = rd32(p + 20);
    wc.hcursor = rd32(p + 24);
    wc.hbrush = rd32(p + 28);
    wc.menu_id = rd32(p + 32) < 0x10000 ? rd32(p + 32) : 0;
    wc.menu = wc.menu_id ? "" : (wide ? gm_wstr(rd32(p + 32)) : gm_str(rd32(p + 32)));
    wc.unicode = wide;
    wc.extra.resize((wc.cls_extra + 3) / 4);
    std::string name = class_key(rd32(p + 36), wide);
    wc.name = wide ? gm_wstr(rd32(p + 36)) : gm_str(rd32(p + 36));

    // The class is also reachable through the returned ATOM, which is what a
    // caller passes to CreateWindowExA when it keeps the RegisterClass result.
    uint32_t atom = 0xc000 + (uint32_t)classes().size();
    wc.atom = atom;
    classes()[name] = wc;
    char atom_key[32];
    snprintf(atom_key, sizeof atom_key, "#atom%u", atom);
    classes()[atom_key] = wc;
    LOGV("RegisterClassA(\"%s\", wndproc=%08x) -> atom %u", name.c_str(), wc.wndproc, atom);
    set_eax(c, atom);
}

void u_RegisterClassA(X86 *c) {
    register_class_named(c, false, 0);
}

void u_RegisterClassExA(X86 *c) {
    register_class_named(c, false, 4);
}

void u_UnregisterClassA(X86 *c) {
    classes().erase(class_key(arg(c, 0)));
    set_eax(c, 1);
}

// Track WH_CBT hooks (idHook == 5) so MFC's _AfxCbtFilterHook can subclass
// newly created windows and dialogs via HCBT_CREATEWND (3).
struct HookEntry {
    uint32_t handle;
    int32_t id_hook;
    uint32_t proc;
    uint32_t tid;
};
std::vector<HookEntry> &hooks() {
    static std::vector<HookEntry> v;
    return v;
}
void fire_cbt_createwnd(X86 *c, uint32_t hwnd, uint32_t cs) {
    uint32_t tid = guest_current_thread_id();
    // Copy list in case the hook unhooks itself inside the callback (MFC does!).
    auto snap = hooks();
    uint32_t cbt = heap_alloc(8, true);
    if (cbt) {
        wr32(cbt + 0, cs);
        wr32(cbt + 4, 0); // hwndInsertAfter
    }
    for (auto it = snap.rbegin(); it != snap.rend(); ++it) {
        if (it->id_hook == 5 && (it->tid == 0 || it->tid == tid) && it->proc) {
            guest_call(c, it->proc, 3u /* HCBT_CREATEWND */, hwnd, cbt);
        }
    }
    if (cbt)
        heap_free(cbt);
}

// Create a guest window and deliver WM_NCCREATE/WM_CREATE through its window procedure.
// Honor callback rejection and the initial visibility transition before returning the window handle.
namespace {
void create_window_body(X86 *c, bool wide) {
    uint32_t exstyle = arg(c, 0);
    std::string cls = class_key(arg(c, 1), wide);
    std::string title = wide ? gm_wstr(arg(c, 2)) : gm_str(arg(c, 2));
    uint32_t style = arg(c, 3);
    int32_t x = (int32_t)arg(c, 4), y = (int32_t)arg(c, 5);
    int32_t w = (int32_t)arg(c, 6), h = (int32_t)arg(c, 7);
    uint32_t hinst = arg(c, 10), param = arg(c, 11);

    ensure_system_classes();
    auto ci = classes().find(cls);
    if (ci == classes().end()) {
        LOGW("CreateWindowExA: class \"%s\" was never registered", cls.c_str());
        set_last_error(1407); // ERROR_CANNOT_FIND_WND_CLASS
        set_eax(c, 0);
        return;
    }
    // CW_USEDEFAULT
    if (x == (int32_t)0x80000000)
        x = 0;
    if (y == (int32_t)0x80000000)
        y = 0;
    if (w == (int32_t)0x80000000)
        w = 640;
    if (h == (int32_t)0x80000000)
        h = 480;

    uint32_t hwnd = g_next_hwnd;
    g_next_hwnd += 4;
    Window win;
    win.hwnd = hwnd;
    win.wndproc = ci->second.wndproc;
    win.style = style;
    win.exstyle = exstyle;
    win.x = x;
    win.y = y;
    win.w = w;
    win.h = h;
    win.cls = class_key(ci->second.atom);
    win.title_utf8 = title;
    win.unicode = wide;
    win.parent = style & 0x40000000u ? arg(c, 8) : 0;
    win.owner = win.parent ? 0 : arg(c, 8);
    win.menu = win.parent ? 0 : arg(c, 9);
    win.id = win.parent ? arg(c, 9) : 0;
    win.enabled = !(style & 0x08000000u);
    win.thread = guest_current_thread_id();
    win.hinstance = hinst;
    win.extra.assign((ci->second.wnd_extra + 3) / 4, 0);
    windows()[hwnd] = win;
    if (!g_main_hwnd)
        g_main_hwnd = hwnd;
    LOGV("CreateWindowExA(\"%s\", \"%s\", %dx%d at %d,%d) -> %08x", cls.c_str(), title.c_str(), w,
         h, x, y, hwnd);

    // Windows sends WM_CREATE (with a CREATESTRUCT) before returning.
    uint32_t cs = heap_alloc(48, true);
    if (cs) {
        wr32(cs + 0, param);
        wr32(cs + 4, hinst);
        wr32(cs + 8, arg(c, 9));  // hMenu
        wr32(cs + 12, arg(c, 8)); // hwndParent
        wr32(cs + 16, (uint32_t)h);
        wr32(cs + 20, (uint32_t)w);
        wr32(cs + 24, (uint32_t)y);
        wr32(cs + 28, (uint32_t)x);
        wr32(cs + 32, style);
        wr32(cs + 36, arg(c, 2)); // lpszName
        wr32(cs + 40, arg(c, 1)); // lpszClass
        wr32(cs + 44, exstyle);
    }
    // Fire WH_CBT HCBT_CREATEWND before WM_NCCREATE so MFC can subclass the window.
    fire_cbt_createwnd(c, hwnd, cs);
    // Windows sends WM_NCCREATE first; FALSE from it cancels creation, and
    // -1 from WM_CREATE does the same.
    // A procedure that does not handle WM_NCCREATE passes it to DefWindowProc,
    // which answers TRUE, so only an explicit FALSE cancels creation.
    uint32_t nc = host_dispatch_to_wndproc(c, hwnd, 0x0081 /* WM_NCCREATE */, 0, cs);
    bool cancelled = (win.wndproc != 0 && nc == 0);
    if (!cancelled) {
        uint32_t cr = host_dispatch_to_wndproc(c, hwnd, 0x0001 /* WM_CREATE */, 0, cs);
        cancelled = (cr == 0xffffffffu);
    }
    if (cs)
        heap_free(cs);
    if (cancelled) {
        LOGW("CreateWindowExA(\"%s\"): the window procedure cancelled creation", cls.c_str());
        windows().erase(hwnd);
        if (g_main_hwnd == hwnd)
            g_main_hwnd = windows().empty() ? 0 : windows().begin()->first;
        set_eax(c, 0);
        return;
    }
    // WS_VISIBLE in the style shows the window as part of creation, without a
    // separate ShowWindow. That is the same hidden-to-visible transition, so
    // it invalidates the window and tells the host, and a host does not have
    // to know that CreateWindowExA can be a show as well.
    if (Window *nw = find_window(hwnd)) {
        send_geometry(c, hwnd, nw, true, true);
        nw->shown = true;
        if ((nw->style & WS_VISIBLE) && !nw->visible) {
            nw->visible = true;
            nw->update_pending = true;
            if (g_window_shown)
                g_window_shown(hwnd);
        }
    }
    window_created(hwnd);
    set_eax(c, hwnd);
}
} // namespace
// The creation messages, and the imports their handlers make, run without a
// thread switch; see sched_atomic_enter. A guest exception that unwinds past
// the call ends the stretch through the unwinder.
void create_window_named(X86 *c, bool wide) {
    sched_atomic_enter(c->r[R_ESP]);
    create_window_body(c, wide);
    sched_atomic_leave();
}

void u_CreateWindowExA(X86 *c) {
    create_window_named(c, false);
}

void u_DestroyWindow(X86 *c) {
    set_eax(c, destroy_window(c, arg(c, 0)));
}

// A hidden window becoming visible: it goes to the top unless the show keeps
// the stacking, its whole client area needs painting, and the host is told it
// has appeared. ShowWindow and SetWindowPos's SWP_SHOWWINDOW both make it.
static void became_visible(Window *w, bool to_top) {
    if (to_top)
        reorder_window(w->hwnd, 0);
    w->update_pending = true;
    w->shown = true;
    if (g_window_shown)
        g_window_shown(w->hwnd);
}

// SW_HIDE is the only command that hides; every other one shows the window in
// some form. Showing a window that was hidden invalidates its whole client
// area, which is what makes the following UpdateWindow paint something, and it
// is the transition a host watches to know a window has appeared.
void u_ShowWindow(X86 *c) {
    Window *w = find_window(arg(c, 0));
    bool was = w && w->visible;
    if (!w) {
        set_eax(c, 0);
        return;
    }
    uint32_t cmd = arg(c, 1);
    w->show_cmd = cmd;
    w->visible = cmd != 0; // SW_HIDE == 0
    if (w->visible)
        w->style |= WS_VISIBLE;
    else
        w->style &= ~WS_VISIBLE;
    if (cmd == 2 || cmd == 6 || cmd == 7 || cmd == 11) {
        w->style |= 0x20000000u;
        w->style &= ~0x01000000u;
    } else if (cmd == 3) {
        w->style |= 0x01000000u;
        w->style &= ~0x20000000u;
    } else if (cmd == 1 || cmd == 9)
        w->style &= ~0x21000000u;
    set_eax(c, was ? 1 : 0);
    LOGV("ShowWindow(%08x, %u) was=%d", arg(c, 0), cmd, was ? 1 : 0);
    if (was != w->visible)
        host_dispatch_to_wndproc(c, w->hwnd, 0x0018 /* WM_SHOWWINDOW */, w->visible ? 1 : 0, 0);
    if (!was && w->visible)
        became_visible(w, cmd != 4 && cmd != 7 && cmd != 8); // SW_*NOACTIVATE keeps stacking
}

// UpdateWindow sends WM_PAINT directly to the window procedure, synchronously,
// and only if the update region is not empty. It does not post: a posted
// WM_PAINT would arrive whenever the guest next pumped, and a guest that never
// pumps would never paint.
void u_UpdateWindow(X86 *c) {
    Window *w = find_window(arg(c, 0));
    if (!w) {
        set_eax(c, 0);
        return;
    }
    if (w->update_pending) {
        // The region is NOT validated here. Windows validates it in BeginPaint
        // (or ValidateRect), so a WNDPROC that ignores WM_PAINT leaves the
        // window dirty and gets asked again. Clearing it up front would lose
        // the paint entirely for such a window.
        host_dispatch_to_wndproc(c, w->hwnd, 0x000f /* WM_PAINT */, 0, 0);
    }
    set_eax(c, 1);
}

// SetWindowPos's body, shared with the runtime's own callers: DXGI sizes a
// fullscreen swap chain's output window the way Windows does, through here.
void set_window_pos(X86 *c, Window *w, uint32_t after, int32_t x, int32_t y, int32_t cx, int32_t cy,
                    uint32_t flags, bool refresh_size = false) {
    if (!(flags & 4)) // SWP_NOZORDER
        reorder_window(w->hwnd, after);
    // A WM_SIZE handler may set the same size while arranging children.
    // Only actual changes notify it again, or paint is starved forever.
    bool moved = !(flags & 0x0002) && (w->x != x || w->y != y);
    bool sized = !(flags & 0x0001) && (refresh_size || w->w != cx || w->h != cy);
    if (moved) {
        w->x = x;
        w->y = y;
    } // SWP_NOMOVE
    if (sized) {
        w->w = cx;
        w->h = cy;
        if (!(flags & 0x0008)) // SWP_NOREDRAW
            w->update_pending = true;
    } // SWP_NOSIZE
    LOGV("SetWindowPos(%08x): %dx%d at %d,%d, flags=%08x changed=%d/%d", w->hwnd, w->w, w->h, w->x,
         w->y, flags, moved, sized);
    // SWP_SHOWWINDOW and SWP_HIDEWINDOW are ShowWindow's transitions, made
    // before WM_WINDOWPOSCHANGED as Windows makes them. The VCL shows every
    // child control this way, so ignoring them left each one hidden.
    if ((flags & 0x0040) && !w->visible) {
        w->visible = true;
        w->style |= WS_VISIBLE;
        became_visible(w, false); // stacking is SWP_NOZORDER's business
    } else if ((flags & 0x0080) && w->visible) {
        w->visible = false;
        w->style &= ~WS_VISIBLE;
    }
    if (moved || sized) {
        // SetWindowPos sends this before returning. VCL updates its cached
        // bounds here before setting another dimension. DefWindowProc is
        // responsible for the derived WM_MOVE/WM_SIZE notifications.
        uint32_t pos = heap_alloc(28, true);
        if (pos) {
            wr32(pos, w->hwnd);
            wr32(pos + 4, after);
            wr32(pos + 8, w->x);
            wr32(pos + 12, w->y);
            wr32(pos + 16, w->w);
            wr32(pos + 20, w->h);
            wr32(pos + 24, flags | (moved ? 0 : 2) | (sized ? 0 : 1));
            host_dispatch_to_wndproc(c, w->hwnd, 0x47 /* WM_WINDOWPOSCHANGED */, 0, pos);
            heap_free(pos);
        }
    }
}
void u_SetWindowPos(X86 *c) {
    if (Window *w = find_window(arg(c, 0)))
        set_window_pos(c, w, arg(c, 1), int32_t(arg(c, 2)), int32_t(arg(c, 3)), int32_t(arg(c, 4)),
                       int32_t(arg(c, 5)), arg(c, 6));
    set_eax(c, 1);
}
// The bounds a window had before a display took it over, by handle.
std::map<uint32_t, std::array<int32_t, 4>> &covered_bounds() {
    static std::map<uint32_t, std::array<int32_t, 4>> m;
    return m;
}

void u_GetWindowRect(X86 *c) {
    Window *w = find_window(arg(c, 0));
    uint32_t r = arg(c, 1);
    if (!w || !r) {
        if (r && gm_valid(r, 16)) {
            wr32(r, 0);
            wr32(r + 4, 0);
            wr32(r + 8, 0);
            wr32(r + 12, 0);
        }
        set_eax(c, 0);
        return;
    }
    int32_t x, y;
    client_origin(w->hwnd, &x, &y);
    wr32(r, x);
    wr32(r + 4, y);
    wr32(r + 8, x + w->w);
    wr32(r + 12, y + w->h);
    set_eax(c, 1);
}

void u_GetClientRect(X86 *c) {
    Window *w = find_window(arg(c, 0));
    uint32_t r = arg(c, 1);
    if (!r) {
        set_eax(c, 0);
        return;
    }
    wr32(r + 0, 0);
    wr32(r + 4, 0);
    wr32(r + 8, (uint32_t)(w ? w->w : 640));
    wr32(r + 12, (uint32_t)(w ? w->h : 480));
    set_eax(c, 1);
}

void u_SystemParametersInfoA(X86 *c) {
    const uint32_t SPI_GETWORKAREA = 48;
    uint32_t action = arg(c, 0), param = arg(c, 2);
    if (action == SPI_GETWORKAREA && param) {
        Window *w = find_window(host_main_window());
        wr32(param + 0, 0);
        wr32(param + 4, 0);
        wr32(param + 8, (uint32_t)(w ? w->w : 640));
        wr32(param + 12, (uint32_t)(w ? w->h : 480));
        set_eax(c, 1);
        return;
    }
    log_once(("spi:" + std::to_string(action)).c_str(), "SystemParametersInfoA(%u) unsupported",
             action);
    set_eax(c, 0);
}

void u_AdjustWindowRectEx(X86 *c) {
    // The host window has no non-client area, so the client rect is the window
    // rect. Leaving the rectangle untouched keeps the guest's requested client
    // size intact.
    log_once("AdjustWindowRectEx", "AdjustWindowRectEx: no non-client area is modelled");
    set_eax(c, 1);
}

// ScreenToClient is ClientToScreen's inverse: the window's own position taken
// off a point the game polled with GetCursorPos.
void u_ScreenToClient(X86 *c) {
    Window *w = find_window(arg(c, 0));
    uint32_t p = arg(c, 1);
    if (p && w) {
        LOGV("ScreenToClient(%08x): (%d,%d) through a window at %d,%d", arg(c, 0), (int32_t)rd32(p),
             (int32_t)rd32(p + 4), w->x, w->y);
        int32_t x, y;
        client_origin(w->hwnd, &x, &y);
        wr32(p, rd32(p) - uint32_t(x));
        wr32(p + 4, rd32(p + 4) - uint32_t(y));
    }
    set_eax(c, w ? 1 : 0);
}

void u_OpenIcon(X86 *c) {
    set_eax(c, 1);
}

void u_FindWindowA(X86 *c) {
    set_eax(c, 0);
}

void u_ClientToScreen(X86 *c) {
    Window *w = find_window(arg(c, 0));
    uint32_t p = arg(c, 1);
    if (p && w) {
        int32_t x, y;
        client_origin(w->hwnd, &x, &y);
        wr32(p, rd32(p) + uint32_t(x));
        wr32(p + 4, rd32(p + 4) + uint32_t(y));
    }
    set_eax(c, 1);
}

void u_SetRect(X86 *c) {
    uint32_t r = arg(c, 0);
    if (r) {
        wr32(r + 0, arg(c, 1));
        wr32(r + 4, arg(c, 2));
        wr32(r + 8, arg(c, 3));
        wr32(r + 12, arg(c, 4));
    }
    set_eax(c, 1);
}

// A null hwnd invalidates every window, which is what Windows does.
void u_InvalidateRect(X86 *c) {
    uint32_t hwnd = arg(c, 0);
    if (!hwnd) {
        for (auto &kv : windows())
            kv.second.update_pending = true;
    } else if (Window *w = find_window(hwnd)) {
        w->update_pending = true;
    }
    set_eax(c, 1);
}

// A layered window's transparency. Windows requires WS_EX_LAYERED, and the
// VCL sets it before calling; a window without it is refused, as there it is.
void u_SetLayeredWindowAttributes(X86 *c) {
    Window *w = find_window(arg(c, 0));
    if (!w || !(w->exstyle & 0x00080000u)) { // WS_EX_LAYERED
        set_last_error(87);                  // ERROR_INVALID_PARAMETER
        set_eax(c, 0);
        return;
    }
    w->layered_key = arg(c, 1) & 0xffffffu;
    w->layered_alpha = arg(c, 2) & 255u;
    w->layered_flags = arg(c, 3) & 3u;
    w->surface.dirty = true;
    LOGV("SetLayeredWindowAttributes(%08x, key=%06x, alpha=%u, flags=%u)", w->hwnd, w->layered_key,
         w->layered_alpha, w->layered_flags);
    set_eax(c, 1);
}

void u_GetLayeredWindowAttributes(X86 *c) {
    Window *w = find_window(arg(c, 0));
    if (!w || !(w->exstyle & 0x00080000u) || !w->layered_flags) {
        set_last_error(87); // ERROR_INVALID_PARAMETER
        set_eax(c, 0);
        return;
    }
    if (uint32_t p = arg(c, 1))
        wr32(p, w->layered_key);
    if (uint32_t p = arg(c, 2))
        wr8(p, uint8_t(w->layered_alpha));
    if (uint32_t p = arg(c, 3))
        wr32(p, w->layered_flags);
    set_eax(c, 1);
}

void u_SetWindowLongA(X86 *c) {
    Window *w = find_window(arg(c, 0));
    int32_t idx = (int32_t)arg(c, 1);
    uint32_t v = arg(c, 2), old = 0;
    if (!w) {
        set_eax(c, 0);
        return;
    }
    switch (idx) {
    case -4:
        old = w->wndproc;
        w->wndproc = v;
        break; // GWL_WNDPROC
    case -6:
        old = w->hinstance;
        w->hinstance = v;
        break;
    case -8:
        old = w->parent;
        w->parent = v;
        break;
    case -12:
        old = w->id;
        w->id = v;
        break;
    case -16:
        old = w->style;
        w->style = v;
        w->visible = (v & WS_VISIBLE) != 0;
        w->enabled = !(v & 0x08000000u);
        break;
    case -20:
        old = w->exstyle;
        w->exstyle = v;
        break;
    case -21:
        old = w->userdata;
        w->userdata = v;
        break;
    default:
        if (idx >= 0 && (size_t)(idx / 4) < w->extra.size()) {
            old = w->extra[idx / 4];
            w->extra[idx / 4] = v;
        }
        break;
    }
    set_eax(c, old);
}

void u_GetWindowLongA(X86 *c) {
    Window *w = find_window(arg(c, 0));
    int32_t idx = (int32_t)arg(c, 1);
    if (!w) {
        set_eax(c, 0);
        return;
    }
    switch (idx) {
    case -4:
        set_eax(c, w->wndproc);
        return;
    case -6:
        set_eax(c, w->hinstance);
        return;
    case -8:
        set_eax(c, w->parent);
        return;
    case -12:
        set_eax(c, w->id);
        return;
    case -16:
        set_eax(c, w->style);
        return;
    case -20:
        set_eax(c, w->exstyle);
        return;
    case -21:
        set_eax(c, w->userdata);
        return;
    default:
        if (idx >= 0 && (size_t)(idx / 4) < w->extra.size()) {
            set_eax(c, w->extra[idx / 4]);
            return;
        }
        set_eax(c, 0);
        return;
    }
}

static uint32_t find_dlg_item(uint32_t parent, uint32_t id) {
    for (const auto &kv : windows())
        if (kv.second.parent == parent && kv.second.id == id)
            return kv.first;
    return 0;
}

void u_SetWindowTextA(X86 *c) {
    uint32_t hwnd = arg(c, 0), text = arg(c, 1);
    Window *w = find_window(hwnd);
    if (!w) {
        set_eax(c, 0);
        return;
    }
    if (w->wndproc) {
        set_eax(c, host_dispatch_to_wndproc(c, hwnd, 0x000c /* WM_SETTEXT */, 0, text));
        return;
    }
    w->title_utf8 = gm_str(text);
    set_eax(c, 1);
}

void u_SetDlgItemTextA(X86 *c) {
    uint32_t child = find_dlg_item(arg(c, 0), arg(c, 1));
    Window *w = find_window(child);
    if (!w) {
        set_eax(c, 0);
        return;
    }
    if (w->wndproc) {
        set_eax(c, host_dispatch_to_wndproc(c, child, 0x000c /* WM_SETTEXT */, 0, arg(c, 2)));
        return;
    }
    w->title_utf8 = gm_str(arg(c, 2));
    set_eax(c, 1);
}

void u_CreateDialogParamA(X86 *c) {
    log_once("CreateDialogParamA",
             "CreateDialogParamA: dialogs are not implemented, returning NULL");
    set_eax(c, 0);
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------
// A message passes the filter when the window matches (0 means any) and the
// message id is in [min, max]; min == max == 0 means no id filter at all.
bool msg_matches(const Msg &m, uint32_t filter_hwnd, uint32_t min_msg, uint32_t max_msg) {
    const uint32_t thread = guest_current_thread_id();
    if (m.hwnd) {
        const Window *window = find_window(m.hwnd);
        if (!window || window->thread != thread)
            return false;
    } else if ((m.thread ? m.thread : 1) != thread)
        return false;
    if (m.message == 0x0012)
        return true; // WM_QUIT is never filtered out
    if ((filter_hwnd == 0xffffffffu && m.hwnd) ||
        (filter_hwnd && filter_hwnd != 0xffffffffu && m.hwnd != filter_hwnd))
        return false;
    if (min_msg == 0 && max_msg == 0)
        return true;
    if (max_msg == 0)
        return m.message >= min_msg; // Win32 treats 0 as no upper bound
    return m.message >= min_msg && m.message <= max_msg;
}

// An update region is a persistent source of low-priority messages, not a
// queued item consumed by PM_REMOVE. Only validation (normally BeginPaint)
// makes it disappear. Hidden child hierarchies never receive synthesized paint.
static bool pending_paint(uint32_t p, uint32_t hwnd, uint32_t min_msg, uint32_t max_msg) {
    for (const auto &kv : windows()) {
        const Window *w = &kv.second;
        if (!w->update_pending || !w->visible || w->w <= 0 || w->h <= 0)
            continue;
        const Window *ancestor = w;
        size_t hops = 0;
        while (ancestor && ancestor->visible && (ancestor->style & 0x40000000u) &&
               ancestor->parent && hops++ < windows().size())
            ancestor = find_window(ancestor->parent);
        if (!ancestor || !ancestor->visible || hops > windows().size())
            continue;
        Msg m{w->hwnd, 0xf, 0, 0, host_millis(), uint32_t(g_cursor_x), uint32_t(g_cursor_y)};
        if (!msg_matches(m, hwnd, min_msg, max_msg))
            continue;
        if (!p)
            return true; // only asked whether there is one
        last_message() = m;
        store_msg(p, m);
        return true;
    }
    return false;
}

Msg &last_message() {
    static std::map<uint32_t, Msg> values;
    return values[guest_current_thread_id()];
}
bool queued_message_pending() {
    for (const auto &message : queue())
        if (msg_matches(message, 0, 0, 0))
            return true;
    return false;
}

bool paint_pending() {
    return pending_paint(0, 0, 0, 0);
}

void peek_message(X86 *c) {
    // The game's message loop is PeekMessageA and nothing else: it never calls
    // GetMessageA, so anything hung off that one never runs. Windows services
    // timers while a message is being retrieved, which is what makes this the
    // right place rather than a convenient one, and it is where the audio
    // shims get their tick - a streamed sound is refilled by calling the guest
    // back, and only a thread holding the scheduler baton may do that.
    //
    // The game spins this poll tens of thousands of times per rendered frame
    // (a level-selector screen measured ~83k calls/second). The audio pump and
    // the window z-order present do not advance faster than wall time - audio runs on a 21 ms period and
    // the presenter on the display refresh - so doing it in full on every poll
    // only burns the thread: at 5 fps it was ~83k audio pumps and surface
    // z-order sorts per second, two thirds of the frame. Coalesce the pumps to
    // a short wall-clock interval. A busy poll loop then stays cheap while
    // timers and audio still tick at 1 kHz by default, far above what they
    // need. RECOMP_PEEK_PUMP_US tunes the interval; 0 restores per-poll pumping.
    // Only one guest thread holds the baton at a time, so plain statics are safe.
    static uint64_t last_pump_ns = 0;
    static int64_t pump_interval_ns = -1;
    if (pump_interval_ns < 0) {
        const char *v = recomp_env("PEEK_PUMP_US");
        pump_interval_ns = (int64_t)(v ? strtoul(v, nullptr, 10) : 1000) * 1000;
    }
    // Window timers and mouse input stay per poll: they are cheap, and a
    // message the guest is polling for must be there on the next poll.
    const uint64_t now_ns = os_monotonic_ns();
    const bool pump = now_ns - last_pump_ns >= (uint64_t)pump_interval_ns;
    if (pump) {
        last_pump_ns = now_ns;
        host_pump_timers(c);
    }
    pump_window_timers();
    pump_mouse_input(c);
    wsock::pump_async_select();
    if (pump)
        gdi_present_windows();
    uint32_t p = arg(c, 0), filter_hwnd = arg(c, 1);
    uint32_t min_msg = arg(c, 2), max_msg = arg(c, 3), flags = arg(c, 4);
    for (auto it = queue().begin(); it != queue().end(); ++it) {
        if (!msg_matches(*it, filter_hwnd, min_msg, max_msg))
            continue;
        last_message() = *it;
        store_msg(p, *it);
        if (flags & 1)
            queue().erase(it); // PM_REMOVE
        set_eax(c, 1);
        return;
    }
    set_eax(c, pending_paint(p, filter_hwnd, min_msg, max_msg));
}

// GetMessage blocks until a message arrives. The runtime cannot block on its
// own, so it drives the multimedia timers and then asks the host to wait for
// input. With no host attached and nothing queued there is no message to
// deliver and no way to wait for one, so it reports the documented error
// return (-1) rather than inventing a message the system never sent.
// GetMessageA blocks until a message arrives. It does not return until it has
// one, which is the whole point of the call: a caller that wanted "whatever is
// there right now" would use PeekMessage. It returns 0 for WM_QUIT, and -1
// only for a genuine error, which here means a filter naming a window that
// does not exist.
void u_GetMessageA(X86 *c) {
    uint32_t p = arg(c, 0), filter_hwnd = arg(c, 1);
    uint32_t min_msg = arg(c, 2), max_msg = arg(c, 3);

    if (filter_hwnd && filter_hwnd != 0xffffffffu && !find_window(filter_hwnd)) {
        set_last_error(1400); // ERROR_INVALID_WINDOW_HANDLE
        set_eax(c, 0xffffffffu);
        return;
    }

    for (;;) {
        const Window *main_window = find_window(g_main_hwnd);
        const bool drives_host =
            guest_current_thread_id() == (main_window ? main_window->thread : 1);
        pump_window_timers();
        if (drives_host) {
            pump_mouse_input(c);
            gdi_present_windows();
        }
        for (auto it = queue().begin(); it != queue().end(); ++it) {
            if (!msg_matches(*it, filter_hwnd, min_msg, max_msg))
                continue;
            Msg m = *it;
            queue().erase(it);
            last_message() = m;
            store_msg(p, m);
            set_eax(c, m.message == 0x0012 ? 0 : 1); // WM_QUIT ends the loop
            return;
        }
        if (pending_paint(p, filter_hwnd, min_msg, max_msg)) {
            set_eax(c, 1);
            return;
        }
        if (!drives_host) {
            guest_sleep_ms(1);
            continue;
        }
        host_pump_timers(c);
        pump_window_timers();
        // Without a host there is nothing that could ever post a message, so
        // blocking would be a hang with no way out. That is the one case where
        // the documented error is the honest answer.
        if (!g_message_waiter) {
            log_once("GetMessageA-nohost",
                     "GetMessageA has nothing to deliver and no host to wait on: "
                     "returning -1 rather than blocking forever");
            set_eax(c, 0xffffffffu);
            return;
        }
        // Service the host's event loop. Its answer says whether anything is
        // queued, which is NOT the same as whether anything matches this
        // filter: a queued message the filter rejects would otherwise keep the
        // waiter answering true forever and this loop would spin, starving the
        // very thread that would post the message being waited for.
        g_message_waiter();
        // Nothing matched on this pass, whatever the waiter said. Let the
        // other guest threads run - one of them may be the one that posts it -
        // and if none can, sleep so real time passes for the timers and the
        // host instead of burning the slice.
        if (!host_guest_yield())
            guest_sleep_ms(1);
    }
}

void u_TranslateMessage(X86 *c) {
    uint32_t p = arg(c, 0);
    if (!p) {
        set_eax(c, 0);
        return;
    }
    uint32_t msg = rd32(p + 4), vk = rd32(p + 8), lparam = rd32(p + 12);
    if (msg == 0x0100 || msg == 0x0104) { // WM_KEYDOWN / WM_SYSKEYDOWN
        // The host can supply layout-aware text with a physical key. Preserve
        // that character (including case) instead of appending a second one.
        // Match the complete retrieved MSG so guest-posted/modified keys still
        // take the ordinary translation path, and copied MSG values work too.
        const Msg &last = last_message();
        if (last.character_posted && last.hwnd == rd32(p) && last.message == msg &&
            last.wparam == vk && last.lparam == lparam && last.time == rd32(p + 16) &&
            last.ptx == rd32(p + 20) && last.pty == rd32(p + 24)) {
            set_eax(c, 1);
            return;
        }
        uint32_t ch = 0;
        if (vk >= 0x30 && vk <= 0x5a)
            ch = vk; // digits and letters
        else if (vk == 0x20)
            ch = ' ';
        else if (vk == 0x0d)
            ch = '\r';
        else if (vk == 0x08)
            ch = '\b';
        else if (vk == 0x1b)
            ch = 0x1b;
        if (ch) {
            bool shift = (g_key_state[0x10] & 0x80) != 0;
            if (!shift && ch >= 'A' && ch <= 'Z')
                ch += 32;
            host_post_message(rd32(p), msg == 0x0100 ? 0x0102 : 0x0106, ch, lparam);
            set_eax(c, 1);
            return;
        }
    }
    set_eax(c, 0);
}

void dispatch_message(X86 *c) {
    uint32_t p = arg(c, 0);
    if (!p) {
        set_eax(c, 0);
        return;
    }
    uint32_t hwnd = rd32(p + 0), msg = rd32(p + 4);
    uint32_t wp = rd32(p + 8), lp = rd32(p + 12);
    if (msg == 0x113 && lp)
        set_eax(c, guest_call(c, lp, hwnd, msg, wp, rd32(p + 16)));
    else
        set_eax(c, host_dispatch_to_wndproc(c, hwnd, msg, wp, lp));
}

void u_PostMessageA(X86 *c) {
    // System messages with text pointers cannot be posted asynchronously. A
    // caller must SendMessage so the buffer remains alive through conversion.
    if (arg(c, 1) == 0x000c || arg(c, 1) == 0x000d) {
        set_last_error(1159); // ERROR_MESSAGE_SYNC_ONLY
        set_eax(c, 0);
        return;
    }
    if (arg(c, 0) && !find_window(arg(c, 0))) {
        set_last_error(1400);
        set_eax(c, 0);
        return;
    }
    host_post_message(arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3));
    set_eax(c, 1);
}

void u_PostThreadMessage(X86 *c) {
    const uint32_t thread = arg(c, 0);
    if (!guest_thread_exists(thread)) {
        set_last_error(1444); // ERROR_INVALID_THREAD_ID
        set_eax(c, 0);
        return;
    }
    Msg m{0,
          arg(c, 1),
          arg(c, 2),
          arg(c, 3),
          host_millis(),
          uint32_t(g_cursor_x),
          uint32_t(g_cursor_y)};
    m.thread = thread;
    queue().push_back(m);
    set_eax(c, 1);
}

void u_PeekMessageA(X86 *c) {
    peek_message(c);
}
void u_DispatchMessageA(X86 *c) {
    dispatch_message(c);
}
void u_SendMessageA(X86 *c) {
    send_message(c, false);
}

void def_window_proc(X86 *c, bool wide) {
    uint32_t hwnd = arg(c, 0), msg = arg(c, 1);
    Window *w = find_window(hwnd);
    switch (msg) {
    case 0x000c: // WM_SETTEXT
        if (w)
            w->title_utf8 = wide ? gm_wstr(arg(c, 3)) : gm_str(arg(c, 3));
        set_eax(c, w ? 1 : 0);
        return;
    case 0x000d: { // WM_GETTEXT
        std::string txt = w ? w->title_utf8 : "";
        if (w && txt.empty() && w->cur_sel >= 0 && (size_t)w->cur_sel < w->items.size())
            txt = w->items[(size_t)w->cur_sel].text;
        set_eax(c, put_text(arg(c, 3), arg(c, 2), txt, wide));
        return;
    }
    case 0x000e: { // WM_GETTEXTLENGTH
        std::string txt = w ? w->title_utf8 : "";
        if (w && txt.empty() && w->cur_sel >= 0 && (size_t)w->cur_sel < w->items.size())
            txt = w->items[(size_t)w->cur_sel].text;
        set_eax(c, w ? (wide ? wide_units(txt) : uint32_t(txt.size())) : 0);
        return;
    }
    case 0x00f0: // BM_GETCHECK
        set_eax(c, w ? w->btn_check : 0);
        return;
    case 0x00f1: // BM_SETCHECK
        if (w)
            w->btn_check = arg(c, 2);
        set_eax(c, 0);
        return;
    case 0x00f2: // BM_GETSTATE
        set_eax(c, w ? (w->btn_state | (w->btn_check & 3u)) : 0);
        return;
    case 0x00f3: // BM_SETSTATE
        if (w) {
            if (arg(c, 2))
                w->btn_state |= 4u;
            else
                w->btn_state &= ~4u;
        }
        set_eax(c, 0);
        return;
    case 0x00f4: // BM_SETSTYLE
        if (w)
            w->style = (w->style & ~0xffffu) | (arg(c, 2) & 0xffffu);
        set_eax(c, 0);
        return;
    case 0x0143: // CB_ADDSTRING
    case 0x0180: // LB_ADDSTRING
        if (w) {
            std::string s = wide ? gm_wstr(arg(c, 3)) : gm_str(arg(c, 3));
            uint32_t idx = (uint32_t)w->items.size();
            w->items.push_back({s, 0});
            set_eax(c, idx);
        } else {
            set_eax(c, 0xffffffffu);
        }
        return;
    case 0x014a: // CB_INSERTSTRING
    case 0x0181: // LB_INSERTSTRING
        if (w) {
            int32_t idx = (int32_t)arg(c, 2);
            std::string s = wide ? gm_wstr(arg(c, 3)) : gm_str(arg(c, 3));
            if (idx < 0 || (size_t)idx >= w->items.size()) {
                idx = (int32_t)w->items.size();
                w->items.push_back({s, 0});
            } else {
                w->items.insert(w->items.begin() + idx, {s, 0});
                if (w->cur_sel >= idx) ++w->cur_sel;
            }
            set_eax(c, (uint32_t)idx);
        } else {
            set_eax(c, 0xffffffffu);
        }
        return;
    case 0x0144: // CB_DELETESTRING
    case 0x0182: // LB_DELETESTRING
        if (w && arg(c, 2) < w->items.size()) {
            w->items.erase(w->items.begin() + arg(c, 2));
            if (w->cur_sel == (int32_t)arg(c, 2)) w->cur_sel = -1;
            else if (w->cur_sel > (int32_t)arg(c, 2)) --w->cur_sel;
            set_eax(c, (uint32_t)w->items.size());
        } else {
            set_eax(c, 0xffffffffu);
        }
        return;
    case 0x014b: // CB_RESETCONTENT
    case 0x0184: // LB_RESETCONTENT
        if (w) {
            w->items.clear();
            w->cur_sel = -1;
        }
        set_eax(c, 0);
        return;
    case 0x0146: // CB_GETCOUNT
    case 0x018b: // LB_GETCOUNT
        set_eax(c, w ? (uint32_t)w->items.size() : 0xffffffffu);
        return;
    case 0x0147: // CB_GETCURSEL
    case 0x0188: // LB_GETCURSEL
        set_eax(c, w ? (uint32_t)w->cur_sel : 0xffffffffu);
        return;
    case 0x014e: // CB_SETCURSEL
    case 0x0186: // LB_SETCURSEL
        if (w) {
            int32_t sel = (int32_t)arg(c, 2);
            if (sel < 0 || (size_t)sel >= w->items.size()) {
                w->cur_sel = -1;
                set_eax(c, sel < 0 ? 0 : 0xffffffffu);
            } else {
                w->cur_sel = sel;
                w->title_utf8 = w->items[(size_t)sel].text;
                set_eax(c, (uint32_t)sel);
            }
        } else {
            set_eax(c, 0xffffffffu);
        }
        return;
    case 0x0148: // CB_GETLBTEXT
    case 0x0189: // LB_GETTEXT
        if (w && arg(c, 2) < w->items.size()) {
            set_eax(c, put_text(arg(c, 3), 4096, w->items[arg(c, 2)].text, wide));
        } else {
            set_eax(c, 0xffffffffu);
        }
        return;
    case 0x0149: // CB_GETLBTEXTLEN
    case 0x018a: // LB_GETTEXTLEN
        if (w && arg(c, 2) < w->items.size()) {
            const auto &s = w->items[arg(c, 2)].text;
            set_eax(c, wide ? wide_units(s) : (uint32_t)s.size());
        } else {
            set_eax(c, 0xffffffffu);
        }
        return;
    case 0x0150: // CB_GETITEMDATA
    case 0x0199: // LB_GETITEMDATA
        set_eax(c, (w && arg(c, 2) < w->items.size()) ? w->items[arg(c, 2)].data : 0xffffffffu);
        return;
    case 0x0151: // CB_SETITEMDATA
    case 0x019a: // LB_SETITEMDATA
        if (w && arg(c, 2) < w->items.size()) {
            w->items[arg(c, 2)].data = arg(c, 3);
            set_eax(c, 0);
        } else {
            set_eax(c, 0xffffffffu);
        }
        return;
    case 0x000f: // DefWindowProc validates an otherwise unhandled paint.
        if (w)
            w->update_pending = false;
        set_eax(c, 0);
        return;
    case 0x0021: // WM_MOUSEACTIVATE: children ask their parent first.
        set_eax(c, w && w->parent
                       ? host_dispatch_to_wndproc(c, w->parent, msg, arg(c, 2), arg(c, 3))
                       : 1);
        return;
    case 0x0047: { // WM_WINDOWPOSCHANGED
        uint32_t pos = arg(c, 3);
        if (w && pos) {
            uint32_t flags = rd32(pos + 24);
            uint32_t move = (uint32_t(uint16_t(w->y)) << 16) | uint16_t(w->x);
            uint32_t size = (uint32_t(uint16_t(w->h)) << 16) | uint16_t(w->w);
            if (!(flags & 2))
                host_dispatch_to_wndproc(c, hwnd, 3 /* WM_MOVE */, 0, move);
            if (!(flags & 1) && find_window(hwnd))
                host_dispatch_to_wndproc(c, hwnd, 5 /* WM_SIZE */, 0, size);
        }
        set_eax(c, 0);
        return;
    }
    case 0x0081: // WM_NCCREATE: TRUE, or creation is cancelled
    case 0x0014: // WM_ERASEBKGND: the background counts as erased
        set_eax(c, 1);
        return;
    case 0x0010: // WM_CLOSE -> DestroyWindow
        destroy_window(c, hwnd);
        host_post_message(0, 0x0012 /* WM_QUIT */, 0, 0);
        break;
    default:
        break;
    }
    set_eax(c, 0);
}

void u_DefWindowProcA(X86 *c) {
    def_window_proc(c, false);
}

void u_PostQuitMessage(X86 *c) {
    host_post_message(0, 0x0012, arg(c, 0), 0);
    set_eax(c, 0);
}

// ---------------------------------------------------------------------------
// Paint / DC / cursor / input
// ---------------------------------------------------------------------------
void u_GetDC(X86 *c) {
    set_eax(c, gdi_window_dc(arg(c, 0)));
}
void u_ReleaseDC(X86 *c) {
    set_eax(c, gdi_release_window_dc(arg(c, 0), arg(c, 1)));
}

void u_BeginPaint(X86 *c) {
    uint32_t hwnd = arg(c, 0), ps = arg(c, 1);
    uint32_t hdc = gdi_window_dc(hwnd);
    Window *w = find_window(hwnd);
    if (w)
        w->update_pending = false; // BeginPaint validates the region
    if (recomp_env("TRACE_GDI"))
        LOGW("gdi: BeginPaint hwnd=%08x -> hdc=%08x%s", hwnd, hdc, w ? "" : " (no such window)");
    if (ps) {
        memset(g_mem + ps, 0, 64);
        wr32(ps + 0, hdc);                         // hdc
        wr32(ps + 4, 0);                           // fErase
        wr32(ps + 8, 0);                           // rcPaint.left
        wr32(ps + 12, 0);                          // rcPaint.top
        wr32(ps + 16, (uint32_t)(w ? w->w : 640)); // rcPaint.right
        wr32(ps + 20, (uint32_t)(w ? w->h : 480)); // rcPaint.bottom
    }
    set_eax(c, hdc);
}
void u_EndPaint(X86 *c) {
    uint32_t ps = arg(c, 1);
    set_eax(c, ps && gm_valid(ps, 64) && gdi_release_window_dc(arg(c, 0), rd32(ps)));
}

// Use the desktop fallback until DirectDraw selects a mode, then report that
// mode so a window procedure can size its fullscreen blit rectangle correctly.
void u_GetSystemMetrics(X86 *c) {
    uint32_t width = 0, height = 0, bpp = 0;
    win32_display_mode(&width, &height, &bpp);
    uint32_t v = 0;
    switch (arg(c, 0)) {
    case 0:  // SM_CXSCREEN
    case 16: // SM_CXFULLSCREEN
        v = width;
        LOGV("GetSystemMetrics(%u) -> %u (display mode %ux%ux%u)", arg(c, 0), v, width, height,
             bpp);
        break;
    case 1: // SM_CYSCREEN
        v = height;
        LOGV("GetSystemMetrics(%u) -> %u (display mode %ux%ux%u)", arg(c, 0), v, width, height,
             bpp);
        break;
    case 17: // SM_CYFULLSCREEN
        v = height - 19;
        break;
    case 2:
    case 3:
    case 9:
    case 10:
    case 20:
    case 21:
        v = 17;
        break;
    case 15:
        v = 19;
        break;
    case 19:
    case 22:
    case 23:
    case 41:
    case 42:
    case 44:
    case 63:
    case 74:
        v = 1;
        break;
    case 28:
        v = 112;
        break;
    case 29:
        v = 27;
        break;
    case 30:
    case 31:
    case 52:
    case 53:
        v = 18;
        break;
    case 34:
        v = 112;
        break;
    case 35:
        v = 27;
        break;
    case 36:
    case 37:
    case 49:
    case 50:
        v = 4;
        break;
    case 38:
    case 39:
        v = 75;
        break;
    case 45:
    case 46:
        v = 2;
        break;
    case 47:
        v = 160;
        break;
    case 48:
        v = 24;
        break;
    case 51:
    case 55:
        v = 19;
        break;
    case 54:
    case 59:
    case 61:
    case 78:
        v = width;
        break;
    case 56:
    case 60:
    case 62:
    case 79:
        v = height;
        break;
    case 57:
    case 58:
    case 68:
    case 69:
        v = 16;
        break;
    case 4: // SM_CYCAPTION
        v = 19;
        break;
    case 5: // SM_CXBORDER
    case 6: // SM_CYBORDER
        v = 1;
        break;
    case 7: // SM_CXDLGFRAME
    case 8: // SM_CYDLGFRAME
        v = 3;
        break;
    case 11: // SM_CXICON
    case 12: // SM_CYICON
    case 13: // SM_CXCURSOR
    case 14: // SM_CYCURSOR
        v = 32;
        break;
    case 32: // SM_CXFRAME
    case 33: // SM_CYFRAME
        v = 4;
        break;
    case 43: // SM_CMOUSEBUTTONS
        v = 2;
        break;
    case 80: // SM_CMONITORS
        v = 1;
        break;
    default:
        break;
    }
    set_eax(c, v);
}
// One handle per stock cursor id; the host draws the pointer, so the handle
// only has to be distinct and non-zero.
void u_LoadCursorA(X86 *c) {
    set_eax(c, 0x0002a000u + (arg(c, 1) & 0xfffu));
}
// The creation API determines the encoding delivered to the window procedure.
void u_IsWindowUnicode(X86 *c) {
    Window *w = find_window(arg(c, 0));
    set_eax(c, w && w->unicode);
}
// An icon or cursor assembled from bitmaps: a distinct handle, never drawn by
// the host, which paints its own pointer.
static uint32_t g_next_icon = 0x0002b000u;
// A cursor loaded from a file is the same kind of handle: distinct, and never
// drawn by the host. The file is not read - the host paints its own pointer -
// but a readable path gets a handle so the guest's cursor bookkeeping works.
// The update region is a flag here, so validating any rectangle validates
// the window, and the update rectangle is the whole client while it is set.
void u_ValidateRect(X86 *c) {
    Window *w = find_window(arg(c, 0));
    if (w)
        w->update_pending = false;
    set_eax(c, w != nullptr);
}
void u_GetUpdateRect(X86 *c) {
    Window *w = find_window(arg(c, 0));
    uint32_t rect = arg(c, 1);
    bool pending = w && w->update_pending;
    if (rect && gm_valid(rect, 16)) {
        wr32(rect, 0);
        wr32(rect + 4, 0);
        wr32(rect + 8, pending ? (uint32_t)w->w : 0);
        wr32(rect + 12, pending ? (uint32_t)w->h : 0);
    }
    if (pending && arg(c, 2))
        w->update_pending = false; // bErase: the caller will paint it now
    set_eax(c, pending);
}
void u_LoadCursorFromFileW(X86 *c) {
    set_eax(c, arg(c, 0) && gm_valid(arg(c, 0), 2) ? g_next_icon++ : 0);
}
void u_CreateIconIndirect(X86 *c) {
    uint32_t info = arg(c, 0);
    GdiImage image, mask;
    if (info && gm_valid(info, 20) && gdi_read_bitmap(rd32(info + 16), &image)) {
        if (rd32(info + 12) && gdi_read_bitmap(rd32(info + 12), &mask) &&
            mask.width >= image.width && mask.height >= image.height) {
            for (int32_t y = 0; y < image.height; ++y)
                for (int32_t x = 0; x < image.width; ++x)
                    if (mask.pixels[size_t(y) * mask.width + x] & 0xffffff)
                        image.pixels[size_t(y) * image.width + x] = 0;
        }
        set_eax(c, gdi_create_icon(image));
        return;
    }
    set_eax(c, arg(c, 0) ? g_next_icon++ : 0);
}
void u_DestroyIcon(X86 *c) {
    gdi_delete_icon(arg(c, 0));
    set_eax(c, arg(c, 0) ? 1 : 0);
}
void u_LoadIconA(X86 *c) {
    set_eax(c, 0x00029001);
}
void u_SetCursor(X86 *c) {
    uint32_t prev = g_cursor;
    g_cursor = arg(c, 0);
    set_eax(c, prev);
}
void u_SetCursorPos(X86 *c) {
    g_cursor_x = (int32_t)arg(c, 0);
    g_cursor_y = (int32_t)arg(c, 1);
    g_cursor_warp_active = g_clipped;
    set_eax(c, 1);
}
void u_GetCursorPos(X86 *c) {
    uint32_t p = arg(c, 0);
    if (p) {
        wr32(p, (uint32_t)g_cursor_x);
        wr32(p + 4, (uint32_t)g_cursor_y);
    }
    set_eax(c, 1);
}
void u_GetMessagePos(X86 *c) {
    set_eax(c, (last_message().pty << 16) | (last_message().ptx & 0xffff));
}
void u_GetMessageTime(X86 *c) {
    set_eax(c, last_message().time);
}
void u_GetAsyncKeyState(X86 *c) {
    uint32_t vk = arg(c, 0);
    uint32_t state = 0;
    if (vk < 256) {
        state = (g_key_state[vk] ? 0x8000u : 0) | g_async_pressed[vk];
        g_async_pressed[vk] = 0;
    }
    set_eax(c, state);
}
void u_GetKeyState(X86 *c) {
    uint32_t vk = arg(c, 0);
    set_eax(c, vk < 256 && g_key_state[vk] ? 0xff80 : 0);
}
void u_ShowCursor(X86 *c) {
    g_cursor_show += arg(c, 0) ? 1 : -1;
    if (g_cursor_show >= 0)
        g_cursor_warp_active = false;
    set_eax(c, (uint32_t)g_cursor_show);
}

// The cursor is confined by bookkeeping only; the host layer reads the clip
// rectangle back when it decides where a real cursor may go.
void u_ClipCursor(X86 *c) {
    uint32_t r = arg(c, 0);
    if (!r) {
        g_clipped = false;
        g_cursor_warp_active = false;
        set_eax(c, 1);
        return;
    }
    for (int i = 0; i < 4; ++i)
        g_clip[i] = (int32_t)rd32(r + 4 * (uint32_t)i);
    g_clipped = true;
    set_eax(c, 1);
}

void u_GetClipCursor(X86 *c) {
    uint32_t r = arg(c, 0);
    if (!r) {
        set_eax(c, 0);
        return;
    }
    Window *w = find_window(g_main_hwnd);
    int32_t def[4] = {0, 0, w ? w->w : 640, w ? w->h : 480};
    for (int i = 0; i < 4; ++i)
        wr32(r + 4 * (uint32_t)i, (uint32_t)(g_clipped ? g_clip[i] : def[i]));
    set_eax(c, 1);
}

void u_GetDoubleClickTime(X86 *c) {
    set_eax(c, 500);
}
void u_GetKeyboardType(X86 *c) {
    switch (arg(c, 0)) {
    case 0:
        set_eax(c, 4);
        break; // enhanced 101/102 key
    case 1:
        set_eax(c, 0);
        break;
    default:
        set_eax(c, 12);
        break; // function keys
    }
}
void u_GetKeyboardLayout(X86 *c) {
    set_eax(c, 0x04090409);
} // en-US

// ---------------------------------------------------------------------------
// Clipboard: nothing is shared with the host clipboard.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Message boxes and formatting
// ---------------------------------------------------------------------------
// The answer a message box gets when there is nobody to click it.
//
// There is no display, so the box cannot be shown and cannot be answered by a
// person. What it must not do is answer with a button the caller never
// offered: returning IDOK to an MB_YESNO box is not one of that box's two
// possible answers, and a caller that switches on the result then takes a path
// Windows could never have produced. So the reply is the default button of the
// button set the caller asked for, which is what pressing Return on the box
// gives, and MB_DEFBUTTON1/2/3 select which of them that is.
uint32_t message_box_default(uint32_t type) {
    // MB_OK, MB_OKCANCEL, MB_ABORTRETRYIGNORE, MB_YESNOCANCEL, MB_YESNO,
    // MB_RETRYCANCEL, MB_CANCELTRYCONTINUE, indexed by type & MB_TYPEMASK.
    static const uint8_t sets[7][3] = {
        {1, 0, 0},   // IDOK
        {1, 2, 0},   // IDOK, IDCANCEL
        {3, 4, 5},   // IDABORT, IDRETRY, IDIGNORE
        {6, 7, 2},   // IDYES, IDNO, IDCANCEL
        {6, 7, 0},   // IDYES, IDNO
        {4, 2, 0},   // IDRETRY, IDCANCEL
        {2, 10, 11}, // IDCANCEL, IDTRYAGAIN, IDCONTINUE
    };
    uint32_t set = type & 0x0000000fu;
    if (set > 6)
        set = 0;
    uint32_t want = (type & 0x00000f00u) >> 8; // MB_DEFBUTTON1/2/3
    if (want > 2 || sets[set][want] == 0)
        want = 0;
    return sets[set][want];
}

void u_MessageBoxA(X86 *c) {
    uint32_t answer = message_box_default(arg(c, 3));
    LOGW("MessageBoxA: [%s] %s -> default button %u", gm_str(arg(c, 2), 256).c_str(),
         gm_str(arg(c, 1), 1024).c_str(), answer);
    set_eax(c, answer);
}

void u_MessageBoxW(X86 *c) {
    std::string text;
    uint32_t p = arg(c, 1);
    for (int i = 0; i < 1024; ++i) {
        uint16_t w = rd16(p + 2 * i);
        if (!w)
            break;
        text.push_back((char)(w < 256 ? w : '?'));
    }
    uint32_t answer = message_box_default(arg(c, 3));
    LOGW("MessageBoxW: %s -> default button %u", text.c_str(), answer);
    set_eax(c, answer);
}

// wvsprintfA: the Win32 subset (%s %c %d %i %u %x %X %% with width/precision
// and the l/h size prefixes). `va` points at the guest argument array.
static void u_wvsprintfA_va(X86 *c, uint32_t out, uint32_t fmt_ptr, uint32_t va) {
    std::string fmt = gm_str(fmt_ptr, 4096);
    std::string res;
    size_t i = 0;
    while (i < fmt.size()) {
        char ch = fmt[i++];
        if (ch != '%') {
            res.push_back(ch);
            continue;
        }
        if (i < fmt.size() && fmt[i] == '%') {
            res.push_back('%');
            ++i;
            continue;
        }
        std::string spec = "%";
        while (i < fmt.size() && strchr("-+ #0", fmt[i]))
            spec.push_back(fmt[i++]);
        while (i < fmt.size() && isdigit((unsigned char)fmt[i]))
            spec.push_back(fmt[i++]);
        if (i < fmt.size() && fmt[i] == '.') {
            spec.push_back(fmt[i++]);
            while (i < fmt.size() && isdigit((unsigned char)fmt[i]))
                spec.push_back(fmt[i++]);
        }
        while (i < fmt.size() && (fmt[i] == 'l' || fmt[i] == 'h'))
            ++i;
        if (i >= fmt.size())
            break;
        char conv = fmt[i++];
        uint32_t v = rd32(va);
        va += 4;
        char buf[512];
        switch (conv) {
        case 's': {
            std::string s = gm_str(v, 1024);
            spec.push_back('s');
            snprintf(buf, sizeof buf, spec.c_str(), s.c_str());
            break;
        }
        case 'c':
            spec.push_back('c');
            snprintf(buf, sizeof buf, spec.c_str(), (int)(v & 0xff));
            break;
        case 'd':
        case 'i':
            spec.push_back('d');
            snprintf(buf, sizeof buf, spec.c_str(), (int)v);
            break;
        case 'u':
            spec.push_back('u');
            snprintf(buf, sizeof buf, spec.c_str(), (unsigned)v);
            break;
        case 'x':
            spec.push_back('x');
            snprintf(buf, sizeof buf, spec.c_str(), (unsigned)v);
            break;
        case 'X':
            spec.push_back('X');
            snprintf(buf, sizeof buf, spec.c_str(), (unsigned)v);
            break;
        default:
            snprintf(buf, sizeof buf, "%%%c", conv);
            va -= 4;
            break;
        }
        res += buf;
    }
    if (out)
        memcpy(g_mem + out, res.c_str(), res.size() + 1);
    set_eax(c, (uint32_t)res.size());
}

} // namespace user32

void win32_refresh_display_window(X86 *c, uint32_t hwnd, uint32_t w, uint32_t h, uint32_t bpp) {
    auto *win = user32::find_window(hwnd);
    if (!win || hwnd == user32::desktop_handle)
        return;
    // Exclusive DirectDraw transitions notify the window even when a movie
    // restores the same resolution. Its WM_SIZE handler may have discarded
    // the old presentation rectangle. Ordinary SetWindowPos still suppresses
    // unchanged sizes, so layout calls made by that handler cannot recurse.
    user32::set_window_pos(c, win, 0, 0, 0, int32_t(w), int32_t(h),
                           0x54 /* NOZORDER|NOACTIVATE|SHOWWINDOW */, true);
    host_dispatch_to_wndproc(c, hwnd, 0x7e /* WM_DISPLAYCHANGE */, bpp, (h << 16) | (w & 0xffff));
}

// A display taken over by a fullscreen swap chain has one window on it.
// Windows sizes that window to the mode, and gives its bounds back when the
// chain leaves fullscreen; a window procedure learns both through
// WM_WINDOWPOSCHANGED, as from any SetWindowPos.
void win32_cover_display(X86 *c, uint32_t hwnd, uint32_t w, uint32_t h) {
    auto *win = user32::find_window(hwnd);
    if (!win || hwnd == user32::desktop_handle)
        return;
    auto &saved = user32::covered_bounds();
    if (!saved.count(hwnd))
        saved[hwnd] = {win->x, win->y, win->w, win->h};
    user32::set_window_pos(c, win, 0, 0, 0, int32_t(w), int32_t(h), 0x14 /* NOZORDER|NOACTIVATE */);
}
bool win32_top_level(uint32_t hwnd) {
    auto *win = user32::find_window(hwnd);
    return win && hwnd != user32::desktop_handle && !win->parent;
}
void win32_uncover_display(X86 *c, uint32_t hwnd) {
    auto &saved = user32::covered_bounds();
    auto it = saved.find(hwnd);
    if (it == saved.end())
        return;
    auto b = it->second;
    saved.erase(it);
    if (auto *win = user32::find_window(hwnd))
        user32::set_window_pos(c, win, 0, b[0], b[1], b[2], b[3], 0x14);
}

void u_AdjustWindowRect(X86 *c) {
    // Same as the Ex form: no non-client area, so the client rectangle the
    // guest passed in is already the window rectangle.
    log_once("AdjustWindowRect", "AdjustWindowRect: no non-client area is modelled");
    set_eax(c, 1);
}

// One window is ever active and focused: the game's main window.
void u_GetActiveWindow(X86 *c) {
    set_eax(c, g_main_hwnd);
}
void u_GetForegroundWindow(X86 *c) {
    set_eax(c, g_main_hwnd);
}
void u_SetActiveWindow(X86 *c) {
    set_eax(c, host_main_window());
}
void u_SetForegroundWindow(X86 *c) {
    set_eax(c, 1);
}
void u_SetFocus(X86 *c) {
    set_eax(c, find_window(arg(c, 0)) ? g_main_hwnd : 0);
}
void u_GetMenu(X86 *c) {
    set_eax(c, 0);
}
void u_IsIconic(X86 *c) {
    Window *w = find_window(arg(c, 0));
    set_eax(c, w && (w->style & 0x20000000u) ? 1 : 0);
}
// CloseWindow minimizes the guest window; DestroyWindow owns its lifetime.
void u_CloseWindow(X86 *c) {
    Window *w = find_window(arg(c, 0));
    if (!w) {
        set_last_error(1400);
        set_eax(c, 0);
        return;
    }
    w->show_cmd = 2;
    w->style = (w->style | 0x20000000u) & ~0x01000000u;
    set_eax(c, 1);
}
void u_WaitMessage(X86 *c) {
    // A blocking wait in the original; here a scheduling checkpoint so the
    // service threads run, then return as if a message arrived.
    sched_checkpoint();
    set_eax(c, 1);
}

// Clipboard: nothing is shared with the host clipboard.
void u_OpenClipboard(X86 *c) {
    set_eax(c, 1);
}
void u_CloseClipboard(X86 *c) {
    set_eax(c, 1);
}
void u_IsClipboardFormatAvailable(X86 *c) {
    set_eax(c, 0);
}
void u_GetClipboardData(X86 *c) {
    set_eax(c, 0);
}

// Mouse capture: only the handle is remembered. The host already delivers
// pointer input to the one window whether the button is over it or not.
uint32_t g_capture_hwnd = 0;
void u_SetCapture(X86 *c) {
    uint32_t prev = g_capture_hwnd;
    g_capture_hwnd = arg(c, 0);
    set_eax(c, prev);
}
void u_ReleaseCapture(X86 *c) {
    g_capture_hwnd = 0;
    set_eax(c, 1);
}

// Virtual-key codes against set-1 scan codes on a US layout.
struct VkScan {
    uint8_t vk, scan;
};
const VkScan kVkScan[] = {
    {0x1B, 0x01}, {'1', 0x02},  {'2', 0x03},  {'3', 0x04},  {'4', 0x05},  {'5', 0x06},
    {'6', 0x07},  {'7', 0x08},  {'8', 0x09},  {'9', 0x0A},  {'0', 0x0B},  {0xBD, 0x0C},
    {0xBB, 0x0D}, {0x08, 0x0E}, {0x09, 0x0F}, {'Q', 0x10},  {'W', 0x11},  {'E', 0x12},
    {'R', 0x13},  {'T', 0x14},  {'Y', 0x15},  {'U', 0x16},  {'I', 0x17},  {'O', 0x18},
    {'P', 0x19},  {0xDB, 0x1A}, {0xDD, 0x1B}, {0x0D, 0x1C}, {0x11, 0x1D}, {'A', 0x1E},
    {'S', 0x1F},  {'D', 0x20},  {'F', 0x21},  {'G', 0x22},  {'H', 0x23},  {'J', 0x24},
    {'K', 0x25},  {'L', 0x26},  {0xBA, 0x27}, {0xDE, 0x28}, {0xC0, 0x29}, {0x10, 0x2A},
    {0xDC, 0x2B}, {'Z', 0x2C},  {'X', 0x2D},  {'C', 0x2E},  {'V', 0x2F},  {'B', 0x30},
    {'N', 0x31},  {'M', 0x32},  {0xBC, 0x33}, {0xBE, 0x34}, {0xBF, 0x35}, {0xA1, 0x36},
    {0x6A, 0x37}, {0x12, 0x38}, {0x20, 0x39}, {0x14, 0x3A}, {0x70, 0x3B}, {0x71, 0x3C},
    {0x72, 0x3D}, {0x73, 0x3E}, {0x74, 0x3F}, {0x75, 0x40}, {0x76, 0x41}, {0x77, 0x42},
    {0x78, 0x43}, {0x79, 0x44}, {0x90, 0x45}, {0x91, 0x46}, {0x24, 0x47}, {0x26, 0x48},
    {0x21, 0x49}, {0x6D, 0x4A}, {0x25, 0x4B}, {0x0C, 0x4C}, {0x27, 0x4D}, {0x6B, 0x4E},
    {0x23, 0x4F}, {0x28, 0x50}, {0x22, 0x51}, {0x2D, 0x52}, {0x2E, 0x53}, {0x7A, 0x57},
    {0x7B, 0x58}, {0xA0, 0x2A}, {0xA2, 0x1D}, {0xA3, 0x1D}, {0xA4, 0x38}, {0xA5, 0x38},
};
// (uCode, uMapType[, dwhkl]): 0 VK to scan, 1 scan to VK, 2 VK to character,
// 3 scan to VK distinguishing left and right.
uint32_t map_virtual_key(uint32_t code, uint32_t type) {
    switch (type) {
    case 0:
        for (const VkScan &e : kVkScan)
            if (e.vk == code)
                return e.scan;
        return 0;
    case 1:
    case 3:
        for (const VkScan &e : kVkScan)
            if (e.scan == code) {
                if (type == 1 && e.vk == 0xA1)
                    return 0x10;
                return type == 3 && e.vk == 0x10 ? 0xA0 : e.vk;
            }
        return 0;
    case 2:
        if ((code >= '0' && code <= '9') || (code >= 'A' && code <= 'Z') || code == ' ' ||
            code == 0x0D || code == 0x08 || code == 0x09 || code == 0x1B)
            return code;
        switch (code) {
        case 0xBA:
            return ';';
        case 0xBB:
            return '=';
        case 0xBC:
            return ',';
        case 0xBD:
            return '-';
        case 0xBE:
            return '.';
        case 0xBF:
            return '/';
        case 0xC0:
            return '`';
        case 0xDB:
            return '[';
        case 0xDC:
            return '\\';
        case 0xDD:
            return ']';
        case 0xDE:
            return '\'';
        }
        if (code >= 0x60 && code <= 0x69)
            return '0' + (code - 0x60);
        return 0;
    default:
        return 0;
    }
}
void u_MapVirtualKeyA(X86 *c) {
    set_eax(c, map_virtual_key(arg(c, 0), arg(c, 1)));
}

void u_RegisterWindowMessageA(X86 *c) {
    static std::map<std::string, uint32_t> msgs;
    static uint32_t next_msg = 0xC000;
    std::string s = gm_str(arg(c, 0));
    auto it = msgs.find(s);
    if (it != msgs.end()) { set_eax(c, it->second); return; }
    uint32_t id = next_msg++;
    msgs[s] = id;
    set_eax(c, id);
}
void u_RegisterClipboardFormatA(X86 *c) {
    static std::map<std::string, uint32_t> fmts;
    static uint32_t next_fmt = 0xC400;
    std::string s = gm_str(arg(c, 0));
    auto it = fmts.find(s);
    if (it != fmts.end()) { set_eax(c, it->second); return; }
    uint32_t id = next_fmt++;
    fmts[s] = id;
    set_eax(c, id);
}
void u_SetWindowsHookExA(X86 *c) {
    static uint32_t next_hook = 0x484F0001u;
    uint32_t h = next_hook++;
    hooks().push_back(HookEntry{h, (int32_t)arg(c, 0), arg(c, 1), arg(c, 3)});
    set_eax(c, h);
}
void u_UnhookWindowsHookEx(X86 *c) {
    uint32_t h = arg(c, 0);
    auto &v = hooks();
    for (auto it = v.begin(); it != v.end(); ++it) {
        if (it->handle == h) {
            v.erase(it);
            set_eax(c, 1);
            return;
        }
    }
    set_eax(c, 0);
}
void u_CallNextHookEx(X86 *c) { set_eax(c, 0); }

static std::map<std::pair<uint32_t, std::string>, uint32_t> &win_props() {
    static auto *m = new std::map<std::pair<uint32_t, std::string>, uint32_t>();
    return *m;
}
static std::string prop_key(uint32_t p) {
    if (p < 0x10000) return "#" + std::to_string(p);
    return gm_str(p);
}
void u_SetPropA(X86 *c) {
    win_props()[{arg(c, 0), prop_key(arg(c, 1))}] = arg(c, 2);
    set_eax(c, 1);
}
void u_GetPropA(X86 *c) {
    auto &m = win_props();
    auto it = m.find({arg(c, 0), prop_key(arg(c, 1))});
    set_eax(c, it != m.end() ? it->second : 0);
}
void u_RemovePropA(X86 *c) {
    auto &m = win_props();
    auto it = m.find({arg(c, 0), prop_key(arg(c, 1))});
    uint32_t v = 0;
    if (it != m.end()) { v = it->second; m.erase(it); }
    set_eax(c, v);
}
void u_CharUpperA(X86 *c) {
    uint32_t p = arg(c, 0);
    if (p < 0x10000u) {
        set_eax(c, (uint32_t)std::toupper((unsigned char)(p & 0xff)));
        return;
    }
    for (uint32_t i = 0; i < 0x8000; i++) {
        uint8_t ch = rd8(p + i);
        if (!ch) break;
        wr8(p + i, (uint8_t)std::toupper(ch));
    }
    set_eax(c, p);
}
void u_CharLowerA(X86 *c) {
    uint32_t p = arg(c, 0);
    if (p < 0x10000u) {
        set_eax(c, (uint32_t)std::tolower((unsigned char)(p & 0xff)));
        return;
    }
    for (uint32_t i = 0; i < 0x8000; i++) {
        uint8_t ch = rd8(p + i);
        if (!ch) break;
        wr8(p + i, (uint8_t)std::tolower(ch));
    }
    set_eax(c, p);
}
void u_wvsprintfA(X86 *c) { u_wvsprintfA_va(c, arg(c, 0), arg(c, 1), arg(c, 2)); }
void u_wsprintfA(X86 *c) { u_wvsprintfA_va(c, arg(c, 0), arg(c, 1), c->r[4] + 12); }
void u_GetClassInfoA(X86 *c) { set_eax(c, 0); }
void u_GetClassInfoExA(X86 *c) { set_eax(c, 0); }
void u_LockWindowUpdate(X86 *c) { set_eax(c, 1); }
void u_ChangeDisplaySettingsA(X86 *c) { set_eax(c, 0); } // DISP_CHANGE_SUCCESSFUL
void u_DrawTextA(X86 *c) { set_eax(c, 16); }
void u_GetWindowTextLengthA(X86 *c) {
    uint32_t hwnd = arg(c, 0);
    Window *w = find_window(hwnd);
    if (!w) {
        set_eax(c, 0);
        return;
    }
    if (w->wndproc) {
        set_eax(c, host_dispatch_to_wndproc(c, hwnd, 0x000e /* WM_GETTEXTLENGTH */, 0, 0));
        return;
    }
    set_eax(c, (uint32_t)w->title_utf8.size());
}
void u_GetClassNameA(X86 *c) {
    uint32_t buf = arg(c, 1), cap = arg(c, 2);
    set_eax(c, (buf && cap) ? gm_put_str(buf, "WormsArmageddon", cap) : 0);
}
void u_GetClassLongA(X86 *c) { set_eax(c, 0); }
void u_ModifyMenuA(X86 *c) { set_eax(c, 1); }
void u_LoadBitmapA(X86 *c) {
    uint32_t bytes = 0, p = resource_data(resource_find(2, arg(c, 1)), &bytes);
    GdiImage image;
    set_eax(c, p && gdi_decode_image(p, bytes, &image) ? gdi_image_bitmap(image) : 0);
}
void u_SendDlgItemMessageA(X86 *c) {
    uint32_t child = find_dlg_item(arg(c, 0), arg(c, 1));
    Window *w = find_window(child);
    if (!w) {
        set_eax(c, 0);
        return;
    }
    set_eax(c, host_dispatch_to_wndproc(c, child, arg(c, 2), arg(c, 3), arg(c, 4)));
}
void u_IsDlgButtonChecked(X86 *c) { set_eax(c, 0); }
void u_SetKeyboardState(X86 *c) { set_eax(c, 1); }
void u_VkKeyScanA(X86 *c) { set_eax(c, arg(c, 0) & 0xffu); }
void u_OpenInputDesktop(X86 *c) { set_eax(c, 0x4445534bu); }
void u_CloseDesktop(X86 *c) { set_eax(c, 1); }
void u_WinHelpA(X86 *c) { set_eax(c, 1); }
void u_MoveWindow(X86 *c) {
    if (Window *w = find_window(arg(c, 0)))
        set_window_pos(c, w, 0, int32_t(arg(c, 1)), int32_t(arg(c, 2)), int32_t(arg(c, 3)),
                       int32_t(arg(c, 4)), 0x0004 /* SWP_NOZORDER */);
    set_eax(c, 1);
}
void u_LoadImageA(X86 *c) {
    uint32_t name = arg(c, 1), type = arg(c, 2), flags = arg(c, 5);
    if (type == 0) { // IMAGE_BITMAP
        uint32_t temp = 0, data = 0, bytes = 0, pixel_offset = 0;
        if (flags & 0x10) { // LR_LOADFROMFILE
            std::string path = win32_host_path(gm_str(name, 260));
            FILE *f = path.empty() ? nullptr : fopen(path.c_str(), "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long n = ftell(f);
                rewind(f);
                if (n >= 54 && uint64_t(n) < GUEST_SIZE) {
                    temp = heap_alloc(uint32_t(n));
                    if (temp && fread(g_mem + temp, 1, size_t(n), f) == size_t(n) &&
                        rd16(temp) == 0x4d42 && rd32(temp + 10) >= 54 &&
                        rd32(temp + 10) <= uint32_t(n)) {
                        pixel_offset = rd32(temp + 10) - 14;
                        data = temp + 14;
                        bytes = uint32_t(n) - 14;
                    }
                }
                fclose(f);
            }
        } else {
            uint32_t entry = resource_find(2, name);
            if (entry)
                data = resource_data(entry, &bytes);
        }
        uint32_t hbmp = 0;
        if (data) {
            if (flags & 0x2000) { // LR_CREATEDIBSECTION
                hbmp = gdi_create_dib_from_packed(data, bytes, pixel_offset);
            } else {
                GdiImage image;
                if (gdi_decode_image(data, bytes, &image, pixel_offset))
                    hbmp = gdi_image_bitmap(image);
            }
        }
        if (temp)
            heap_free(temp);
        set_eax(c, hbmp);
        return;
    }
    if (type == 1) { // IMAGE_ICON
        u_LoadIconA(c);
        return;
    }
    if (type == 2) { // IMAGE_CURSOR
        u_LoadCursorA(c);
        return;
    }
    set_eax(c, 0);
}
void u_GetWindowTextA(X86 *c) {
    uint32_t hwnd = arg(c, 0), buf = arg(c, 1), cap = arg(c, 2);
    Window *w = find_window(hwnd);
    if (!w || !buf || !cap) {
        set_eax(c, 0);
        return;
    }
    if (w->wndproc) {
        set_eax(c, host_dispatch_to_wndproc(c, hwnd, 0x000d /* WM_GETTEXT */, cap, buf));
        return;
    }
    set_eax(c, gm_put_str(buf, w->title_utf8.c_str(), cap));
}
void u_PtInRect(X86 *c) {
    uint32_t r = arg(c, 0);
    int32_t x = (int32_t)arg(c, 1), y = (int32_t)arg(c, 2);
    if (!r || !gm_valid(r, 16)) { set_eax(c, 0); return; }
    int32_t l = (int32_t)rd32(r), t = (int32_t)rd32(r + 4), ri = (int32_t)rd32(r + 8), b = (int32_t)rd32(r + 12);
    set_eax(c, (x >= l && x < ri && y >= t && y < b) ? 1 : 0);
}
void u_CopyRect(X86 *c) {
    uint32_t d = arg(c, 0), s = arg(c, 1);
    if (!d || !s || !gm_valid(d, 16) || !gm_valid(s, 16)) { set_eax(c, 0); return; }
    memcpy(g_mem + d, g_mem + s, 16);
    set_eax(c, 1);
}
void u_OffsetRect(X86 *c) {
    uint32_t r = arg(c, 0);
    int32_t dx = (int32_t)arg(c, 1), dy = (int32_t)arg(c, 2);
    if (!r || !gm_valid(r, 16)) { set_eax(c, 0); return; }
    wr32(r, (int32_t)rd32(r) + dx);
    wr32(r + 4, (int32_t)rd32(r + 4) + dy);
    wr32(r + 8, (int32_t)rd32(r + 8) + dx);
    wr32(r + 12, (int32_t)rd32(r + 12) + dy);
    set_eax(c, 1);
}
void u_InflateRect(X86 *c) {
    uint32_t r = arg(c, 0);
    int32_t dx = (int32_t)arg(c, 1), dy = (int32_t)arg(c, 2);
    if (!r || !gm_valid(r, 16)) { set_eax(c, 0); return; }
    wr32(r, (int32_t)rd32(r) - dx);
    wr32(r + 4, (int32_t)rd32(r + 4) - dy);
    wr32(r + 8, (int32_t)rd32(r + 8) + dx);
    wr32(r + 12, (int32_t)rd32(r + 12) + dy);
    set_eax(c, 1);
}
void u_SetRectEmpty(X86 *c) {
    uint32_t r = arg(c, 0);
    if (!r || !gm_valid(r, 16)) { set_eax(c, 0); return; }
    memset(g_mem + r, 0, 16);
    set_eax(c, 1);
}
void u_EqualRect(X86 *c) {
    uint32_t a = arg(c, 0), b = arg(c, 1);
    if (!a || !b || !gm_valid(a, 16) || !gm_valid(b, 16)) { set_eax(c, 0); return; }
    set_eax(c, memcmp(g_mem + a, g_mem + b, 16) == 0 ? 1 : 0);
}
void u_SubtractRect(X86 *c) {
    uint32_t d = arg(c, 0), s1 = arg(c, 1);
    if (!d || !s1 || !gm_valid(d, 16) || !gm_valid(s1, 16)) { set_eax(c, 0); return; }
    memcpy(g_mem + d, g_mem + s1, 16);
    set_eax(c, 1);
}
void u_ret0(X86 *c) { set_eax(c, 0); }
void u_ret1(X86 *c) { set_eax(c, 1); }
void u_GetMenuCheckMarkDimensions(X86 *c) { set_eax(c, (13u << 16) | 13u); }
void u_GetDlgItemTextA(X86 *c) {
    uint32_t child = find_dlg_item(arg(c, 0), arg(c, 1));
    uint32_t buf = arg(c, 2), cap = arg(c, 3);
    Window *w = find_window(child);
    if (!w || !buf || !cap) {
        if (buf && cap && gm_valid(buf, 1)) wr8(buf, 0);
        set_eax(c, 0);
        return;
    }
    if (w->wndproc) {
        set_eax(c, host_dispatch_to_wndproc(c, child, 0x000d /* WM_GETTEXT */, cap, buf));
        return;
    }
    set_eax(c, gm_put_str(buf, w->title_utf8.c_str(), cap));
}
void u_GetDlgItemInt(X86 *c) {
    if (arg(c, 2) && gm_valid(arg(c, 2), 4)) wr32(arg(c, 2), 0);
    set_eax(c, 0);
}
void u_GetDlgItem(X86 *c) {
    uint32_t parent = arg(c, 0), id = arg(c, 1);
    for (const auto &kv : windows()) {
        if (kv.second.parent == parent && kv.second.id == id) {
            set_eax(c, kv.first);
            return;
        }
    }
    set_eax(c, 0);
}
static uint32_t dlg_skip_sz_or_ord(uint32_t p, std::string *out = nullptr) {
    if (!gm_valid(p, 2)) return p;
    uint16_t first = rd16(p);
    if (first == 0) return p + 2;
    if (first == 0xffff) {
        if (out) *out = "#" + std::to_string(rd16(p + 2));
        return p + 4;
    }
    std::string s;
    while (gm_valid(p, 2)) {
        uint16_t ch = rd16(p);
        p += 2;
        if (!ch) break;
        if (ch < 128) s.push_back((char)ch);
    }
    if (out) *out = s;
    return p;
}
void u_CreateDialogIndirectParamA(X86 *c) {
    uint32_t hinst = arg(c, 0), tpl = arg(c, 1), parent = arg(c, 2), dlgproc = arg(c, 3), lparam = arg(c, 4);
    if (!tpl || !gm_valid(tpl, 18)) {
        set_eax(c, 0);
        return;
    }
    bool is_ex = (rd16(tpl) == 1 && rd16(tpl + 2) == 0xffff);
    uint32_t exstyle = 0, style = 0;
    uint16_t cdit = 0;
    int16_t x = 0, y = 0, cx = 320, cy = 240;
    uint32_t p = tpl;
    if (is_ex) {
        exstyle = rd32(p + 8);
        style = rd32(p + 12);
        cdit = rd16(p + 16);
        x = (int16_t)rd16(p + 18);
        y = (int16_t)rd16(p + 20);
        cx = (int16_t)rd16(p + 22);
        cy = (int16_t)rd16(p + 24);
        p += 26;
    } else {
        style = rd32(p + 0);
        exstyle = rd32(p + 4);
        cdit = rd16(p + 8);
        x = (int16_t)rd16(p + 10);
        y = (int16_t)rd16(p + 12);
        cx = (int16_t)rd16(p + 14);
        cy = (int16_t)rd16(p + 16);
        p += 18;
    }
    std::string title;
    p = dlg_skip_sz_or_ord(p); // menu
    p = dlg_skip_sz_or_ord(p); // class
    p = dlg_skip_sz_or_ord(p, &title); // title
    if (style & 0x40u) { // DS_SETFONT
        p += is_ex ? 6 : 2;
        p = dlg_skip_sz_or_ord(p); // typeface
    }
    int32_t px = (x * 6) / 4, py = (y * 13) / 8;
    int32_t pw = cx > 0 ? (cx * 6) / 4 : 640, ph = cy > 0 ? (cy * 13) / 8 : 480;

    uint32_t hwnd = g_next_hwnd;
    g_next_hwnd += 4;
    Window win;
    win.hwnd = hwnd;
    win.wndproc = dlgproc;
    win.style = style;
    win.exstyle = exstyle;
    win.x = px;
    win.y = py;
    win.w = pw;
    win.h = ph;
    win.cls = "#32770";
    win.title_utf8 = title;
    win.parent = (style & 0x40000000u) ? parent : 0;
    win.owner = win.parent ? 0 : parent;
    win.enabled = !(style & 0x08000000u);
    win.thread = guest_current_thread_id();
    win.hinstance = hinst;
    win.extra.assign(8, 0);
    windows()[hwnd] = win;
    if (!g_main_hwnd)
        g_main_hwnd = hwnd;

    uint32_t cs = heap_alloc(48, true);
    if (cs) {
        wr32(cs + 0, lparam);
        wr32(cs + 4, hinst);
        wr32(cs + 12, parent);
        wr32(cs + 16, (uint32_t)ph);
        wr32(cs + 20, (uint32_t)pw);
        wr32(cs + 24, (uint32_t)py);
        wr32(cs + 28, (uint32_t)px);
        wr32(cs + 32, style);
        wr32(cs + 44, exstyle);
    }
    sched_atomic_enter(c->r[R_ESP]);
    fire_cbt_createwnd(c, hwnd, cs);
    host_dispatch_to_wndproc(c, hwnd, 0x0081 /* WM_NCCREATE */, 0, cs);
    host_dispatch_to_wndproc(c, hwnd, 0x0001 /* WM_CREATE */, 0, cs);
    if (cs)
        heap_free(cs);

    // Create child controls from DLGITEMTEMPLATE / DLGITEMTEMPLATEEX
    ensure_system_classes();
    uint32_t first_child = 0;
    for (uint16_t i = 0; i < cdit; ++i) {
        p = (p + 3u) & ~3u;
        if (!gm_valid(p, is_ex ? 24 : 18)) break;
        uint32_t istyle = 0, iex = 0, iid = 0;
        int16_t ix = 0, iy = 0, icx = 0, icy = 0;
        if (is_ex) {
            iex = rd32(p + 4);
            istyle = rd32(p + 8);
            ix = (int16_t)rd16(p + 12);
            iy = (int16_t)rd16(p + 14);
            icx = (int16_t)rd16(p + 16);
            icy = (int16_t)rd16(p + 18);
            iid = rd32(p + 20);
            p += 24;
        } else {
            istyle = rd32(p + 0);
            iex = rd32(p + 4);
            ix = (int16_t)rd16(p + 8);
            iy = (int16_t)rd16(p + 10);
            icx = (int16_t)rd16(p + 12);
            icy = (int16_t)rd16(p + 14);
            iid = rd16(p + 16);
            p += 18;
        }
        std::string icls, ititle;
        p = dlg_skip_sz_or_ord(p, &icls);
        if (icls == "#128") icls = "Button";
        else if (icls == "#129") icls = "Edit";
        else if (icls == "#130") icls = "Static";
        else if (icls == "#131") icls = "ListBox";
        else if (icls == "#132") icls = "ScrollBar";
        else if (icls == "#133") icls = "ComboBox";
        p = dlg_skip_sz_or_ord(p, &ititle);
        uint16_t extra_cb = gm_valid(p, 2) ? rd16(p) : 0;
        p += 2 + extra_cb;

        uint32_t chwnd = g_next_hwnd;
        g_next_hwnd += 4;
        Window cw;
        cw.hwnd = chwnd;
        auto cit = classes().find(lower(icls));
        cw.wndproc = (cit != classes().end() && cit->second.wndproc)
                         ? cit->second.wndproc
                         : imports_resolve("USER32.dll", "DefWindowProcA");
        cw.style = istyle | 0x40000000u;
        cw.exstyle = iex;
        cw.x = (ix * 6) / 4;
        cw.y = (iy * 13) / 8;
        cw.w = (icx * 6) / 4;
        cw.h = (icy * 13) / 8;
        cw.cls = icls;
        cw.title_utf8 = ititle;
        cw.parent = hwnd;
        cw.id = iid;
        cw.visible = (istyle & WS_VISIBLE) != 0;
        cw.enabled = !(istyle & 0x08000000u);
        cw.thread = guest_current_thread_id();
        cw.hinstance = hinst;
        windows()[chwnd] = cw;
        window_created(chwnd);
        if (!first_child) first_child = chwnd;
    }

    host_dispatch_to_wndproc(c, hwnd, 0x0110 /* WM_INITDIALOG */, first_child, lparam);
    if (Window *nw = find_window(hwnd)) {
        send_geometry(c, hwnd, nw, true, true);
        nw->shown = true;
        if ((nw->style & WS_VISIBLE) && !nw->visible) {
            nw->visible = true;
            nw->update_pending = true;
            if (g_window_shown)
                g_window_shown(hwnd);
        }
    }
    sched_atomic_leave();
    window_created(hwnd);
    LOGV("CreateDialogIndirectParamA(\"%s\", %u items) -> %08x", title.c_str(), cdit, hwnd);
    set_eax(c, hwnd);
}
void u_MapDialogRect(X86 *c) {
    uint32_t r = arg(c, 1);
    if (!r || !gm_valid(r, 16)) { set_eax(c, 0); return; }
    wr32(r + 0, ((int32_t)rd32(r + 0) * 6) / 4);
    wr32(r + 4, ((int32_t)rd32(r + 4) * 13) / 8);
    wr32(r + 8, ((int32_t)rd32(r + 8) * 6) / 4);
    wr32(r + 12, ((int32_t)rd32(r + 12) * 13) / 8);
    set_eax(c, 1);
}
void u_DialogBoxParamA(X86 *c) { set_eax(c, 1); } // IDOK
void u_EndDialog(X86 *c) { set_eax(c, 1); }

const ImportShim g_user32_shims[] = {
    {"USER32.dll", "RegisterClassA", 1, u_RegisterClassA},
    {"USER32.dll", "UnregisterClassA", 2, u_UnregisterClassA},
    {"USER32.dll", "CreateWindowExA", 12, u_CreateWindowExA},
    {"USER32.dll", "DestroyWindow", 1, u_DestroyWindow},
    {"USER32.dll", "ShowWindow", 2, u_ShowWindow},
    {"USER32.dll", "UpdateWindow", 1, u_UpdateWindow},
    {"USER32.dll", "SetWindowPos", 7, u_SetWindowPos},
    {"USER32.dll", "GetWindowRect", 2, u_GetWindowRect},
    {"USER32.dll", "GetClientRect", 2, u_GetClientRect},
    {"USER32.dll", "SystemParametersInfoA", 4, u_SystemParametersInfoA},
    {"USER32.dll", "AdjustWindowRectEx", 4, u_AdjustWindowRectEx},
    {"USER32.dll", "ClientToScreen", 2, u_ClientToScreen},
    {"USER32.dll", "SetRect", 5, u_SetRect},
    {"USER32.dll", "InvalidateRect", 3, u_InvalidateRect},
    {"USER32.dll", "SetLayeredWindowAttributes", 4, u_SetLayeredWindowAttributes},
    {"USER32.dll", "GetLayeredWindowAttributes", 4, u_GetLayeredWindowAttributes},
    {"USER32.dll", "SetWindowLongA", 3, u_SetWindowLongA},
    {"USER32.dll", "GetWindowLongA", 2, u_GetWindowLongA},
    {"USER32.dll", "SetWindowTextA", 2, u_SetWindowTextA},
    {"USER32.dll", "SetDlgItemTextA", 3, u_SetDlgItemTextA},
    {"USER32.dll", "CreateDialogParamA", 5, u_CreateDialogParamA},
    {"USER32.dll", "PeekMessageA", 5, u_PeekMessageA},
    {"USER32.dll", "GetMessageA", 4, u_GetMessageA},
    {"USER32.dll", "GetMessagePos", 0, u_GetMessagePos},
    {"USER32.dll", "GetMessageTime", 0, u_GetMessageTime},
    {"USER32.dll", "TranslateMessage", 1, u_TranslateMessage},
    {"USER32.dll", "DispatchMessageA", 1, u_DispatchMessageA},
    {"USER32.dll", "PostMessageA", 4, u_PostMessageA},
    {"USER32.dll", "DefWindowProcA", 4, u_DefWindowProcA},
    {"USER32.dll", "GetDC", 1, u_GetDC},
    {"USER32.dll", "ReleaseDC", 2, u_ReleaseDC},
    {"USER32.dll", "BeginPaint", 2, u_BeginPaint},
    {"USER32.dll", "EndPaint", 2, u_EndPaint},
    {"USER32.dll", "LoadIconA", 2, u_LoadIconA},
    {"USER32.dll", "LoadCursorA", 2, u_LoadCursorA},
    {"USER32.dll", "ValidateRect", 2, u_ValidateRect},
    {"USER32.dll", "GetUpdateRect", 3, u_GetUpdateRect},
    {"USER32.dll", "GetScrollBarInfo", 3, nullptr},
    {"USER32.dll", "LoadCursorFromFileA", 1, u_LoadCursorFromFileW},
    {"USER32.dll", "LoadCursorFromFileW", 1, u_LoadCursorFromFileW},
    {"USER32.dll", "CreateIconIndirect", 1, u_CreateIconIndirect},
    {"USER32.dll", "ScreenToClient", 2, u_ScreenToClient},
    {"USER32.dll", "OpenIcon", 1, u_OpenIcon},
    {"USER32.dll", "FindWindowA", 2, u_FindWindowA},
    {"USER32.dll", "DestroyIcon", 1, u_DestroyIcon},
    {"USER32.dll", "GetSystemMetrics", 1, u_GetSystemMetrics},
    {"USER32.dll", "IsWindowUnicode", 1, u_IsWindowUnicode},
    {"USER32.dll", "SetCursor", 1, u_SetCursor},
    {"USER32.dll", "SetCursorPos", 2, u_SetCursorPos},
    {"USER32.dll", "GetDoubleClickTime", 0, u_GetDoubleClickTime},
    {"USER32.dll", "GetKeyboardType", 1, u_GetKeyboardType},
    {"USER32.dll", "GetKeyboardLayout", 1, u_GetKeyboardLayout},
    {"USER32.dll", "MessageBoxA", 4, u_MessageBoxA},
    {"USER32.dll", "MessageBoxW", 4, u_MessageBoxW},
    {"USER32.dll", "wvsprintfA", 3, u_wvsprintfA},
    // Not imported by D3DPopTB.exe, but registered so GetProcAddress and the
    // host layer can reach them.
    {"USER32.dll", "SendMessageA", 4, u_SendMessageA},
    {"USER32.dll", "PostQuitMessage", 1, u_PostQuitMessage},
    {"USER32.dll", "GetCursorPos", 1, u_GetCursorPos},
    {"USER32.dll", "GetAsyncKeyState", 1, u_GetAsyncKeyState},
    {"USER32.dll", "GetKeyState", 1, u_GetKeyState},
    {"USER32.dll", "ShowCursor", 1, u_ShowCursor},
    {"USER32.dll", "ClipCursor", 1, u_ClipCursor},
    {"USER32.dll", "GetClipCursor", 1, u_GetClipCursor},
    {"USER32.dll", "RegisterClassExA", 1, u_RegisterClassExA},
    {"USER32.dll", "AdjustWindowRect", 3, u_AdjustWindowRect},
    {"USER32.dll", "SetCapture", 1, u_SetCapture},
    {"USER32.dll", "ReleaseCapture", 0, u_ReleaseCapture},
    {"USER32.dll", "GetDesktopWindow", 0, nullptr},
    {"USER32.dll", "MapVirtualKeyA", 2, u_MapVirtualKeyA},
    {"USER32.dll", "MapVirtualKeyExA", 3, u_MapVirtualKeyA},
    {"USER32.dll", "ToUnicode", 6, nullptr},
    {"USER32.dll", "SendInput", 3, nullptr},
    {"USER32.dll", "PostThreadMessageA", 4, u_PostThreadMessage},
    {"USER32.dll", "PostThreadMessageW", 4, u_PostThreadMessage},
    {"USER32.dll", "GetForegroundWindow", 0, u_GetForegroundWindow},
    {"USER32.dll", "WaitMessage", 0, u_WaitMessage},
    {"USER32.dll", "GetActiveWindow", 0, u_GetActiveWindow},
    {"USER32.dll", "SetFocus", 1, u_SetFocus},
    {"USER32.dll", "GetMenu", 1, u_GetMenu},
    {"USER32.dll", "IsIconic", 1, u_IsIconic},
    {"USER32.dll", "CloseWindow", 1, u_CloseWindow},
    {"USER32.dll", "SetForegroundWindow", 1, u_SetForegroundWindow},
    {"USER32.dll", "SetActiveWindow", 1, u_SetActiveWindow},
    {"USER32.dll", "OpenClipboard", 1, u_OpenClipboard},
    {"USER32.dll", "CloseClipboard", 0, u_CloseClipboard},
    {"USER32.dll", "IsClipboardFormatAvailable", 1, u_IsClipboardFormatAvailable},
    {"USER32.dll", "GetClipboardData", 1, u_GetClipboardData},
    {"USER32.dll", "RegisterWindowMessageA", 1, u_RegisterWindowMessageA},
    {"USER32.dll", "RegisterClipboardFormatA", 1, u_RegisterClipboardFormatA},
    {"USER32.dll", "SetWindowsHookExA", 4, u_SetWindowsHookExA},
    {"USER32.dll", "UnhookWindowsHookEx", 1, u_UnhookWindowsHookEx},
    {"USER32.dll", "CallNextHookEx", 4, u_CallNextHookEx},
    {"USER32.dll", "SetPropA", 3, u_SetPropA},
    {"USER32.dll", "GetPropA", 2, u_GetPropA},
    {"USER32.dll", "RemovePropA", 2, u_RemovePropA},
    {"USER32.dll", "CharUpperA", 1, u_CharUpperA},
    {"USER32.dll", "CharLowerA", 1, u_CharLowerA},
    {"USER32.dll", "wsprintfA", 0, u_wsprintfA},
    {"USER32.dll", "GetClassInfoA", 3, u_GetClassInfoA},
    {"USER32.dll", "GetClassInfoExA", 3, u_GetClassInfoExA},
    {"USER32.dll", "LockWindowUpdate", 1, u_LockWindowUpdate},
    {"USER32.dll", "ChangeDisplaySettingsA", 2, u_ChangeDisplaySettingsA},
    {"USER32.dll", "DrawTextA", 5, u_DrawTextA},
    {"USER32.dll", "GetWindowTextLengthA", 1, u_GetWindowTextLengthA},
    {"USER32.dll", "GetClassNameA", 3, u_GetClassNameA},
    {"USER32.dll", "GetClassLongA", 2, u_GetClassLongA},
    {"USER32.dll", "ModifyMenuA", 5, u_ModifyMenuA},
    {"USER32.dll", "LoadBitmapA", 2, u_LoadBitmapA},
    {"USER32.dll", "SendDlgItemMessageA", 5, u_SendDlgItemMessageA},
    {"USER32.dll", "IsDlgButtonChecked", 2, u_IsDlgButtonChecked},
    {"USER32.dll", "SetKeyboardState", 1, u_SetKeyboardState},
    {"USER32.dll", "VkKeyScanA", 1, u_VkKeyScanA},
    {"USER32.dll", "OpenInputDesktop", 3, u_OpenInputDesktop},
    {"USER32.dll", "CloseDesktop", 1, u_CloseDesktop},
    {"USER32.dll", "WinHelpA", 4, u_WinHelpA},
    {"USER32.dll", "MoveWindow", 6, u_MoveWindow},
    {"USER32.dll", "LoadImageA", 6, u_LoadImageA},
    {"USER32.dll", "GetWindowTextA", 3, u_GetWindowTextA},
    {"USER32.dll", "PtInRect", 3, u_PtInRect},
    {"USER32.dll", "CopyRect", 2, u_CopyRect},
    {"USER32.dll", "OffsetRect", 3, u_OffsetRect},
    {"USER32.dll", "InflateRect", 3, u_InflateRect},
    {"USER32.dll", "SetRectEmpty", 1, u_SetRectEmpty},
    {"USER32.dll", "EqualRect", 2, u_EqualRect},
    {"USER32.dll", "SubtractRect", 3, u_SubtractRect},
    {"USER32.dll", "BringWindowToTop", 1, u_ret1},
    {"USER32.dll", "FlashWindow", 2, u_ret0},
    {"USER32.dll", "GetQueueStatus", 1, u_ret0},
    {"USER32.dll", "GetDlgItem", 2, u_GetDlgItem},
    {"USER32.dll", "GetDlgItemTextA", 4, u_GetDlgItemTextA},
    {"USER32.dll", "GetDlgItemInt", 4, u_GetDlgItemInt},
    {"USER32.dll", "DialogBoxParamA", 5, u_DialogBoxParamA},
    {"USER32.dll", "CreateDialogIndirectParamA", 5, u_CreateDialogIndirectParamA},
    {"USER32.dll", "EndDialog", 2, u_EndDialog},
    {"USER32.dll", "FindWindowExA", 4, u_ret0},
    {"USER32.dll", "ChildWindowFromPointEx", 4, u_ret0},
    {"USER32.dll", "GetNextDlgTabItem", 3, u_ret0},
    {"USER32.dll", "MapDialogRect", 2, u_MapDialogRect},
    {"USER32.dll", "GetMenuCheckMarkDimensions", 0, u_GetMenuCheckMarkDimensions},
    {"USER32.dll", "SetMenuItemBitmaps", 5, u_ret1},
    {"USER32.dll", "TabbedTextOutA", 8, u_ret0},
    {"USER32.dll", "DrawTextExA", 6, u_DrawTextA},
    {"USER32.dll", "GrayStringA", 9, u_ret1},
    {"USER32.dll", "mouse_event", 5, u_ret0},
};
const size_t g_user32_shim_count = sizeof(g_user32_shims) / sizeof(g_user32_shims[0]);
