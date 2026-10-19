// SoundLatch - PUBG audio post-processor (WASAPI realtime)
// Tray app: starts hidden in the notification area, a compact popup on click,
// full settings window behind the gear icon. Follows the system light/dark theme.
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include "dsp.h"
#include "engine.h"

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")

using namespace sl;

#define WM_TRAYICON (WM_APP + 1)
#define WM_SHOWPOPUP (WM_APP + 2)
#define IDI_APP 1
#define IDM_POPUP   1
#define IDM_QUIT    3

// ----------------------------------------------------------------- ids
enum {
    IDC_A = 101, IDC_B = 102,
    IDC_ENVAMT = 110, IDC_ENVTHR = 111, IDC_STEPAMT = 112, IDC_STEPSENS = 113,
    IDC_CEIL = 114,
    IDC_CAP = 120, IDC_REN = 121, IDC_LOOPBACK = 122, IDC_RESET = 124, IDC_AUTOSTART = 125, IDC_PIN = 126,
    IDC_METER = 130
};

static HINSTANCE g_hinst;
static HWND g_hwnd;                  // 隐藏的消息窗口（托盘/定时器宿主）
static HWND g_popup;                 // 唯一可见的界面：托盘弹窗
// 弹窗锚点：最近一次点击托盘图标的位置（右下角对齐它），没点过就退回到图标矩形
static POINT g_anchor{};
static bool  g_anchorValid = false;
static bool  g_popupShown = false;          // 面板是否处于展开状态（唯一状态源）
static ULONGLONG g_autoHideTick = 0;        // 因失焦自动收起的时刻
static ULONGLONG g_showTick = 0;           // 最近一次显示面板的时刻
static HWND g_hA, g_hB, g_hEnvAmt, g_hEnvThr, g_hStepAmt, g_hStepSens, g_hCeil;
static HWND g_hCap, g_hRen, g_hLoop, g_hReset, g_hMeter, g_hAuto, g_hPin;
static HWND g_hQ1, g_hQ2, g_tip;   // 问号说明 + 悬浮提示
static bool g_pinned = false;   // 固定到屏幕：失焦也不收起
static HWND g_hLblEnvAmt, g_hLblEnvThr, g_hLblStepAmt, g_hLblStepSens, g_hLblCeil;

static DspCore g_dsp;
static DspParams g_params;
static AudioEngine g_engine;
static std::vector<DeviceInfo> g_caps, g_rens;
static bool g_running = false;
static bool g_quit = false;

static void slLog(const wchar_t* fmt, ...);
static void HidePopup();
// 电平表（0..1 = 标尺比例，见 kMeterMinDb/kMeterMaxDb）
static float g_meterLevel = 0.f;    // 平滑后的输入电平

static HFONT g_fTitle, g_fLabel, g_fValue, g_fBtn, g_fSmall;
static HFONT g_fIcon;      // 图标字体（Segoe Fluent Icons / MDL2），用于恢复默认按钮
static HFONT g_pTitle, g_pLabel, g_pSmall, g_pBtn;   // popup (scaled)
static HICON g_icoBig = nullptr, g_icoSmall = nullptr;
static UINT  g_taskbarCreated = 0;

// ----------------------------------------------------------------- theme
// 深浅色下所有控件/文字都由系统绘制，代码只负责：
//   1) 打开 uxtheme 的 ForceDark（让原生控件变深色）
//   2) 自绘部分（电平表、静态标签底色、滑杆）跟随 g_th
//   3) 主题切换时刷新
struct Theme {
    bool light;
    COLORREF bg, card, text, sub, track, accent, border, green, amber, red, field;
};
static Theme g_th;
static HBRUSH g_brBg = nullptr;        // 窗口客户区底色
static HBRUSH g_brCard = nullptr;      // 标签底色（与 bg 同色，让静态控件不显突兀）
static HBRUSH g_brField = nullptr;     // 输入类控件（下拉框编辑框/列表）底色

static bool ReadThemeFlag(const wchar_t* name, bool def) {
    HKEY k = nullptr;
    bool light = def;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD v = 0, sz = sizeof(v), ty = 0;
        if (RegQueryValueExW(k, name, nullptr, &ty, (LPBYTE)&v, &sz) == ERROR_SUCCESS)
            light = v != 0;
        RegCloseKey(k);
    }
    return light;
}
static bool SystemIsLight() {   // 任务栏 / 系统主题
    return ReadThemeFlag(L"SystemUsesLightTheme", true);
}
static bool AppsIsLight() {     // 应用主题
    return ReadThemeFlag(L"AppsUseLightTheme", true);
}

static void MakeTheme() {
    g_th.light = AppsIsLight();
    if (g_th.light) {
        g_th.bg     = RGB(243, 243, 243);
        g_th.card   = RGB(243, 243, 243);
        g_th.text   = RGB(32, 32, 32);
        g_th.sub    = RGB(96, 96, 96);
        g_th.track  = RGB(120, 120, 120);
        g_th.accent = RGB(0, 120, 215);
        g_th.border = RGB(200, 200, 200);
        g_th.field  = RGB(255, 255, 255);
        g_th.green  = RGB(16, 124, 16);
        g_th.amber  = RGB(197, 89, 17);
        g_th.red    = RGB(196, 43, 28);
    } else {
        g_th.bg     = RGB(32, 32, 32);
        g_th.card   = RGB(32, 32, 32);
        g_th.text   = RGB(255, 255, 255);
        g_th.sub    = RGB(200, 200, 200);
        g_th.track  = RGB(130, 130, 130);
        g_th.accent = RGB(0, 120, 215);
        g_th.border = RGB(80, 80, 80);
        g_th.field  = RGB(51, 51, 51);
        g_th.green  = RGB(0, 200, 90);
        g_th.amber  = RGB(240, 180, 60);
        g_th.red    = RGB(240, 90, 80);
    }
    if (g_brBg) DeleteObject(g_brBg);
    if (g_brCard) DeleteObject(g_brCard);
    if (g_brField) DeleteObject(g_brField);
    g_brBg = CreateSolidBrush(g_th.bg);
    g_brCard = CreateSolidBrush(g_th.card);
    g_brField = CreateSolidBrush(g_th.field);
}

// WM_CTLCOLORBTN / WM_CTLCOLOREDIT / WM_CTLCOLORLISTBOX 统一上色：
// 按钮文字/勾选框文字用窗口底色；下拉框编辑区与下拉列表用 field 底色。
static LRESULT DarkCtlColor(HDC dc, UINT msg) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, g_th.text);
    if (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX) {
        SetBkColor(dc, g_th.field);
        return (LRESULT)g_brField;
    }
    SetBkColor(dc, g_th.bg);
    return (LRESULT)g_brBg;
}

// 通知 DWM 该窗口走深色（原生控件主题由系统绘制，必须靠这个属性才会变深色）
static void ApplyDarkMode(HWND h) {
    if (!h || !IsWindow(h)) return;
    BOOL dark = AppsIsLight() ? FALSE : TRUE;
    HRESULT h1 = DwmSetWindowAttribute(h, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    if (FAILED(h1)) DwmSetWindowAttribute(h, 19, &dark, sizeof(dark));
    // 1903+（尤其 24H2/25H2）AllowDarkModeForWindow(133) 已失效，
    // 正确做法是用 SetWindowCompositionAttribute + WCA_USEDARKMODECOLORS(26)
    using fnSCA = BOOL(WINAPI*)(HWND, void*);
    static fnSCA pfn = (fnSCA)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                             "SetWindowCompositionAttribute");
    if (pfn) {
        struct { DWORD a; PVOID p; SIZE_T c; } d{ 26 /*WCA_USEDARKMODECOLORS*/, &dark, sizeof(dark) };
        pfn(h, &d);
    }
}

// ComCtl32 v6 的控件由系统绘制，DWMWA 属性只管标题栏。
// 25H2 实测有效链路：
//   SetPreferredAppMode(AllowDark)                uxtheme 全局策略（含 MessageBox/菜单）
//   DWMWA_USE_IMMERSIVE_DARK_MODE + WCA(26)      每个顶层窗口
//   SetWindowTheme("Explorer") / ("CFD")         逐个 Button / ComboBox+Edit
//   WM_CTLCOLORSTATIC 返回不透明深色刷           复选框底色的关键（它发的不是 BTN！）
//   WM_CTLCOLORBTN/EDIT/LISTBOX                  按钮与下拉框文字/背景
//   FlushMenuThemes + RefreshImmersiveColorPolicyState  策略/菜单刷新
// 序数：135=SetPreferredAppMode  133=AllowDarkModeForWindow(25H2 已失效)
//       136=FlushMenuThemes  104=RefreshImmersiveColorPolicyState
static bool g_uxInit = false;
static HRESULT (WINAPI* g_pfnPreferredAppMode)(int) = nullptr;
static bool (WINAPI* g_pfnAllowDarkForWindow)(HWND, bool) = nullptr;
static void (WINAPI* g_pfnFlushMenuThemes)() = nullptr;
static void (WINAPI* g_pfnRefreshPolicy)() = nullptr;

