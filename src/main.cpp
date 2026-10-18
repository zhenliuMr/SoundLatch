// SoundLock - PUBG audio post-processor (WASAPI realtime)
#include <windows.h>
#include <commctrl.h>
#include <psapi.h>
#include <mmdeviceapi.h>
#include <string>
#include <vector>
#include <cstdio>
#include "dsp.h"
#include "engine.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "psapi.lib")

using namespace sl;

// ----------------------------------------------------------------- ids
enum {
    IDC_A = 101, IDC_B = 102,
    IDC_ENVAMT = 110, IDC_ENVTHR = 111, IDC_STEPAMT = 112, IDC_STEPSENS = 113,
    IDC_CEIL = 114,
    IDC_CAP = 120, IDC_REN = 121, IDC_LOOPBACK = 122, IDC_REFRESH = 123,
    IDC_START = 124, IDC_BYPASS = 125,
    IDC_METER = 130, IDC_STATUS = 131, IDC_HINT = 132,
    IDC_LBL_ENVAMT = 140, IDC_LBL_ENVTHR, IDC_LBL_STEPAMT, IDC_LBL_STEPSENS, IDC_LBL_CEIL
};

static HINSTANCE g_hinst;
static HWND g_hwnd, g_hA, g_hB, g_hEnvAmt, g_hEnvThr, g_hStepAmt, g_hStepSens, g_hCeil;
static HWND g_hCap, g_hRen, g_hLoop, g_hRefresh, g_hStart, g_hBypass, g_hStatus, g_hMeter;
static HWND g_hLblEnvAmt, g_hLblEnvThr, g_hLblStepAmt, g_hLblStepSens, g_hLblCeil;

static DspCore g_dsp;
static DspParams g_params;
static AudioEngine g_engine;
static std::vector<DeviceInfo> g_caps, g_rens;
static bool g_running = false;
static bool g_applyBusy = false;

static ULONGLONG g_lastCpu = 0, g_lastT = 0;
static float g_cpuPct = 0.f;
static float g_peakDisp = 0.f;

static const int kLeft = 18, kWidth = 520;

// ----------------------------------------------------------------- helpers
static void SetFont(HWND h, HFONT f) { SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE); }

static std::wstring AppDataPath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::wstring p = (n && n < MAX_PATH) ? std::wstring(buf) : std::wstring(L".");
    p += L"\\SoundLock";
    CreateDirectoryW(p.c_str(), nullptr);
    return p + L"\\config.ini";
}

static void SaveConfig() {
    std::wstring path = AppDataPath();
    FILE* f = _wfopen(path.c_str(), L"w");
    if (!f) return;
    fprintf(f, "a_on=%d\nb_on=%d\nenv_amt=%d\nenv_thr=%d\nstep_amt=%d\nstep_sens=%d\nceil=%d\n",
             (int)SendMessageW(g_hA, BM_GETCHECK, 0, 0) == BST_CHECKED,
             (int)SendMessageW(g_hB, BM_GETCHECK, 0, 0) == BST_CHECKED,
             (int)SendMessageW(g_hEnvAmt, TBM_GETPOS, 0, 0),
             (int)SendMessageW(g_hEnvThr, TBM_GETPOS, 0, 0),
             (int)SendMessageW(g_hStepAmt, TBM_GETPOS, 0, 0),
             (int)SendMessageW(g_hStepSens, TBM_GETPOS, 0, 0),
             (int)SendMessageW(g_hCeil, TBM_GETPOS, 0, 0));
    if (!g_caps.empty() && SendMessageW(g_hCap, CB_GETCURSEL, 0, 0) != CB_ERR)
        fprintf(f, "cap_sel=%d\n", (int)SendMessageW(g_hCap, CB_GETCURSEL, 0, 0));
    if (!g_rens.empty() && SendMessageW(g_hRen, CB_GETCURSEL, 0, 0) != CB_ERR)
        fprintf(f, "ren_sel=%d\n", (int)SendMessageW(g_hRen, CB_GETCURSEL, 0, 0));
    fprintf(f, "loopback=%d\n", (int)SendMessageW(g_hLoop, BM_GETCHECK, 0, 0) == BST_CHECKED);
    fclose(f);
}

