/*****************************************************************************
 * xmeye_control_win32.c: floating XMEye control interface for VLC 3.x
 *****************************************************************************
 * Original concept and functional design: Álvaro Ibarz
 * https://github.com/aibarz
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#define VLC_MODULE_LICENSE VLC_LICENSE_GPL_2_PLUS
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_interface.h>
#include <vlc_playlist.h>
#include <vlc_input.h>
#include <vlc_vout.h>

#include <windows.h>
#include <shellapi.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum
{
    ID_TAB_ADJUST = 1001,
    ID_TAB_ZOOM,
    ID_TOPMOST,

    ID_GATE = 1101,
    ID_DEBLOCK,
    ID_DECONV,
    ID_GRAY,
    ID_FOCUS,
    ID_APPLY_VALUES,

    ID_GATE_BLOCK = 1201,
    ID_GATE_MOTION,
    ID_GATE_CHANGED,
    ID_GATE_PREV,
    ID_DB_GRID,
    ID_DB_THR,
    ID_DB_STRENGTH,
    ID_DC_STRENGTH,
    ID_DC_THR,
    ID_DC_LIMIT,
    ID_GRAY_STEP,
    ID_GRAY_ACTIVITY,
    ID_GRAY_RATIO,
    ID_FOCUS_STRENGTH,
    ID_FOCUS_ACTIVITY,
    ID_FOCUS_LIMIT,

    ID_ZOOM_ACTIVE = 1301,
    ID_ZOOM_VALUE,
    ID_ZOOM_X,
    ID_ZOOM_Y,
    ID_SOURCE_W,
    ID_SOURCE_H,
    ID_ZOOM_APPLY,
    ID_ZOOM_CENTER,

    ID_STATUS = 1401,
    ID_CREDIT
};

struct intf_sys_t
{
    HWND window;
    HANDLE ready;
    vlc_thread_t thread;
    int page;
    HWND adjust[64];
    int adjust_count;
    HWND zoom[32];
    int zoom_count;
    HWND status;
    HWND credit;
    HWND legend;
    HFONT credit_font;
    HFONT legend_font;
};

static void add_page_control(HWND *list, int *count, HWND control)
{
    if (control != NULL && *count < 64)
        list[(*count)++] = control;
}

static void set_default_font(HWND h)
{
    if (h != NULL)
        SendMessage(h, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
}

static void settings_path(wchar_t *path, size_t count)
{
    wchar_t appdata[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
        _snwprintf(path, count - 1, L"%s\\vlc\\xmeye-control.ini", appdata);
    else
        _snwprintf(path, count - 1, L"xmeye-control.ini");
    path[count - 1] = 0;
}

static int load_setting(const wchar_t *key, int fallback)
{
    wchar_t path[MAX_PATH];
    settings_path(path, MAX_PATH);
    return (int)GetPrivateProfileIntW(L"window", key, fallback, path);
}

static void save_setting(const wchar_t *key, int value)
{
    wchar_t path[MAX_PATH], text[32];
    settings_path(path, MAX_PATH);
    _snwprintf(text, 31, L"%d", value);
    text[31] = 0;
    WritePrivateProfileStringW(L"window", key, text, path);
}

static HWND make_static(HWND parent, int x, int y, int w, int h, const wchar_t *text, int id)
{
    HWND c = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(NULL), NULL);
    set_default_font(c);
    return c;
}

static HWND make_button(HWND parent, int x, int y, int w, int h,
                        const wchar_t *text, int id, DWORD style)
{
    HWND c = CreateWindowExW(0, L"BUTTON", text,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(NULL), NULL);
    set_default_font(c);
    return c;
}

static HWND make_edit(HWND parent, int x, int y, int w, int h,
                      const wchar_t *text, int id)
{
    HWND c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                             ES_NUMBER | ES_CENTER | ES_AUTOHSCROLL,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(NULL), NULL);
    set_default_font(c);
    return c;
}

static void add_labeled_edit(HWND parent, HWND *list, int *count,
                             int x, int y, const wchar_t *label,
                             const wchar_t *value, int id)
{
    add_page_control(list, count, make_static(parent, x, y + 3, 142, 22, label, 0));
    add_page_control(list, count, make_edit(parent, x + 144, y, 48, 24, value, id));
}

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int edit_int(HWND parent, int id, int fallback, int lo, int hi)
{
    wchar_t buf[64];
    HWND h = GetDlgItem(parent, id);
    if (h == NULL || GetWindowTextW(h, buf, 64) <= 0)
        return fallback;
    return clamp_int(_wtoi(buf), lo, hi);
}

static void set_edit_int(HWND parent, int id, int value)
{
    wchar_t buf[32];
    _snwprintf(buf, 31, L"%d", value);
    buf[31] = 0;
    SetWindowTextW(GetDlgItem(parent, id), buf);
}

static void status_text(intf_sys_t *sys, const wchar_t *text)
{
    if (sys != NULL && sys->status != NULL)
        SetWindowTextW(sys->status, text);
}

static vout_thread_t *current_vout(intf_thread_t *intf)
{
    playlist_t *playlist = pl_Get(intf);
    input_thread_t *input = playlist_CurrentInput(playlist);
    if (input == NULL)
        return NULL;

    vout_thread_t *vout = input_GetVout(input);
    vlc_object_release(input);
    return vout;
}

static bool token_equals(const char *begin, size_t len, const char *name)
{
    return strlen(name) == len && !memcmp(begin, name, len);
}

static bool chain_has(const char *chain, const char *name)
{
    if (chain == NULL || *chain == '\0')
        return false;

    const char *p = chain;
    while (*p)
    {
        const char *e = strchr(p, ':');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (token_equals(p, len, name))
            return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}

static char *chain_without(const char *chain, const char *name)
{
    size_t cap = strlen(chain ? chain : "") + 1;
    char *out = malloc(cap);
    if (out == NULL)
        return NULL;
    out[0] = '\0';

    const char *p = chain ? chain : "";
    bool first = true;
    while (*p)
    {
        const char *e = strchr(p, ':');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len > 0 && !token_equals(p, len, name))
        {
            if (!first)
                strcat(out, ":");
            strncat(out, p, len);
            first = false;
        }
        if (!e) break;
        p = e + 1;
    }
    return out;
}

static void set_filter(intf_thread_t *intf, const char *name, bool enabled)
{
    playlist_t *playlist = pl_Get(intf);
    char *old = var_GetString(playlist, "video-filter");
    if (old == NULL)
        old = strdup("");

    char *next = NULL;
    if (enabled)
    {
        if (chain_has(old, name))
            next = strdup(old);
        else
        {
            size_t n = strlen(old) + strlen(name) + 2;
            next = malloc(n);
            if (next != NULL)
            {
                if (*old)
                    snprintf(next, n, "%s:%s", old, name);
                else
                    snprintf(next, n, "%s", name);
            }
        }
    }
    else
        next = chain_without(old, name);

    if (next != NULL)
    {
        var_SetString(playlist, "video-filter", next);
        vout_thread_t *vout = current_vout(intf);
        if (vout != NULL)
        {
            var_SetString(vout, "video-filter", next);
            vlc_object_release(vout);
        }
        free(next);
    }
    free(old);
}

static void set_option_int(intf_thread_t *intf, const char *name, int value)
{
    playlist_t *playlist = pl_Get(intf);
    var_Create(playlist, name, VLC_VAR_INTEGER | VLC_VAR_DOINHERIT);
    var_SetInteger(playlist, name, value);

    vout_thread_t *vout = current_vout(intf);
    if (vout != NULL)
    {
        var_Create(vout, name, VLC_VAR_INTEGER | VLC_VAR_DOINHERIT | VLC_VAR_ISCOMMAND);
        var_SetInteger(vout, name, value);
        vlc_object_release(vout);
    }
}

static void set_option_bool(intf_thread_t *intf, const char *name, bool value)
{
    playlist_t *playlist = pl_Get(intf);
    var_Create(playlist, name, VLC_VAR_BOOL | VLC_VAR_DOINHERIT);
    var_SetBool(playlist, name, value);

    vout_thread_t *vout = current_vout(intf);
    if (vout != NULL)
    {
        var_Create(vout, name, VLC_VAR_BOOL | VLC_VAR_DOINHERIT | VLC_VAR_ISCOMMAND);
        var_SetBool(vout, name, value);
        vlc_object_release(vout);
    }
}

static bool checked(HWND window, int id)
{
    return SendMessage(GetDlgItem(window, id), BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static void apply_filter_switch(intf_thread_t *intf, int id)
{
    HWND w = intf->p_sys->window;
    switch (id)
    {
        case ID_GATE:    set_filter(intf, "xmeyegate", checked(w, ID_GATE)); break;
        case ID_DEBLOCK: set_filter(intf, "xmeyedeblock", checked(w, ID_DEBLOCK)); break;
        case ID_DECONV:  set_filter(intf, "xmeyedeconv", checked(w, ID_DECONV)); break;
        case ID_GRAY:    set_filter(intf, "xmeyegrayguard", checked(w, ID_GRAY)); break;
        case ID_FOCUS:   set_filter(intf, "xmeyefocus", checked(w, ID_FOCUS)); break;
    }
}

static void apply_values(intf_thread_t *intf)
{
    HWND w = intf->p_sys->window;

    set_option_bool(intf, "xmeyegate-enabled", checked(w, ID_GATE));
    set_option_int(intf, "xmeyegate-block-size", edit_int(w, ID_GATE_BLOCK, 16, 8, 32));
    set_option_int(intf, "xmeyegate-motion-threshold", edit_int(w, ID_GATE_MOTION, 4, 1, 32));
    set_option_int(intf, "xmeyegate-max-changed-percent", edit_int(w, ID_GATE_CHANGED, 8, 0, 50));
    set_option_int(intf, "xmeyegate-max-prev-weight", edit_int(w, ID_GATE_PREV, 50, 0, 60));

    set_option_bool(intf, "xmeyedeblock-enabled", checked(w, ID_DEBLOCK));
    set_option_int(intf, "xmeyedeblock-grid", edit_int(w, ID_DB_GRID, 8, 4, 32));
    set_option_int(intf, "xmeyedeblock-threshold", edit_int(w, ID_DB_THR, 8, 1, 32));
    set_option_int(intf, "xmeyedeblock-strength", edit_int(w, ID_DB_STRENGTH, 35, 0, 100));

    set_option_bool(intf, "xmeyedeconv-enabled", checked(w, ID_DECONV));
    set_option_int(intf, "xmeyedeconv-strength", edit_int(w, ID_DC_STRENGTH, 30, 0, 100));
    set_option_int(intf, "xmeyedeconv-threshold", edit_int(w, ID_DC_THR, 5, 0, 32));
    set_option_int(intf, "xmeyedeconv-correction-limit", edit_int(w, ID_DC_LIMIT, 5, 1, 32));

    set_option_bool(intf, "xmeyegrayguard-enabled", checked(w, ID_GRAY));
    set_option_bool(intf, "xmeyegrayguard-adaptive", true);
    set_option_int(intf, "xmeyegrayguard-sample-step", edit_int(w, ID_GRAY_STEP, 8, 2, 32));
    set_option_int(intf, "xmeyegrayguard-flat-activity", edit_int(w, ID_GRAY_ACTIVITY, 3, 0, 32));
    set_option_int(intf, "xmeyegrayguard-ratio-percent", edit_int(w, ID_GRAY_RATIO, 20, 5, 60));

    set_option_bool(intf, "xmeyefocus-enabled", checked(w, ID_FOCUS));
    set_option_int(intf, "xmeyefocus-strength", edit_int(w, ID_FOCUS_STRENGTH, 25, 0, 100));
    set_option_int(intf, "xmeyefocus-activity-threshold", edit_int(w, ID_FOCUS_ACTIVITY, 6, 0, 32));
    set_option_int(intf, "xmeyefocus-correction-limit", edit_int(w, ID_FOCUS_LIMIT, 6, 1, 32));

    status_text(intf->p_sys, L"Valores XMEye aplicados.");
}

static void apply_zoom(intf_thread_t *intf)
{
    intf_sys_t *sys = intf->p_sys;
    HWND w = sys->window;
    vout_thread_t *vout = current_vout(intf);
    if (vout == NULL)
    {
        status_text(sys, L"No hay vídeo activo; la interfaz funciona sin cargar vídeo.");
        InvalidateRect(w, NULL, FALSE);
        return;
    }

    if (!checked(w, ID_ZOOM_ACTIVE))
    {
        var_SetString(vout, "crop", "0+0+0+0");
        var_SetBool(vout, "autoscale", true);
        vlc_object_release(vout);
        status_text(sys, L"Zoom desactivado.");
        InvalidateRect(w, NULL, FALSE);
        return;
    }

    int zoom = edit_int(w, ID_ZOOM_VALUE, 200, 100, 800);
    int cx = edit_int(w, ID_ZOOM_X, 50, 0, 100);
    int cy = edit_int(w, ID_ZOOM_Y, 50, 0, 100);
    int sw = edit_int(w, ID_SOURCE_W, 2592, 16, 10000);
    int sh = edit_int(w, ID_SOURCE_H, 1944, 16, 10000);

    int vw = (sw * 100 + zoom / 2) / zoom;
    int vh = (sh * 100 + zoom / 2) / zoom;
    if (vw < 2) vw = 2;
    if (vh < 2) vh = 2;

    int left = (cx * sw) / 100 - vw / 2;
    int top = (cy * sh) / 100 - vh / 2;
    left = clamp_int(left, 0, sw - vw);
    top = clamp_int(top, 0, sh - vh);
    int right = sw - vw - left;
    int bottom = sh - vh - top;

    char crop[128];
    snprintf(crop, sizeof(crop), "%d+%d+%d+%d", left, top, right, bottom);
    var_SetString(vout, "crop", crop);
    var_SetBool(vout, "autoscale", true);
    vlc_object_release(vout);

    wchar_t text[160];
    _snwprintf(text, 159, L"Zoom %d%% · centro %d%%/%d%% · ROI %dx%d", zoom, cx, cy, vw, vh);
    text[159] = 0;
    status_text(sys, text);
    InvalidateRect(w, NULL, FALSE);
}

static void show_page(intf_sys_t *sys, int page)
{
    sys->page = page;
    for (int i = 0; i < sys->adjust_count; ++i)
        ShowWindow(sys->adjust[i], page == 0 ? SW_SHOW : SW_HIDE);
    for (int i = 0; i < sys->zoom_count; ++i)
        ShowWindow(sys->zoom[i], page == 1 ? SW_SHOW : SW_HIDE);
    InvalidateRect(sys->window, NULL, TRUE);
}

static void create_adjust_page(HWND hwnd, intf_sys_t *sys)
{
    int y = 92;
    add_page_control(sys->adjust, &sys->adjust_count,
        make_static(hwnd, 24, 66, 620, 24, L"Filtros XMEye · activar y ajustar sin tocar la interfaz Qt de VLC", 0));

    add_page_control(sys->adjust, &sys->adjust_count,
        make_button(hwnd, 24, y, 142, 26, L"Static Gate V2", ID_GATE, BS_AUTOCHECKBOX));
    add_labeled_edit(hwnd, sys->adjust, &sys->adjust_count, 190, y, L"Bloque [8,32] {16}", L"16", ID_GATE_BLOCK);
    add_labeled_edit(hwnd, sys->adjust, &sys->adjust_count, 390, y, L"Δ muestra [1,32] {4}", L"4", ID_GATE_MOTION);
    y += 38;
    add_labeled_edit(hwnd, sys->adjust, &sys->adjust_count, 190, y, L"% cambio [0,50] {8}", L"8", ID_GATE_CHANGED);
    add_labeled_edit(hwnd, sys->adjust, &sys->adjust_count, 390, y, L"Peso ant.% [0,60] {50}", L"50", ID_GATE_PREV);

    y += 48;
    add_page_control(sys->adjust, &sys->adjust_count,
        make_button(hwnd, 24, y, 142, 26, L"Deblock", ID_DEBLOCK, BS_AUTOCHECKBOX));
    add_labeled_edit(hwnd, sys->adjust, &sys->adjust_count, 190, y, L"Grid [4,32] {8}", L"8", ID_DB_GRID);
    add_labeled_edit(hwnd, sys->adjust, &sys->adjust_count, 390, y, L"Umbral [1,32] {8}", L"8", ID_DB_THR);
    y += 38;
    add_labeled_edit(hwnd, sys->adjust, &sys->adjust_count, 190, y, L"Fuerza % [0,100] {35}", L"35", ID_DB_STRENGTH);

    y += 48;
    add_page_control(sys->adjust, &sys->adjust_count,
        make_button(hwnd, 24, y, 142, 26, L"Deconv", ID_DECONV, BS_AUTOCHECKBOX));
    add_labeled_edit(hwnd, sys->adjust, &sys->adjust_count, 190, y, L"Fuerza % [0,100] {30}", L"30", ID_DC_STRENGTH);
    add_labeled_edit(hwnd, sys->adjust, &sys->adjust_count, 390, y, L"Umbral [0,32] {5}", L"5", ID_DC_THR);
    y += 38;
    add_labeled_edit(hwnd, sys->adjust, &sys->adjust_count, 190, y, L"Límite [1,32] {5}", L"5", ID_DC_LIMIT);

    y += 48;
    add_page_control(sys->adjust, &sys->adjust_count,
        make_button(hwnd, 24, y, 142, 26, L"Gray Guard", ID_GRAY, BS_AUTOCHECKBOX));
    add_labeled_edit(hwnd, sys->adjust, &sys->adjust_count, 190, y, L"Paso [2,32] {8}", L"8", ID_GRAY_STEP);
    add_labeled_edit(hwnd, sys->adjust, &sys->adjust_count, 390, y, L"Actividad [0,32] {3}", L"3", ID_GRAY_ACTIVITY);
    y += 38;
    add_labeled_edit(hwnd, sys->adjust, &sys->adjust_count, 190, y, L"Ratio % [5,60] {20}", L"20", ID_GRAY_RATIO);

    y += 48;
    add_page_control(sys->adjust, &sys->adjust_count,
        make_button(hwnd, 24, y, 142, 26, L"Focus", ID_FOCUS, BS_AUTOCHECKBOX));
    add_labeled_edit(hwnd, sys->adjust, &sys->adjust_count, 190, y, L"Fuerza % [0,100] {25}", L"25", ID_FOCUS_STRENGTH);
    add_labeled_edit(hwnd, sys->adjust, &sys->adjust_count, 390, y, L"Actividad [0,32] {6}", L"6", ID_FOCUS_ACTIVITY);
    y += 38;
    add_labeled_edit(hwnd, sys->adjust, &sys->adjust_count, 190, y, L"Límite [1,32] {6}", L"6", ID_FOCUS_LIMIT);

    add_page_control(sys->adjust, &sys->adjust_count,
        make_button(hwnd, 500, 486, 156, 30, L"Aplicar valores", ID_APPLY_VALUES, BS_PUSHBUTTON));
}

static void create_zoom_page(HWND hwnd, intf_sys_t *sys)
{
    add_page_control(sys->zoom, &sys->zoom_count,
        make_static(hwnd, 24, 66, 620, 24, L"Zoom interactivo externo · VLC oficial permanece intacto", 0));

    add_page_control(sys->zoom, &sys->zoom_count,
        make_button(hwnd, 24, 466, 112, 26, L"Zoom activo", ID_ZOOM_ACTIVE, BS_AUTOCHECKBOX));
    SendMessage(GetDlgItem(hwnd, ID_ZOOM_ACTIVE), BM_SETCHECK, BST_CHECKED, 0);

    add_labeled_edit(hwnd, sys->zoom, &sys->zoom_count, 150, 466, L"Zoom % [100,800] {200}", L"200", ID_ZOOM_VALUE);
    add_labeled_edit(hwnd, sys->zoom, &sys->zoom_count, 365, 466, L"Centro X [0,100] {50}", L"50", ID_ZOOM_X);
    add_labeled_edit(hwnd, sys->zoom, &sys->zoom_count, 580, 466, L"Centro Y [0,100] {50}", L"50", ID_ZOOM_Y);

    add_labeled_edit(hwnd, sys->zoom, &sys->zoom_count, 24, 504, L"Fuente W [16,10000] {2592}", L"2592", ID_SOURCE_W);
    add_labeled_edit(hwnd, sys->zoom, &sys->zoom_count, 270, 504, L"Fuente H [16,10000] {1944}", L"1944", ID_SOURCE_H);

    add_page_control(sys->zoom, &sys->zoom_count,
        make_button(hwnd, 510, 504, 110, 28, L"Centrar", ID_ZOOM_CENTER, BS_PUSHBUTTON));
    add_page_control(sys->zoom, &sys->zoom_count,
        make_button(hwnd, 630, 504, 130, 28, L"Aplicar zoom", ID_ZOOM_APPLY, BS_DEFPUSHBUTTON));
}

static void paint_zoom_preview(intf_sys_t *sys, HDC dc)
{
    if (sys->page != 1)
        return;

    RECT r = {24, 98, 780, 438};
    HBRUSH bg = CreateSolidBrush(RGB(68, 68, 68));
    FillRect(dc, &r, bg);
    DeleteObject(bg);

    HPEN grid = CreatePen(PS_SOLID, 1, RGB(118, 118, 118));
    HPEN old = SelectObject(dc, grid);
    for (int i = 1; i < 4; ++i)
    {
        int x = r.left + (r.right-r.left) * i / 4;
        int y = r.top + (r.bottom-r.top) * i / 4;
        MoveToEx(dc, x, r.top, NULL); LineTo(dc, x, r.bottom);
        MoveToEx(dc, r.left, y, NULL); LineTo(dc, r.right, y);
    }
    SelectObject(dc, old);
    DeleteObject(grid);

    int zoom = edit_int(sys->window, ID_ZOOM_VALUE, 200, 100, 800);
    int cx = edit_int(sys->window, ID_ZOOM_X, 50, 0, 100);
    int cy = edit_int(sys->window, ID_ZOOM_Y, 50, 0, 100);
    int rw = (r.right-r.left) * 100 / zoom;
    int rh = (r.bottom-r.top) * 100 / zoom;
    int centerx = r.left + (r.right-r.left) * cx / 100;
    int centery = r.top + (r.bottom-r.top) * cy / 100;

    RECT roi = {centerx-rw/2, centery-rh/2, centerx+rw/2, centery+rh/2};
    if (roi.left < r.left) OffsetRect(&roi, r.left-roi.left, 0);
    if (roi.right > r.right) OffsetRect(&roi, r.right-roi.right, 0);
    if (roi.top < r.top) OffsetRect(&roi, 0, r.top-roi.top);
    if (roi.bottom > r.bottom) OffsetRect(&roi, 0, r.bottom-roi.bottom);

    HPEN white = CreatePen(PS_SOLID, 3, RGB(245,245,245));
    HBRUSH hollow = GetStockObject(HOLLOW_BRUSH);
    old = SelectObject(dc, white);
    HBRUSH oldb = SelectObject(dc, hollow);
    Rectangle(dc, roi.left, roi.top, roi.right, roi.bottom);
    int mx = (roi.left + roi.right) / 2;
    int my = (roi.top + roi.bottom) / 2;
    MoveToEx(dc, mx-10, my, NULL); LineTo(dc, mx+10, my);
    MoveToEx(dc, mx, my-10, NULL); LineTo(dc, mx, my+10);
    SelectObject(dc, oldb);
    SelectObject(dc, old);
    DeleteObject(white);
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    intf_thread_t *intf = (intf_thread_t *)(uintptr_t)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    intf_sys_t *sys = intf ? intf->p_sys : NULL;

    if (msg == WM_NCCREATE)
    {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        intf = cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)(uintptr_t)intf);
        sys = intf->p_sys;
    }

    switch (msg)
    {
        case WM_CREATE:
        {
            sys->window = hwnd;
            make_static(hwnd, 22, 14, 210, 30, L"XMEye Control", 0);
            make_button(hwnd, 240, 12, 110, 30, L"Ajuste", ID_TAB_ADJUST, BS_PUSHBUTTON);
            make_button(hwnd, 356, 12, 110, 30, L"Zoom", ID_TAB_ZOOM, BS_PUSHBUTTON);
            make_button(hwnd, 510, 14, 146, 24, L"Siempre encima", ID_TOPMOST, BS_AUTOCHECKBOX);

            create_adjust_page(hwnd, sys);
            create_zoom_page(hwnd, sys);

            sys->status = make_static(hwnd, 24, 548, 756, 24,
                                      L"Listo. No es necesario cargar un vídeo para probar la interfaz.",
                                      ID_STATUS);
            sys->legend = make_static(hwnd, 24, 576, 390, 18,
                                      L"[min,max] rango permitido · {x} valor por defecto", 0);
            sys->credit = CreateWindowExW(0, L"STATIC",
                                      L"Álvaro Ibarz · github.com/aibarz",
                                      WS_CHILD | WS_VISIBLE | SS_NOTIFY | SS_RIGHT,
                                      500, 574, 280, 20, hwnd, (HMENU)(INT_PTR)ID_CREDIT,
                                      GetModuleHandleW(NULL), NULL);
            sys->legend_font = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                           DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            sys->credit_font = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, TRUE, FALSE,
                                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                           DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            SendMessage(sys->legend, WM_SETFONT, (WPARAM)sys->legend_font, TRUE);
            SendMessage(sys->credit, WM_SETFONT, (WPARAM)sys->credit_font, TRUE);

            int saved_page = clamp_int(load_setting(L"page", 0), 0, 1);
            bool saved_topmost = load_setting(L"topmost", 0) != 0;
            SendMessage(GetDlgItem(hwnd, ID_TOPMOST), BM_SETCHECK, saved_topmost ? BST_CHECKED : BST_UNCHECKED, 0);
            if (saved_topmost)
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            show_page(sys, saved_page);
            SetEvent(sys->ready);
            return 0;
        }

        case WM_COMMAND:
        {
            int id = LOWORD(wp);
            if (id == ID_TAB_ADJUST) { show_page(sys, 0); return 0; }
            if (id == ID_TAB_ZOOM)   { show_page(sys, 1); return 0; }

            if (id == ID_TOPMOST)
            {
                bool on = checked(hwnd, ID_TOPMOST);
                SetWindowPos(hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST,
                             0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                return 0;
            }

            if (id == ID_CREDIT && HIWORD(wp) == STN_CLICKED)
            {
                ShellExecuteW(hwnd, L"open", L"https://github.com/aibarz", NULL, NULL, SW_SHOWNORMAL);
                return 0;
            }

            if (id >= ID_GATE && id <= ID_FOCUS && HIWORD(wp) == BN_CLICKED)
            {
                apply_filter_switch(intf, id);
                status_text(sys, L"Cadena de filtros actualizada.");
                return 0;
            }

            if (id == ID_APPLY_VALUES)
            {
                apply_values(intf);
                return 0;
            }

            if (id == ID_ZOOM_CENTER)
            {
                set_edit_int(hwnd, ID_ZOOM_X, 50);
                set_edit_int(hwnd, ID_ZOOM_Y, 50);
                apply_zoom(intf);
                return 0;
            }

            if (id == ID_ZOOM_APPLY || id == ID_ZOOM_ACTIVE)
            {
                apply_zoom(intf);
                return 0;
            }

            if (id == ID_ZOOM_VALUE || id == ID_ZOOM_X || id == ID_ZOOM_Y)
            {
                if (HIWORD(wp) == EN_CHANGE)
                    InvalidateRect(hwnd, NULL, FALSE);
            }
            break;
        }

        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            paint_zoom_preview(sys, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (sys != NULL)
            {
                RECT wr;
                if (GetWindowRect(hwnd, &wr))
                {
                    save_setting(L"x", wr.left);
                    save_setting(L"y", wr.top);
                }
                save_setting(L"page", sys->page);
                save_setting(L"topmost", checked(hwnd, ID_TOPMOST) ? 1 : 0);
                sys->window = NULL;
                if (sys->credit_font != NULL)
                {
                    DeleteObject(sys->credit_font);
                    sys->credit_font = NULL;
                }
                if (sys->legend_font != NULL)
                {
                    DeleteObject(sys->legend_font);
                    sys->legend_font = NULL;
                }
            }
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void *UiThread(void *opaque)
{
    intf_thread_t *intf = opaque;
    intf_sys_t *sys = intf->p_sys;
    HINSTANCE instance = GetModuleHandleW(NULL);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"VLCXMEyeControlWindow";
    RegisterClassExW(&wc);

    int wx = load_setting(L"x", CW_USEDEFAULT);
    int wy = load_setting(L"y", CW_USEDEFAULT);
    if (wx != CW_USEDEFAULT && wy != CW_USEDEFAULT)
    {
        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (wx < vx - 700 || wx > vx + vw - 80 || wy < vy - 500 || wy > vy + vh - 40)
        {
            wx = CW_USEDEFAULT;
            wy = CW_USEDEFAULT;
        }
    }

    HWND window = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        wc.lpszClassName,
        L"XMEye Control",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        wx, wy, 820, 640,
        NULL, NULL, instance, intf);

    if (window == NULL)
    {
        SetEvent(sys->ready);
        return NULL;
    }

    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0)
    {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return NULL;
}

static int Open(vlc_object_t *obj)
{
    intf_thread_t *intf = (intf_thread_t *)obj;
    intf_sys_t *sys = calloc(1, sizeof(*sys));
    if (sys == NULL)
        return VLC_ENOMEM;

    intf->p_sys = sys;
    sys->ready = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (sys->ready == NULL)
    {
        free(sys);
        return VLC_EGENERIC;
    }

    if (vlc_clone(&sys->thread, UiThread, intf, VLC_THREAD_PRIORITY_LOW))
    {
        CloseHandle(sys->ready);
        free(sys);
        return VLC_ENOMEM;
    }

    WaitForSingleObject(sys->ready, INFINITE);
    CloseHandle(sys->ready);
    sys->ready = NULL;

    if (sys->window == NULL)
    {
        vlc_join(sys->thread, NULL);
        free(sys);
        return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

static void Close(vlc_object_t *obj)
{
    intf_thread_t *intf = (intf_thread_t *)obj;
    intf_sys_t *sys = intf->p_sys;
    if (sys == NULL)
        return;

    if (sys->window != NULL)
        PostMessageW(sys->window, WM_CLOSE, 0, 0);

    vlc_join(sys->thread, NULL);
    free(sys);
}

vlc_module_begin()
    set_shortname(N_("XMEye Control"))
    set_description(N_("Floating XMEye controls for VLC"))
    set_category(CAT_INTERFACE)
    set_subcategory(SUBCAT_INTERFACE_CONTROL)
    set_capability("interface", 0)
    set_callbacks(Open, Close)
    add_shortcut("xmeyecontrol")
vlc_module_end()