static void InitUxThemeApi() {
    if (g_uxInit) return;
    g_uxInit = true;
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    if (!ux) return;
    g_pfnPreferredAppMode   = (HRESULT(WINAPI*)(int))GetProcAddress(ux, (LPCSTR)135);
    g_pfnAllowDarkForWindow = (bool(WINAPI*)(HWND, bool))GetProcAddress(ux, (LPCSTR)133);
    g_pfnFlushMenuThemes    = (void(WINAPI*)())GetProcAddress(ux, (LPCSTR)136);
    g_pfnRefreshPolicy      = (void(WINAPI*)())GetProcAddress(ux, (LPCSTR)104);
}

// 全局策略：0=Default(跟随系统) 1=AllowDark(允许深色，跟随系统) 2=ForceDark。
// 调用后必须刷新策略与菜单缓存。
static void SetAppDarkMode(bool dark) {
    InitUxThemeApi();
    if (g_pfnPreferredAppMode) g_pfnPreferredAppMode(dark ? 1 : 0);
    if (g_pfnRefreshPolicy) g_pfnRefreshPolicy();
    if (g_pfnFlushMenuThemes) g_pfnFlushMenuThemes();
}

// ---- 逐控件深色主题（win32-darkmode 标准做法，非自绘）----------------
// 按钮/复选框：主题类仍为 "Explorer"，靠 AllowDarkModeForWindow(控件) 切深浅
// 下拉框/编辑框：深色主题类为 "CFD"
static void ApplyControlTheme(HWND h, bool dark) {
    wchar_t cls[64] = L"";
    if (!GetClassNameW(h, cls, 64)) return;
    if (!_wcsicmp(cls, L"Button")) {
        SetWindowTheme(h, L"Explorer", nullptr);
        if (g_pfnAllowDarkForWindow) g_pfnAllowDarkForWindow(h, dark);
        SendMessageW(h, WM_THEMECHANGED, 0, 0);
    } else if (!_wcsicmp(cls, L"ComboBox") || !_wcsicmp(cls, L"Edit")) {
        SetWindowTheme(h, dark ? L"CFD" : nullptr, nullptr);
        if (g_pfnAllowDarkForWindow) g_pfnAllowDarkForWindow(h, dark);
        SendMessageW(h, WM_THEMECHANGED, 0, 0);
    } else return;
    // trackbar 走 NM_CUSTOMDRAW 自绘，无需主题类
}

static BOOL CALLBACK ControlThemeEnum(HWND h, LPARAM lp) {
    ApplyControlTheme(h, lp != 0);
    return TRUE;
}

static void SyncDarkMode(HWND h) {
    InitUxThemeApi();
    bool dark = !AppsIsLight();
    SetAppDarkMode(dark);
    if (h && IsWindow(h)) {
        ApplyDarkMode(h);
        if (g_pfnAllowDarkForWindow) g_pfnAllowDarkForWindow(h, dark);
        EnumChildWindows(h, ControlThemeEnum, (LPARAM)(dark ? 1 : 0));
        RedrawWindow(h, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    }
    slLog(L"theme: dark=%d api=%d hwnd=%p", dark,
          (g_pfnPreferredAppMode && g_pfnAllowDarkForWindow) ? 1 : 0, (void*)h);
}

// 提示气泡跟随主题：走和面板同一套暗色 API，再覆盖底色/字色
static void ThemeTooltip() {
    if (!g_tip || !IsWindow(g_tip)) return;
    InitUxThemeApi();
    bool dark = !AppsIsLight();
    SetAppDarkMode(dark);
    ApplyDarkMode(g_tip);
    if (g_pfnAllowDarkForWindow) g_pfnAllowDarkForWindow(g_tip, dark);
    SendMessageW(g_tip, TTM_SETTIPBKCOLOR, (WPARAM)g_th.bg, 0);
    SendMessageW(g_tip, TTM_SETTIPTEXTCOLOR, (WPARAM)g_th.text, 0);
}

static void RefreshTheme(HWND h) {
    MakeTheme();
    SyncDarkMode(h);
    ThemeTooltip();                  // 提示气泡也要跟着主题换
    if (IsWindow(h)) {
        // 类背景刷也要跟着主题换，否则系统垫底永远是浅色
        SetClassLongPtrW(h, GCLP_HBRBACKGROUND, (LONG_PTR)g_brBg);
        RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
}

// ----------------------------------------------------------------- layout units
static float g_scale = 1.0f;
static int CALLBACK FontEnumCb(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lp) {
    *(bool*)lp = true;
    return 0;
}
static bool FontExists(const wchar_t* face) {
    HDC dc = GetDC(nullptr);
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, face);
    bool found = false;
    EnumFontFamiliesExW(dc, &lf, FontEnumCb, (LPARAM)&found, 0);
    ReleaseDC(nullptr, dc);
    return found;
}
static inline int S(int v) { return (int)(v * g_scale + 0.5f); }
static inline RECT R4(int x, int y, int w, int h) {
    RECT r{ S(x), S(y), S(x + w), S(y + h) };
    return r;
}

// ----------------------------------------------------------------- helpers
static void slLog(const wchar_t* fmt, ...) {
    static HANDLE f = nullptr;
    if (!f) {
        wchar_t p[MAX_PATH];
        GetTempPathW(MAX_PATH, p);
        wcscat_s(p, L"soundlatch_theme.log");
        f = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (!f || f == INVALID_HANDLE_VALUE) return;
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vswprintf(buf, 512, fmt, ap);
    va_end(ap);
    if (n > 0) {
        DWORD wr;
        WriteFile(f, buf, (DWORD)wcslen(buf) * 2, &wr, nullptr);
        WriteFile(f, L"\r\n", 4, &wr, nullptr);
    }
}
static void SetFont(HWND h, HFONT f) { SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE); }

static void FillR(HDC dc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}
static void FillRound(HDC dc, RECT r, int rad, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, S(rad), S(rad));
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
}
static void StrokeRound(HDC dc, RECT r, int rad, COLORREF line) {
    HBRUSH b = (HBRUSH)GetStockObject(NULL_BRUSH);
    HPEN p = CreatePen(PS_SOLID, 1, line);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, S(rad), S(rad));
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(p);
}
static void Txt(HDC dc, const wchar_t* s, RECT r, COLORREF c, HFONT f, UINT fmt) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    HGDIOBJ old = SelectObject(dc, f);
    DrawTextW(dc, s, -1, &r, fmt | DT_NOPREFIX);
    SelectObject(dc, old);
}

static HWND SliderCtl(int i) {
    switch (i) {
    case 0: return g_hEnvAmt;
    case 1: return g_hEnvThr;
    case 2: return g_hStepAmt;
    case 3: return g_hStepSens;
    default: return g_hCeil;
    }
}
static int SliderPos(int i) { return (int)SendMessageW(SliderCtl(i), TBM_GETPOS, 0, 0); }
static void SliderSet(int i, int v) { SendMessageW(SliderCtl(i), TBM_SETPOS, TRUE, v); }

// ---- 电平表 / 限幅阈值共用的一条 dB 标尺 ----
// 柱形与游标都用它换算：左端 -18 dBFS，右端 0 dBFS（数字满刻度）。
// 左端取 -18 而不是更低，是为了和「最大输出」滑杆的域完全一致：
// 滑杆 1% = 柱形最左 = -18 dBFS，100% = 柱形最右 = 0 dBFS，拖到哪游标就落在哪。
static constexpr float kMeterMinDb = -18.0f;
static constexpr float kMeterMaxDb =   0.0f;

// ---- 限幅阈值（最大输出）滑杆的取值域 ----
// 语义：这就是"当前允许的最大输出音量"——高于它的峰值被压到这条线，低于它的原样通过。
//   位置 0       = 静音（真·0 输出）
//   位置 1..1000 = -18 .. 0 dBFS（1% = -18 dBFS，100% = 0 dBFS 不限制）
// 下限只到 -18 dBFS：再低就只剩"整体变小"，听感差异用系统扬声器音量调更直观。
static constexpr float kCeilMuteDb = -120.0f;                   // 位置 0 用的静音值
static constexpr float kCeilMinDb  = -18.0f;
static constexpr int   kCeilPosMax = 1000;
static inline float CeilPosToDb(int pos) {
    if (pos <= 0) return kCeilMuteDb;
    float db = kCeilMinDb + (float)(pos - 1) / (float)(kCeilPosMax - 1) * (0.0f - kCeilMinDb);
    return roundf(db * 10.0f) / 10.0f;                          // 量化到 0.1 dB
}
static inline int CeilDbToPos(float db) {
    if (db <= kCeilMuteDb + 1.0f) return 0;
    int p = (int)lroundf((db - kCeilMinDb) / (0.0f - kCeilMinDb) * (float)(kCeilPosMax - 1) + 1.0f);
    return p < 1 ? 1 : (p > kCeilPosMax ? kCeilPosMax : p);
}

