// apo_ui.cpp - SoundLock APO control panel (writes HKLM\SOFTWARE\SoundLock)
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <string>
#include <cstdio>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "advapi32.lib")

enum {
    IDC_A = 201, IDC_B = 202,
    IDC_ENVAMT = 210, IDC_ENVTHR = 211, IDC_STEPAMT = 212, IDC_STEPSENS = 213, IDC_CEIL = 214,
    IDC_APPLY = 220, IDC_INSTALL = 221, IDC_UNINSTALL = 222, IDC_SOUND = 223, IDC_STATUS = 230
};

static HINSTANCE g_inst;
static HWND g_hA, g_hB, g_envAmt, g_envThr, g_stepAmt, g_stepSens, g_ceil;
static HWND g_l1, g_l2, g_l3, g_l4, g_l5, g_status;
static const int kLeft = 18, kW = 520;

static void SetFont(HWND h, HFONT f) { SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE); }

static bool IsElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION e{};
    DWORD sz = sizeof(e);
    bool ok = GetTokenInformation(tok, TokenElevation, &e, sz, &sz) && e.TokenIsElevated;
    CloseHandle(tok);
    return ok;
}

static void SetStatus(const wchar_t* s) { SetWindowTextW(g_status, s); }

static void UpdateLabels() {
    wchar_t b[128];
    int ea = (int)SendMessageW(g_envAmt, TBM_GETPOS, 0, 0);
    int et = (int)SendMessageW(g_envThr, TBM_GETPOS, 0, 0);
    int sa = (int)SendMessageW(g_stepAmt, TBM_GETPOS, 0, 0);
    int ss = (int)SendMessageW(g_stepSens, TBM_GETPOS, 0, 0);
    int ce = (int)SendMessageW(g_ceil, TBM_GETPOS, 0, 0);
    swprintf(b, 128, L"环境抑制   %d %%", ea);                 SetWindowTextW(g_l1, b);
    swprintf(b, 128, L"环境阈值   %d dBFS", -60 + et);        SetWindowTextW(g_l2, b);
    swprintf(b, 128, L"脚步增强   +%d dB", (sa * 12 + 50) / 100); SetWindowTextW(g_l3, b);
    swprintf(b, 128, L"瞬态灵敏度 %d %%", ss);                SetWindowTextW(g_l4, b);
    swprintf(b, 128, L"最大输出   %.1f dBFS", -24.0f + ce / 10.0f); SetWindowTextW(g_l5, b);
}

static void ApplyConfig() {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\SoundLock", 0, nullptr, 0,
                        KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) {
        SetStatus(L"写入失败：需要管理员权限。请右键以管理员身份运行。");
        return;
    }
    auto wr = [&](const wchar_t* n, DWORD v) {
        RegSetValueExW(k, n, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    };
    wr(L"AOn", (DWORD)(SendMessageW(g_hA, BM_GETCHECK, 0, 0) == BST_CHECKED));
    wr(L"BOn", (DWORD)(SendMessageW(g_hB, BM_GETCHECK, 0, 0) == BST_CHECKED));
    wr(L"EnvAmt", (DWORD)SendMessageW(g_envAmt, TBM_GETPOS, 0, 0));
    wr(L"EnvThr", (DWORD)SendMessageW(g_envThr, TBM_GETPOS, 0, 0));
    wr(L"StepAmt", (DWORD)SendMessageW(g_stepAmt, TBM_GETPOS, 0, 0));
    wr(L"StepSens", (DWORD)SendMessageW(g_stepSens, TBM_GETPOS, 0, 0));
    wr(L"Ceil", (DWORD)SendMessageW(g_ceil, TBM_GETPOS, 0, 0));
    RegCloseKey(k);
    SetStatus(L"已保存。参数在下一次音频流启动时生效（切换设备或重新开始播放）。");
}

static void RunElevated(const wchar_t* file, const wchar_t* args, const wchar_t* okMsg) {
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = file;
    sei.lpParameters = args;
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) SetStatus(okMsg);
    else SetStatus(L"操作被取消或需要管理员权限。");
}

