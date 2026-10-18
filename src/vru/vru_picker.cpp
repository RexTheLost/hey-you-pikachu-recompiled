// Word picker: a small always-on-top Win32 window with two tabs.
//  - "Say now": the words the game currently understands (its active VRU
//    dictionary), grouped, under the name of the active word set. Type to
//    filter, then Enter / double-click to "say" the word.
//  - "Browse all words": every word in the game, grouped, read-only; the
//    selected word shows which word sets (scenes) use it.
// Tab in the game window toggles the picker.
#ifdef _WIN32
#include <Windows.h>
#include <CommCtrl.h>
#pragma comment(lib, "Comctl32.lib")

#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "hyp_menu.h"
#include "hyp_vru.h"

namespace {
    constexpr int ID_FILTER = 101, ID_LIST = 102, ID_SAY = 103, ID_TABS = 104, ID_HEADER = 105, ID_INFO = 106;
    constexpr UINT_PTR TIMER_REFRESH = 1;
    constexpr UINT WM_HYP_TOGGLE = WM_APP + 1;
    constexpr LPARAM HEADER_ITEM = -1;

    // Display order of word groups (see hyp::vru::group_name).
    const char* const group_order[] = {
        "Calling Pikachu", "Greetings & talk", "Commands", "Questions", "Praise & feelings",
        "Items", "Pokémon & things", "Places", "Other",
    };

    std::atomic<HWND> picker_window = nullptr;
    HWND tabs, header_label, filter_edit, word_list, say_button, info_label;
    bool browsing = false;
    unsigned shown_generation = ~0u;
    std::string shown_filter;
    bool shown_browsing = false;
    std::vector<hyp::vru::WordInfo> browse_words;