static void FormatValue(int i, wchar_t* b, int n) {
    int p = SliderPos(i);
    switch (i) {
    case 0: swprintf(b, n, L"%d%%", p); break;
    case 1: swprintf(b, n, L"%d dBFS", -60 + p); break;
    case 2: swprintf(b, n, L"+%d dB", (p * 12 + 50) / 100); break;
    case 3: swprintf(b, n, L"%d%%", p); break;
    case 4:
        if (p <= 0) swprintf(b, n, L"静音");
        else {
            int pct = (p < 10) ? 1 : (p + 5) / 10;   // 位置 1 就是 1%（= -18 dBFS），别显示成 0%
            swprintf(b, n, L"%d%%", pct);
        }
        break;
    }
}

static std::wstring AppDataPath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::wstring p = (n && n < MAX_PATH) ? std::wstring(buf) : std::wstring(L".");
    p += L"\\SoundLatch";
    CreateDirectoryW(p.c_str(), nullptr);
    return p + L"\\config.ini";
}

// 参数快照：PushParams 里随时更新。写配置只读它，不依赖控件是否还活着
// （弹窗是主窗口的 owned window，退出时会被先拆掉，那时再去问控件只能得到 0）。
static struct SavedState {
    bool valid = false;
    int  a = 1, b = 1;
    int  envAmt = 55, envThr = 18, stepAmt = 65, stepSens = 70, ceil = 950;
    int  capSel = -1, renSel = -1;
    int  loop = 0;
    int  pin = 0;
} g_saved;

static void SaveConfig() {
    if (!g_saved.valid) return;
    FILE* f = _wfopen(AppDataPath().c_str(), L"w");
    if (!f) return;
    fprintf(f, "a_on=%d\nb_on=%d\nenv_amt=%d\nenv_thr=%d\nstep_amt=%d\nstep_sens=%d\nceil=%d\n",
            g_saved.a, g_saved.b, g_saved.envAmt, g_saved.envThr,
            g_saved.stepAmt, g_saved.stepSens, g_saved.ceil);
    if (g_saved.capSel >= 0) fprintf(f, "cap_sel=%d\n", g_saved.capSel);
    if (g_saved.renSel >= 0) fprintf(f, "ren_sel=%d\n", g_saved.renSel);
    fprintf(f, "loopback=%d\n", g_saved.loop);
    fprintf(f, "pin=%d\n", g_saved.pin);
    fclose(f);
}

static void LoadConfig() {
    FILE* f = _wfopen(AppDataPath().c_str(), L"r");
    if (!f) return;
    char line[256];
    int capSel = -1, renSel = -1;
    while (fgets(line, 256, f)) {
        char k[64]; int v = 0;
        if (sscanf(line, "%63[^=]=%d", k, &v) != 2) continue;
        if (!strcmp(k, "a_on")) SendMessageW(g_hA, BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0);
        else if (!strcmp(k, "b_on")) SendMessageW(g_hB, BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0);
        else if (!strcmp(k, "env_amt")) SliderSet(0, v);
        else if (!strcmp(k, "env_thr")) SliderSet(1, v);
        else if (!strcmp(k, "step_amt")) SliderSet(2, v);
        else if (!strcmp(k, "step_sens")) SliderSet(3, v);
        else if (!strcmp(k, "ceil")) SliderSet(4, v);
        else if (!strcmp(k, "cap_sel")) capSel = v;
        else if (!strcmp(k, "ren_sel")) renSel = v;
        else if (!strcmp(k, "loopback")) SendMessageW(g_hLoop, BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0);
        else if (!strcmp(k, "pin")) { g_pinned = (v != 0); g_saved.pin = v ? 1 : 0; }
    }
    fclose(f);
    if (capSel >= 0 && capSel < (int)g_caps.size()) SendMessageW(g_hCap, CB_SETCURSEL, capSel, 0);
    if (renSel >= 0 && renSel < (int)g_rens.size()) SendMessageW(g_hRen, CB_SETCURSEL, renSel, 0);
}

static void UpdateLabels() {
    HWND* lbl[5] = { &g_hLblEnvAmt, &g_hLblEnvThr, &g_hLblStepAmt, &g_hLblStepSens, &g_hLblCeil };
    const wchar_t* names[5] = { L"环境抑制", L"环境阈值", L"脚步增强", L"瞬态灵敏度", L"最大输出" };
    wchar_t b[64];
    for (int i = 0; i < 5; ++i) {
        FormatValue(i, b, 64);
        std::wstring t = std::wstring(names[i]) + L"   " + b;
        SetWindowTextW(*lbl[i], t.c_str());
    }
}

