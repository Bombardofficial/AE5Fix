#include "resource.h"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <shellscalingapi.h>
#include <stdio.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shcore.lib")

#define WM_TRAY (WM_APP + 1)
#define WM_FIXDONE (WM_APP + 2)
#define WM_CAPTURED (WM_APP + 3)
#define HOTKEY_ID 1

/* Layout at 96 DPI. Scaled with the window DPI. */
enum {
    UI_W = 300,
    UI_H = 184
};

static HINSTANCE g_inst;
static HWND g_hwnd, g_status, g_key;
static HICON g_icon;
static NOTIFYICONDATAW g_nid;
static HFONT g_font;
static int g_target;
static int g_autostart;
static UINT g_mods, g_vk;
static int g_capturing;
static int g_busy;
static int g_tray;
static int g_layout_lock;
static HHOOK g_hook;
static wchar_t g_dir[MAX_PATH];
static wchar_t g_exe[MAX_PATH];
static wchar_t g_sbz[MAX_PATH];
static wchar_t g_ini[MAX_PATH];

static int dp(int px, UINT dpi) {
    return MulDiv(px, (int)dpi, 96);
}

static void status(const wchar_t *s) {
    if (g_status) SetWindowTextW(g_status, s);
}

static void ini_load(void) {
    g_target = GetPrivateProfileIntW(L"ae5", L"target", 0, g_ini);
    g_autostart = GetPrivateProfileIntW(L"ae5", L"autostart", 0, g_ini);
    g_mods = GetPrivateProfileIntW(L"ae5", L"mods", 0, g_ini);
    g_vk = GetPrivateProfileIntW(L"ae5", L"vk", VK_F10, g_ini);
    if (g_target != 0 && g_target != 1) g_target = 0;
    if (!g_vk) g_vk = VK_F10;
}

static void ini_save(void) {
    wchar_t b[32];
    swprintf_s(b, 32, L"%d", g_target);
    WritePrivateProfileStringW(L"ae5", L"target", b, g_ini);
    swprintf_s(b, 32, L"%d", g_autostart);
    WritePrivateProfileStringW(L"ae5", L"autostart", b, g_ini);
    swprintf_s(b, 32, L"%u", g_mods);
    WritePrivateProfileStringW(L"ae5", L"mods", b, g_ini);
    swprintf_s(b, 32, L"%u", g_vk);
    WritePrivateProfileStringW(L"ae5", L"vk", b, g_ini);
}

static void key_text(wchar_t *out, int n) {
    wchar_t name[64];
    UINT scan = MapVirtualKeyW(g_vk, MAPVK_VK_TO_VSC);
    LONG lp = (LONG)(scan << 16);
    if (g_vk == VK_LEFT || g_vk == VK_RIGHT || g_vk == VK_UP || g_vk == VK_DOWN ||
        g_vk == VK_PRIOR || g_vk == VK_NEXT || g_vk == VK_END || g_vk == VK_HOME ||
        g_vk == VK_INSERT || g_vk == VK_DELETE || g_vk == VK_DIVIDE || g_vk == VK_NUMLOCK)
        lp |= 1 << 24;
    if (!GetKeyNameTextW(lp, name, 64) || !name[0])
        swprintf_s(name, 64, L"VK%u", g_vk);
    swprintf_s(out, n, L"%s%s%s%s%s",
               (g_mods & MOD_CONTROL) ? L"Ctrl+" : L"",
               (g_mods & MOD_ALT) ? L"Alt+" : L"",
               (g_mods & MOD_SHIFT) ? L"Shift+" : L"",
               (g_mods & MOD_WIN) ? L"Win+" : L"",
               name);
}

static void show_key(void) {
    wchar_t t[128];
    key_text(t, 128);
    if (g_key) SetWindowTextW(g_key, t);
}

static void apply_autostart(void) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    if (g_autostart) {
        wchar_t cmd[MAX_PATH + 8];
        swprintf_s(cmd, MAX_PATH + 8, L"\"%s\"", g_exe);
        RegSetValueExW(key, L"AE5Fix", 0, REG_SZ, (BYTE *)cmd,
                       (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, L"AE5Fix");
    }
    RegCloseKey(key);
}

static void refresh_idle(void) {
    if (GetFileAttributesW(g_sbz) == INVALID_FILE_ATTRIBUTES)
        status(L"sbz-switch.exe not found");
    else
        status(L"Ready");
}

static void register_hotkey(void) {
    UnregisterHotKey(g_hwnd, HOTKEY_ID);
    if (!RegisterHotKey(g_hwnd, HOTKEY_ID, g_mods | MOD_NOREPEAT, g_vk))
        status(L"Shortcut already in use");
    else if (!g_capturing)
        refresh_idle();
}

