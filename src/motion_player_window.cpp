// Motion player window (Win32): pick one of Pikachu's actions or motions and play it.
// F2 in the game window or Tools > Motion player toggles it.
#ifdef _WIN32
#include <Windows.h>
#include <CommCtrl.h>
#pragma comment(lib, "Comctl32.lib")

#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "hyp_action_names.h"
#include "hyp_menu.h"
#include "hyp_motion.h"

namespace {
    constexpr int ID_TABS = 201, ID_FILTER = 202, ID_LIST = 203, ID_PLAY = 204, ID_REPEAT = 205, ID_STOP = 206, ID_STATUS = 207;
    constexpr UINT_PTR TIMER_STATUS = 1;
    constexpr UINT WM_HYP_TOGGLE = WM_APP + 2;

    std::atomic<HWND> window = nullptr;
    HWND tabs, filter_edit, list, play_button, repeat_check, stop_button, status_label;
    bool motions_tab = false;

    struct Entry { int id; std::wstring text; };

    std::wstring widen(const std::string& s) {
        return std::wstring(s.begin(), s.end()); // names are ASCII
    }

    std::string lower(std::string s) {
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    bool is_pikachu_motion(int m) {
        return m >= hyp::motion::pikachu_first_motion && m <= hyp::motion::pikachu_last_motion;
    }

    std::vector<Entry> entries(const std::string& filter) {
        std::vector<Entry> out;
        if (!motions_tab) {
            for (const auto& a : hyp::motion::action_names) {
                if (!is_pikachu_motion(a.motion)) continue;
                std::string text = std::to_string(a.action) + "  " + a.name + "   (motion " + std::to_string(a.motion) + ")";
                if (filter.empty() || lower(text).find(filter) != std::string::npos) out.push_back({ a.action, widen(text) });
            }
        } else {
            std::map<int, std::string> names;
            for (const auto& a : hyp::motion::action_names) {
                if (!is_pikachu_motion(a.motion)) continue;
                std::string& n = names[a.motion];
                if (n.find(a.name) == std::string::npos) n += (n.empty() ? "" : " / ") + std::string(a.name);
            }
            for (int m = hyp::motion::pikachu_first_motion; m <= hyp::motion::pikachu_last_motion; m++) {
                auto it = names.find(m);
                std::string text = std::to_string(m) + "  " + (it != names.end() ? it->second : "(not used by any action)");
                if (filter.empty() || lower(text).find(filter) != std::string::npos) out.push_back({ m, widen(text) });
            }
        }
        return out;
    }

    void refresh_list() {
        char buf[128] = "";
        GetWindowTextA(filter_edit, buf, sizeof(buf));
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (const auto& e : entries(lower(buf))) {
            LRESULT i = SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)e.text.c_str());
            SendMessageW(list, LB_SETITEMDATA, i, e.id);
        }
        SendMessageW(list, LB_SETCURSEL, 0, 0);
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
    }

    void play_selected() {
        LRESULT sel = SendMessageW(list, LB_GETCURSEL, 0, 0);
        if (sel == LB_ERR) return;
        int id = (int)SendMessageW(list, LB_GETITEMDATA, sel, 0);
        hyp::motion::set_repeat(SendMessageW(repeat_check, BM_GETCHECK, 0, 0) == BST_CHECKED);
        if (motions_tab) hyp::motion::play_motion(id);
        else hyp::motion::play_action(id);
    }

    void update_status() {
        std::wstring s = !hyp::motion::pikachu_present() ? L"Pikachu isn't in this scene"
                       : L"Pikachu's motion now: " + std::to_wstring(hyp::motion::current_motion()) +
                         (hyp::motion::queued() > 0 ? L"   (" + std::to_wstring(hyp::motion::queued()) + L" queued)" : L"");
        wchar_t old[128] = L"";
        GetWindowTextW(status_label, old, 128);
        if (s != old) SetWindowTextW(status_label, s.c_str());
    }

    void layout(HWND hwnd) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right, h = rc.bottom;
        MoveWindow(tabs, 4, 4, w - 8, 26, TRUE);
        MoveWindow(filter_edit, 6, 36, w - 70, 24, TRUE);
        MoveWindow(play_button, w - 58, 36, 52, 24, TRUE);
        MoveWindow(list, 6, 66, w - 12, h - 126, TRUE);
        MoveWindow(repeat_check, 8, h - 54, w - 90, 22, TRUE);
        MoveWindow(stop_button, w - 78, h - 56, 72, 24, TRUE);
        MoveWindow(status_label, 8, h - 26, w - 16, 20, TRUE);
    }

    LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_CREATE: {
                HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
                tabs = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 0, 0, hwnd,
                                       (HMENU)ID_TABS, nullptr, nullptr);
                TCITEMW item{};
                item.mask = TCIF_TEXT;
                item.pszText = const_cast<wchar_t*>(L"Actions");
                SendMessageW(tabs, TCM_INSERTITEMW, 0, (LPARAM)&item);
                item.pszText = const_cast<wchar_t*>(L"Motions");
                SendMessageW(tabs, TCM_INSERTITEMW, 1, (LPARAM)&item);
                filter_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                              0, 0, 0, 0, hwnd, (HMENU)ID_FILTER, nullptr, nullptr);
                list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                       WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                                       0, 0, 0, 0, hwnd, (HMENU)ID_LIST, nullptr, nullptr);
                play_button = CreateWindowExW(0, L"BUTTON", L"Play", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                              0, 0, 0, 0, hwnd, (HMENU)ID_PLAY, nullptr, nullptr);
                repeat_check = CreateWindowExW(0, L"BUTTON", L"Repeat (replay it each time it ends)",
                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd,
                                               (HMENU)ID_REPEAT, nullptr, nullptr);
                stop_button = CreateWindowExW(0, L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, hwnd,
                                              (HMENU)ID_STOP, nullptr, nullptr);
                status_label = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_NOPREFIX, 0, 0, 0, 0, hwnd,
                                               (HMENU)ID_STATUS, nullptr, nullptr);
                for (HWND h : { tabs, filter_edit, list, play_button, repeat_check, stop_button, status_label }) {
                    SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
                }
                SetTimer(hwnd, TIMER_STATUS, 250, nullptr);
                layout(hwnd);
                refresh_list();
                return 0;
            }
            case WM_SIZE:
                layout(hwnd);
                return 0;
            case WM_TIMER:
                update_status();
                return 0;
            case WM_NOTIFY:
                if (reinterpret_cast<NMHDR*>(lp)->idFrom == ID_TABS && reinterpret_cast<NMHDR*>(lp)->code == TCN_SELCHANGE) {
                    motions_tab = SendMessageW(tabs, TCM_GETCURSEL, 0, 0) == 1;
                    refresh_list();
                }
                return 0;
            case WM_COMMAND:
                switch (LOWORD(wp)) {
                    case ID_FILTER:
                        if (HIWORD(wp) == EN_CHANGE) refresh_list();
                        break;
                    case ID_LIST:
                        if (HIWORD(wp) == LBN_DBLCLK) play_selected();
                        break;
                    case ID_PLAY:
                    case IDOK:
                        play_selected();
                        break;
                    case ID_REPEAT:
                        hyp::motion::set_repeat(SendMessageW(repeat_check, BM_GETCHECK, 0, 0) == BST_CHECKED);
                        break;
                    case ID_STOP:
                        SendMessageW(repeat_check, BM_SETCHECK, BST_UNCHECKED, 0);
                        hyp::motion::stop();
                        break;
                }
                return 0;
            case WM_HYP_TOGGLE:
                if (IsWindowVisible(hwnd)) {
                    ShowWindow(hwnd, SW_HIDE);
                } else {
                    ShowWindow(hwnd, SW_SHOW);
                    SetForegroundWindow(hwnd);
                    SetFocus(filter_edit);
                }
                return 0;
            case WM_CLOSE:
                ShowWindow(hwnd, SW_HIDE);
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void window_thread() {
        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_TAB_CLASSES };
        InitCommonControlsEx(&icc);
        WNDCLASSW wc{};
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"HypMotionPlayer";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassW(&wc);
        HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"Motion player", WS_OVERLAPPEDWINDOW,
                                    CW_USEDEFAULT, CW_USEDEFAULT, 420, 560, nullptr, nullptr, wc.hInstance, nullptr);
        window = hwnd;
        PostMessageW(hwnd, WM_HYP_TOGGLE, 0, 0);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_KEYDOWN && msg.hwnd == filter_edit && (msg.wParam == VK_UP || msg.wParam == VK_DOWN)) {
                SendMessageW(list, msg.message, msg.wParam, msg.lParam);
                continue;
            }
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
                ShowWindow(hwnd, SW_HIDE);
                continue;
            }
            if (!IsDialogMessageW(hwnd, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
}

void hyp::motion::toggle_window() {
    static std::atomic<bool> started = false;
    if (!started.exchange(true)) {
        std::thread(window_thread).detach();
    } else if (HWND hwnd = window) {
        PostMessageW(hwnd, WM_HYP_TOGGLE, 0, 0);
    }
}
#else
#include "hyp_motion.h"
void hyp::motion::toggle_window() {}
#endif