static void LoadConfig() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\SoundLock", 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    auto rd = [&](const wchar_t* n, DWORD def) -> DWORD {
        DWORD v = def, sz = sizeof(v), ty = REG_DWORD;
        if (RegQueryValueExW(k, n, nullptr, &ty, (LPBYTE)&v, &sz) != ERROR_SUCCESS) return def;
        return v;
    };
    SendMessageW(g_hA, BM_SETCHECK, rd(L"AOn", 1) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_hB, BM_SETCHECK, rd(L"BOn", 1) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_envAmt, TBM_SETPOS, TRUE, (LPARAM)rd(L"EnvAmt", 55));
    SendMessageW(g_envThr, TBM_SETPOS, TRUE, (LPARAM)rd(L"EnvThr", 18));
    SendMessageW(g_stepAmt, TBM_SETPOS, TRUE, (LPARAM)rd(L"StepAmt", 65));
    SendMessageW(g_stepSens, TBM_SETPOS, TRUE, (LPARAM)rd(L"StepSens", 70));
    SendMessageW(g_ceil, TBM_SETPOS, TRUE, (LPARAM)rd(L"Ceil", 210));
    RegCloseKey(k);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        HFONT f = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        HFONT fb = CreateFontW(-15, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        int y = 12;
        HWND t = CreateWindowExW(0, L"STATIC", L"SoundLock APO  ·  引擎内处理（延迟 ~2 ms）",
                                 WS_CHILD | WS_VISIBLE, kLeft, y, kW, 24, h, nullptr, g_inst, nullptr);
        SetFont(t, fb); y += 34;

        g_hA = CreateWindowExW(0, L"BUTTON", L"① 降低环境音 + 强化脚步声",
                               WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, kLeft, y, kW, 24,
                               h, (HMENU)IDC_A, g_inst, nullptr);
        SetFont(g_hA, fb); SendMessageW(g_hA, BM_SETCHECK, BST_CHECKED, 0); y += 30;

        auto mk = [&](int id, int lo, int hi, int pos, int yy) {
            HWND s = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ,
                                     kLeft + 12, yy, kW - 40, 26, h, (HMENU)id, g_inst, nullptr);
            SendMessageW(s, TBM_SETRANGE, TRUE, MAKELONG(lo, hi));
            SendMessageW(s, TBM_SETPOS, TRUE, pos);
            return s;
        };
        g_l1 = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, kLeft + 12, y, 220, 20, h, nullptr, g_inst, nullptr);
        g_envAmt = mk(IDC_ENVAMT, 0, 100, 55, y + 20); SetFont(g_l1, f); y += 52;
        g_l2 = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, kLeft + 12, y, 220, 20, h, nullptr, g_inst, nullptr);
        g_envThr = mk(IDC_ENVTHR, 0, 42, 18, y + 20); SetFont(g_l2, f); y += 52;
        g_l3 = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, kLeft + 12, y, 220, 20, h, nullptr, g_inst, nullptr);
        g_stepAmt = mk(IDC_STEPAMT, 0, 100, 65, y + 20); SetFont(g_l3, f); y += 52;
        g_l4 = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, kLeft + 12, y, 220, 20, h, nullptr, g_inst, nullptr);
        g_stepSens = mk(IDC_STEPSENS, 0, 100, 70, y + 20); SetFont(g_l4, f); y += 58;

        g_hB = CreateWindowExW(0, L"BUTTON", L"② 锁定最大输出音量（限幅）",
                               WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, kLeft, y, kW, 24,
                               h, (HMENU)IDC_B, g_inst, nullptr);
        SetFont(g_hB, fb); SendMessageW(g_hB, BM_SETCHECK, BST_CHECKED, 0); y += 30;
        g_l5 = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, kLeft + 12, y, 220, 20, h, nullptr, g_inst, nullptr);
        g_ceil = mk(IDC_CEIL, 0, 240, 210, y + 20); SetFont(g_l5, f); y += 56;

        HWND b1 = CreateWindowExW(0, L"BUTTON", L"保存参数", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  kLeft, y, 110, 32, h, (HMENU)IDC_APPLY, g_inst, nullptr);
        SetFont(b1, fb);
        HWND b2 = CreateWindowExW(0, L"BUTTON", L"安装 APO", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  kLeft + 120, y, 110, 32, h, (HMENU)IDC_INSTALL, g_inst, nullptr);
        SetFont(b2, f);
        HWND b3 = CreateWindowExW(0, L"BUTTON", L"卸载 APO", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  kLeft + 240, y, 110, 32, h, (HMENU)IDC_UNINSTALL, g_inst, nullptr);
        SetFont(b3, f);
        HWND b4 = CreateWindowExW(0, L"BUTTON", L"声音设置", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  kLeft + 360, y, 110, 32, h, (HMENU)IDC_SOUND, g_inst, nullptr);
        SetFont(b4, f);
        y += 44;

        g_status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_SIMPLE,
                                   kLeft, y, kW, 22, h, (HMENU)IDC_STATUS, g_inst, nullptr);
        SetFont(g_status, f);
        y += 30;
        HWND hint = CreateWindowExW(0, L"STATIC",
                                    L"APO 装在耳机端点：所有声音都会处理，无需虚拟声卡。",
                                    WS_CHILD | WS_VISIBLE | SS_SIMPLE, kLeft, y, kW, 20,
                                    h, nullptr, g_inst, nullptr);
        SetFont(hint, f);

        LoadConfig();
        UpdateLabels();
        SetStatus(IsElevated() ? L"管理员权限：可保存参数、安装 APO。"
                               : L"当前非管理员：保存/安装会弹 UAC，或右键以管理员身份运行。");
        return 0;
    }
    case WM_HSCROLL:
        UpdateLabels();
        return 0;
    case WM_COMMAND: {
        switch (LOWORD(w)) {
        case IDC_APPLY: ApplyConfig(); break;
        case IDC_INSTALL: {
            wchar_t dir[MAX_PATH];
            GetModuleFileNameW(nullptr, dir, MAX_PATH);
            wchar_t* p = wcsrchr(dir, L'\\');
            if (p) *p = 0;
            std::wstring bat = std::wstring(dir) + L"\\install_apo.bat";
            RunElevated(bat.c_str(), nullptr, L"已启动安装程序，请在弹出的窗口里确认。");
            break;
        }
        case IDC_UNINSTALL: {
            wchar_t dir[MAX_PATH];
            GetModuleFileNameW(nullptr, dir, MAX_PATH);
            wchar_t* p = wcsrchr(dir, L'\\');
            if (p) *p = 0;
            std::wstring bat = std::wstring(dir) + L"\\uninstall_apo.bat";
            RunElevated(bat.c_str(), nullptr, L"已启动卸载程序。");
            break;
        }
        case IDC_SOUND:
            ShellExecuteW(nullptr, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    g_inst = inst;
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"SoundLockApoCtl";
    RegisterClassExW(&wc);

    RECT r{0, 0, kW + kLeft * 2, 620};
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    HWND h = CreateWindowExW(0, L"SoundLockApoCtl", L"SoundLock APO 控制面板",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                             nullptr, nullptr, inst, nullptr);
    if (!h) return 1;
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