static void LoadConfig() {
    std::wstring path = AppDataPath();
    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f) return;
    char line[256];
    int capSel = -1, renSel = -1;
    while (fgets(line, 256, f)) {
        char k[64]; int v = 0;
        if (sscanf(line, "%63[^=]=%d", k, &v) != 2) continue;
        if (!strcmp(k, "a_on")) SendMessageW(g_hA, BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0);
        else if (!strcmp(k, "b_on")) SendMessageW(g_hB, BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0);
        else if (!strcmp(k, "env_amt")) SendMessageW(g_hEnvAmt, TBM_SETPOS, TRUE, v);
        else if (!strcmp(k, "env_thr")) SendMessageW(g_hEnvThr, TBM_SETPOS, TRUE, v);
        else if (!strcmp(k, "step_amt")) SendMessageW(g_hStepAmt, TBM_SETPOS, TRUE, v);
        else if (!strcmp(k, "step_sens")) SendMessageW(g_hStepSens, TBM_SETPOS, TRUE, v);
        else if (!strcmp(k, "ceil")) SendMessageW(g_hCeil, TBM_SETPOS, TRUE, v);
        else if (!strcmp(k, "cap_sel")) capSel = v;
        else if (!strcmp(k, "ren_sel")) renSel = v;
        else if (!strcmp(k, "loopback")) SendMessageW(g_hLoop, BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    fclose(f);
    if (capSel >= 0 && capSel < (int)g_caps.size()) SendMessageW(g_hCap, CB_SETCURSEL, capSel, 0);
    if (renSel >= 0 && renSel < (int)g_rens.size()) SendMessageW(g_hRen, CB_SETCURSEL, renSel, 0);
}

static void UpdateLabels() {
    wchar_t b[128];
    int ea = (int)SendMessageW(g_hEnvAmt, TBM_GETPOS, 0, 0);
    int et = (int)SendMessageW(g_hEnvThr, TBM_GETPOS, 0, 0);
    int sa = (int)SendMessageW(g_hStepAmt, TBM_GETPOS, 0, 0);
    int ss = (int)SendMessageW(g_hStepSens, TBM_GETPOS, 0, 0);
    int ce = (int)SendMessageW(g_hCeil, TBM_GETPOS, 0, 0);

    swprintf(b, 128, L"环境抑制   %d %%", ea);              SetWindowTextW(g_hLblEnvAmt, b);
    swprintf(b, 128, L"环境阈值   %d dBFS", -60 + et);      SetWindowTextW(g_hLblEnvThr, b);
    swprintf(b, 128, L"脚步增强   +%d dB", (sa * 12 + 50) / 100); SetWindowTextW(g_hLblStepAmt, b);
    swprintf(b, 128, L"瞬态灵敏度 %d %%", ss);              SetWindowTextW(g_hLblStepSens, b);
    swprintf(b, 128, L"最大输出   %.1f dBFS", -24.0f + ce / 10.0f); SetWindowTextW(g_hLblCeil, b);
}

static void PushParams() {
    g_params.aOn      = SendMessageW(g_hA, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_params.bOn      = SendMessageW(g_hB, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_params.bypass   = SendMessageW(g_hBypass, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_params.envAmt   = (int)SendMessageW(g_hEnvAmt, TBM_GETPOS, 0, 0) / 100.0f;
    g_params.envThrDb = -60.0f + (float)SendMessageW(g_hEnvThr, TBM_GETPOS, 0, 0);
    g_params.stepAmt  = (int)SendMessageW(g_hStepAmt, TBM_GETPOS, 0, 0) / 100.0f;
    g_params.stepSens = (int)SendMessageW(g_hStepSens, TBM_GETPOS, 0, 0) / 100.0f;
    g_params.ceilDb   = -24.0f + (int)SendMessageW(g_hCeil, TBM_GETPOS, 0, 0) / 10.0f;
    g_dsp.SetParams(g_params);
    UpdateLabels();
}

static void FillCombos() {
    g_caps = AudioEngine::EnumCapture();
    g_rens = AudioEngine::EnumRender();

    SendMessageW(g_hCap, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_hRen, CB_RESETCONTENT, 0, 0);

    int capDef = 0, renDef = 0;
    // prefer a real (non-virtual) output: virtual ones add their own latency
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
    wchar_t b[512];
    if (!g_running) {
        swprintf(b, 512, L"状态：已停止%s%s", extra ? L" · " : L"", extra ? extra : L"");
        SetWindowTextW(g_hStatus, b);
        return;
    }
    swprintf(b, 512,
             L"延迟 %.1f ms ｜ 欠载 %d ｜ 采样比 %.3f ｜ CPU %.1f%%",
             g_engine.LatencyMs(), g_engine.Underruns(), g_engine.DbgRatio(), g_cpuPct);
    SetWindowTextW(g_hStatus, b);
}

static void StartEngine() {
    if (g_running) return;
    bool loop = SendMessageW(g_hLoop, BM_GETCHECK, 0, 0) == BST_CHECKED;
    int ci = (int)SendMessageW(g_hCap, CB_GETCURSEL, 0, 0);
    int ri = (int)SendMessageW(g_hRen, CB_GETCURSEL, 0, 0);
    if (ri < 0 || ri >= (int)g_rens.size()) {
        MessageBoxW(g_hwnd, L"请先选择输出设备（你的耳机）。", L"SoundLock", MB_ICONWARNING);
        return;
    }
    std::wstring capId;
    if (!loop) {
        if (ci < 0 || ci >= (int)g_caps.size()) {
            MessageBoxW(g_hwnd, L"请选择采集设备（通常为 CABLE Output）。\n若未安装 VB-Cable，可勾选“回环捕获”。",
                        L"SoundLock", MB_ICONWARNING);
            return;
        }
        capId = g_caps[ci].id;
    }
    if (g_rens[ri].name.find(L"CABLE") != std::wstring::npos) {
        if (MessageBoxW(g_hwnd, L"输出设备选成了虚拟声卡本身，会形成回授啸叫。确定要继续吗？",
                        L"SoundLock", MB_ICONWARNING | MB_YESNO) != IDYES) return;
    }
    std::wstring err;
    g_dsp.Prepare(48000.0f);
    PushParams();
    if (!g_engine.Start(capId, g_rens[ri].id, loop, &g_dsp, err)) {
        std::wstring msg = L"启动失败：" + err;
        MessageBoxW(g_hwnd, msg.c_str(), L"SoundLock", MB_ICONERROR);
        return;
    }
    g_running = true;
    SetWindowTextW(g_hStart, L"停止");
    UpdateStatus();
}

static void StopEngine() {
    if (!g_running) return;
    g_engine.Stop();
    g_running = false;
    SetWindowTextW(g_hStart, L"启动");
    UpdateStatus();
}

// ----------------------------------------------------------------- meter
static LRESULT CALLBACK MeterProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(28, 28, 28));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);

        int wdt = rc.right - rc.left;
        float dbv = 20.0f * log10f(g_peakDisp + 1e-6f);
        float norm = (dbv + 60.0f) / 60.0f;
        if (norm < 0) norm = 0;
        if (norm > 1) norm = 1;
        RECT bar = rc;
        bar.bottom -= 6;
        bar.right = rc.left + (LONG)(norm * wdt);

        COLORREF c = RGB(60, 180, 110);
        if (norm > 0.93f) c = RGB(210, 70, 60);
        else if (norm > 0.80f) c = RGB(210, 170, 60);
        HBRUSH fb = CreateSolidBrush(c);
        FillRect(dc, &bar, fb);
        DeleteObject(fb);

        // -6 dB / -18 dB ticks
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(90, 90, 90));
        HGDIOBJ old = SelectObject(dc, pen);
        for (float d : {-18.f, -6.f}) {
            int x = rc.left + (int)(((d + 60.f) / 60.f) * wdt);
            MoveToEx(dc, x, rc.top, nullptr);
            LineTo(dc, x, rc.bottom - 6);
        }
        SelectObject(dc, old);
        DeleteObject(pen);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ----------------------------------------------------------------- window
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

        HWND t = CreateWindowExW(0, L"STATIC", L"SoundLock  ·  PUBG 音频后处理", WS_CHILD | WS_VISIBLE,
                                 kLeft, y, kWidth, 24, h, nullptr, g_hinst, nullptr);
        SetFont(t, fb);
        y += 34;
        g_hA = CreateWindowExW(0, L"BUTTON", L"① 开启：降低环境音 + 强化脚步声",
                               WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                               kLeft, y, kWidth, 24, h, (HMENU)IDC_A, g_hinst, nullptr);
        SetFont(g_hA, fb);
        SendMessageW(g_hA, BM_SETCHECK, BST_CHECKED, 0);
        y += 30;

        struct Row { int id; int* dummy; };
        auto mkSlider = [&](int id, int lo, int hi, int pos, int x, int yy, int wdt) {
            HWND s = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ,
                                     x, yy, wdt, 26, h, (HMENU)id, g_hinst, nullptr);
            SendMessageW(s, TBM_SETRANGE, TRUE, MAKELONG(lo, hi));
            SendMessageW(s, TBM_SETPOS, TRUE, pos);
            SendMessageW(s, TBM_SETTICFREQ, (hi - lo) / 10 ? (hi - lo) / 10 : 1, 0);
            return s;
        };

        g_hLblEnvAmt = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                       kLeft + 12, y, 200, 20, h, nullptr, g_hinst, nullptr);
        g_hEnvAmt = mkSlider(IDC_ENVAMT, 0, 100, 55, kLeft + 12, y + 20, kWidth - 40);
        SetFont(g_hLblEnvAmt, f);
        y += 52;

        g_hLblEnvThr = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                       kLeft + 12, y, 200, 20, h, nullptr, g_hinst, nullptr);
        g_hEnvThr = mkSlider(IDC_ENVTHR, 0, 42, 18, kLeft + 12, y + 20, kWidth - 40);
        SetFont(g_hLblEnvThr, f);
        y += 52;

        g_hLblStepAmt = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                        kLeft + 12, y, 200, 20, h, nullptr, g_hinst, nullptr);
        g_hStepAmt = mkSlider(IDC_STEPAMT, 0, 100, 65, kLeft + 12, y + 20, kWidth - 40);
        SetFont(g_hLblStepAmt, f);
        y += 52;

        g_hLblStepSens = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                         kLeft + 12, y, 200, 20, h, nullptr, g_hinst, nullptr);
        g_hStepSens = mkSlider(IDC_STEPSENS, 0, 100, 70, kLeft + 12, y + 20, kWidth - 40);
        SetFont(g_hLblStepSens, f);
        y += 58;

        g_hB = CreateWindowExW(0, L"BUTTON", L"② 开启：锁定最大输出音量（限幅）",
                               WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                               kLeft, y, kWidth, 24, h, (HMENU)IDC_B, g_hinst, nullptr);
        SetFont(g_hB, fb);
        SendMessageW(g_hB, BM_SETCHECK, BST_CHECKED, 0);
        y += 30;

        g_hLblCeil = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                     kLeft + 12, y, 220, 20, h, nullptr, g_hinst, nullptr);
        g_hCeil = mkSlider(IDC_CEIL, 0, 240, 210, kLeft + 12, y + 20, kWidth - 40);
        SetFont(g_hLblCeil, f);
        y += 56;

        HWND l1 = CreateWindowExW(0, L"STATIC", L"采集（CABLE Output）", WS_CHILD | WS_VISIBLE,
                                  kLeft, y, 200, 20, h, nullptr, g_hinst, nullptr);
        SetFont(l1, f);
        y += 22;
        g_hCap = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                 kLeft, y, kWidth, 240, h, (HMENU)IDC_CAP, g_hinst, nullptr);
        SetFont(g_hCap, f);
        y += 32;
        HWND l2 = CreateWindowExW(0, L"STATIC", L"输出（你的耳机）", WS_CHILD | WS_VISIBLE,
                                  kLeft, y, 200, 20, h, nullptr, g_hinst, nullptr);
        SetFont(l2, f);
        y += 22;
        g_hRen = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                 kLeft, y, kWidth, 240, h, (HMENU)IDC_REN, g_hinst, nullptr);
        SetFont(g_hRen, f);
        y += 34;

        g_hLoop = CreateWindowExW(0, L"BUTTON", L"回环捕获（未装 VB-Cable 时测试用）",
                                  WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                  kLeft, y, 320, 22, h, (HMENU)IDC_LOOPBACK, g_hinst, nullptr);
        SetFont(g_hLoop, f);
        g_hRefresh = CreateWindowExW(0, L"BUTTON", L"刷新", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     kLeft + 400, y - 2, 60, 26, h, (HMENU)IDC_REFRESH, g_hinst, nullptr);
        SetFont(g_hRefresh, f);
        y += 32;

        g_hStart = CreateWindowExW(0, L"BUTTON", L"启动", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   kLeft, y, 120, 32, h, (HMENU)IDC_START, g_hinst, nullptr);
        SetFont(g_hStart, fb);
        g_hBypass = CreateWindowExW(0, L"BUTTON", L"旁通(A/B)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                    kLeft + 132, y, 120, 32, h, (HMENU)IDC_BYPASS, g_hinst, nullptr);
        SetFont(g_hBypass, f);
        y += 40;

        g_hMeter = CreateWindowExW(0, L"SLMETER", L"", WS_CHILD | WS_VISIBLE,
                                   kLeft, y, kWidth, 26, h, (HMENU)IDC_METER, g_hinst, nullptr);
        y += 32;
        g_hStatus = CreateWindowExW(0, L"STATIC", L"状态：已停止",
                                    WS_CHILD | WS_VISIBLE | SS_SIMPLE | SS_NOPREFIX,
                                    kLeft, y, kWidth, 22, h, (HMENU)IDC_STATUS, g_hinst, nullptr);
        SetFont(g_hStatus, f);
        y += 56;
        HWND hint = CreateWindowExW(0, L"STATIC",
                                    L"游戏输出→CABLE Input；本程序采集 CABLE Output→耳机。",
                                    WS_CHILD | WS_VISIBLE | SS_SIMPLE | SS_NOPREFIX,
                                    kLeft, y + 22, kWidth, 20, h, nullptr, g_hinst, nullptr);
        SetFont(hint, f);

        FillCombos();
        LoadConfig();
        PushParams();

        SetTimer(h, 1, 60, nullptr);    // level meter
        SetTimer(h, 2, 250, nullptr);   // status / cpu
        return 0;
    }
    case WM_HSCROLL: {
        PushParams();
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDC_A || id == IDC_B || id == IDC_BYPASS) {
            PushParams();
        } else if (id == IDC_REFRESH) {
            bool wasRunning = g_running;
            StopEngine();
            FillCombos();
            if (wasRunning) StartEngine();
        } else if (id == IDC_START) {
            if (g_running) { StopEngine(); SaveConfig(); }
            else StartEngine();
        }
        return 0;
    }
    case WM_TIMER: {
        if (w == 1) {
            g_peakDisp = g_dsp.MeterPeak();
            InvalidateRect(g_hMeter, nullptr, FALSE);
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
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w;
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)GetStockObject(NULL_BRUSH);
    }
    case WM_DESTROY:
        KillTimer(h, 1);
        StopEngine();
        SaveConfig();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    g_hinst = hInst;
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"SoundLockWnd";
    RegisterClassExW(&wc);

    WNDCLASSEXW mc{};
    mc.cbSize = sizeof(mc);
    mc.lpfnWndProc = MeterProc;
    mc.hInstance = hInst;
    mc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    mc.lpszClassName = L"SLMETER";
    RegisterClassExW(&mc);

    RECT r{0, 0, kWidth + kLeft * 2, 860};
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    g_hwnd = CreateWindowExW(0, L"SoundLockWnd", L"SoundLock", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                             nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}
