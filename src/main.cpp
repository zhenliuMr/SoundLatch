// SoundLatch - PUBG audio post-processor (WASAPI realtime)
// Tray app: starts hidden in the notification area, a compact popup on click,
// full settings window behind the gear icon. Follows the system light/dark theme.
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cmath>
#include "dsp.h"
#include "engine.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")

using namespace sl;

#define WM_TRAYICON (WM_APP + 1)
#define IDI_APP 1
#define IDM_POPUP   1
#define IDM_SETTINGS 2
#define IDM_QUIT    3

// ----------------------------------------------------------------- ids
enum {
    IDC_A = 101, IDC_B = 102,
    IDC_ENVAMT = 110, IDC_ENVTHR = 111, IDC_STEPAMT = 112, IDC_STEPSENS = 113,
    IDC_CEIL = 114,
    IDC_CAP = 120, IDC_REN = 121, IDC_LOOPBACK = 122, IDC_REFRESH = 123,
    IDC_START = 124, IDC_BYPASS = 125,
    IDC_METER = 130, IDC_STATUS = 131
};

static HINSTANCE g_hinst;
static HWND g_hwnd;                  // settings
static HWND g_popup;                 // tray popup
static HWND g_hA, g_hB, g_hEnvAmt, g_hEnvThr, g_hStepAmt, g_hStepSens, g_hCeil;
static HWND g_hCap, g_hRen, g_hLoop, g_hRefresh, g_hStart, g_hBypass, g_hStatus, g_hMeter;
static HWND g_hLblEnvAmt, g_hLblEnvThr, g_hLblStepAmt, g_hLblStepSens, g_hLblCeil;

static DspCore g_dsp;
static DspParams g_params;
static AudioEngine g_engine;
static std::vector<DeviceInfo> g_caps, g_rens;
static bool g_running = false;
static bool g_quit = false;

static ULONGLONG g_lastCpu = 0, g_lastT = 0;
static float g_cpuPct = 0.f;
static float g_peakDisp = 0.f;
static wchar_t g_statusText[256] = L"已停止";

static HFONT g_fTitle, g_fLabel, g_fValue, g_fBtn, g_fSmall;
static HFONT g_pTitle, g_pLabel, g_pSmall, g_pBtn;   // popup (scaled)
static HICON g_icoBig = nullptr, g_icoSmall = nullptr;
static UINT  g_taskbarCreated = 0;

// ----------------------------------------------------------------- theme
struct Theme {
    bool light;
    COLORREF bg, card, text, sub, track, accent, border, green, amber, red;
};
static Theme g_th;
static HBRUSH g_brBg = nullptr;

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
        g_th.bg     = RGB(255, 255, 255);
        g_th.card   = RGB(245, 246, 248);
        g_th.text   = RGB(26, 28, 32);
        g_th.sub    = RGB(110, 116, 126);
        g_th.track  = RGB(226, 229, 234);
        g_th.accent = RGB(47, 111, 237);
        g_th.border = RGB(221, 224, 230);
        g_th.green  = RGB(28, 150, 100);
        g_th.amber  = RGB(186, 118, 12);
        g_th.red    = RGB(198, 52, 52);
    } else {
        g_th.bg     = RGB(30, 32, 37);
        g_th.card   = RGB(39, 42, 49);
        g_th.text   = RGB(233, 235, 239);
        g_th.sub    = RGB(152, 158, 168);
        g_th.track  = RGB(58, 62, 72);
        g_th.accent = RGB(91, 140, 255);
        g_th.border = RGB(56, 60, 70);
        g_th.green  = RGB(58, 200, 140);
        g_th.amber  = RGB(230, 172, 74);
        g_th.red    = RGB(226, 92, 92);
    }
    if (g_brBg) DeleteObject(g_brBg);
    g_brBg = CreateSolidBrush(g_th.bg);
}

// ----------------------------------------------------------------- layout units
static float g_scale = 1.0f;
static inline int S(int v) { return (int)(v * g_scale + 0.5f); }
static inline RECT R4(int x, int y, int w, int h) {
    RECT r{ S(x), S(y), S(x + w), S(y + h) };
    return r;
}