static int run_one(int output) {
    wchar_t cmd[1200];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code;
    swprintf_s(cmd, 1200, L"\"%s\" set -i \"Device Control\" SelectOutput %d", g_sbz, output);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, g_dir, &si, &pi))
        return 0;
    if (WaitForSingleObject(pi.hProcess, 8000) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        code = 1;
    } else if (!GetExitCodeProcess(pi.hProcess, &code)) {
        code = 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

static DWORD WINAPI fix_thread(LPVOID unused) {
    int other, ok;
    (void)unused;
    other = g_target == 0 ? 1 : 0;
    ok = run_one(other);
    Sleep(700);
    if (ok) ok = run_one(g_target);
    PostMessageW(g_hwnd, WM_FIXDONE, ok, 0);
    return 0;
}

static void set_busy(int busy) {
    g_busy = busy;
    EnableWindow(GetDlgItem(g_hwnd, IDC_TEST), busy ? FALSE : TRUE);
}

static void start_fix(void) {
    HANDLE t;
    if (g_busy) return;
    if (GetFileAttributesW(g_sbz) == INVALID_FILE_ATTRIBUTES) {
        status(L"sbz-switch.exe not found");
        return;
    }
    set_busy(1);
    status(L"Switching...");
    t = CreateThread(NULL, 0, fix_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else set_busy(0);
}

static void add_tray(void) {
    if (g_tray) return;
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_icon;
    wcscpy_s(g_nid.szTip, sizeof(g_nid.szTip) / sizeof(wchar_t), L"AE-5 Fix");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_tray = 1;
}

static void hide_to_tray(void) {
    add_tray();
    ShowWindow(g_hwnd, SW_HIDE);
}

static void show_main(void) {
    ShowWindow(g_hwnd, SW_SHOW);
    ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
}

static void tray_menu(void) {
    HMENU m = CreatePopupMenu();
    POINT p;
    AppendMenuW(m, MF_STRING, ID_OPEN, L"Open");
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    GetCursorPos(&p);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, p.x, p.y, 0, g_hwnd, NULL);
    DestroyMenu(m);
}

static int is_modifier(UINT vk) {
    return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
           vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
           vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
           vk == VK_LWIN || vk == VK_RWIN;
}

static LRESULT CALLBACK llhook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_capturing && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {
        KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lp;
        if (k->vkCode == VK_ESCAPE || !is_modifier(k->vkCode)) {
            PostMessageW(g_hwnd, WM_CAPTURED, k->vkCode, 0);
            return 1;
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

static void capture_begin(void) {
    g_capturing = 1;
    if (!g_hook) g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, llhook, GetModuleHandleW(NULL), 0);
    status(L"Press a key. Esc cancels.");
}

static void capture_end(UINT vk) {
    g_capturing = 0;
    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = NULL;
    }
    if (vk == VK_ESCAPE || is_modifier(vk)) {
        register_hotkey();
        return;
    }
    g_mods = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) g_mods |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_MENU) & 0x8000) g_mods |= MOD_ALT;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) g_mods |= MOD_SHIFT;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000))
        g_mods |= MOD_WIN;
    g_vk = vk;
    show_key();
    ini_save();
    register_hotkey();
}

static BOOL CALLBACK set_font(HWND child, LPARAM lp) {
    SendMessageW(child, WM_SETFONT, (WPARAM)lp, TRUE);
    return TRUE;
}

static void use_font(HWND hwnd, UINT dpi) {
    HFONT next = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    SendMessageW(hwnd, WM_SETFONT, (WPARAM)next, TRUE);
    EnumChildWindows(hwnd, set_font, (LPARAM)next);
    if (g_font) DeleteObject(g_font);
    g_font = next;
}

