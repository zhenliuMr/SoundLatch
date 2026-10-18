p='src/main.cpp'
s=open(p,encoding='utf-8').read()

old_a = s.index('// 弹窗 -> 设置窗口')
old_b = s.index('static void ShowPopup()')
new = '''// 弹窗 -> 设置窗口
static void PushFromPopup() {
    SendMessageW(g_hA, BM_SETCHECK, SendMessageW(g_pA, BM_GETCHECK, 0, 0), 0);
    SendMessageW(g_hB, BM_SETCHECK, SendMessageW(g_pB, BM_GETCHECK, 0, 0), 0);
    SendMessageW(g_hBypass, BM_SETCHECK, SendMessageW(g_pByp, BM_GETCHECK, 0, 0), 0);
    for (int k = 0; k < 4; ++k) {
        SliderSet(g_pIdx[k], (int)SendMessageW(g_pSl[k], TBM_GETPOS, 0, 0));
    }
    PushParams();
}

// 设置窗口 -> 弹窗
static void SyncPopup() {
    if (!g_popup) return;
    SendMessageW(g_pA, BM_SETCHECK, SendMessageW(g_hA, BM_GETCHECK, 0, 0), 0);
    SendMessageW(g_pB, BM_SETCHECK, SendMessageW(g_hB, BM_GETCHECK, 0, 0), 0);
    SendMessageW(g_pByp, BM_SETCHECK, SendMessageW(g_hBypass, BM_GETCHECK, 0, 0), 0);
    for (int k = 0; k < 4; ++k) {
        int idx = g_pIdx[k];
        SendMessageW(g_pSl[k], TBM_SETPOS, TRUE, SliderPos(idx));
        wchar_t val[64];
        FormatValue(idx, val, 64);
        std::wstring t = std::wstring(g_slName[idx]) + L"   " + val;
        SetWindowTextW(g_pLbl[k], t.c_str());
    }
    SetWindowTextW(g_pStart, g_running ? L"停止" : L"启动");
}

static LRESULT CALLBACK PopupProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        int kLeft = 14, kW = 332;
        int y = 10;
        g_pA = CreateWindowExW(0, L"BUTTON", L"① 降低环境音 + 强化脚步声",
                               WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                               kLeft, y, kW, 24, h, (HMENU)201, g_hinst, nullptr);
        SetFont(g_pA, g_fBtn);
        y += 30;
        for (int k = 0; k < 3; ++k) {          // 环境抑制 / 环境阈值 / 脚步增强
            g_pLbl[k] = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                        kLeft + 10, y, 240, 20, h, nullptr, g_hinst, nullptr);
            SetFont(g_pLbl[k], g_fLabel);
            g_pSl[k] = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                                       WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                                       kLeft + 10, y + 20, kW - 20, 26, h, (HMENU)(300 + k), g_hinst, nullptr);
            y += 52;
        }
        g_pB = CreateWindowExW(0, L"BUTTON", L"② 锁定最大输出音量（限幅）",
                               WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                               kLeft, y, kW, 24, h, (HMENU)202, g_hinst, nullptr);
        SetFont(g_pB, g_fBtn);
        y += 30;
        g_pLbl[3] = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                    kLeft + 10, y, 240, 20, h, nullptr, g_hinst, nullptr);
        SetFont(g_pLbl[3], g_fLabel);
        g_pSl[3] = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                                   WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                                   kLeft + 10, y + 20, kW - 20, 26, h, (HMENU)303, g_hinst, nullptr);
        y += 56;
        g_pMeter = CreateWindowExW(0, L"SLMETER", L"", WS_CHILD | WS_VISIBLE,
                                   kLeft, y, kW, 24, h, (HMENU)204, g_hinst, nullptr);
        y += 30;
        g_pStart = CreateWindowExW(0, L"BUTTON", L"启动", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   kLeft, y, 90, 30, h, (HMENU)205, g_hinst, nullptr);
        SetFont(g_pStart, g_fBtn);
        g_pByp = CreateWindowExW(0, L"BUTTON", L"旁通(A/B)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                 kLeft + 100, y, 100, 30, h, (HMENU)206, g_hinst, nullptr);
        SetFont(g_pByp, g_fLabel);
        g_pSet = CreateWindowExW(0, L"BUTTON", L"设置…", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 kLeft + 210, y, 90, 30, h, (HMENU)207, g_hinst, nullptr);
        SetFont(g_pSet, g_fLabel);
        y += 38;
        g_pStatus = CreateWindowExW(0, L"STATIC", L"已停止",
                                    WS_CHILD | WS_VISIBLE | SS_SIMPLE | SS_NOPREFIX,
                                    kLeft, y, kW, 20, h, nullptr, g_hinst, nullptr);
        SetFont(g_pStatus, g_fLabel);

        for (int k = 0; k < 4; ++k) {
            int idx = g_pIdx[k];
            int hi = (idx == 1) ? 42 : (idx == 4 ? 240 : 100);
            SendMessageW(g_pSl[k], TBM_SETRANGE, TRUE, MAKELONG(0, hi));
            SendMessageW(g_pSl[k], TBM_SETPOS, TRUE, SliderPos(idx));
        }
        SyncPopup();
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w;
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    case WM_HSCROLL:
        PushFromPopup();
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == 201 || id == 202 || id == 206) {
            PushFromPopup();
        } else if (id == 205) {
            if (g_running) StopEngine(); else StartEngine(true);
            SaveConfig();
        } else if (id == 207) {
            ShowWindow(h, SW_HIDE);
            ShowWindow(g_hwnd, SW_SHOW);
            SetForegroundWindow(g_hwnd);
        }
        return 0;
    }
    case WM_TIMER:
        if (w == 1) {
            if (g_pMeter) InvalidateRect(g_pMeter, nullptr, FALSE);
            wchar_t b[256];
            if (g_running) swprintf(b, 256, L"%.0f ms · 欠载 %d · CPU %.0f%%",
                                    g_engine.LatencyMs(), g_engine.Underruns(), g_cpuPct);
            else swprintf(b, 256, L"未运行 · 点击「启动」开始");
            SetWindowTextW(g_pStatus, b);
            InvalidateRect(g_pStatus, nullptr, TRUE);
            return 0;
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

'''
s = s[:old_a] + new + s[old_b:]

# 映射表与控件数组
s = s.replace('''static HWND g_pSl[5], g_pLbl[5];
static const wchar_t* g_slName[5] = { L"环境抑制", L"环境阈值", L"脚步增强", L"瞬态灵敏度", L"最大输出" };''',
'''static HWND g_pSl[4], g_pLbl[4];
static const int    g_pIdx[4]   = { 0, 1, 2, 4 };   // envAmt, envThr, stepAmt, ceil
static const wchar_t* g_slName[5] = { L"环境抑制", L"环境阈值", L"脚步增强", L"瞬态灵敏度", L"最大输出" };''')

open(p,'w',encoding='utf-8').write(s)
print('ok')
