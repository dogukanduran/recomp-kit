// The system STATIC control. Windows draws a static control's caption in the
// class's own window procedure, and a program that subclasses the class - the
// VCL's TStaticText does, through GetClassInfoW - forwards every message it
// leaves unhandled there. Without the class that lookup fails, the subclass
// falls back to DefWindowProc, and every caption stays blank.
#include "memory.h"
#include "user32_internal.h"
#include <climits>

namespace user32 {
namespace {
// DrawText flags for the styles that show text (SS_TYPEMASK), and 0 for the
// ones that show an icon, a bitmap or a frame instead.
uint32_t text_flags(uint32_t style) {
    constexpr uint32_t center = 1, right = 2, vcenter = 4, wordbreak = 0x10, singleline = 0x20,
                       expandtabs = 0x40, noclip = 0x100, noprefix = 0x800;
    uint32_t flags;
    switch (style & 0x1f) {
    case 0x0: // SS_LEFT
        flags = wordbreak | expandtabs;
        break;
    case 0x1: // SS_CENTER
        flags = center | wordbreak | expandtabs;
        break;
    case 0x2: // SS_RIGHT
        flags = right | wordbreak | expandtabs;
        break;
    case 0xb: // SS_SIMPLE
        flags = singleline | noclip;
        break;
    case 0xc: // SS_LEFTNOWORDWRAP
        flags = expandtabs;
        break;
    default:
        return 0;
    }
    if (style & 0x80) // SS_NOPREFIX
        flags |= noprefix;
    if (style & 0x200) // SS_CENTERIMAGE
        flags |= vcenter | singleline;
    return flags;
}

// `into` is WM_PAINT's or WM_PRINTCLIENT's wParam: a DC to paint into instead
// of the window. A program that double-buffers its controls - the VCL does -
// paints them into a memory DC this way and copies it over the window after,
// so painting the window directly would be covered by that copy.
void paint(X86 *c, uint32_t hwnd, uint32_t into) {
    Window *w = find_window(hwnd);
    if (!w)
        return;
    if (!into)
        w->update_pending = false; // painting validates, as BeginPaint does
    const uint32_t parent = w->parent, font = w->font, flags = text_flags(w->style);
    const int32_t width = w->w, height = w->h;
    const std::string text = w->title_utf8;
    uint32_t hdc = into ? into : gdi_window_dc(hwnd);
    auto *dc = gdi::dc_of(hdc);
    if (!dc)
        return;
    const uint32_t caller_font = dc->font;
    if (font)
        dc->font = font;
    // The parent chooses the colours: it sets them on the DC and returns the
    // background brush, or a null brush to leave what is underneath. The VCL
    // reflects the message back to the control, which is where it answers.
    uint32_t brush =
        parent ? host_dispatch_to_wndproc(c, parent, 0x0138 /* WM_CTLCOLORSTATIC */, hdc, hwnd) : 0;
    uint32_t pixel;
    if (brush && gdi::brush_color(brush, &pixel))
        gdi::fill(hdc, {0, 0, width, height}, pixel);
    uint32_t units = text.empty() ? 0 : wide_units(text);
    if (flags && units && units < (UINT32_MAX - 32) / 2) {
        if (uint32_t block = heap_alloc(16 + 2 * (units + 1), true)) {
            wr32(block + 8, uint32_t(width));
            wr32(block + 12, uint32_t(height));
            gm_put_wstr(block + 16, text, units + 1);
            gdi::draw_text(hdc, block + 16, units, block, flags);
            heap_free(block);
        }
    }
    if (!into)
        gdi_release_window_dc(hwnd, hdc);
    else if (auto *caller = gdi::dc_of(hdc)) // the caller's DC keeps its own font
        caller->font = caller_font;
}

void static_proc(X86 *c) {
    uint32_t hwnd = arg(c, 0), msg = arg(c, 1);
    Window *w = find_window(hwnd);
    switch (msg) {
    case 0x000c: // WM_SETTEXT: the new caption has to be drawn
        def_window_proc(c, w && w->unicode);
        if (Window *after = find_window(hwnd))
            after->update_pending = true;
        return;
    case 0x000f: // WM_PAINT
    case 0x0318: // WM_PRINTCLIENT
        paint(c, hwnd, arg(c, 2));
        set_eax(c, 0);
        return;
    case 0x0030: // WM_SETFONT
        if (w) {
            w->font = arg(c, 2);
            if (arg(c, 3))
                w->update_pending = true;
        }
        set_eax(c, 0);
        return;
    case 0x0031: // WM_GETFONT
        set_eax(c, w ? w->font : 0);
        return;
    default:
        def_window_proc(c, w && w->unicode);
    }
}
} // namespace

void ensure_system_classes() {
    if (classes().count("static"))
        return;
    auto reg = [](const char *key, const char *name, uint32_t proc, uint32_t style = 0x4088) {
        WndClass wc;
        wc.style = style;
        wc.wndproc = proc;
        wc.name = name;
        wc.unicode = false;
        wc.atom = 0xc000 + uint32_t(classes().size());
        classes()[key] = wc;
        classes()["#atom" + std::to_string(wc.atom)] = wc;
    };
    uint32_t static_wndproc =
        imports_alloc_trampoline("USER32.dll", "STATIC window procedure", static_proc, 4);
    uint32_t def_wndproc = imports_resolve("USER32.dll", "DefWindowProcA");
    reg("static", "Static", static_wndproc);
    classes()["static"].unicode = true;
    reg("button", "Button", def_wndproc, 0x4008);
    reg("edit", "Edit", def_wndproc, 0x4088);
    reg("combobox", "ComboBox", def_wndproc, 0x4088);
    reg("listbox", "ListBox", def_wndproc, 0x4088);
    reg("scrollbar", "ScrollBar", def_wndproc, 0x4088);
    reg("mdiclient", "MDIClient", def_wndproc, 0x4000);
    reg("#32770", "#32770", def_wndproc, 0x4808);
    reg("msctls_trackbar32", "msctls_trackbar32", def_wndproc, 0x4008);
    reg("msctls_updown32", "msctls_updown32", def_wndproc, 0x4008);
    reg("msctls_progress32", "msctls_progress32", def_wndproc, 0x4008);
    reg("msctls_statusbar32", "msctls_statusbar32", def_wndproc, 0x4008);
    reg("msctls_hotkey32", "msctls_hotkey32", def_wndproc, 0x4008);
    reg("syslistview32", "SysListView32", def_wndproc, 0x4008);
    reg("systreeview32", "SysTreeView32", def_wndproc, 0x4008);
    reg("systabcontrol32", "SysTabControl32", def_wndproc, 0x4008);
    reg("sysheader32", "SysHeader32", def_wndproc, 0x4008);
    reg("sysanimate32", "SysAnimate32", def_wndproc, 0x4008);
    reg("tooltips_class32", "tooltips_class32", def_wndproc, 0x4008);
    reg("toolbarwindow32", "ToolbarWindow32", def_wndproc, 0x4008);
    reg("rebarwindow32", "ReBarWindow32", def_wndproc, 0x4008);
    reg("comboboxex32", "ComboBoxEx32", def_wndproc, 0x4008);
    reg("richedit", "RichEdit", def_wndproc, 0x4008);
    reg("richedit20a", "RichEdit20A", def_wndproc, 0x4008);
    reg("richedit20w", "RichEdit20W", def_wndproc, 0x4008);
}
} // namespace user32