static const int kLeft = 18, kWidth = 520;

// ----------------------------------------------------------------- helpers
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

static void FormatValue(int i, wchar_t* b, int n) {
    int p = SliderPos(i);
    switch (i) {
    case 0: swprintf(b, n, L"%d%%", p); break;
    case 1: swprintf(b, n, L"%d dBFS", -60 + p); break;
    case 2: swprintf(b, n, L"+%d dB", (p * 12 + 50) / 100); break;
    case 3: swprintf(b, n, L"%d%%", p); break;
    case 4: swprintf(b, n, L"%.1f dBFS", -24.0f + p / 10.0f); break;
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

static void SaveConfig() {
    FILE* f = _wfopen(AppDataPath().c_str(), L"w");
    if (!f) return;
    fprintf(f, "a_on=%d\nb_on=%d\nenv_amt=%d\nenv_thr=%d\nstep_amt=%d\nstep_sens=%d\nceil=%d\n",
            (int)SendMessageW(g_hA, BM_GETCHECK, 0, 0) == BST_CHECKED,
            (int)SendMessageW(g_hB, BM_GETCHECK, 0, 0) == BST_CHECKED,
            SliderPos(0), SliderPos(1), SliderPos(2), SliderPos(3), SliderPos(4));
    if (!g_caps.empty() && SendMessageW(g_hCap, CB_GETCURSEL, 0, 0) != CB_ERR)
        fprintf(f, "cap_sel=%d\n", (int)SendMessageW(g_hCap, CB_GETCURSEL, 0, 0));
    if (!g_rens.empty() && SendMessageW(g_hRen, CB_GETCURSEL, 0, 0) != CB_ERR)
        fprintf(f, "ren_sel=%d\n", (int)SendMessageW(g_hRen, CB_GETCURSEL, 0, 0));
    fprintf(f, "loopback=%d\n", (int)SendMessageW(g_hLoop, BM_GETCHECK, 0, 0) == BST_CHECKED);
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
    g_params.aOn      = SendMessageW(g_hA, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_params.bOn      = SendMessageW(g_hB, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_params.bypass   = SendMessageW(g_hBypass, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_params.envAmt   = SliderPos(0) / 100.0f;
    g_params.envThrDb = -60.0f + (float)SliderPos(1);
    g_params.stepAmt  = SliderPos(2) / 100.0f;
    g_params.stepSens = SliderPos(3) / 100.0f;
    g_params.ceilDb   = -24.0f + SliderPos(4) / 10.0f;
    g_dsp.SetParams(g_params);
    UpdateLabels();
    if (g_popup) InvalidateRect(g_popup, nullptr, FALSE);
}

static void FillCombos() {
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
    if (!g_caps.empty()) SendMessageW(g_hCap, CB_SETCURSEL, capDef, 0);
    if (!g_rens.empty()) SendMessageW(g_hRen, CB_SETCURSEL, renDef, 0);
}

static void UpdateStatus(const wchar_t* extra = nullptr) {
    if (!g_running) {
        swprintf(g_statusText, 256, L"状态：已停止%s%s", extra ? L" · " : L"", extra ? extra : L"");
    } else {
        swprintf(g_statusText, 256, L"运行中 · 延迟 %.1f ms · 欠载 %d · CPU %.1f%%",
                 g_engine.LatencyMs(), g_engine.Underruns(), g_cpuPct);
    }
    if (g_hStatus) {
        SetWindowTextW(g_hStatus, g_statusText);
        InvalidateRect(g_hStatus, nullptr, TRUE);
    }
}

static void StartEngine(bool silent = false) {
    if (g_running) return;
    bool loop = SendMessageW(g_hLoop, BM_GETCHECK, 0, 0) == BST_CHECKED;
    int ci = (int)SendMessageW(g_hCap, CB_GETCURSEL, 0, 0);
    int ri = (int)SendMessageW(g_hRen, CB_GETCURSEL, 0, 0);
    if (ri < 0 || ri >= (int)g_rens.size()) {
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
        if (!silent) {
            std::wstring msg = L"启动失败：" + err;
            MessageBoxW(g_hwnd, msg.c_str(), L"SoundLatch", MB_ICONERROR);
        }
        return;
    }
    g_running = true;
    SetWindowTextW(g_hStart, L"停止");
    UpdateStatus();
    if (g_popup) InvalidateRect(g_popup, nullptr, FALSE);
}

static void StopEngine() {
    if (!g_running) return;
    g_engine.Stop();
    g_running = false;
    SetWindowTextW(g_hStart, L"启动");
    UpdateStatus();
    if (g_popup) InvalidateRect(g_popup, nullptr, FALSE);
}

// ----------------------------------------------------------------- tray
static void TrayAdd() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = (HICON)LoadImageW(g_hinst, MAKEINTRESOURCEW(SystemIsLight() ? 2 : 3),
                                  IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                  GetSystemMetrics(SM_CYSMICON), 0);
    wcscpy_s(nid.szTip, L"SoundLatch · 脚步增强 / 音量限幅");
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
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

// ----------------------------------------------------------------- popup
static const int PW = 320, PH = 372;
static RECT g_pSwA{}, g_pSwB{}, g_pMaster{}, g_pGear{}, g_pMeter{};
static RECT g_pRow[4]{};
static const int g_pIdx[4] = { 0, 1, 2, 4 };   // envAmt, envThr, stepAmt, ceil
static int g_drag = -1;

static void DrawSwitch(HDC dc, RECT r, bool on) {
    int h = r.bottom - r.top;
    FillRound(dc, r, h / 2, on ? GetSysColor(COLOR_HIGHLIGHT) : GetSysColor(COLOR_SCROLLBAR));
    int knob = h - S(6);
    int kx = on ? (r.right - S(3) - knob) : (r.left + S(3));
    RECT k{ kx, r.top + S(3), kx + knob, r.top + S(3) + knob };
    HBRUSH wb = CreateSolidBrush(on ? GetSysColor(COLOR_WINDOW) : GetSysColor(COLOR_GRAYTEXT));
    HGDIOBJ ob = SelectObject(dc, wb);
    HPEN p = CreatePen(PS_SOLID, 1, on ? GetSysColor(COLOR_HIGHLIGHT) : GetSysColor(COLOR_SCROLLBAR));
    HGDIOBJ op = SelectObject(dc, p);
    Ellipse(dc, k.left, k.top, k.right, k.bottom);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(wb); DeleteObject(p);
}

static void DrawRow(HDC dc, RECT area, int idx) {
    wchar_t val[32];
    FormatValue(idx, val, 32);
    const wchar_t* names[5] = { L"环境抑制", L"环境阈值", L"脚步增强", L"瞬态灵敏度", L"最大输出" };

    RECT lr{ area.left, area.top, area.left + S(120), area.top + S(16) };
    Txt(dc, names[idx], lr, GetSysColor(COLOR_GRAYTEXT), g_pSmall, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    RECT vr{ area.right - S(96), area.top, area.right, area.top + S(16) };
    Txt(dc, val, vr, GetSysColor(COLOR_WINDOWTEXT), g_pSmall, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

    int ty = area.top + S(22);
    RECT track{ area.left, ty, area.right, ty + S(5) };
    FillRound(dc, track, 3, GetSysColor(COLOR_SCROLLBAR));

    int lo = (idx == 1) ? 0 : 0;
    int hi = (idx == 1) ? 42 : (idx == 4 ? 240 : 100);
    int pos = SliderPos(idx);
    (void)lo;
    float t = (float)pos / (float)hi;
    int fx = area.left + (int)(t * (area.right - area.left));
    if (fx > area.left + S(2)) {
        RECT fill{ area.left, ty, fx, ty + S(5) };
        FillRound(dc, fill, 3, GetSysColor(COLOR_HIGHLIGHT));
    }
    int cy = ty + S(2);
    int rr = S(6);
    HBRUSH b = CreateSolidBrush(GetSysColor(COLOR_HIGHLIGHT));
    HGDIOBJ ob = SelectObject(dc, b);
    HPEN p = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_BTNFACE));
    HGDIOBJ op = SelectObject(dc, p);
    Ellipse(dc, fx - rr, cy - rr, fx + rr, cy + rr);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
}

static void PaintPopup(HWND h) {
    PAINTSTRUCT ps;
    HDC dc0 = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    HDC dc = CreateCompatibleDC(dc0);
    HBITMAP bmp = CreateCompatibleBitmap(dc0, rc.right, rc.bottom);
    HGDIOBJ obm = SelectObject(dc, bmp);

    FillR(dc, rc, GetSysColor(COLOR_BTNFACE));
    StrokeRound(dc, rc, 14, g_th.border);

    if (g_icoSmall) DrawIconEx(dc, S(14), S(14), g_icoSmall, S(24), S(24), 0, nullptr, DI_NORMAL);
    Txt(dc, L"SoundLatch", R4(46, 12, 180, 28), GetSysColor(COLOR_WINDOWTEXT), g_pTitle, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    g_pGear = R4(PW - 44, 12, 30, 30);
    {
        int cx = (g_pGear.left + g_pGear.right) / 2, cy = (g_pGear.top + g_pGear.bottom) / 2;
        HPEN p = CreatePen(PS_SOLID, S(2), GetSysColor(COLOR_GRAYTEXT));
        HGDIOBJ op = SelectObject(dc, p);
        HBRUSH nb = (HBRUSH)GetStockObject(NULL_BRUSH);
        HGDIOBJ obr = SelectObject(dc, nb);
        Ellipse(dc, cx - S(7), cy - S(7), cx + S(7), cy + S(7));
        Ellipse(dc, cx - S(2), cy - S(2), cx + S(2), cy + S(2));
        SelectObject(dc, op); SelectObject(dc, obr);
        DeleteObject(p);
    }

    // vertical level meter
    g_pMeter = R4(16, 50, 20, PH - 118);
    FillRound(dc, g_pMeter, 6, GetSysColor(COLOR_SCROLLBAR));
    {
        float db = 20.0f * log10f(g_peakDisp + 1e-6f);
        float n = (db + 60.0f) / 60.0f;
        if (n < 0) n = 0;
        if (n > 1) n = 1;
        int hgt = g_pMeter.bottom - g_pMeter.top;
        int fh = (int)(n * hgt);
        if (fh > S(3)) {
            RECT f = g_pMeter;
            f.top = f.bottom - fh;
            FillRound(dc, f, 6, n > 0.92f ? g_th.red : g_th.green);
        }
    }

    const int x = 50, w = PW - 50 - 16;

    g_pSwA = R4(x, 54, 40, 22);
    DrawSwitch(dc, g_pSwA, SendMessageW(g_hA, BM_GETCHECK, 0, 0) == BST_CHECKED);
    Txt(dc, L"环境音 / 脚步", R4(x + 50, 52, w - 50, 26),
        SendMessageW(g_hA, BM_GETCHECK, 0, 0) == BST_CHECKED ? GetSysColor(COLOR_WINDOWTEXT) : GetSysColor(COLOR_GRAYTEXT),
        g_pSmall, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    g_pRow[0] = R4(x, 86, w, 34);
    g_pRow[1] = R4(x, 122, w, 34);
    g_pRow[2] = R4(x, 158, w, 34);
    DrawRow(dc, g_pRow[0], 0);
    DrawRow(dc, g_pRow[1], 1);
    DrawRow(dc, g_pRow[2], 2);

    g_pSwB = R4(x, 200, 40, 22);
    DrawSwitch(dc, g_pSwB, SendMessageW(g_hB, BM_GETCHECK, 0, 0) == BST_CHECKED);
    Txt(dc, L"输出限幅", R4(x + 50, 198, w - 50, 26),
        SendMessageW(g_hB, BM_GETCHECK, 0, 0) == BST_CHECKED ? GetSysColor(COLOR_WINDOWTEXT) : GetSysColor(COLOR_GRAYTEXT),
        g_pSmall, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    g_pRow[3] = R4(x, 232, w, 34);
    DrawRow(dc, g_pRow[3], 4);

    // master on/off
    g_pMaster = R4(x, 284, w, 40);
    {
        int half = (g_pMaster.right - g_pMaster.left) / 2;
        RECT offR{ g_pMaster.left, g_pMaster.top, g_pMaster.left + half, g_pMaster.bottom };
        RECT onR{ g_pMaster.left + half, g_pMaster.top, g_pMaster.right, g_pMaster.bottom };
        FillRound(dc, g_pMaster, 10, GetSysColor(COLOR_BTNFACE));
        if (g_running) FillRound(dc, onR, 10, GetSysColor(COLOR_HIGHLIGHT));
        else FillRound(dc, offR, 10, GetSysColor(COLOR_SCROLLBAR));
        Txt(dc, L"关闭", offR, g_running ? GetSysColor(COLOR_GRAYTEXT) : GetSysColor(COLOR_WINDOWTEXT), g_pBtn, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        Txt(dc, L"开启", onR, g_running ? GetSysColor(COLOR_WINDOW) : GetSysColor(COLOR_GRAYTEXT), g_pBtn, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }

    wchar_t line[128];
    if (g_running) swprintf(line, 128, L"%.0f ms · 欠载 %d · CPU %.0f%%",
                            g_engine.LatencyMs(), g_engine.Underruns(), g_cpuPct);
    else           swprintf(line, 128, L"未运行 · 点击「开启」开始");
    Txt(dc, line, R4(x, 332, w, 20), GetSysColor(COLOR_GRAYTEXT), g_pSmall, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    BitBlt(dc0, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, obm);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

static void ApplyPopupDrag(int px) {
    RECT r = g_pRow[g_drag < 3 ? g_drag : 3];
    int idx = g_pIdx[g_drag];
    float t = (float)(px - r.left) / (float)(r.right - r.left);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int hi = (idx == 1) ? 42 : (idx == 4 ? 240 : 100);
    int v = (int)(t * hi + 0.5f);
    if (v != SliderPos(idx)) {
        SliderSet(idx, v);
        PushParams();
    }
}

static LRESULT CALLBACK PopupProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT:
        PaintPopup(h);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && g_drag < 0) ShowWindow(h, SW_HIDE);
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) ShowWindow(h, SW_HIDE);
        return 0;
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        auto in = [&](RECT r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
        if (in(g_pGear)) {
            ShowWindow(h, SW_HIDE);
            ShowWindow(g_hwnd, SW_SHOW);
            SetForegroundWindow(g_hwnd);
            return 0;
        }
        if (in(g_pSwA)) {
            LRESULT v = SendMessageW(g_hA, BM_GETCHECK, 0, 0);
            SendMessageW(g_hA, BM_SETCHECK, v == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
            PushParams();
            return 0;
        }
        if (in(g_pSwB)) {
            LRESULT v = SendMessageW(g_hB, BM_GETCHECK, 0, 0);
            SendMessageW(g_hB, BM_SETCHECK, v == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
            PushParams();
            return 0;
        }
        if (in(g_pMaster)) {
            if (g_running) StopEngine(); else StartEngine(true);
            SaveConfig();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        for (int i = 0; i < 4; ++i) {
            if (in(g_pRow[i])) {
                g_drag = i;
                SetCapture(h);
                ApplyPopupDrag(x);
                return 0;
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_drag >= 0) { ApplyPopupDrag(GET_X_LPARAM(l)); return 0; }
        return 0;
    case WM_LBUTTONUP:
        if (g_drag >= 0) { g_drag = -1; ReleaseCapture(); SaveConfig(); }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void ShowPopup() {
    if (!g_popup) {
        g_popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"SoundLatchPopup", L"",
                                  WS_POPUP, 0, 0, S(PW), S(PH), g_hwnd, nullptr, g_hinst, nullptr);
        HRGN rgn = CreateRoundRectRgn(0, 0, S(PW) + 1, S(PH) + 1, S(14), S(14));
        SetWindowRgn(g_popup, rgn, TRUE);
    }
    RECT tr = TrayRect();
    int w = S(PW), h = S(PH);
    int x = (int)tr.left - w / 2;
    int y = (int)tr.top - h - S(8);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    if (x + w > wa.right - S(8)) x = wa.right - w - S(8);
    if (x < wa.left + S(8)) x = wa.left + S(8);
    if (y < wa.top + S(8)) y = wa.top + S(8);
    SetWindowPos(g_popup, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
    SetForegroundWindow(g_popup);
    InvalidateRect(g_popup, nullptr, FALSE);
}

// ----------------------------------------------------------------- meter
static LRESULT CALLBACK MeterProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        FillR(dc, rc, GetSysColor(COLOR_BTNFACE));
        FrameRect(dc, &rc, GetSysColorBrush(COLOR_3DSHADOW));

        int wdt = rc.right - rc.left;
        float dbv = 20.0f * log10f(g_peakDisp + 1e-6f);
        float norm = (dbv + 60.0f) / 60.0f;
        if (norm < 0) norm = 0;
        if (norm > 1) norm = 1;
        RECT bar = rc;
        bar.bottom -= S(4);
        bar.right = rc.left + (LONG)(norm * wdt);
        FillR(dc, bar, norm > 0.93f ? g_th.red : (norm > 0.80f ? g_th.amber : g_th.green));
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, m, w, l);
}

// ----------------------------------------------------------------- settings window
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        int y = 12;

        HWND t = CreateWindowExW(0, L"STATIC", L"SoundLatch  ·  PUBG 音频后处理", WS_CHILD | WS_VISIBLE,
                                 kLeft, y, kWidth, 24, h, nullptr, g_hinst, nullptr);
        SetFont(t, g_fBtn);
        y += 34;
        g_hA = CreateWindowExW(0, L"BUTTON", L"① 开启：降低环境音 + 强化脚步声",
                               WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                               kLeft, y, kWidth, 24, h, (HMENU)IDC_A, g_hinst, nullptr);
        SetFont(g_hA, g_fBtn);
        SendMessageW(g_hA, BM_SETCHECK, BST_CHECKED, 0);
        y += 30;

        auto mkSlider = [&](int id, int lo, int hi, int pos, int x, int yy, int wdt) {
            HWND s = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                                     x, yy, wdt, 26, h, (HMENU)id, g_hinst, nullptr);
            SendMessageW(s, TBM_SETRANGE, TRUE, MAKELONG(lo, hi));
            SendMessageW(s, TBM_SETPOS, TRUE, pos);
            return s;
        };

        g_hLblEnvAmt = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                       kLeft + 12, y, 220, 20, h, nullptr, g_hinst, nullptr);
        g_hEnvAmt = mkSlider(IDC_ENVAMT, 0, 100, 55, kLeft + 12, y + 20, kWidth - 40);
        SetFont(g_hLblEnvAmt, g_fLabel);
        y += 52;

        g_hLblEnvThr = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                       kLeft + 12, y, 220, 20, h, nullptr, g_hinst, nullptr);
        g_hEnvThr = mkSlider(IDC_ENVTHR, 0, 42, 18, kLeft + 12, y + 20, kWidth - 40);
        SetFont(g_hLblEnvThr, g_fLabel);
        y += 52;

        g_hLblStepAmt = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                        kLeft + 12, y, 220, 20, h, nullptr, g_hinst, nullptr);
        g_hStepAmt = mkSlider(IDC_STEPAMT, 0, 100, 65, kLeft + 12, y + 20, kWidth - 40);
        SetFont(g_hLblStepAmt, g_fLabel);
        y += 52;

        g_hLblStepSens = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                         kLeft + 12, y, 220, 20, h, nullptr, g_hinst, nullptr);
        g_hStepSens = mkSlider(IDC_STEPSENS, 0, 100, 70, kLeft + 12, y + 20, kWidth - 40);
        SetFont(g_hLblStepSens, g_fLabel);
        y += 58;

        g_hB = CreateWindowExW(0, L"BUTTON", L"② 开启：锁定最大输出音量（限幅）",
                               WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                               kLeft, y, kWidth, 24, h, (HMENU)IDC_B, g_hinst, nullptr);
        SetFont(g_hB, g_fBtn);
        SendMessageW(g_hB, BM_SETCHECK, BST_CHECKED, 0);
        y += 30;

        g_hLblCeil = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                     kLeft + 12, y, 220, 20, h, nullptr, g_hinst, nullptr);
        g_hCeil = mkSlider(IDC_CEIL, 0, 240, 210, kLeft + 12, y + 20, kWidth - 40);
        SetFont(g_hLblCeil, g_fLabel);
        y += 56;

        HWND l1 = CreateWindowExW(0, L"STATIC", L"采集（CABLE Output）", WS_CHILD | WS_VISIBLE,
                                  kLeft, y, 240, 20, h, nullptr, g_hinst, nullptr);
        SetFont(l1, g_fLabel);
        y += 22;
        g_hCap = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                 kLeft, y, kWidth, 240, h, (HMENU)IDC_CAP, g_hinst, nullptr);
        SetFont(g_hCap, g_fLabel);
        y += 32;
        HWND l2 = CreateWindowExW(0, L"STATIC", L"输出（你的耳机）", WS_CHILD | WS_VISIBLE,
                                  kLeft, y, 240, 20, h, nullptr, g_hinst, nullptr);
        SetFont(l2, g_fLabel);
        y += 22;
        g_hRen = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                 kLeft, y, kWidth, 240, h, (HMENU)IDC_REN, g_hinst, nullptr);
        SetFont(g_hRen, g_fLabel);
        y += 34;

        g_hLoop = CreateWindowExW(0, L"BUTTON", L"回环捕获（未装 VB-Cable 时测试用）",
                                  WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                  kLeft, y, 320, 22, h, (HMENU)IDC_LOOPBACK, g_hinst, nullptr);
        SetFont(g_hLoop, g_fLabel);
        g_hRefresh = CreateWindowExW(0, L"BUTTON", L"刷新", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     kLeft + 400, y - 2, 60, 26, h, (HMENU)IDC_REFRESH, g_hinst, nullptr);
        SetFont(g_hRefresh, g_fLabel);
        y += 32;

        g_hStart = CreateWindowExW(0, L"BUTTON", L"启动", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   kLeft, y, 120, 32, h, (HMENU)IDC_START, g_hinst, nullptr);
        SetFont(g_hStart, g_fBtn);
        g_hBypass = CreateWindowExW(0, L"BUTTON", L"旁通(A/B)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                    kLeft + 132, y, 120, 32, h, (HMENU)IDC_BYPASS, g_hinst, nullptr);
        SetFont(g_hBypass, g_fLabel);
        y += 40;

        g_hMeter = CreateWindowExW(0, L"SLMETER", L"", WS_CHILD | WS_VISIBLE,
                                   kLeft, y, kWidth, 24, h, (HMENU)IDC_METER, g_hinst, nullptr);
        y += 30;
        g_hStatus = CreateWindowExW(0, L"STATIC", L"状态：已停止",
                                    WS_CHILD | WS_VISIBLE | SS_SIMPLE | SS_NOPREFIX,
                                    kLeft, y, kWidth, 22, h, (HMENU)IDC_STATUS, g_hinst, nullptr);
        SetFont(g_hStatus, g_fLabel);
        y += 30;
        HWND hint = CreateWindowExW(0, L"STATIC",
                                    L"游戏输出→CABLE Input；本程序采集 CABLE Output→耳机。",
                                    WS_CHILD | WS_VISIBLE | SS_SIMPLE | SS_NOPREFIX,
                                    kLeft, y + 4, kWidth, 20, h, nullptr, g_hinst, nullptr);
        SetFont(hint, g_fLabel);

        FillCombos();
        LoadConfig();
        PushParams();
        UpdateStatus();

        SetTimer(h, 1, 60, nullptr);
        SetTimer(h, 2, 250, nullptr);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w;
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    case WM_SETTINGCHANGE:
        if (l && !wcscmp((const wchar_t*)l, L"ImmersiveColorSet")) {
            MakeTheme();
            RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
            if (g_popup) InvalidateRect(g_popup, nullptr, TRUE);
        }
        return 0;
    case WM_HSCROLL:
        PushParams();
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDC_A || id == IDC_B || id == IDC_BYPASS) {
            PushParams();
        } else if (id == IDC_REFRESH) {
            bool wasRunning = g_running;
            StopEngine();
            FillCombos();
            if (wasRunning) StartEngine(true);
        } else if (id == IDC_START) {
            if (g_running) { StopEngine(); SaveConfig(); }
            else StartEngine();
        }
        return 0;
    }
    case WM_TRAYICON: {
        UINT ev = LOWORD(l);
        if (ev == WM_LBUTTONUP || ev == NIN_SELECT || ev == NIN_KEYSELECT) {
            if (g_popup && IsWindowVisible(g_popup)) ShowWindow(g_popup, SW_HIDE);
            else ShowPopup();
        } else if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, IDM_POPUP, L"控制面板");
            AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"设置…");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, IDM_QUIT, L"退出");
            SetForegroundWindow(h);
            int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
            DestroyMenu(menu);
            if (cmd == IDM_POPUP) ShowPopup();
            else if (cmd == IDM_SETTINGS) { ShowWindow(h, SW_SHOW); SetForegroundWindow(h); }
            else if (cmd == IDM_QUIT) { g_quit = true; DestroyWindow(h); }
        }
        return 0;
    }
    case WM_TIMER: {
        if (w == 1) {
            g_peakDisp = g_dsp.MeterPeak();
            if (g_hMeter) InvalidateRect(g_hMeter, nullptr, FALSE);
            if (g_popup && IsWindowVisible(g_popup)) {
                RECT mr = g_pMeter;
                InflateRect(&mr, S(2), S(2));
                InvalidateRect(g_popup, &mr, FALSE);
            }
            return 0;
        }
        ULONGLONG now = GetTickCount64();
        FILETIME ct, et, kt, ut;
        if (GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut)) {
            ULONGLONG cpu = ((ULONGLONG)kt.dwHighDateTime << 32 | kt.dwLowDateTime) +
                            ((ULONGLONG)ut.dwHighDateTime << 32 | ut.dwLowDateTime);
            static ULONGLONG accCpu = 0, accT = 0;
            if (g_lastT && now > g_lastT) {
                accCpu += cpu - g_lastCpu;
                accT += now - g_lastT;
                if (accT >= 500) {
                    g_cpuPct = (float)((double)accCpu / 10000.0 / (double)accT * 100.0);
                    accCpu = 0; accT = 0;
                }
            }
            g_lastCpu = cpu;
            g_lastT = now;
        }
        UpdateStatus();
        if (g_popup && IsWindowVisible(g_popup)) {
            RECT sr = R4(50, 330, PW - 66, 22);
            InvalidateRect(g_popup, &sr, FALSE);
        }
        return 0;
    }
    case WM_CLOSE:
        if (!g_quit) { ShowWindow(h, SW_HIDE); return 0; }
        break;
    case WM_DESTROY:
        KillTimer(h, 1);
        KillTimer(h, 2);
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
    MakeTheme();

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
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"SoundLatchWnd";
    wc.hIcon = g_icoBig;
    wc.hIconSm = g_icoSmall;
    RegisterClassExW(&wc);

    WNDCLASSEXW pc{};
    pc.cbSize = sizeof(pc);
    pc.lpfnWndProc = PopupProc;
    pc.hInstance = hInst;
    pc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pc.lpszClassName = L"SoundLatchPopup";
    RegisterClassExW(&pc);

    WNDCLASSEXW mc{};
    mc.cbSize = sizeof(mc);
    mc.lpfnWndProc = MeterProc;
    mc.hInstance = hInst;
    mc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    mc.lpszClassName = L"SLMETER";
    RegisterClassExW(&mc);

    RECT r{0, 0, kWidth + kLeft * 2, 735};
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    g_hwnd = CreateWindowExW(0, L"SoundLatchWnd", L"SoundLatch",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                             nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;

    TrayAdd();
    // start in the tray, no window on screen
    ShowWindow(g_hwnd, SW_HIDE);

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