static void PushParams() {
    g_saved.valid    = true;
    g_saved.a        = (int)SendMessageW(g_hA, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_saved.b        = (int)SendMessageW(g_hB, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_saved.loop     = (int)SendMessageW(g_hLoop, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_saved.envAmt   = SliderPos(0);
    g_saved.envThr   = SliderPos(1);
    g_saved.stepAmt  = SliderPos(2);
    g_saved.stepSens = SliderPos(3);
    g_saved.ceil     = SliderPos(4);
    {
        int ci = (int)SendMessageW(g_hCap, CB_GETCURSEL, 0, 0);
        int ri = (int)SendMessageW(g_hRen, CB_GETCURSEL, 0, 0);
        if (ci != CB_ERR) g_saved.capSel = ci;
        if (ri != CB_ERR) g_saved.renSel = ri;
    }

    g_params.aOn      = SendMessageW(g_hA, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_params.bOn      = SendMessageW(g_hB, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_params.envAmt   = SliderPos(0) / 100.0f;
    g_params.envThrDb = -60.0f + (float)SliderPos(1);
    g_params.stepAmt  = SliderPos(2) / 100.0f;
    g_params.stepSens = SliderPos(3) / 100.0f;
    g_params.ceilDb   = CeilPosToDb(SliderPos(4));
    g_dsp.SetParams(g_params);
    UpdateLabels();
    if (g_popup) InvalidateRect(g_popup, nullptr, FALSE);
}

// ---- 开机自启：写/删 HKCU\Software\Microsoft\Windows\CurrentVersion\Run 下的 SoundLatch 项 ----
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunVal = L"SoundLatch";

static bool AutoStartEnabled() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return false;
    wchar_t buf[MAX_PATH * 2]{};
    DWORD cb = sizeof(buf);
    bool on = RegQueryValueExW(k, kRunVal, nullptr, nullptr, (BYTE*)buf, &cb) == ERROR_SUCCESS;
    RegCloseKey(k);
    return on;
}

static void SetAutoStart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring v = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(k, kRunVal, 0, REG_SZ, (const BYTE*)v.c_str(),
                       (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kRunVal);
    }
    RegCloseKey(k);
}

// 恢复默认参数
static void ResetDefaults() {
    SendMessageW(g_hA, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(g_hB, BM_SETCHECK, BST_CHECKED, 0);
    SliderSet(0, 55);
    SliderSet(1, 18);
    SliderSet(2, 65);
    SliderSet(3, 70);
    SliderSet(4, 950);
    for (int i = 0; i < 5; ++i) InvalidateRect(SliderCtl(i), nullptr, FALSE);
    PushParams();
}

// 枚举设备。已有选择时按设备 id 记住并恢复（热插拔后不会跳回默认）
static void FillCombos() {
    std::wstring keepCap, keepRen;
    int ci0 = (int)SendMessageW(g_hCap, CB_GETCURSEL, 0, 0);
    if (ci0 >= 0 && ci0 < (int)g_caps.size()) keepCap = g_caps[ci0].id;
    int ri0 = (int)SendMessageW(g_hRen, CB_GETCURSEL, 0, 0);
    if (ri0 >= 0 && ri0 < (int)g_rens.size()) keepRen = g_rens[ri0].id;

    g_caps = AudioEngine::EnumCapture();
    g_rens = AudioEngine::EnumRender();

    SendMessageW(g_hCap, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_hRen, CB_RESETCONTENT, 0, 0);

    int capDef = 0, renDef = 0;
    auto isVirtual = [](const std::wstring& n) {
        return n.find(L"CABLE") != std::wstring::npos ||
               n.find(L"Broadcast") != std::wstring::npos ||
               n.find(L"Virtual") != std::wstring::npos ||
               n.find(L"VoiceMeeter") != std::wstring::npos;
    };
    int firstReal = -1, firstNonCable = -1;
    for (size_t i = 0; i < g_rens.size(); ++i) {
        if (!isVirtual(g_rens[i].name) && firstReal < 0) firstReal = (int)i;
        if (g_rens[i].name.find(L"CABLE") == std::wstring::npos && firstNonCable < 0)
            firstNonCable = (int)i;
    }
    renDef = firstReal >= 0 ? firstReal : (firstNonCable >= 0 ? firstNonCable : 0);

    for (size_t i = 0; i < g_caps.size(); ++i) {
        SendMessageW(g_hCap, CB_ADDSTRING, 0, (LPARAM)g_caps[i].name.c_str());
        if (g_caps[i].name.find(L"CABLE Output") != std::wstring::npos) capDef = (int)i;
    }
    for (size_t i = 0; i < g_rens.size(); ++i)
        SendMessageW(g_hRen, CB_ADDSTRING, 0, (LPARAM)g_rens[i].name.c_str());
    for (size_t i = 0; i < g_caps.size(); ++i) if (!keepCap.empty() && g_caps[i].id == keepCap) capDef = (int)i;
    for (size_t i = 0; i < g_rens.size(); ++i) if (!keepRen.empty() && g_rens[i].id == keepRen) renDef = (int)i;
    if (!g_caps.empty()) SendMessageW(g_hCap, CB_SETCURSEL, capDef, 0);
    if (!g_rens.empty()) SendMessageW(g_hRen, CB_SETCURSEL, renDef, 0);
}

static void StartEngine(bool silent = false) {
    if (g_running) return;
    bool loop = SendMessageW(g_hLoop, BM_GETCHECK, 0, 0) == BST_CHECKED;
    int ci = (int)SendMessageW(g_hCap, CB_GETCURSEL, 0, 0);
    int ri = (int)SendMessageW(g_hRen, CB_GETCURSEL, 0, 0);
    if (ri < 0 || ri >= (int)g_rens.size()) {
        slLog(L"engine start skipped: bad render sel ri=%d n=%d", ri, (int)g_rens.size());
        if (!silent) MessageBoxW(g_hwnd, L"请先选择输出设备（你的耳机）。", L"SoundLatch", MB_ICONWARNING);
        return;
    }
    std::wstring capId;
    if (!loop) {
        if (ci < 0 || ci >= (int)g_caps.size()) {
            if (!silent) MessageBoxW(g_hwnd, L"请选择采集设备（通常为 CABLE Output）。\n若未安装 VB-Cable，可勾选「回环捕获」。",
                                     L"SoundLatch", MB_ICONWARNING);
            return;
        }
        capId = g_caps[ci].id;
    }
    if (g_rens[ri].name.find(L"CABLE") != std::wstring::npos) {
        if (!silent && MessageBoxW(g_hwnd, L"输出设备选成了虚拟声卡本身，会形成回授啸叫。确定要继续吗？",
                                   L"SoundLatch", MB_ICONWARNING | MB_YESNO) != IDYES) return;
    }
    std::wstring err;
    g_dsp.Prepare(48000.0f);
    PushParams();
    if (!g_engine.Start(capId, g_rens[ri].id, loop, &g_dsp, err)) {
        slLog(L"engine start FAILED: cap=%s ren=%s loop=%d err=%s",
              capId.c_str(), g_rens[ri].name.c_str(), loop ? 1 : 0, err.c_str());
        if (!silent) {
            std::wstring msg = L"启动失败：" + err;
            MessageBoxW(g_hwnd, msg.c_str(), L"SoundLatch", MB_ICONERROR);
        }
        return;
    }
    g_running = true;
    slLog(L"engine started: cap=%s ren=%s loop=%d lat=%.1fms",
          capId.c_str(), g_rens[ri].name.c_str(), loop ? 1 : 0, g_engine.LatencyMs());
}

static void StopEngine() {
    if (!g_running) return;
    g_engine.Stop();
    g_running = false;
}

// 设备/回环/刷新等变更后自动（重新）启动，保持「打开即启用」
static void AutoRestart() {
    if (g_running) StopEngine();
    StartEngine(true);
}

// ----------------------------------------------------------------- tray
static void TrayAdd() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    // 托盘图标不分深浅色，统一用应用图标（带底色的那版）
    nid.hIcon = (HICON)LoadImageW(g_hinst, MAKEINTRESOURCEW(IDI_APP),
                                  IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                  GetSystemMetrics(SM_CYSMICON), 0);
    wcscpy_s(nid.szTip, L"SoundLatch · 脚步增强 / 音量限幅   build " __DATE__ L" " __TIME__);
    Shell_NotifyIconW(NIM_ADD, &nid);
}
static void TrayRemove() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}
static RECT TrayRect() {
    RECT r{};
    NOTIFYICONIDENTIFIER id{};
    id.cbSize = sizeof(id);
    id.hWnd = g_hwnd;
    id.uID = 1;
    if (FAILED(Shell_NotifyIconGetRect(&id, &r))) {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0);
        r.left = r.right - S(340);
        r.top = r.bottom - S(420);
    }
    return r;
}


static const wchar_t* g_slName[5] = { L"环境抑制", L"环境阈值", L"脚步增强", L"瞬态灵敏度", L"最大输出" };
static HWND g_hLblCap, g_hLblRen;

// ---- 自适应布局：先测内容尺寸定窗口，再用真实 client 宽高摆控件 ----
static int MeasureText(HWND h, HFONT f, const wchar_t* s) {
    HDC dc = GetDC(h);
    HFONT old = (HFONT)SelectObject(dc, f);
    SIZE sz{0, 0};
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    SelectObject(dc, old);
    ReleaseDC(h, dc);
    return sz.cx;
}

static void LayoutPopupIn(HWND h, int cw, int ch) {
    if (!h || cw <= 0 || ch <= 0) return;
    if (!g_hA || !g_hB || !g_hMeter || !g_hCap || !g_hRen) return;

    const int pad = S(14);
    const int kW  = cw - pad * 2;
    const int ind = S(10);
    const int qs  = S(20);                 // 问号方块
    int y = S(10);

    // 复选框只覆盖"方框 + 文字"，点空白处不会误触发；问号说明独立放在最右
    int wA = MeasureText(h, g_fBtn, L"降低环境音 + 强化脚步声") + S(28);
    int wB = MeasureText(h, g_fBtn, L"锁定最大输出音量") + S(28);
    MoveWindow(g_hA, pad, y, wA, S(24), TRUE);
    MoveWindow(g_hQ1, pad + kW - qs, y + (S(24) - qs) / 2, qs, qs, TRUE);
    y += S(28);

    HWND sl[4] = { g_hEnvAmt, g_hEnvThr, g_hStepAmt, g_hStepSens };
    HWND lb[4] = { g_hLblEnvAmt, g_hLblEnvThr, g_hLblStepAmt, g_hLblStepSens };
    for (int k = 0; k < 4; ++k) {
        MoveWindow(lb[k], pad + ind, y, kW - ind, S(18), TRUE);
        MoveWindow(sl[k], pad + ind, y + S(18), kW - ind * 2, S(24), TRUE);
        y += S(44);
    }

    MoveWindow(g_hB, pad, y, wB, S(24), TRUE);
    MoveWindow(g_hQ2, pad + kW - qs, y + (S(24) - qs) / 2, qs, qs, TRUE);
    y += S(28);

    MoveWindow(g_hLblCeil, pad + ind, y, kW - ind, S(18), TRUE);
    MoveWindow(g_hCeil, pad + ind, y + S(18), kW - ind * 2, S(24), TRUE);
    y += S(18) + S(24) + S(5);            // 电平柱紧贴滑杆下沿，同属限幅这一组
    MoveWindow(g_hMeter, pad + ind, y, kW - ind * 2, S(24), TRUE);
    y += S(24) + S(10);

    MoveWindow(g_hLblCap, pad, y, kW, S(16), TRUE);
    y += S(18);
    MoveWindow(g_hCap, pad, y, kW, 240, TRUE);
    y += S(30);
    MoveWindow(g_hLblRen, pad, y, kW, S(16), TRUE);
    y += S(18);
    MoveWindow(g_hRen, pad, y, kW, 240, TRUE);
    y += S(30);

    // 底行：回环捕获 · 开机自启动 · 恢复默认(图标按钮)
    const int bs = S(24);
    MoveWindow(g_hReset, pad + kW - bs, y, bs, bs, TRUE);
    MoveWindow(g_hPin, pad + kW - bs * 2 - S(6), y, bs, bs, TRUE);
    const int rest = kW - bs * 2 - S(16);
    MoveWindow(g_hLoop, pad, y, rest / 2, bs, TRUE);
    MoveWindow(g_hAuto, pad + rest / 2, y, rest / 2, bs, TRUE);
}

static void PopupContentSize(HWND h, int& cw, int& ch) {
    const int pad = S(14);
    int w = MeasureText(h, g_fBtn, L"降低环境音 + 强化脚步声") + S(28) + S(30);
    int w2 = MeasureText(h, g_fBtn, L"锁定最大输出音量") + S(28) + S(30);
    if (w2 > w) w = w2;
    for (int i = 0; i < 5; ++i) {
        wchar_t t[96];
        const wchar_t* ph = (i == 4) ? L"100%" : (i == 1 ? L"-60 dBFS" : L"100 %");
        swprintf(t, 96, L"%s   %s", g_slName[i], ph);
        int lw = MeasureText(h, g_fLabel, t) + S(30);
        if (lw > w) w = lw;
    }
    int lw2 = MeasureText(h, g_fLabel, L"采集") + S(10);
    if (lw2 > w) w = lw2;
    if (w < S(250)) w = S(250);

    cw = w + pad * 2;
    ch = S(10) + S(28) + S(44) * 4 + S(28) + S(18) + S(24) + S(5) + S(24) + S(10)
       + S(18) + S(30) + S(18) + S(30) + S(22) + S(10);
}

// 基于当前真实 client 尺寸重排（窗口被缩放时也走这里）
static void LayoutPopup(HWND h) {
    if (!h) return;
    RECT rc;
    if (!GetClientRect(h, &rc)) return;
    LayoutPopupIn(h, rc.right - rc.left, rc.bottom - rc.top);
}

// ----------------------------------------------------------------- slider custom draw
// ComCtl32 的 trackbar 在暗色主题下凹槽/滑块仍是浅色，必须自己画。
// 注意：必须在 CDDS_PREPAINT 整块画完并返回 SKIPDEFAULT。
// 如果放到 CDDS_ITEMPREPAINT 按 item 画，拖动时系统只把"旧滑块/新滑块"的小矩形
// 放进重绘区：轨道不会被重画 -> 拖影；14px 的滑块又被裁进原生 item 矩形 -> 变窄。
static LRESULT DrawSlider(HWND sl, NMCUSTOMDRAW* cd) {
    if (cd->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;

    int pos = (int)SendMessageW(sl, TBM_GETPOS, 0, 0);
    int lo  = (int)SendMessageW(sl, TBM_GETRANGEMIN, 0, 0);
    int hi  = (int)SendMessageW(sl, TBM_GETRANGEMAX, 0, 0);
    if (hi <= lo) hi = lo + 1;
    double frac = (double)(pos - lo) / (double)(hi - lo);
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;

    RECT rc;
    GetClientRect(sl, &rc);
    HDC dc = cd->hdc;
    FillR(dc, rc, g_th.bg);                       // 先铺底，把旧滑块彻底擦掉

    int th = S(4);
    int ty = rc.top + ((rc.bottom - rc.top) - th) / 2;
    int tw = S(14);
    int usable = (rc.right - rc.left) - tw;
    if (usable < 1) usable = 1;
    int tx = (int)(frac * usable);

    // 轨道底色
    HBRUSH bTrack = CreateSolidBrush(g_th.track);
    RECT tr{rc.left, ty, rc.right, ty + th};
    FillRect(dc, &tr, bTrack);
    DeleteObject(bTrack);

    // 已填充段
    if (tx > 0) {
        HBRUSH bA = CreateSolidBrush(g_th.accent);
        RECT fr{rc.left, ty, rc.left + tx + tw / 2, ty + th};
        FillRect(dc, &fr, bA);
        DeleteObject(bA);
    }

    // 滑块（圆角矩形 + 浅色描边，暗色下才有对比）
    RECT gr{rc.left + tx, ty - S(4), rc.left + tx + tw, ty + th + S(4)};
    HBRUSH bG = CreateSolidBrush(g_th.accent);
    HPEN pEdge = CreatePen(PS_SOLID, 1, g_th.light ? RGB(255, 255, 255) : RGB(20, 22, 26));
    HGDIOBJ ob = SelectObject(dc, bG), op = SelectObject(dc, pEdge);
    RoundRect(dc, gr.left, gr.top, gr.right, gr.bottom, S(4), S(4));
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(bG); DeleteObject(pEdge);
    return CDRF_SKIPDEFAULT;
}

static LRESULT CALLBACK PopupProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        SyncDarkMode(h);          // 必须在子控件创建之前，否则控件按旧主题定型

        auto mkLabel = [&](const wchar_t* txt) {
            HWND lb2 = CreateWindowExW(0, L"STATIC", txt, WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                                       0, 0, 100, S(16), h, nullptr, g_hinst, nullptr);
            SetFont(lb2, g_fLabel);
            return lb2;
        };
        auto mkSlider = [&](int id, int lo, int hi, int pos) {
            HWND sl = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                                      WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                                      0, 0, 100, S(24), h, (HMENU)id, g_hinst, nullptr);
            SendMessageW(sl, TBM_SETRANGE, TRUE, MAKELONG(lo, hi));
            SendMessageW(sl, TBM_SETPOS, TRUE, pos);
            return sl;
        };

        g_hA = CreateWindowExW(0, L"BUTTON", L"降低环境音 + 强化脚步声",
                               WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                               0, 0, 100, S(24), h, (HMENU)IDC_A, g_hinst, nullptr);
        SetFont(g_hA, g_fBtn);
        SendMessageW(g_hA, BM_SETCHECK, BST_CHECKED, 0);

        g_hLblEnvAmt   = mkLabel(L"");
        g_hEnvAmt      = mkSlider(IDC_ENVAMT, 0, 100, 55);
        g_hLblEnvThr   = mkLabel(L"");
        g_hEnvThr      = mkSlider(IDC_ENVTHR, 0, 42, 18);
        g_hLblStepAmt  = mkLabel(L"");
        g_hStepAmt     = mkSlider(IDC_STEPAMT, 0, 100, 65);
        g_hLblStepSens = mkLabel(L"");
        g_hStepSens    = mkSlider(IDC_STEPSENS, 0, 100, 70);

        g_hB = CreateWindowExW(0, L"BUTTON", L"锁定最大输出音量",
                               WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                               0, 0, 100, S(24), h, (HMENU)IDC_B, g_hinst, nullptr);
        SetFont(g_hB, g_fBtn);
        SendMessageW(g_hB, BM_SETCHECK, BST_CHECKED, 0);

        // 问号说明：鼠标悬浮显示下面几条滑杆对 PUBG 声音的影响
        auto mkQ = [&](const wchar_t* tip) {
            HWND q = CreateWindowExW(0, L"STATIC", L"?",
                                     WS_CHILD | WS_VISIBLE | SS_NOTIFY | SS_CENTER | SS_CENTERIMAGE,
                                     0, 0, S(20), S(20), h, nullptr, g_hinst, nullptr);
            SetFont(q, g_fLabel);
            if (g_tip) {
                TOOLINFOW ti{};
                ti.cbSize = sizeof(ti);
                ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;   // uId 直接放窗口句柄，否则提示会挂到父窗口上
                ti.hwnd   = h;
                ti.uId    = (UINT_PTR)q;
                ti.lpszText = (LPWSTR)tip;
                SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
            }
            return q;
        };
        g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP,
                                0, 0, 0, 0, h, nullptr, g_hinst, nullptr);
        if (g_tip) {
            SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, S(320));
            SendMessageW(g_tip, TTM_SETDELAYTIME, TTDT_INITIAL, 200);    // 悬停 0.2 s 弹出
            SendMessageW(g_tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 600000); // 停留期间不自动关
            SendMessageW(g_tip, TTM_SETDELAYTIME, TTDT_RESHOW, 100);
            ThemeTooltip();
        }
        g_hQ1 = mkQ(L"\u9648\u4f4e\u73af\u5883\u97f3 + \u5f3a\u5316\u811a\u6b65\u58f0\n"
                    L"\n"
                    L"\u73af\u5883\u6291\u5236\uff1a\u538b\u4f4e 20-150 Hz\uff08\u7206\u70b8\u8f6f\u9e23\u3001\u8f7d\u5177\u5f15\u64ce\u3001\u8fdc\u5904\u70ae\u58f0\uff09\u3002\u8c03\u9ad8\u66f4\u5e72\u51c0\uff0c\u4f46\u7206\u70b8\u51b2\u51fb\u611f\u53d8\u5f31\u3002\n"
                    L"\u73af\u5883\u9608\u503c\uff1a\u4f4e\u4e8e\u8fd9\u4e2a\u7535\u5e73\u7684\u4f4e\u9891\u624d\u7b97\u73af\u5883\uff0c\u4f1a\u88ab\u989d\u5916\u538b\u4f4e\uff1b\u9ad8\u4e8e\u5b83\u89c6\u4e3a\u5b9e\u97f3\u4fdd\u7559\u3002\n"
                    L"\u811a\u6b65\u589e\u5f3a\uff1a\u62ac\u9ad8 150 Hz \u4ee5\u4e0a\uff0c\u5176\u4e2d 0.6-3k\uff08\u811a\u6b65\u4e3b\u4f53\uff09\u62ac\u5f97\u6700\u591a\uff0c3-16k \u62ac\u4e00\u534a\u3002\n"
                    L"\u77ac\u6001\u7075\u654f\u5ea6\uff1a\u591a\u5c0f\u7684\u54cd\u52a8\u624d\u89e6\u53d1\u589e\u5f3a\u3002\u9ad8=\u8f7b\u811a\u6b65\u4e5f\u6293\u5f97\u5230\uff0c\u4f4e=\u53ea\u6709\u5f00\u67aa\u7206\u70b8\u624d\u89e6\u53d1\u3002");
        g_hQ2 = mkQ(L"\u9501\u5b9a\u6700\u5927\u8f93\u51fa\u97f3\u91cf\n"
                    L"\n"
                    L"\u6700\u5927\u8f93\u51fa\uff1a\u8f93\u51fa\u5cf0\u503c\u7684\u5929\u82b1\u677f\u3002\u8d85\u8fc7\u7684\u90e8\u5206\u77ac\u95f4\u538b\u5230\u8fd9\u6761\u7ebf\uff0c80 ms \u540e\u6062\u590d\u3002\n"
                    L"100% = 0 dBFS \u4e0d\u9650\u5236\uff1b1% = -18 dBFS\uff1b0% = \u9759\u97f3\u3002\n"
                    L"\u8d85\u8fc7\u5f97\u591a\u538b\u5f97\u8d8a\u591a\uff0c\u4f46\u97f3\u8272\u4e0d\u53d8\uff08\u6574\u6761\u4fe1\u53f7\u540c\u4e00\u4e2a\u589e\u76ca\uff09\u3002\n"
                    L"\u4e5f\u53ef\u4ee5\u76f4\u63a5\u5728\u4e0b\u65b9\u7684\u7535\u5e73\u67f1\u4e0a\u6309\u4f4f\u5de6\u53f3\u62d6\u52a8\u6765\u8bbe\u7f6e\u3002");

        g_hLblCeil = mkLabel(L"");
        g_hCeil    = mkSlider(IDC_CEIL, 0, kCeilPosMax, 950);
        g_hMeter   = CreateWindowExW(0, L"SLMETER", L"", WS_CHILD | WS_VISIBLE,
                                     0, 0, 100, S(24), h, (HMENU)IDC_METER, g_hinst, nullptr);

        g_hLblCap = mkLabel(L"采集");
        g_hCap = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                 0, 0, 100, 240, h, (HMENU)IDC_CAP, g_hinst, nullptr);
        SetFont(g_hCap, g_fLabel);
        g_hLblRen = mkLabel(L"输出");
        g_hRen = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                 0, 0, 100, 240, h, (HMENU)IDC_REN, g_hinst, nullptr);
        SetFont(g_hRen, g_fLabel);

        g_hLoop = CreateWindowExW(0, L"BUTTON", L"回环捕获", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                  0, 0, 100, S(22), h, (HMENU)IDC_LOOPBACK, g_hinst, nullptr);
        SetFont(g_hLoop, g_fLabel);
        g_hAuto = CreateWindowExW(0, L"BUTTON", L"开机自启动", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                  0, 0, 100, S(22), h, (HMENU)IDC_AUTOSTART, g_hinst, nullptr);
        SetFont(g_hAuto, g_fLabel);
        {   // 状态以注册表为准；若已开启，顺手把路径更新成当前 exe（程序被移动过也不会失效）
            bool on = AutoStartEnabled();
            SendMessageW(g_hAuto, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
            if (on) SetAutoStart(true);
        }
        g_hReset = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                   0, 0, S(22), S(22), h, (HMENU)IDC_RESET, g_hinst, nullptr);
        g_hPin = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                 0, 0, S(22), S(22), h, (HMENU)IDC_PIN, g_hinst, nullptr);

        FillCombos();
        LoadConfig();
        PushParams();
        LayoutPopup(h);          // 字体已就位，按实测内容定窗口尺寸
        StartEngine(true);       // 控件就绪、设备已选好，这时才能真正开流
        SyncDarkMode(h);         // 子控件已全部创建，逐个挂深色主题类
        SetTimer(h, 1, 33, nullptr);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        // 复选框/单选钮发送的是 WM_CTLCOLORSTATIC（不是 BTN），
        // 必须返回不透明的深色刷，否则控件底色保持类默认的白色
        HDC dc = (HDC)w;
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, g_th.bg);
        SetTextColor(dc, g_th.text);
        return (LRESULT)g_brBg;
    }
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return DarkCtlColor((HDC)w, m);
    case WM_NOTIFY: {
        NMHDR* nh = (NMHDR*)l;
        if (nh->code == NM_CUSTOMDRAW && nh->idFrom >= IDC_ENVAMT && nh->idFrom <= IDC_CEIL)
            return DrawSlider(nh->hwndFrom, (NMCUSTOMDRAW*)l);
        break;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)l;
        if (di->CtlID == IDC_RESET || di->CtlID == IDC_PIN) {
            FillR(di->hDC, di->rcItem, g_th.bg);
            bool pressed = (di->itemState & ODS_SELECTED) != 0;
            bool on = (di->CtlID == IDC_PIN) && g_pinned;      // 固定状态用强调色
            SetBkMode(di->hDC, TRANSPARENT);
            SetTextColor(di->hDC, (pressed || on) ? g_th.accent : g_th.text);
            HGDIOBJ of = SelectObject(di->hDC, g_fIcon ? g_fIcon : g_fBtn);
            RECT r = di->rcItem;
            if (pressed) OffsetRect(&r, S(1), S(1));
            DrawTextW(di->hDC, di->CtlID == IDC_PIN ? L"" : L"", -1, &r,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(di->hDC, of);
            return TRUE;
        }
        break;
    }
    case WM_ERASEBKGND: {
        RECT* pr = (RECT*)l;
        if (!pr) return 1;
        FillR((HDC)w, *pr, g_th.bg);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        FillR(dc, rc, g_th.bg);       // 客户区底色始终由自己垫，不依赖系统 erase
        HBRUSH bd = CreateSolidBrush(g_th.border);
        FrameRect(dc, &rc, bd);       // 无标题栏，画一圈细边框
        DeleteObject(bd);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) {
            // 激活到别处 -> 收起。组合框展开的下拉列表是本窗口的子窗口，不算
            HWND next = (HWND)l;
            if (!next) next = GetForegroundWindow();
            if (next == h || (next && IsChild(h, next))) return 0;
            if (g_pinned) return 0;              // 已固定到屏幕，失焦也不收
            // 刚显示出来的那一刻可能附带一次"未激活"（SetForegroundWindow 失败时必然如此），
            // 忽略它，否则面板会一闪就没
            if (GetTickCount64() - g_showTick < 250) return 0;
            g_autoHideTick = GetTickCount64();
            HidePopup();
        }
        return 0;
    case WM_SIZE:
        LayoutPopup(h);               // 展示时窗口尺寸确定后再摆一次，避免溢出/错位
        return 0;
    case WM_NCHITTEST: {
        // 无标题栏的面板：客户区空白处当标题栏用，按住即可拖动；子控件区域不受影响
        LRESULT hit = DefWindowProcW(h, m, w, l);
        return (hit == HTCLIENT) ? HTCAPTION : hit;
    }
    case WM_MOVE:
        // 记住拖到的新位置，下次点托盘图标时还弹在这里（而不是跳回图标旁）
        if (g_popupShown) {
            RECT rc;
            GetWindowRect(h, &rc);
            g_anchor.x = rc.right;
            g_anchor.y = rc.bottom;
            g_anchorValid = true;
        }
        break;
    case WM_SETTINGCHANGE:
        if (l && !wcscmp((const wchar_t*)l, L"ImmersiveColorSet")) {
            RefreshTheme(h);
            return 0;
        }
        break;
    case WM_HSCROLL:
        if (l) InvalidateRect((HWND)l, nullptr, FALSE);   // 整条滑杆重画，杜绝拖动残影
        PushParams();
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDC_A || id == IDC_B) {
            PushParams();
        } else if (id == IDC_LOOPBACK) {
            PushParams();
            AutoRestart();
        } else if (id == IDC_AUTOSTART) {
            SetAutoStart(SendMessageW(g_hAuto, BM_GETCHECK, 0, 0) == BST_CHECKED);
        } else if (id == IDC_PIN) {
            g_pinned = !g_pinned;
            g_saved.pin = g_pinned ? 1 : 0;
            InvalidateRect(g_hPin, nullptr, FALSE);
            if (g_pinned) SetWindowPos(g_popup, HWND_TOPMOST, 0, 0, 0, 0,
                                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            SaveConfig();
        } else if (id == IDC_RESET) {
            ResetDefaults();
        } else if (HIWORD(w) == CBN_SELCHANGE && (id == IDC_CAP || id == IDC_REN)) {
            AutoRestart();                       // 切换设备后自动重连
        }
        return 0;
    }
    case WM_TIMER:
        if (w == 1) {
            if (g_hMeter) InvalidateRect(g_hMeter, nullptr, FALSE);
            return 0;
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void HidePopup() {
    static bool reentering = false;      // ShowWindow(SW_HIDE) 会同步回 WM_ACTIVATE，防递归
    if (reentering) return;
    if (!g_popup || !IsWindow(g_popup)) { g_popupShown = false; return; }
    if (!g_popupShown && !IsWindowVisible(g_popup)) return;
    reentering = true;
    g_popupShown = false;                // 先改状态，再隐藏
    ShowWindow(g_popup, SW_HIDE);
    SaveConfig();                        // 收起面板就落盘，避免被强制结束进程时丢参数
    reentering = false;
}

// activate=false 用于开机自动弹出：不抢焦点
// 只创建窗口（不显示）：控件、设备枚举、引擎启动都在 WM_CREATE 里完成
static bool EnsurePopup() {
    if (g_popup && IsWindow(g_popup)) return true;
    const DWORD st = WS_POPUP | WS_CLIPCHILDREN;
    g_popup = CreateWindowExW(WS_EX_TOOLWINDOW, L"SoundLatchPopup", L"SoundLatch 控制面板",
                              st, 0, 0, S(320), S(320), g_hwnd, nullptr, g_hinst, nullptr);
    if (!g_popup) return false;
    DWORD corner = 2;                              // Win11 圆角面板 DWMWCP_ROUND
    DwmSetWindowAttribute(g_popup, 33, &corner, sizeof(corner));
    SetTimer(g_popup, 1, 33, nullptr);
    return true;
}

static void ShowPopup(bool activate = true) {
    const DWORD st = WS_POPUP | WS_CLIPCHILDREN;   // 不裁子控件 -> 每次重绘刷到控件上，拖动时闪
    if (!EnsurePopup()) { g_popupShown = false; return; }

    // 只测一次内容尺寸 -> 推出窗口尺寸
    int cw = 0, ch = 0;
    PopupContentSize(g_popup, cw, ch);
    RECT wr{0, 0, cw, ch};
    AdjustWindowRectEx(&wr, st, FALSE, WS_EX_TOOLWINDOW);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;

    // 右下角锚定到点击位置：面板从这里向左上展开，贴边时再夹回工作区
    POINT pt;
    if (g_anchorValid) {
        pt = g_anchor;
    } else {
        RECT tr = TrayRect();                 // 开机自动弹出时没有点击点，用图标矩形右下角
        pt.x = tr.right;
        pt.y = tr.bottom;
    }
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = pt.x - w;                         // 右边缘对齐点击点
    int y = pt.y - h - S(6);                  // 底边略高于点击点，不压住图标
    if (x < wa.left + S(8)) x = wa.left + S(8);
    if (x + w > wa.right - S(8)) x = wa.right - w - S(8);
    if (y < wa.top + S(8)) y = wa.top + S(8);
    if (y + h > wa.bottom - S(8)) y = wa.bottom - h - S(8);

    // HWND_TOPMOST：托盘展开浮窗是 topmost 的，普通窗口会被压在它下面
    SetWindowPos(g_popup, HWND_TOPMOST, x, y, w, h,
                 SWP_SHOWWINDOW | (activate ? 0u : SWP_NOACTIVATE));
    g_popupShown = true;
    g_showTick = GetTickCount64();
    if (activate) SetForegroundWindow(g_popup);
    // 窗口可见后再同步一次主题（WCA/AllowDark 对隐藏窗口可能不生效）
    SyncDarkMode(g_popup);
    // 窗口尺寸定好后再按真实 client 尺寸摆控件，避免溢出/错位
    FillCombos();               // 每次打开都重新枚举设备（热插拔/换耳机不用手动刷新）
    LayoutPopup(g_popup);
    slLog(L"ShowPopup: at %d,%d win %dx%d client %dx%d", x, y, w, h, cw, ch);
}

// ----------------------------------------------------------------- meter
// 标尺：kMeterMinDb .. kMeterMaxDb 线性映射到 0..1（常量在文件上方与限幅阈值共用）。
// 左端 -18 dBFS、右端 0 dBFS，和「最大输出」滑杆同域：滑杆 1% 就在柱形最左。
// 低于 -18 dBFS 的信号画 1px 短条，表示"有声音但在标尺外"。
static constexpr float kMeterRelease = 0.55f;   // 回落速度（标尺比例/秒，约 36 dB/s）

static inline float Clamp01f(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

static float MeterNormDb(float db) {
    return Clamp01f((db - kMeterMinDb) / (kMeterMaxDb - kMeterMinDb));
}
static float MeterNormLin(float lin) {
    return MeterNormDb(20.0f * log10f(lin + 1e-7f));
}

// 每帧推进柱形：瞬时上冲、缓慢回落
static void MeterTick() {
    static ULONGLONG lastMs = 0;
    ULONGLONG now = GetTickCount64();
    float dt = lastMs ? (float)(now - lastMs) / 1000.0f : 0.033f;
    lastMs = now;
    if (dt > 0.25f) dt = 0.25f;

    float nrm = MeterNormLin(g_dsp.MeterInPeak());
    if (nrm > g_meterLevel) g_meterLevel = nrm;                                  // attack: 立即
    else g_meterLevel = nrm > g_meterLevel - kMeterRelease * dt
                            ? nrm : g_meterLevel - kMeterRelease * dt;           // release
}

// ---- 渐变色 ----
struct Col { int r, g, b; };
static inline Col ColOf(COLORREF c) { return Col{ (int)GetRValue(c), (int)GetGValue(c), (int)GetBValue(c) }; }
static inline Col MixC(Col a, Col b, float t) {
    t = Clamp01f(t);
    return Col{ (int)(a.r + (b.r - a.r) * t + 0.5f),
                (int)(a.g + (b.g - a.g) * t + 0.5f),
                (int)(a.b + (b.b - a.b) * t + 0.5f) };
}
static inline COLORREF ToCR(const Col& c) { return RGB(c.r, c.g, c.b); }
// 32bpp DIB 的内存字节序是 B,G,R：DWORD = R<<16 | G<<8 | B（与 RGB() 宏相反）
static inline DWORD DibCR(const Col& c) { return ((DWORD)c.r << 16) | ((DWORD)c.g << 8) | (DWORD)c.b; }

// 位置 -> 颜色：绿 -> 黄 -> 橙 -> 红（RGB 宏的输出正好是 32bpp DIB 需要的 0x00BBGGRR）
static Col MeterGrad(float t) {
    t = Clamp01f(t);
    Col gr = ColOf(g_th.green), am = ColOf(g_th.amber), rd = ColOf(g_th.red);
    Col dim = g_th.light ? Col{255, 255, 255} : Col{0, 0, 0};
    const int n = 6;
    Col cs[n] = { MixC(gr, dim, 0.34f), gr, MixC(gr, am, 0.55f), am, MixC(am, rd, 0.45f), rd };
    float ps[n] = { 0.00f, 0.35f, 0.62f, 0.80f, 0.91f, 1.00f };
    if (t <= ps[0]) return cs[0];
    for (int i = 1; i < n; ++i)
        if (t <= ps[i]) return MixC(cs[i - 1], cs[i], (t - ps[i - 1]) / (ps[i] - ps[i - 1]));
    return cs[n - 1];
}

// 在电平表上按住拖动 = 拖动限幅阈值（x 按标尺换算成 dB，直接写回天花板）
static void SetCeilFromMeter(HWND h, int x) {
    RECT rc;
    GetClientRect(h, &rc);
    int W = rc.right - rc.left;
    if (W <= 0 || !g_hCeil) return;
    float t  = Clamp01f((float)(x - rc.left) / (float)W);
    int pos  = (t < 0.02f) ? 0                                   // 最左一小段 = 静音（拖到底）
                           : CeilDbToPos(kMeterMinDb + t * (kMeterMaxDb - kMeterMinDb));
    if (pos == SliderPos(4)) return;
    SliderSet(4, pos);
    if (g_hCeil) InvalidateRect(g_hCeil, nullptr, FALSE);
    PushParams();
}

static LRESULT CALLBACK MeterProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        FillR(dc, rc, g_th.bg);

        const int pad = S(5);
        RECT tr = rc;                                  // 槽
        tr.top += pad; tr.bottom -= pad;
        int W = tr.right - tr.left, H = tr.bottom - tr.top;

        if (W > 4 && H > 3) {
            Col grayed = MixC(ColOf(g_th.bg), ColOf(g_th.text), g_th.light ? 0.10f : 0.14f);
            COLORREF trackBg = ToCR(grayed);
            COLORREF edgeC = ToCR(MixC(ColOf(g_th.border), ColOf(g_th.text), g_th.light ? 0.15f : 0.10f));

            // 直角槽：按需求不做圆角，填充、柱形、边框全是方角
            FillR(dc, tr, trackBg);

            // 参考刻度：-16 / -12 / -8 / -4 dBFS（0 dBFS 就是右边界）
            {
                const float tks[4] = { -16.f, -12.f, -8.f, -4.f };
                for (int i = 0; i < 4; ++i) {
                    int x = tr.left + (int)(MeterNormDb(tks[i]) * W + 0.5f);
                    if (x >= tr.right - 1) x = tr.right - 1;
                    FillR(dc, RECT{ x, tr.top, x + 1, tr.bottom },
                          ToCR(MixC(grayed, ColOf(g_th.text), 0.22f)));
                }
            }

            // ---- 柱形 ----
            // 阈值 = 显示上限：柱形永远长不过游标，超出的部分只画虚影。
            // 限幅打开时虚影是红色（在压），关闭时是灰色（只是超了上限，没在压）。
            const bool limiting = g_params.bOn;
            const float thrN    = MeterNormDb(g_params.ceilDb);
            const float raw     = g_meterLevel;
            const float body    = raw < thrN ? raw : thrN;
            const float over    = raw > thrN ? raw : body;
            int xBody  = (int)(body * W + 0.5f);
            int xOver  = (int)(over * W + 0.5f);
            // 低于标尺左端（<-18 dBFS）也给 1px：看得出"有信号，只是在范围外"
            if (xBody < 1 && body > 0.001f) xBody = 1;
            if (xOver < 1 && over > 0.001f) xOver = 1;
            if (xBody > W) xBody = W;
            if (xOver > W) xOver = W;

            Col redC  = MixC(MixC(ColOf(g_th.red), grayed, 0.30f), ColOf(g_th.red), 0.55f);
            Col dimC  = MixC(grayed, ColOf(g_th.text), 0.30f);
            // 静音位（阈值 = 0）时整条都是虚影，用灰色，别糊成一片红
            Col overC = (limiting && thrN > 0.002f) ? redC : dimC;
            std::vector<DWORD> bits((size_t)W * H);
            for (int x = 0; x < W; ++x) {
                float t = (W > 1) ? (float)x / (float)(W - 1) : 0.f;
                Col base = MeterGrad(t);
                if (x >= xOver) {
                    for (int y = 0; y < H; ++y) bits[(size_t)y * W + x] = DibCR(grayed);
                    continue;
                }
                Col c0 = (x >= xBody) ? overC : base;
                for (int y = 0; y < H; ++y) {
                    float vy = (H > 1) ? (float)y / (float)(H - 1) : 1.f;
                    float k  = 0.74f + 0.26f * vy;                 // 上暗下亮：柱形的体积感
                    bits[(size_t)y * W + x] = DibCR(Col{ (int)(c0.r * k + 0.5f),
                                                         (int)(c0.g * k + 0.5f),
                                                         (int)(c0.b * k + 0.5f) });
                }
            }
            BITMAPINFOHEADER bi{};
            bi.biSize = sizeof(bi);
            bi.biWidth = W; bi.biHeight = -H;          // top-down
            bi.biPlanes = 1; bi.biBitCount = 32; bi.biCompression = BI_RGB;
            BITMAPINFO bmi{};
            bmi.bmiHeader = bi;
            SetDIBitsToDevice(dc, tr.left, tr.top, (DWORD)W, (DWORD)H, 0, 0, 0, (UINT)H,
                              bits.data(), &bmi, DIB_RGB_COLORS);

            SelectClipRgn(dc, nullptr);
            HBRUSH eb = CreateSolidBrush(edgeC);
            FrameRect(dc, &tr, eb);               // 方角边框，与填充贴合
            DeleteObject(eb);

            // ---- 限幅阈值：一条最普通的分割线，只有颜色随开关变 ----
            {
                int tx = tr.left + (int)(thrN * W + 0.5f);
                if (tx > tr.right - 1) tx = tr.right - 1;
                COLORREF c = limiting ? g_th.amber : ToCR(MixC(grayed, ColOf(g_th.text), 0.45f));
                FillR(dc, RECT{ tx, tr.top, tx + 1, tr.bottom }, c);
            }
        }
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_LBUTTONDOWN) {
        SetCapture(h);
        SetCeilFromMeter(h, GET_X_LPARAM(l));
        return 0;
    }
    if (m == WM_MOUSEMOVE) {
        if (GetCapture() == h) SetCeilFromMeter(h, GET_X_LPARAM(l));
        return 0;
    }
    if (m == WM_LBUTTONUP) {
        if (GetCapture() == h) ReleaseCapture();
        return 0;
    }
    if (m == WM_SETCURSOR) {
        SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));   // 提示可以横向拖动
        return TRUE;
    }
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, m, w, l);
}

