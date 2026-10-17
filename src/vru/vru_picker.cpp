// Word picker: a small always-on-top Win32 window listing the words the game
// currently understands (its active VRU dictionary). Type to filter, then
// Enter / double-click to "say" the word. Tab in the game window toggles it.
#ifdef _WIN32
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <set>
#include <string>
#include <thread>

#include "hyp_vru.h"

namespace {
    constexpr int ID_FILTER = 101, ID_LIST = 102, ID_SAY = 103;
    constexpr UINT_PTR TIMER_REFRESH = 1;
    constexpr UINT WM_HYP_TOGGLE = WM_APP + 1;

    std::atomic<HWND> picker_window = nullptr;
    HWND filter_edit, word_list;
    unsigned shown_generation = ~0u;
    std::string shown_filter;

    std::wstring widen(const std::string& s) {
        return std::wstring(s.begin(), s.end()); // word names are ASCII
    }

    std::string lower(std::string s) {
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    std::string current_filter() {
        char buf[128] = "";
        GetWindowTextA(filter_edit, buf, sizeof(buf));
        return lower(buf);
    }

    // Rebuild the list from the dictionary: one entry per spoken form (variants
    // like "Pikachu2".."Pikachu8" collapse), item data = first slot with it.
    void refresh(bool force) {
        unsigned generation = 0;
        auto words = hyp::vru::dictionary(&generation);
        std::string filter = current_filter();
        if (!force && generation == shown_generation && filter == shown_filter) return;
        shown_generation = generation;
        shown_filter = filter;

        SendMessageW(word_list, WM_SETREDRAW, FALSE, 0);
        SendMessageW(word_list, LB_RESETCONTENT, 0, 0);
        std::set<std::string> seen;
        for (const auto& w : words) {
            if (!seen.insert(lower(w.display)).second) continue;
            if (!filter.empty() && lower(w.display).find(filter) == std::string::npos) continue;
            LRESULT idx = SendMessageW(word_list, LB_ADDSTRING, 0, (LPARAM)widen(w.display).c_str());
            SendMessageW(word_list, LB_SETITEMDATA, idx, (LPARAM)w.slot);
        }
        if (SendMessageW(word_list, LB_GETCOUNT, 0, 0) > 0) {
            SendMessageW(word_list, LB_SETCURSEL, 0, 0);
        }
        SendMessageW(word_list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(word_list, nullptr, TRUE);

        std::wstring title = words.empty() ? L"Say (no words yet)" : L"Say — " + std::to_wstring(seen.size()) + L" words";
        SetWindowTextW(picker_window, title.c_str());
    }

    void say_selected() {
        LRESULT sel = SendMessageW(word_list, LB_GETCURSEL, 0, 0);
        if (sel == LB_ERR) return;
        int slot = (int)SendMessageW(word_list, LB_GETITEMDATA, sel, 0);
        hyp::vru::say_slot(slot);
    }

    LRESULT CALLBACK picker_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_CREATE: {
                HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
                filter_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                              0, 0, 0, 0, hwnd, (HMENU)ID_FILTER, nullptr, nullptr);
                word_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                                            0, 0, 0, 0, hwnd, (HMENU)ID_LIST, nullptr, nullptr);
                HWND say = CreateWindowExW(0, L"BUTTON", L"Say", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                           0, 0, 0, 0, hwnd, (HMENU)ID_SAY, nullptr, nullptr);
                for (HWND h : { filter_edit, word_list, say }) SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
                SetTimer(hwnd, TIMER_REFRESH, 250, nullptr);
                refresh(true);
                return 0;
            }
            case WM_SIZE: {
                int w = LOWORD(lp), h = HIWORD(lp);
                MoveWindow(filter_edit, 6, 6, w - 70, 24, TRUE);
                MoveWindow(GetDlgItem(hwnd, ID_SAY), w - 58, 6, 52, 24, TRUE);
                MoveWindow(word_list, 6, 36, w - 12, h - 42, TRUE);
                return 0;
            }
            case WM_TIMER:
                refresh(false);
                return 0;
            case WM_COMMAND:
                if (LOWORD(wp) == ID_FILTER && HIWORD(wp) == EN_CHANGE) refresh(true);
                if ((LOWORD(wp) == ID_LIST && HIWORD(wp) == LBN_DBLCLK) || LOWORD(wp) == ID_SAY || LOWORD(wp) == IDOK) {
                    say_selected();
                }
                return 0;
            case WM_HYP_TOGGLE:
                if (IsWindowVisible(hwnd)) {
                    ShowWindow(hwnd, SW_HIDE);
                } else {
                    ShowWindow(hwnd, SW_SHOW);
                    SetForegroundWindow(hwnd);
                    SetFocus(filter_edit);
                    SendMessageW(filter_edit, EM_SETSEL, 0, -1);
                }
                return 0;
            case WM_CLOSE:
                ShowWindow(hwnd, SW_HIDE);
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void picker_thread() {
        WNDCLASSW wc{};
        wc.lpfnWndProc = picker_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"HypVruPicker";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassW(&wc);
        HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"Say",
                                    WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 260, 420,
                                    nullptr, nullptr, wc.hInstance, nullptr);
        picker_window = hwnd;
        PostMessageW(hwnd, WM_HYP_TOGGLE, 0, 0); // show on first toggle request

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            // Up/Down in the filter box move the list selection; Enter says it.
            if (msg.message == WM_KEYDOWN && msg.hwnd == filter_edit && (msg.wParam == VK_UP || msg.wParam == VK_DOWN)) {
                SendMessageW(word_list, msg.message, msg.wParam, msg.lParam);
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

void hyp::vru::toggle_picker() {
    static std::atomic<bool> started = false;
    if (!started.exchange(true)) {
        std::thread(picker_thread).detach(); // creates the window and shows it
    } else if (HWND hwnd = picker_window) {
        PostMessageW(hwnd, WM_HYP_TOGGLE, 0, 0);
    }
}
#else
#include "hyp_vru.h"
void hyp::vru::toggle_picker() {}
#endif