    std::wstring widen(const std::string& s) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(n > 0 ? n - 1 : 0, L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        return w;
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

    int group_rank(const std::string& group) {
        for (int i = 0; i < (int)std::size(group_order); i++) if (group == group_order[i]) return i;
        return (int)std::size(group_order);
    }

    void add_header(const std::string& group) {
        LRESULT idx = SendMessageW(word_list, LB_ADDSTRING, 0, (LPARAM)(L"— " + widen(group) + L" —").c_str());
        SendMessageW(word_list, LB_SETITEMDATA, idx, HEADER_ITEM);
    }

    void add_word(const std::string& text, LPARAM data) {
        LRESULT idx = SendMessageW(word_list, LB_ADDSTRING, 0, (LPARAM)(L"    " + widen(text)).c_str());
        SendMessageW(word_list, LB_SETITEMDATA, idx, data);
    }

    // Select the first real word at or after `from` (headers can't be selected).
    void select_word(int from, int step) {
        int count = (int)SendMessageW(word_list, LB_GETCOUNT, 0, 0);
        for (int i = from; i >= 0 && i < count; i += step) {
            if (SendMessageW(word_list, LB_GETITEMDATA, i, 0) != HEADER_ITEM) {
                SendMessageW(word_list, LB_SETCURSEL, i, 0);
                return;
            }
        }
    }

    void show_info() {
        if (!browsing) return;
        LRESULT sel = SendMessageW(word_list, LB_GETCURSEL, 0, 0);
        LPARAM data = sel == LB_ERR ? HEADER_ITEM : SendMessageW(word_list, LB_GETITEMDATA, sel, 0);
        if (data == HEADER_ITEM || data >= (LPARAM)browse_words.size()) {
            SetWindowTextW(info_label, L"Select a word to see where the game listens for it.");
            return;
        }
        const auto& w = browse_words[data];
        std::string text = w.display + "  —  used in: ";
        if (w.sets.empty()) text += "no word set (unused)";
        for (size_t i = 0; i < w.sets.size(); i++) text += (i ? ", " : "") + hyp::vru::set_name(w.sets[i]);
        if (w.variants > 1) text += "  (" + std::to_string(w.variants) + " pronunciations)";
        SetWindowTextW(info_label, widen(text).c_str());
    }

    // Rebuild the list for the current tab and filter.
    void refresh(bool force) {
        unsigned generation = 0;
        auto words = hyp::vru::dictionary(&generation);
        std::string filter = current_filter();
        if (!force && generation == shown_generation && filter == shown_filter && browsing == shown_browsing) return;
        shown_generation = generation;
        shown_filter = filter;
        shown_browsing = browsing;

        SendMessageW(word_list, WM_SETREDRAW, FALSE, 0);
        SendMessageW(word_list, LB_RESETCONTENT, 0, 0);
        size_t shown = 0;
        if (!browsing) {
            // One entry per spoken form (variants like "Pikachu2".."Pikachu8" collapse);
            // item data = first slot with it.
            std::map<std::pair<int, std::string>, std::vector<std::pair<std::string, int>>> groups;
            std::set<std::string> seen;
            for (const auto& w : words) {
                if (!seen.insert(lower(w.display)).second) continue;
                if (!filter.empty() && lower(w.display).find(filter) == std::string::npos) continue;
                std::string group = hyp::vru::group_name(w.category, w.display);
                groups[{ group_rank(group), group }].push_back({ w.display, w.slot });
            }
            for (const auto& [key, list] : groups) {
                add_header(key.second);
                for (const auto& [text, slot] : list) add_word(text, slot);
                shown += list.size();
            }
            int set = hyp::vru::active_set();
            std::string header = words.empty() ? "Pikachu isn't listening for words right now"
                                               : "Listening for: " + (set >= 0 ? hyp::vru::set_name(set) : std::string("(unknown set)")) +
                                                 "  (" + std::to_string(seen.size()) + " words)";
            SetWindowTextW(header_label, widen(header).c_str());
        } else {
            if (browse_words.empty()) browse_words = hyp::vru::all_words();
            std::map<std::pair<int, std::string>, std::vector<std::pair<std::string, int>>> groups;
            for (int i = 0; i < (int)browse_words.size(); i++) {
                const auto& w = browse_words[i];
                if (!filter.empty() && lower(w.display).find(filter) == std::string::npos) continue;
                std::string group = hyp::vru::group_name(w.category, w.display);
                groups[{ group_rank(group), group }].push_back({ w.display, i });
            }
            for (auto& [key, list] : groups) {
                std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return lower(a.first) < lower(b.first); });
                add_header(key.second);
                for (const auto& [text, index] : list) add_word(text, index);
                shown += list.size();
            }
            std::string header = browse_words.empty() ? "The word list loads once the game starts"
                                                      : "All " + std::to_string(browse_words.size()) + " words (read-only)";
            SetWindowTextW(header_label, widen(header).c_str());
        }
        select_word(0, 1);
        SendMessageW(word_list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(word_list, nullptr, TRUE);
        show_info();

        std::wstring title = L"Say — " + std::to_wstring(shown) + L" words";
        SetWindowTextW(picker_window, title.c_str());
    }

    void say_selected() {
        if (browsing) return; // read-only tab
        LRESULT sel = SendMessageW(word_list, LB_GETCURSEL, 0, 0);
        if (sel == LB_ERR) return;
        LPARAM slot = SendMessageW(word_list, LB_GETITEMDATA, sel, 0);
        if (slot != HEADER_ITEM) hyp::vru::say_slot((int)slot);
        // Give the keyboard back to the game, so Z and the other keys keep working.
        if (HWND game = (HWND)hyp::menu::native_window()) SetForegroundWindow(game);
    }