// ----------------------------------------------------------------- settings window
// 只负责托盘图标、定时器与退出；界面全在弹窗里
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SHOWPOPUP:
        ShowPopup();
        return 0;
    case WM_TRAYICON:
        // 只处理左键（开/收面板）和右键（菜单），没有其它版本语义需要兼容
        if (l == WM_LBUTTONUP) {
            if (GetTickCount64() - g_autoHideTick < 300) return 0;  // 这次点击就是让它失焦收起的那次
            GetCursorPos(&g_anchor);          // 记下点击位置，弹窗以它为锚点
            g_anchorValid = true;
            if (g_popupShown) HidePopup();
            else ShowPopup(true);
        } else if (l == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, IDM_POPUP, L"控制面板");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, IDM_QUIT, L"退出");
            SetForegroundWindow(h);
            int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
            DestroyMenu(menu);
            if (cmd == IDM_POPUP) {
                g_anchor = pt;
                g_anchorValid = true;
                ShowPopup();
            } else if (cmd == IDM_QUIT) {
                g_quit = true;
                SaveConfig();          // 必须先存：DestroyWindow 会先拆掉 owned 的弹窗
                DestroyWindow(h);
            }
        }
        return 0;
    case WM_TIMER:
        if (w == 1) {
            MeterTick();
            if (g_hMeter) InvalidateRect(g_hMeter, nullptr, FALSE);
            static int tick = 0;
            if (g_running && ++tick % 1800 == 0)     // 每 ~60 s 记一次运行状态，便于排查
                slLog(L"run: lat=%.1fms cap=%.1f ring=%.1f ren=%.1f lvl=%.3f under=%d",
                      g_engine.LatencyMs(), g_engine.CapBufMs(), g_engine.RingMs(),
                      g_engine.RenBufMs(), g_meterLevel, g_engine.Underruns());
            return 0;
        }
        if (w == 9) {                  // 启动后静默建好面板（不开窗），只为了把引擎跑起来
            KillTimer(h, 9);
            EnsurePopup();
            return 0;
        }
        return 0;
    case WM_DESTROY:
        KillTimer(h, 1);
        TrayRemove();
        StopEngine();
        SaveConfig();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ----------------------------------------------------------------- entry
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    g_hinst = hInst;
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    // 单实例：已有实例在跑就把它的面板叫到前台，否则重复启动会「看不见任何东西」
    {
        HANDLE h = CreateMutexW(nullptr, TRUE, L"Local\\SoundLatch_SingleInstance");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND prev = FindWindowW(L"SoundLatchWnd", nullptr);
            if (prev) {
                PostMessageW(prev, WM_APP + 2, 0, 0);   // 让已有实例弹出面板
                CloseHandle(h);
                return 0;
            }
        }
    }

    MakeTheme();
    // 必须在创建任何窗口/控件之前切好全局深色策略，
    // 否则控件按浅色主题创建，之后再改只影响标题栏
    SetAppDarkMode(!AppsIsLight());
    slLog(L"startup: AppsIsLight=%d SystemIsLight=%d pfMode=%p pfAllow=%p pfFlush=%p pfRefresh=%p",
          AppsIsLight() ? 1 : 0, SystemIsLight() ? 1 : 0,
          (void*)g_pfnPreferredAppMode, (void*)g_pfnAllowDarkForWindow,
          (void*)g_pfnFlushMenuThemes, (void*)g_pfnRefreshPolicy);

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    HDC screen = GetDC(nullptr);
    float dpiScale = (float)GetDeviceCaps(screen, LOGPIXELSX) / 96.0f;
    ReleaseDC(nullptr, screen);
    if (dpiScale < 0.8f) dpiScale = 0.8f;
    g_scale = dpiScale;

    auto mkf = [](int px, int weight, bool scaled) {
        int h = scaled ? -S(px) : -px;
        return CreateFontW(h, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    };
    g_fTitle = mkf(15, FW_SEMIBOLD, false);
    g_fLabel = mkf(15, FW_NORMAL, false);
    g_fValue = mkf(14, FW_SEMIBOLD, false);
    g_fBtn   = mkf(15, FW_SEMIBOLD, false);
    g_fSmall = mkf(12, FW_NORMAL, false);

    // 图标字体：Win11 用 Fluent Icons，Win10 回退 MDL2（矢量字形，比 GDI 手绘清晰）
    {
        const wchar_t* icons = FontExists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons"
                            : (FontExists(L"Segoe MDL2 Assets") ? L"Segoe MDL2 Assets" : nullptr);
        if (icons)
            g_fIcon = CreateFontW(-S(16), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH | FF_DONTCARE, icons);
    }
    g_pTitle = mkf(15, FW_SEMIBOLD, true);
    g_pLabel = mkf(12, FW_NORMAL, true);
    g_pSmall = mkf(11, FW_NORMAL, true);
    g_pBtn   = mkf(14, FW_SEMIBOLD, true);

    g_icoBig = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    g_icoSmall = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_brBg;                 // 跟随主题的类背景刷
    wc.lpszClassName = L"SoundLatchWnd";
    wc.hIcon = g_icoBig;
    wc.hIconSm = g_icoSmall;
    RegisterClassExW(&wc);

    WNDCLASSEXW pc{};
    pc.cbSize = sizeof(pc);
    pc.lpfnWndProc = PopupProc;
    pc.hInstance = hInst;
    pc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pc.hbrBackground = g_brBg;                 // 跟随主题的类背景刷
    pc.lpszClassName = L"SoundLatchPopup";
    RegisterClassExW(&pc);

    WNDCLASSEXW mc{};
    mc.cbSize = sizeof(mc);
    mc.lpfnWndProc = MeterProc;
    mc.hInstance = hInst;
    mc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    mc.lpszClassName = L"SLMETER";
    RegisterClassExW(&mc);

    // 隐藏的消息窗口：只做托盘图标与定时器的宿主，界面上不存在
    g_hwnd = CreateWindowExW(0, L"SoundLatchWnd", L"SoundLatch",
                             WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;

    TrayAdd();
    SetTimer(g_hwnd, 1, 33, nullptr);   // 电平表 30 fps
    // 托盘图标可能落在折叠区，双击后看不到任何东西会以为没启动。
    // 延后到初始化全部完成、消息循环跑起来之后再弹面板。
    SetTimer(g_hwnd, 9, 400, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == g_taskbarCreated) {
            TrayAdd();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_icoBig) DestroyIcon(g_icoBig);
    if (g_icoSmall) DestroyIcon(g_icoSmall);
    CoUninitialize();
    return 0;
}