static void move_ctl(HWND hwnd, int id, UINT dpi, int x, int y, int w, int h) {
    SetWindowPos(GetDlgItem(hwnd, id), NULL, dp(x, dpi), dp(y, dpi), dp(w, dpi), dp(h, dpi),
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

static void relayout(HWND hwnd, UINT dpi, int center, const RECT *suggested) {
    RECT r, work;
    DWORD style, ex;
    int ww, wh, x, y;
    HMONITOR mon;
    MONITORINFO mi;

    if (g_layout_lock) return;
    g_layout_lock = 1;

    use_font(hwnd, dpi);
    move_ctl(hwnd, IDC_LBL_OUT, dpi, 16, 14, 268, 18);
    move_ctl(hwnd, IDC_HP, dpi, 16, 36, 124, 22);
    move_ctl(hwnd, IDC_SP, dpi, 160, 36, 124, 22);
    move_ctl(hwnd, IDC_LBL_KEY, dpi, 16, 70, 56, 28);
    move_ctl(hwnd, IDC_KEY, dpi, 76, 70, 118, 28);
    move_ctl(hwnd, IDC_SET, dpi, 202, 70, 82, 28);
    move_ctl(hwnd, IDC_AUTO, dpi, 16, 110, 268, 22);
    move_ctl(hwnd, IDC_TEST, dpi, 16, 144, 88, 28);
    move_ctl(hwnd, IDC_STATUS, dpi, 112, 144, 172, 28);

    r.left = 0;
    r.top = 0;
    r.right = dp(UI_W, dpi);
    r.bottom = dp(UI_H, dpi);
    style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
    ex = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    AdjustWindowRectExForDpi(&r, style, FALSE, ex, dpi);
    ww = r.right - r.left;
    wh = r.bottom - r.top;

    if (!center && suggested) {
        x = suggested->left;
        y = suggested->top;
    } else {
        POINT pt;
        GetCursorPos(&pt);
        mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(mon, &mi);
        work = mi.rcWork;
        x = work.left + (work.right - work.left - ww) / 2;
        y = work.top + (work.bottom - work.top - wh) / 2;
    }
    SetWindowPos(hwnd, NULL, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(hwnd, NULL, TRUE);
    g_layout_lock = 0;
}

static HICON load_icon(int metric, UINT dpi) {
    int n = GetSystemMetricsForDpi(metric, dpi);
    return (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, n, n, LR_SHARED);
}

static INT_PTR CALLBACK dlgproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        UINT dpi;
        g_hwnd = hwnd;
        g_status = GetDlgItem(hwnd, IDC_STATUS);
        g_key = GetDlgItem(hwnd, IDC_KEY);
        {
            POINT pt;
            HMONITOR mon;
            UINT dpi_y = 96;
            GetCursorPos(&pt);
            mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
            dpi = 96;
            if (GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi, &dpi_y) != S_OK)
                dpi = GetDpiForWindow(hwnd);
        }
        g_icon = load_icon(SM_CXSMICON, dpi);
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)load_icon(SM_CXICON, dpi));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_icon);
        CheckRadioButton(hwnd, IDC_HP, IDC_SP, g_target == 0 ? IDC_HP : IDC_SP);
        if (g_autostart) CheckDlgButton(hwnd, IDC_AUTO, BST_CHECKED);
        show_key();
        apply_autostart();
        relayout(hwnd, dpi, 1, NULL);
        add_tray();
        register_hotkey();
        return TRUE;
    }
    case WM_DPICHANGED:
        relayout(hwnd, HIWORD(wp), 0, (RECT *)lp);
        g_icon = load_icon(SM_CXSMICON, HIWORD(wp));
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)load_icon(SM_CXICON, HIWORD(wp)));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_icon);
        if (g_tray) {
            g_nid.hIcon = g_icon;
            Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        }
        return TRUE;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_HP && HIWORD(wp) == BN_CLICKED) {
            g_target = 0;
            ini_save();
        } else if (id == IDC_SP && HIWORD(wp) == BN_CLICKED) {
            g_target = 1;
            ini_save();
        } else if (id == IDC_AUTO && HIWORD(wp) == BN_CLICKED) {
            g_autostart = SendMessageW(GetDlgItem(hwnd, IDC_AUTO), BM_GETCHECK, 0, 0) == BST_CHECKED;
            apply_autostart();
            ini_save();
        } else if (id == IDC_SET && HIWORD(wp) == BN_CLICKED) {
            if (!g_capturing) capture_begin();
        } else if (id == IDC_TEST && HIWORD(wp) == BN_CLICKED) {
            start_fix();
        } else if (id == ID_OPEN) {
            show_main();
        } else if (id == ID_EXIT) {
            DestroyWindow(hwnd);
        }
        return TRUE;
    }
    case WM_HOTKEY:
        if (!g_capturing) start_fix();
        return TRUE;
    case WM_CAPTURED:
        capture_end((UINT)wp);
        return TRUE;
    case WM_FIXDONE:
        set_busy(0);
        status(wp ? L"Restored" : L"Couldn't switch output");
        return TRUE;
    case WM_TRAY:
        if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) show_main();
        else if (lp == WM_RBUTTONUP) tray_menu();
        return TRUE;
    case WM_CLOSE:
        hide_to_tray();
        return TRUE;
    case WM_DESTROY:
        UnregisterHotKey(hwnd, HOTKEY_ID);
        if (g_hook) UnhookWindowsHookEx(g_hook);
        if (g_tray) Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_font) DeleteObject(g_font);
        g_font = NULL;
        PostQuitMessage(0);
        return TRUE;
    }
    return FALSE;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show) {
    INITCOMMONCONTROLSEX icc;
    HWND hwnd;
    MSG m;
    HANDLE mutex;
    (void)prev;
    (void)cmd;
    (void)show;

    mutex = CreateMutexW(NULL, FALSE, L"AE5FixSingleton");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"#32770", L"AE-5 Fix");
        if (other) {
            ShowWindow(other, SW_SHOW);
            ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        CloseHandle(mutex);
        return 0;
    }

    g_inst = inst;
    GetModuleFileNameW(NULL, g_exe, MAX_PATH);
    wcscpy_s(g_dir, MAX_PATH, g_exe);
    {
        wchar_t *slash = wcsrchr(g_dir, L'\\');
        if (slash) *slash = 0;
    }
    swprintf_s(g_sbz, MAX_PATH, L"%s\\sbz-switch.exe", g_dir);
    swprintf_s(g_ini, MAX_PATH, L"%s\\ae5fix.ini", g_dir);
    ini_load();

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    hwnd = CreateDialogW(inst, MAKEINTRESOURCEW(IDD_MAIN), NULL, dlgproc);
    if (!hwnd) {
        CloseHandle(mutex);
        return 0;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    while (GetMessageW(&m, NULL, 0, 0)) {
        if (!IsWindow(hwnd) || !IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    CloseHandle(mutex);
    return 0;
}