    void layout(HWND hwnd) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right, h = rc.bottom;
        MoveWindow(tabs, 4, 4, w - 8, 26, TRUE);
        MoveWindow(header_label, 8, 34, w - 16, 18, TRUE);
        MoveWindow(filter_edit, 6, 54, w - (browsing ? 12 : 70), 24, TRUE);
        MoveWindow(say_button, w - 58, 54, 52, 24, TRUE);
        int info_h = browsing ? 96 : 0;
        MoveWindow(word_list, 6, 84, w - 12, h - 90 - info_h, TRUE);
        MoveWindow(info_label, 8, h - info_h - 2, w - 16, info_h, TRUE);
        ShowWindow(say_button, browsing ? SW_HIDE : SW_SHOW);
        ShowWindow(info_label, browsing ? SW_SHOW : SW_HIDE);
    }

    LRESULT CALLBACK picker_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_CREATE: {
                HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
                tabs = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                       0, 0, 0, 0, hwnd, (HMENU)ID_TABS, nullptr, nullptr);
                TCITEMW item{};
                item.mask = TCIF_TEXT;
                item.pszText = const_cast<wchar_t*>(L"Say now");
                SendMessageW(tabs, TCM_INSERTITEMW, 0, (LPARAM)&item);
                item.pszText = const_cast<wchar_t*>(L"Browse all words");
                SendMessageW(tabs, TCM_INSERTITEMW, 1, (LPARAM)&item);
                header_label = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS | SS_NOPREFIX,
                                               0, 0, 0, 0, hwnd, (HMENU)ID_HEADER, nullptr, nullptr);
                filter_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                              0, 0, 0, 0, hwnd, (HMENU)ID_FILTER, nullptr, nullptr);
                word_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                                            0, 0, 0, 0, hwnd, (HMENU)ID_LIST, nullptr, nullptr);
                say_button = CreateWindowExW(0, L"BUTTON", L"Say", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                             0, 0, 0, 0, hwnd, (HMENU)ID_SAY, nullptr, nullptr);
                info_label = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_NOPREFIX,
                                             0, 0, 0, 0, hwnd, (HMENU)ID_INFO, nullptr, nullptr);
                for (HWND h : { tabs, header_label, filter_edit, word_list, say_button, info_label }) {
                    SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
                }
                SetTimer(hwnd, TIMER_REFRESH, 250, nullptr);
                layout(hwnd);
                refresh(true);
                return 0;
            }
            case WM_SIZE:
                layout(hwnd);
                return 0;
            case WM_TIMER:
                refresh(false);
                return 0;
            case WM_NOTIFY: {
                auto* hdr = reinterpret_cast<NMHDR*>(lp);
                if (hdr->idFrom == ID_TABS && hdr->code == TCN_SELCHANGE) {
                    browsing = SendMessageW(tabs, TCM_GETCURSEL, 0, 0) == 1;
                    layout(hwnd);
                    refresh(true);
                    SetFocus(filter_edit);
                }
                return 0;
            }
            case WM_COMMAND:
                if (LOWORD(wp) == ID_FILTER && HIWORD(wp) == EN_CHANGE) refresh(true);
                if (LOWORD(wp) == ID_LIST && HIWORD(wp) == LBN_SELCHANGE) {
                    // Keep the selection off headers.
                    LRESULT sel = SendMessageW(word_list, LB_GETCURSEL, 0, 0);
                    if (sel != LB_ERR && SendMessageW(word_list, LB_GETITEMDATA, sel, 0) == HEADER_ITEM) select_word((int)sel + 1, 1);
                    show_info();
                }
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
        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_TAB_CLASSES };
        InitCommonControlsEx(&icc);
        WNDCLASSW wc{};
        wc.lpfnWndProc = picker_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"HypVruPicker";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassW(&wc);
        HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"Say",
                                    WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 320, 520,
                                    nullptr, nullptr, wc.hInstance, nullptr);
        picker_window = hwnd;
        PostMessageW(hwnd, WM_HYP_TOGGLE, 0, 0); // show on first toggle request

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            // Up/Down in the filter box move the list selection (skipping headers); Enter says it.
            if (msg.message == WM_KEYDOWN && msg.hwnd == filter_edit && (msg.wParam == VK_UP || msg.wParam == VK_DOWN)) {
                LRESULT sel = SendMessageW(word_list, LB_GETCURSEL, 0, 0);
                int step = msg.wParam == VK_DOWN ? 1 : -1;
                select_word(sel == LB_ERR ? 0 : (int)sel + step, step);
                show_info();
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
