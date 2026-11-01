// Copyright (C) 2026 AutoGavy
// SPDX-License-Identifier: AGPL-3.0-only
// See LICENSE in the project root for the full license text.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <psapi.h>
#include <tlhelp32.h>

#include "Core.hpp"
#include "Platform.hpp"
#include "ProcessResources.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace regionlock;
#ifdef REGIONLOCK_SMOKE
constexpr wchar_t kClassName[] = L"RegionLock.Native.Smoke.Window.v1";
#else
constexpr wchar_t kClassName[] = L"RegionLock.Native.Window.v1";
#endif
constexpr wchar_t kWindowTitle[] = L"Region Lock";
constexpr wchar_t kExitLabel[] = L"Unlock regions and exit";
constexpr UINT kJobDone = WM_APP + 1;
constexpr UINT kProbeDone = WM_APP + 2;
constexpr UINT kTrayMessage = WM_APP + 3;
constexpr UINT kSmokeMessage = WM_APP + 4;
constexpr UINT_PTR kSmokeTimer = 101;
constexpr UINT kTrayId = 1;
constexpr int kIconId = 101;
constexpr COLORREF kBackground = RGB(12, 14, 18);
constexpr COLORREF kSurface = RGB(18, 21, 27);
constexpr COLORREF kText = RGB(226, 232, 240);
constexpr COLORREF kMuted = RGB(137, 150, 171);
constexpr COLORREF kBorder = RGB(39, 47, 61);
enum ControlId : int {
    GamePath = 1001, Browse, Detect, RegionList, SelectAll, SelectNone,
    Apply, Unlock, Test, Update, Cancel, ShowWindowCommand, ExitCommand
};
enum class Job { Initial, Apply, Unlock, Update, Probe, Exit };
#ifdef REGIONLOCK_SMOKE
constexpr bool kSmokeBuild = true;
#else
constexpr bool kSmokeBuild = false;
#endif

std::filesystem::path ModuleDirectory() {
    std::wstring text(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, text.data(), static_cast<DWORD>(text.size()));
    if (!length || length >= text.size()) throw std::runtime_error("Cannot locate application directory");
    text.resize(length);
    return std::filesystem::path(text).parent_path();
}
std::wstring WindowText(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring result(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(window, result.data(), length + 1);
    result.resize(static_cast<std::size_t>(length));
    return result;
}
std::wstring ErrorText(const std::exception& error) {
    try { return Utf8ToWide(error.what()); }
    catch (...) { return L"An unexpected error occurred."; }
}
std::wstring Number(std::size_t value) { return std::to_wstring(value); }
int MessageBoxEnglish(HWND owner, const wchar_t* text, const wchar_t* title, UINT flags) {
    return MessageBoxExW(owner, text, title, flags, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
}
std::uint64_t CpuTime() {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) return 0;
    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
    return k.QuadPart + u.QuadPart;
}
int ThreadBasePriority() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 entry{sizeof(entry)};
    int priority{};
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32ThreadID == GetCurrentThreadId()) { priority = entry.tpBasePri; break; }
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return priority;
}

struct Result {
    Job job{};
    bool success{};
    bool canceled{};
    std::wstring message;
    std::optional<Catalog> catalog;
    std::optional<Settings> settings;
    std::optional<FirewallState> firewall;
};
struct Options {
    std::filesystem::path report;
    std::filesystem::path screenshot;
};

class App {
public:
    explicit App(Options options) : options_(std::move(options)) {}
    ~App() {
        if (worker_.joinable()) { worker_.request_stop(); worker_.join(); }
        DeleteTray();
        if (font_) DeleteObject(font_);
        if (titleFont_) DeleteObject(titleFont_);
        if (boldFont_) DeleteObject(boldFont_);
        if (background_) DeleteObject(background_);
        if (surface_) DeleteObject(surface_);
        if (stateImages_) ImageList_Destroy(stateImages_);
        if (smokeSecondProcess_) CloseHandle(smokeSecondProcess_);
    }
    HWND Window() const { return window_; }
    bool SmokePassed() const { return smokePassed_; }
    bool Create(HINSTANCE instance) {
        instance_ = instance;
        background_ = CreateSolidBrush(kBackground);
        surface_ = CreateSolidBrush(kSurface);
        WNDCLASSEXW cls{sizeof(cls)};
        cls.style = CS_HREDRAW | CS_VREDRAW;
        cls.lpfnWndProc = WindowProc;
        cls.hInstance = instance;
        cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(kIconId));
        cls.hIconSm = cls.hIcon;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hbrBackground = background_;
        cls.lpszClassName = kClassName;
        if (!RegisterClassExW(&cls)) return false;
        const UINT dpi = GetDpiForSystem();
        const int width = MulDiv(1040, static_cast<int>(dpi), 96);
        const int height = MulDiv(790, static_cast<int>(dpi), 96);
        POINT cursor{};
        GetCursorPos(&cursor);
        MONITORINFO monitor{sizeof(monitor)};
        const bool haveMonitor = GetMonitorInfoW(
            MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor) != FALSE;
        window_ = CreateWindowExW(WS_EX_CONTROLPARENT, kClassName, kWindowTitle,
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            haveMonitor ? monitor.rcWork.left : CW_USEDEFAULT,
            haveMonitor ? monitor.rcWork.top : CW_USEDEFAULT, width, height,
            nullptr, nullptr, instance, this);
        if (window_ && haveMonitor) {
            // Resolve the target monitor's DPI before centering the hidden window.
            const int windowDpi = static_cast<int>(GetDpiForWindow(window_));
            const int scaledWidth = MulDiv(1040, windowDpi, 96);
            const int scaledHeight = MulDiv(790, windowDpi, 96);
            SetWindowPos(window_, nullptr,
                monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - scaledWidth) / 2,
                monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - scaledHeight) / 2,
                scaledWidth, scaledHeight, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return window_ != nullptr;
    }

private:
    HWND window_{};
    HINSTANCE instance_{};
    HBRUSH background_{}, surface_{};
    HIMAGELIST stateImages_{};
    HFONT font_{}, titleFont_{}, boldFont_{};
    UINT dpi_{96};
    HWND title_{}, subtitle_{}, badge_{}, gameLabel_{}, path_{}, browse_{}, detect_{};
    HWND hint_{}, regionsLabel_{}, selectionLabel_{}, list_{}, all_{}, none_{};
    HWND dataLabel_{}, apply_{}, unlock_{}, test_{}, update_{}, cancel_{}, status_{};
    Paths paths_;
    Settings settings_;
    Settings appliedSettings_;
    Catalog catalog_;
    std::jthread worker_;
    ProcessResources resources_;
    Job job_{Job::Initial};
    bool busy_{}, exiting_{}, closing_{}, suppressChecks_{}, trayAdded_{}, loaded_{};
    bool firewallKnown_{}, firewallEnabled_{}, preferencesReady_{}, trayMenuOpen_{};
    std::size_t activeRules_{}, probeCompleted_{}, legacyRules_{};
    std::vector<std::wstring> probeDetails_;
    UINT taskbarCreated_{};
    Options options_;
    bool smokePassed_{true}, smokeClosed_{}, smokeMinimized_{}, smokeRestored_{};
    bool smokeDark_{true}, smokeEnglish_{true}, smokeMaximizedRestore_{};
    bool smokeBusyDark_{}, smokeBusyReadOnly_{}, smokeReadyEditable_{}, smokeSingleInstance_{};
    bool smokeSecondRestored_{};
    bool smokeBackground_{}, smokeEco_{}, smokeResourceRestored_{}, smokeExitNormal_{};
    bool smokeCancellationError_{}, smokeExplicitCancellation_{};
    bool smokeFirewallStatus_{};
    DWORD smokeOriginalPriority_{}, smokeTrayPriority_{};
    ULONG smokeTrayMemoryPriority_{};
    ULONG smokeOriginalMemoryPriority_{};
    bool smokeMemoryReadable_{};
    int smokeOriginalThreadPriority_{}, smokeTrayThreadPriority_{};
    PROCESS_POWER_THROTTLING_STATE smokeOriginalPower_{};
    bool smokePowerReadable_{};
    IO_COUNTERS smokeIoBaseline_{};
    ULONGLONG smokeHiddenIoBytes_{};
    HANDLE smokeSecondProcess_{};
    ULONGLONG smokeSecondStarted_{};
    int smokeStage_{};
    std::uint64_t smokeCpuBaseline_{}, smokeVisibleCpu_{}, smokeHiddenCpu_{};
    SIZE_T smokeWorkingSet_{}, smokeTrayWorkingSet_{}, smokePrivateBytes_{};
    bool smokeScreenshot_{};

    int S(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        App* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            app->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        try {
            return app ? app->Handle(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
        } catch (const std::exception& error) {
            MessageBoxEnglish(window, ErrorText(error).c_str(), L"Operation incomplete", MB_OK | MB_ICONERROR);
            return message == WM_CREATE ? -1 : 0;
        }
    }
    HWND Control(const wchar_t* type, const wchar_t* text, DWORD style, int id = 0, DWORD extra = 0) {
        HWND control = CreateWindowExW(extra, type, text, WS_CHILD | WS_VISIBLE | style,
            0, 0, 1, 1, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        if (!control) throw std::runtime_error("Cannot create window control");
        return control;
    }
    void CreateControls() {
        dpi_ = GetDpiForWindow(window_);
        if (HMENU menu = GetSystemMenu(window_, FALSE)) {
            ModifyMenuW(menu, SC_CLOSE, MF_BYCOMMAND | MF_STRING, SC_CLOSE, L"Unlock regions and exit\tAlt+F4");
        }
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        const COLORREF caption = kBackground;
        const COLORREF captionText = kText;
        DwmSetWindowAttribute(window_, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
        DwmSetWindowAttribute(window_, DWMWA_TEXT_COLOR, &captionText, sizeof(captionText));
        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
        // Explorer runs at medium integrity while this window is elevated.
        // Only allow the pointer-free taskbar recreation notification through UIPI.
        if (taskbarCreated_) ChangeWindowMessageFilterEx(window_, taskbarCreated_, MSGFLT_ALLOW, nullptr);
        title_ = Control(L"STATIC", L"Region Lock", SS_LEFT);
        subtitle_ = Control(L"STATIC", L"Choose where Battlefield 6 can match.", SS_LEFT);
        badge_ = Control(L"STATIC", L"Loading...", SS_RIGHT);
        gameLabel_ = Control(L"STATIC", L"Game executable", SS_LEFT);
        path_ = Control(L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP, GamePath);
        browse_ = Control(L"BUTTON", L"Browse...", BS_OWNERDRAW | WS_TABSTOP, Browse);
        detect_ = Control(L"BUTTON", L"Detect game", BS_OWNERDRAW | WS_TABSTOP, Detect);
        hint_ = Control(L"STATIC", L"Minimize to the system tray. Close: unlock regions and exit.", SS_LEFT);
        regionsLabel_ = Control(L"STATIC", L"Allowed regions", SS_LEFT);
        selectionLabel_ = Control(L"STATIC", L"", SS_RIGHT);
        all_ = Control(L"BUTTON", L"All", BS_OWNERDRAW | WS_TABSTOP, SelectAll);
        none_ = Control(L"BUTTON", L"None", BS_OWNERDRAW | WS_TABSTOP, SelectNone);
        list_ = Control(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS | WS_TABSTOP,
            RegionList);
        ListView_SetExtendedListViewStyle(list_, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
        ListView_SetBkColor(list_, kSurface);
        ListView_SetTextBkColor(list_, kSurface);
        ListView_SetTextColor(list_, kText);
        SetWindowTheme(list_, L"DarkMode_Explorer", nullptr);
        if (HWND tooltip = ListView_GetToolTips(list_)) {
            SetWindowTheme(tooltip, L"", L"");
            SendMessageW(tooltip, TTM_SETTIPBKCOLOR, kSurface, 0);
            SendMessageW(tooltip, TTM_SETTIPTEXTCOLOR, kText, 0);
        }
        SetWindowTheme(path_, L"", L"");
        HWND header = ListView_GetHeader(list_);
        SetWindowSubclass(header, HeaderProc, 1, reinterpret_cast<DWORD_PTR>(this));
        SetWindowTheme(header, L"", L"");
        CreateCheckImages();
        const wchar_t* columns[] = {L"Location", L"AWS region", L"IPv4 ranges", L"IPv6 ranges", L"Latency", L"Replies"};
        for (int i = 0; i < 6; ++i) {
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
            column.pszText = const_cast<wchar_t*>(columns[i]);
            column.cx = S(120);
            column.fmt = i >= 2 ? LVCFMT_RIGHT : LVCFMT_LEFT;
            ListView_InsertColumn(list_, i, &column);
        }
        dataLabel_ = Control(L"STATIC", L"Loading region data...", SS_LEFT);
        apply_ = Control(L"BUTTON", L"Apply lock", BS_OWNERDRAW | WS_TABSTOP, Apply);
        unlock_ = Control(L"BUTTON", L"Unlock", BS_OWNERDRAW | WS_TABSTOP, Unlock);
        test_ = Control(L"BUTTON", L"Test network", BS_OWNERDRAW | WS_TABSTOP, Test);
        update_ = Control(L"BUTTON", L"Update data", BS_OWNERDRAW | WS_TABSTOP, Update);
        cancel_ = Control(L"BUTTON", L"Cancel", BS_OWNERDRAW | WS_TABSTOP, Cancel);
        status_ = Control(L"STATIC", L"", SS_LEFT);
        CreateFonts();
        Layout();
        SetBusy(true, Job::Initial);
        BeginInitial();
    }
    void CreateCheckImages() {
        const int side = S(18);
        HIMAGELIST images = ImageList_Create(side, side, ILC_COLOR32, 2, 0);
        if (!images) throw std::runtime_error("Cannot create checkbox images");
        for (int checked = 0; checked < 2; ++checked) {
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = side;
            info.bmiHeader.biHeight = -side;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* pixels{};
            HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
            HDC dc = CreateCompatibleDC(nullptr);
            if (!bitmap || !dc) {
                if (bitmap) DeleteObject(bitmap);
                if (dc) DeleteDC(dc);
                ImageList_Destroy(images);
                throw std::runtime_error("Cannot create checkbox bitmap");
            }
            const auto previousBitmap = SelectObject(dc, bitmap);
            RECT rect{0, 0, side, side};
            HBRUSH transparent = CreateSolidBrush(RGB(255, 0, 255));
            FillRect(dc, &rect, transparent);
            DeleteObject(transparent);
            HBRUSH fill = CreateSolidBrush(checked ? RGB(42, 99, 192) : RGB(22, 27, 35));
            HPEN edge = CreatePen(PS_SOLID, 1, checked ? RGB(71, 132, 229) : RGB(89, 104, 126));
            const auto previousBrush = SelectObject(dc, fill), previousPen = SelectObject(dc, edge);
            RoundRect(dc, S(2), S(2), side - S(2), side - S(2), S(4), S(4));
            SelectObject(dc, previousBrush); SelectObject(dc, previousPen);
            DeleteObject(fill); DeleteObject(edge);
            if (checked) {
                HPEN tick = CreatePen(PS_SOLID, std::max(1, S(2)), RGB(236, 242, 251));
                const auto oldTick = SelectObject(dc, tick);
                MoveToEx(dc, S(5), S(9), nullptr); LineTo(dc, S(8), S(12)); LineTo(dc, S(13), S(6));
                SelectObject(dc, oldTick); DeleteObject(tick);
            }
            GdiFlush();
            auto* colors = static_cast<std::uint32_t*>(pixels);
            for (int i = 0; i < side * side; ++i) {
                const auto rgb = colors[i] & 0x00ffffffU;
                colors[i] = rgb == 0x00ff00ffU ? 0 : rgb | 0xff000000U;
            }
            SelectObject(dc, previousBitmap); DeleteDC(dc);
            const int added = ImageList_Add(images, bitmap, nullptr);
            DeleteObject(bitmap);
            if (added < 0) { ImageList_Destroy(images); throw std::runtime_error("Cannot store checkbox bitmap"); }
        }
        const auto previous = ListView_SetImageList(list_, images, LVSIL_STATE);
        if (previous) ImageList_Destroy(previous);
        stateImages_ = images;
    }
    void PaintHeader(HWND header, HDC dc) {
        RECT client{}; GetClientRect(header, &client);
        FillRect(dc, &client, surface_);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, kMuted);
        const auto previousFont = SelectObject(dc, font_);
        HPEN separator = CreatePen(PS_SOLID, 1, kBorder);
        const auto previousPen = SelectObject(dc, separator);
        for (int i = 0; i < Header_GetItemCount(header); ++i) {
            RECT rect{};
            if (!Header_GetItemRect(header, i, &rect)) continue;
            wchar_t text[128]{};
            HDITEMW item{}; item.mask = HDI_TEXT | HDI_FORMAT; item.pszText = text; item.cchTextMax = static_cast<int>(std::size(text));
            Header_GetItem(header, i, &item);
            MoveToEx(dc, rect.right - 1, rect.top + S(7), nullptr); LineTo(dc, rect.right - 1, rect.bottom - S(7));
            rect.left += S(10); rect.right -= S(10);
            DrawTextW(dc, text, -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | ((item.fmt & HDF_RIGHT) ? DT_RIGHT : DT_LEFT));
        }
        MoveToEx(dc, 0, client.bottom - 1, nullptr); LineTo(dc, client.right, client.bottom - 1);
        SelectObject(dc, previousPen); DeleteObject(separator); SelectObject(dc, previousFont);
    }
    static LRESULT CALLBACK HeaderProc(HWND header, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data) {
        auto* app = reinterpret_cast<App*>(data);
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{}; HDC dc = BeginPaint(header, &paint);
            app->PaintHeader(header, dc); EndPaint(header, &paint); return 0;
        }
        if (message == WM_PRINTCLIENT) { app->PaintHeader(header, reinterpret_cast<HDC>(wparam)); return 0; }
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_NCDESTROY) RemoveWindowSubclass(header, HeaderProc, 1);
        return DefSubclassProc(header, message, wparam, lparam);
    }
    void CreateFonts() {
        HFONT previous = font_, previousTitle = titleFont_, previousBold = boldFont_;
        font_ = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        titleFont_ = CreateFontW(-S(25), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        boldFont_ = CreateFontW(-S(14), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        EnumChildWindows(window_, [](HWND child, LPARAM parameter) -> BOOL {
            SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(parameter), TRUE); return TRUE;
        }, reinterpret_cast<LPARAM>(font_));
        SendMessageW(title_, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont_), TRUE);
        for (HWND child : {gameLabel_, regionsLabel_, badge_, apply_})
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(boldFont_), TRUE);
        if (previous) DeleteObject(previous);
        if (previousTitle) DeleteObject(previousTitle);
        if (previousBold) DeleteObject(previousBold);
    }
    void Layout() {
        if (!title_) return;
        RECT client{}; GetClientRect(window_, &client);
        const int width = MulDiv(client.right, 96, static_cast<int>(dpi_));
        const int height = MulDiv(client.bottom, 96, static_cast<int>(dpi_));
        auto place = [&](HWND control, int x, int y, int w, int h) {
            MoveWindow(control, S(x), S(y), S(std::max(1, w)), S(std::max(1, h)), TRUE);
        };
        place(title_, 24, 21, width - 235, 36);
        place(subtitle_, 25, 62, width - 60, 24);
        place(badge_, width - 190, 32, 164, 26);
        place(gameLabel_, 25, 100, 180, 23);
        place(path_, 24, 127, width - 274, 33);
        place(browse_, width - 240, 127, 104, 33);
        place(detect_, width - 126, 127, 102, 33);
        place(hint_, 25, 169, width - 50, 24);
        place(regionsLabel_, 25, 212, 190, 26);
        place(selectionLabel_, 210, 212, width - 402, 26);
        place(all_, width - 176, 206, 72, 31);
        place(none_, width - 96, 206, 72, 31);
        place(list_, 24, 245, width - 48, height - 389);
        place(dataLabel_, 25, height - 133, width - 50, 25);
        place(apply_, 24, height - 97, 136, 39);
        place(unlock_, 172, height - 97, 108, 39);
        place(test_, 292, height - 97, 112, 39);
        place(update_, 416, height - 97, 140, 39);
        place(cancel_, width - 140, height - 97, 116, 39);
        place(status_, 25, height - 46, width - 50, 40);
        const int fixed[] = {0, 195, 92, 92, 95, 95};
        int remainder = width - 48 - 22;
        for (int i = 1; i < 6; ++i) remainder -= fixed[i];
        ListView_SetColumnWidth(list_, 0, S(std::max(remainder, 180)));
        for (int i = 1; i < 6; ++i) ListView_SetColumnWidth(list_, i, S(fixed[i]));
    }
    void SetStatus(const std::wstring& message) {
        std::wstring text = message;
        if (legacyRules_) text += L"\nLegacy BF6_Region_Lock rules detected. Unlock in the old tool first; those rules can still affect matchmaking.";
        SetWindowTextW(status_, text.c_str());
    }
    std::wstring FirewallLabel() const {
        return !firewallKnown_ ? L"State unknown" : !activeRules_ ? L"Unlocked"
            : !firewallEnabled_ ? L"Firewall off" : L"Lock active";
    }
    void UpdateBadge() {
        const auto text = FirewallLabel();
        SetWindowTextW(badge_, text.c_str());
        InvalidateRect(badge_, nullptr, TRUE);
        UpdateTrayTip();
    }
    void SetBusy(bool busy, Job job) {
        busy_ = busy;
        job_ = job;
        for (HWND control : {browse_, detect_, all_, none_}) EnableWindow(control, !busy);
        // A disabled native list view paints a system-colored background.
        // Keep it enabled for dark rendering and scrolling; veto check edits while busy.
        EnableWindow(apply_, !busy && loaded_);
        EnableWindow(unlock_, !busy);
        EnableWindow(test_, !busy && loaded_);
        EnableWindow(update_, !busy);
        ShowWindow(cancel_, busy && (job == Job::Probe || job == Job::Update) ? SW_SHOW : SW_HIDE);
        EnableWindow(cancel_, busy);
        InvalidateRect(apply_, nullptr, TRUE);
        UpdateResourceMode();
    }
    std::vector<std::string> Selection() const {
        std::vector<std::string> selected;
        for (std::size_t i = 0; i < catalog_.regions.size(); ++i)
            if (ListView_GetCheckState(list_, static_cast<int>(i))) selected.push_back(catalog_.regions[i].code);
        return selected;
    }
    Settings CurrentSettings() const {
        Settings settings;
        settings.gamePath = WindowText(path_);
        settings.allowedRegions = Selection();
        return settings;
    }
    void SelectionChanged(bool notify) {
        const auto selected = Selection();
        const auto text = L"Selected " + Number(selected.size()) + L" / " + Number(catalog_.regions.size()) + L" regions";
        SetWindowTextW(selectionLabel_, text.c_str());
        if (notify && !busy_) SetStatus(activeRules_ ? L"Selection changed. Apply lock to update; the previous rules remain active."
                                                   : L"Choose the regions to keep available, then select Apply lock.");
    }
    void Populate(const std::vector<std::string>& selected) {
        suppressChecks_ = true;
        SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(list_);
        probeDetails_.assign(catalog_.regions.size(), L"");
        for (std::size_t i = 0; i < catalog_.regions.size(); ++i) {
            const auto& region = catalog_.regions[i];
            LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = static_cast<int>(i);
            item.pszText = const_cast<wchar_t*>(region.name.c_str());
            ListView_InsertItem(list_, &item);
            std::wstring code = Utf8ToWide(region.code);
            ListView_SetItemText(list_, item.iItem, 1, code.data());
            const auto v6 = static_cast<std::size_t>(std::count_if(region.prefixes.begin(), region.prefixes.end(),
                [](const Prefix& prefix) { return prefix.ipv6; }));
            std::wstring v4text = Number(region.prefixes.size() - v6), v6text = Number(v6);
            ListView_SetItemText(list_, item.iItem, 2, v4text.data());
            ListView_SetItemText(list_, item.iItem, 3, v6text.data());
            ListView_SetItemText(list_, item.iItem, 4, const_cast<wchar_t*>(L"—"));
            ListView_SetItemText(list_, item.iItem, 5, const_cast<wchar_t*>(L"—"));
            ListView_SetCheckState(list_, item.iItem, std::find(selected.begin(), selected.end(), region.code) != selected.end());
        }
        SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list_, nullptr, TRUE);
        suppressChecks_ = false;
        loaded_ = !catalog_.regions.empty();
        const auto info = L"Catalog: " + Utf8ToWide(catalog_.created) + L" UTC    ·    " + Number(catalog_.regions.size())
            + L" regions    |    Selected game only: UDP 21000-21299";
        SetWindowTextW(dataLabel_, info.c_str());
        SelectionChanged(false);
    }
    void SelectEvery(bool value) {
        suppressChecks_ = true;
        SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
        for (std::size_t i = 0; i < catalog_.regions.size(); ++i) ListView_SetCheckState(list_, static_cast<int>(i), value);
        SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list_, nullptr, TRUE);
        suppressChecks_ = false;
        SelectionChanged(true);
    }
    template<class Function> void RunJob(Job job, Function function) {
        if (worker_.joinable()) worker_.join();
        SetBusy(true, job);
        try { worker_ = std::jthread([this, job, function = std::move(function)](std::stop_token stop) mutable {
            auto result = std::make_unique<Result>();
            result->job = job;
            try {
                function(*result, stop);
                result->success = true;
            } catch (const OperationCancelled&) {
                result->canceled = true;
                result->message = L"Task canceled.";
            } catch (const std::exception& error) {
                // A stop request does not turn a failed commit/rollback into
                // cancellation. Preserve the real error and recovery details.
                result->message = ErrorText(error);
                if constexpr (!kSmokeBuild) {
                    if (job == Job::Apply || job == Job::Unlock || job == Job::Update || job == Job::Exit)
                        try { result->firewall = QueryFirewall(); } catch (...) {}
                }
            } catch (...) { result->message = L"An unexpected error prevented the operation from completing."; }
            if (PostMessageW(window_, kJobDone, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
        }); } catch (...) { SetBusy(false, job); throw; }
    }
    static void RefreshFirewall(Result& result, std::size_t fallbackCount) {
        result.firewall = FirewallState{fallbackCount, false, L"", 0, false};
        try { result.firewall = QueryFirewall(); }
        catch (const std::exception& error) { result.message += L" State verification failed: " + ErrorText(error); }
    }
    static void SavePreferences(Result& result, const Paths& paths, const Settings& settings) {
        try { SaveSettings(paths, settings); }
        catch (const std::exception& error) { result.message += L" Preferences could not be saved: " + ErrorText(error); }
    }
    void BeginInitial() {
        SetStatus(L"Loading region data and checking rules...");
        RunJob(Job::Initial, [this](Result& result, std::stop_token) {
            if constexpr (kSmokeBuild) {
                Settings settings;
                settings.gamePath = L"C:\\Games\\Battlefield 6\\bf6.exe";
                settings.allowedRegions = {"ap-northeast-1", "ap-east-1"};
                auto seed = ModuleDirectory() / L"ip-ranges.json";
                if (!std::filesystem::exists(seed)) seed = std::filesystem::current_path() / L"assets" / L"ip-ranges.json";
                result.catalog = ParseCatalog(ReadFile(seed));
                result.settings = settings;
                result.firewall = FirewallState{0, true, L"Smoke mode: no firewall mutations"};
                result.message = L"Choose regions and select Apply lock. Restart Battlefield 6 after changing regions.";
            } else {
                paths_ = GetPaths();
                std::wstring warning;
                // Recover stale rules independently of settings/data validity.
                auto state = QueryFirewall();
                if (state.ruleCount) {
                    RemoveOwnedRules();
                    state = QueryFirewall();
                    warning += L"Recovered rules left by a previous unexpected exit. ";
                }
                result.firewall = state;
                try {
                    Settings settings = LoadSettings(paths_);
                    if (settings.gamePath.empty()) settings.gamePath = FindGameExecutable();
                    result.settings = std::move(settings);
                } catch (const std::exception& error) {
                    warning += L"Preferences could not be read. Choose the executable and regions again to apply. " + ErrorText(error) + L" ";
                }
                if (std::filesystem::exists(paths_.catalogFile)) {
                    try { result.catalog = ParseCatalog(ReadFile(paths_.catalogFile)); }
                    catch (const std::exception&) { warning += L"Invalid cached catalog; using the bundled data instead. "; }
                }
                if (!result.catalog) result.catalog = ParseCatalog(ReadFile(ModuleDirectory() / L"ip-ranges.json"));
                result.message = warning + (state.firewallEnabled ?
                    L"Choose regions and select Apply lock. Restart Battlefield 6 after changing regions." : state.detail);
            }
        });
    }
    void BeginApply() {
        if (busy_ || !loaded_) return;
        if constexpr (kSmokeBuild) { SetStatus(L"Firewall changes are disabled in the smoke-test build. "); return; }
        Settings requested = CurrentSettings();
        if (requested.allowedRegions.empty()) {
            MessageBoxEnglish(window_, L"Select at least one region to keep available. To remove restrictions, select Unlock.", L"Choose regions", MB_OK | MB_ICONINFORMATION);
            return;
        }
        auto catalog = catalog_;
        auto paths = paths_;
        SetStatus(L"Applying rules. Previous settings remain protected until replacement completes...");
        RunJob(Job::Apply, [requested = std::move(requested), catalog = std::move(catalog), paths](Result& result, std::stop_token) mutable {
            requested.gamePath = ValidateGameExecutable(requested.gamePath);
            auto rules = BuildRules(catalog, requested.allowedRegions, requested.gamePath, NewGeneration());
            if (rules.empty()) RemoveOwnedRules(); else ApplyFirewallRules(rules);
            result.settings = requested;
            result.message = rules.empty() ? L"All regions are allowed. Lock rules have been removed." :
                L"Lock active for the selected game. Restart Battlefield 6 after changing regions.";
            RefreshFirewall(result, rules.size());
            SavePreferences(result, paths, requested);
        });
    }
    void BeginUnlock() {
        if (busy_) return;
        if constexpr (kSmokeBuild) { SetStatus(L"Firewall changes are disabled in the smoke-test build. "); return; }
        SetStatus(L"Removing this tool's lock rules...");
        RunJob(Job::Unlock, [](Result& result, std::stop_token) {
            RemoveOwnedRules();
            result.message = L"Unlocked. This tool no longer restricts access to any region.";
            RefreshFirewall(result, 0);
        });
    }
    void BeginUpdate() {
        if (busy_) return;
        if constexpr (kSmokeBuild) { SetStatus(L"Network access and firewall changes are disabled in the smoke-test build. "); return; }
        const bool active = activeRules_ != 0;
        if (!firewallKnown_) {
            MessageBoxEnglish(window_, L"The current rule state is unknown. Select Unlock before updating region data.", L"Cannot update", MB_OK | MB_ICONWARNING);
            return;
        }
        auto oldCatalog = catalog_;
        auto selected = loaded_ ? Selection() : settings_.allowedRegions;
        auto applied = appliedSettings_;
        auto paths = paths_;
        SetStatus(L"Downloading and validating the official AWS region data...");
        RunJob(Job::Update, [active, oldCatalog = std::move(oldCatalog), selected = std::move(selected), applied, paths]
            (Result& result, std::stop_token stop) {
            std::string data = DownloadCatalog(stop);
            Catalog updated = ParseCatalog(data);
            if (stop.stop_requested()) throw OperationCancelled();
            std::size_t count = 0;
            if (active) {
                auto replacement = BuildRules(updated, applied.allowedRegions, applied.gamePath, NewGeneration());
                count = replacement.size();
                if (replacement.empty()) RemoveOwnedRules(); else ApplyFirewallRules(replacement);
            }
            try { WriteFileAtomic(paths.catalogFile, data); }
            catch (const std::exception& original) {
                if (active) {
                    try {
                        auto rollback = BuildRules(oldCatalog, applied.allowedRegions, applied.gamePath, NewGeneration());
                        if (rollback.empty()) RemoveOwnedRules(); else ApplyFirewallRules(rollback);
                    } catch (const std::exception& error) {
                        throw std::runtime_error(std::string(original.what()) + "; restoring previous rules also failed: " + error.what());
                    }
                }
                throw;
            }
            result.catalog = std::move(updated);
            Settings display = applied;
            display.allowedRegions = selected;
            result.settings = display;
            result.message = active ? L"Region data and active lock rules were updated using the previously applied selection." : L"Region data updated.";
            RefreshFirewall(result, count);
        });
    }
    void BeginProbe() {
        if (busy_ || !loaded_) return;
        if constexpr (kSmokeBuild) { SetStatus(L"Network tests are disabled in the smoke-test build. "); return; }
        std::vector<std::string> codes;
        codes.reserve(catalog_.regions.size());
        for (const auto& region : catalog_.regions) codes.push_back(region.code);
        for (int i = 0; i < ListView_GetItemCount(list_); ++i) {
            ListView_SetItemText(list_, i, 4, const_cast<wchar_t*>(L"Pending..."));
            ListView_SetItemText(list_, i, 5, const_cast<wchar_t*>(L"—"));
        }
        std::fill(probeDetails_.begin(), probeDetails_.end(), L"");
        probeCompleted_ = 0;
        SetStatus(L"Testing up to 4 regions at once, with 5 UDP requests per region...");
        RunJob(Job::Probe, [this, codes = std::move(codes)](Result& result, std::stop_token stop) {
            std::atomic<std::size_t> next{};
            std::vector<std::jthread> workers;
            const auto concurrency = std::min<std::size_t>(4, codes.size());
            for (std::size_t i = 0; i < concurrency; ++i) {
                workers.emplace_back([&, stop] {
                    while (!stop.stop_requested()) {
                        const auto index = next.fetch_add(1, std::memory_order_relaxed);
                        if (index >= codes.size()) return;
                        auto probe = std::make_unique<ProbeResult>();
                        try { *probe = ProbeRegion(codes[index], stop); }
                        catch (const std::exception& error) { probe->code = codes[index]; probe->error = ErrorText(error); }
                        if (PostMessageW(window_, kProbeDone, 0, reinterpret_cast<LPARAM>(probe.get()))) probe.release();
                    }
                });
            }
            workers.clear();
            result.message = stop.stop_requested() ? L"Network test canceled. Completed results have been kept." :
                L"Test complete. Median GameLift latency and replies out of 5 requests; not live game-server latency.";
        });
    }
    void ShowProbe(std::unique_ptr<ProbeResult> probe) {
        for (std::size_t i = 0; i < catalog_.regions.size(); ++i) {
            if (catalog_.regions[i].code != probe->code) continue;
            std::wstring latency = probe->received > 0 ? std::to_wstring(static_cast<long long>(std::llround(probe->medianMs))) + L" ms" : L"No reply";
            if (!probe->error.empty() && !probe->received) latency = L"Unavailable";
            std::wstring quality = std::to_wstring(probe->total ? probe->received * 100 / probe->total : 0) + L"% ("
                + std::to_wstring(probe->received) + L"/" + std::to_wstring(probe->total) + L")";
            ListView_SetItemText(list_, static_cast<int>(i), 4, latency.data());
            ListView_SetItemText(list_, static_cast<int>(i), 5, quality.data());
            probeDetails_[i] = probe->error.empty() ?
                L"GameLift UDP beacon: median of successful replies out of 5 requests, not live game-server latency." : probe->error;
            break;
        }
        ++probeCompleted_;
        SetStatus(L"Testing " + Number(probeCompleted_) + L" / " + Number(catalog_.regions.size()) + L" regions...");
    }
    void FinishJob(std::unique_ptr<Result> result) {
        if (worker_.joinable()) worker_.join();
        const auto kind = result->job;
        if (result->settings) {
            settings_ = *result->settings;
            if (kind == Job::Initial || kind == Job::Apply) preferencesReady_ = true;
            if (kind == Job::Initial || kind == Job::Apply) {
                SetWindowTextW(path_, settings_.gamePath.c_str());
                if (kind == Job::Apply && result->success) appliedSettings_ = settings_;
            }
        }
        if (result->catalog) {
            const auto selected = result->settings ? result->settings->allowedRegions : settings_.allowedRegions;
            catalog_ = std::move(*result->catalog);
            Populate(selected);
        }
        if (result->firewall) {
            firewallKnown_ = result->firewall->verified;
            firewallEnabled_ = result->firewall->firewallEnabled;
            activeRules_ = result->firewall->ruleCount;
            legacyRules_ = result->firewall->legacyRuleCount;
        } else if (kind == Job::Initial || !result->success) firewallKnown_ = false;
        UpdateBadge();
        SetBusy(false, kind);
        SetStatus(result->message);
        if constexpr (kSmokeBuild) {
            if (kind == Job::Update && smokeStage_ == 6) {
                smokeCancellationError_ = !result->success && !result->canceled
                    && result->message.find(L"Injected rollback failure") != std::wstring::npos;
                smokeStage_ = 7;
                RunJob(Job::Update, [](Result&, std::stop_token) { throw OperationCancelled(); });
                return;
            }
            if (kind == Job::Update && smokeStage_ == 7) {
                smokeExplicitCancellation_ = !result->success && result->canceled;
                smokePassed_ = smokePassed_ && smokeCancellationError_ && smokeExplicitCancellation_;
                // Exit from a hidden window must leave background mode before cleanup.
                HideToTray();
                RequestExit();
                return;
            }
        }
        if (kind == Job::Exit) {
            if (result->success) {
                closing_ = true;
                DestroyWindow(window_);
            } else {
                exiting_ = false;
                ShowMain();
                MessageBoxEnglish(window_, (L"Could not unlock. Region Lock will remain open so you can retry.\n\n" + result->message).c_str(),
                    L"Could not unlock and exit", MB_OK | MB_ICONERROR);
            }
            return;
        }
        if (exiting_) { RequestExit(); return; }
        if (!result->success && !result->canceled) {
            ShowMain();
            const auto text = kind == Job::Initial ? L"Initialization did not complete. You can update data or remove leftover rules in this window.\n\n" : L"The operation did not complete.\n\n";
            MessageBoxEnglish(window_, (std::wstring(text) + result->message).c_str(), L"Region Lock", MB_OK | MB_ICONERROR);
        }
        if constexpr (kSmokeBuild) {
            if (kind == Job::Initial) {
                if (!result->success) { smokePassed_ = false; RequestExit(); }
                else PostMessageW(window_, kSmokeMessage, 0, 0);
            }
        }
    }
    void BrowseGame() {
        std::wstring buffer(32768, L'\0');
        const auto current = WindowText(path_);
        if (current.size() < buffer.size()) std::copy(current.begin(), current.end(), buffer.begin());
        OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner = window_;
        dialog.lpstrFilter = L"Game executables (*.exe)\0*.exe\0\0";
        dialog.lpstrFile = buffer.data();
        dialog.nMaxFile = static_cast<DWORD>(buffer.size());
        dialog.lpstrTitle = L"Select the Battlefield 6 game executable (bf6.exe)";
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
        if (GetOpenFileNameW(&dialog)) {
            try {
                const auto validated = ValidateGameExecutable(buffer.c_str());
                SetWindowTextW(path_, validated.c_str());
                SetStatus(activeRules_ ? L"Game path changed. Select Apply lock to update; the previous rules remain active." : L"Game executable selected. Choose the allowed regions. ");
            } catch (const std::exception& error) { MessageBoxEnglish(window_, ErrorText(error).c_str(), L"Invalid executable path", MB_OK | MB_ICONERROR); }
        }
    }
    void DetectGame() {
        const auto game = FindGameExecutable();
        if (game.empty()) {
            MessageBoxEnglish(window_, L"No running bf6.exe or known installation was found.\nSelect Browse to choose the game executable.", L"Choose game executable", MB_OK | MB_ICONINFORMATION);
        } else {
            SetWindowTextW(path_, game.c_str());
            SetStatus(activeRules_ ? L"Game detected. Select Apply lock to update the active rules." : L"Game detected. Choose the allowed regions. ");
        }
    }
    bool AddTray() {
        NOTIFYICONDATAW data{sizeof(data)};
        data.hWnd = window_; data.uID = kTrayId;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = kTrayMessage;
        data.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(kIconId));
        wcscpy_s(data.szTip, (L"Region Lock - " + FirewallLabel()).c_str());
        if (!Shell_NotifyIconW(NIM_ADD, &data)) return false;
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
        trayAdded_ = true;
        return true;
    }
    void DeleteTray() {
        if (!trayAdded_ || !window_) return;
        NOTIFYICONDATAW data{sizeof(data)}; data.hWnd = window_; data.uID = kTrayId;
        Shell_NotifyIconW(NIM_DELETE, &data);
        trayAdded_ = false;
    }
    void UpdateTrayTip() {
        if (!trayAdded_) return;
        NOTIFYICONDATAW data{sizeof(data)}; data.hWnd = window_; data.uID = kTrayId; data.uFlags = NIF_TIP;
        wcscpy_s(data.szTip, (L"Region Lock - " + FirewallLabel()).c_str());
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }
    void HideToTray() {
        if (!trayAdded_ && !AddTray()) {
            ShowWindow(window_, SW_RESTORE);
            MessageBoxEnglish(window_, L"The tray icon could not be created. The window will remain visible.", L"Cannot minimize to tray", MB_OK | MB_ICONWARNING);
            return;
        }
        ShowWindow(window_, SW_HIDE);
        UpdateResourceMode();
    }
    void ShowMain() {
        resources_.SetBackground(false);
        ShowWindow(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(window_);
        DeleteTray();
    }
    void UpdateResourceMode() {
        resources_.SetBackground(trayAdded_ && !IsWindowVisible(window_) && !trayMenuOpen_ && !exiting_);
    }
    void TrayMenu() {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        trayMenuOpen_ = true;
        resources_.SetBackground(false);
        MENUINFO info{sizeof(info)};
        info.fMask = MIM_BACKGROUND;
        info.hbrBack = surface_;
        SetMenuInfo(menu, &info);
        AppendMenuW(menu, MF_OWNERDRAW, ShowWindowCommand, L"Open Region Lock");
        AppendMenuW(menu, MF_OWNERDRAW | (busy_ ? MF_GRAYED : 0), Unlock, L"Unlock");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_OWNERDRAW, ExitCommand, kExitLabel);
        SetMenuDefaultItem(menu, ShowWindowCommand, FALSE);
        POINT point{}; GetCursorPos(&point);
        SetForegroundWindow(window_);
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window_, nullptr);
        DestroyMenu(menu);
        trayMenuOpen_ = false;
        UpdateResourceMode();
        PostMessageW(window_, WM_NULL, 0, 0);
        if (command) SendMessageW(window_, WM_COMMAND, command, 0);
    }
    void RequestExit() {
        if (closing_) return;
        exiting_ = true;
        resources_.SetBackground(false);
        if (busy_) {
            if (worker_.joinable()) worker_.request_stop();
            SetStatus(L"Finishing the current task, then unlocking regions and exiting...");
            EnableWindow(cancel_, FALSE);
            return;
        }
        if constexpr (kSmokeBuild) {
            smokeClosed_ = true;
            smokeExitNormal_ = !resources_.Background() && !resources_.Idle() && !resources_.Eco()
                && GetPriorityClass(GetCurrentProcess()) == smokeOriginalPriority_;
            smokePassed_ = smokePassed_ && smokeExitNormal_;
            closing_ = true;
            DestroyWindow(window_);
            return;
        }
        const auto settings = loaded_ ? CurrentSettings() : settings_;
        const bool savePreferences = preferencesReady_;
        auto paths = paths_;
        SetStatus(L"Unlocking regions and exiting...");
        RunJob(Job::Exit, [settings, paths, savePreferences](Result& result, std::stop_token) {
            RemoveOwnedRules();
            result.firewall = FirewallState{0, true, L""};
            result.message = L"Unlocked.";
            if (savePreferences && !paths.directory.empty()) SavePreferences(result, paths, settings);
        });
    }
    void DrawButton(const DRAWITEMSTRUCT& item) {
        if (item.CtlType == ODT_MENU) {
            const bool selected = (item.itemState & ODS_SELECTED) != 0;
            HBRUSH fill = CreateSolidBrush(selected ? RGB(34, 47, 67) : kSurface);
            FillRect(item.hDC, &item.rcItem, fill); DeleteObject(fill);
            SetBkMode(item.hDC, TRANSPARENT);
            SetTextColor(item.hDC, (item.itemState & ODS_DISABLED) ? kMuted : kText);
            const auto previous = SelectObject(item.hDC, font_);
            const wchar_t* label = item.itemID == ShowWindowCommand ? L"Open Region Lock" : item.itemID == Unlock ? L"Unlock" : kExitLabel;
            RECT text = item.rcItem; text.left += S(16);
            DrawTextW(item.hDC, label, -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(item.hDC, previous);
            return;
        }
        const bool enabled = (item.itemState & ODS_DISABLED) == 0;
        const bool down = (item.itemState & ODS_SELECTED) != 0;
        const bool primary = item.CtlID == Apply;
        FillRect(item.hDC, &item.rcItem, background_);
        COLORREF color = down ? RGB(31, 40, 54) : RGB(22, 28, 38);
        if (primary) color = !enabled ? RGB(29, 43, 63) : down ? RGB(30, 71, 133) : RGB(36, 83, 157);
        HBRUSH brush = CreateSolidBrush(color);
        HPEN pen = CreatePen(PS_SOLID, 1, primary && enabled ? RGB(62, 113, 193) : kBorder);
        auto oldBrush = SelectObject(item.hDC, brush), oldPen = SelectObject(item.hDC, pen);
        RoundRect(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom, S(8), S(8));
        SelectObject(item.hDC, oldBrush); SelectObject(item.hDC, oldPen);
        DeleteObject(brush); DeleteObject(pen);
        SetBkMode(item.hDC, TRANSPARENT); SetTextColor(item.hDC, enabled ? kText : RGB(94, 107, 128));
        const auto oldFont = SelectObject(item.hDC, primary ? boldFont_ : font_);
        RECT text = item.rcItem;
        const auto label = WindowText(item.hwndItem);
        DrawTextW(item.hDC, label.c_str(), -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(item.hDC, oldFont);
        if ((item.itemState & ODS_FOCUS) != 0) { InflateRect(&text, -S(5), -S(5)); DrawFocusRect(item.hDC, &text); }
    }
    void Paint() {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(window_, &paint);
        RECT client{}; GetClientRect(window_, &client);
        FillRect(dc, &paint.rcPaint, background_);
        const auto pen = CreatePen(PS_SOLID, 1, kBorder);
        const auto old = SelectObject(dc, pen);
        MoveToEx(dc, S(24), S(199), nullptr); LineTo(dc, client.right - S(24), S(199));
        SelectObject(dc, old); DeleteObject(pen);
        HBRUSH border = CreateSolidBrush(kBorder);
        for (HWND control : {path_, list_}) {
            RECT rect{}; GetWindowRect(control, &rect);
            MapWindowPoints(HWND_DESKTOP, window_, reinterpret_cast<POINT*>(&rect), 2);
            InflateRect(&rect, 1, 1); FrameRect(dc, &rect, border);
        }
        DeleteObject(border);
        EndPaint(window_, &paint);
    }
    bool CaptureWindow(const std::filesystem::path& destination = {}) {
        const auto& output = destination.empty() ? options_.screenshot : destination;
        if (output.empty()) return true;
        RECT rect{}; GetWindowRect(window_, &rect);
        HDC screen = GetDC(window_), memory = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, rect.right - rect.left, rect.bottom - rect.top);
        auto previous = SelectObject(memory, bitmap);
        const BOOL printed = PrintWindow(window_, memory, 2);
        POINT sample{S(10), S(100)};
        ClientToScreen(window_, &sample);
        const COLORREF background = GetPixel(memory, sample.x - rect.left, sample.y - rect.top);
        smokeDark_ = smokeDark_ && background != CLR_INVALID && GetRValue(background) < 40
            && GetGValue(background) < 40 && GetBValue(background) < 40;
        if (ListView_GetItemCount(list_) > 0) {
            RECT row{};
            ListView_GetItemRect(list_, 0, &row, LVIR_BOUNDS);
            POINT cell{ListView_GetColumnWidth(list_, 0) - S(12), (row.top + row.bottom) / 2};
            ClientToScreen(list_, &cell);
            const COLORREF listBackground = GetPixel(memory, cell.x - rect.left, cell.y - rect.top);
            smokeDark_ = smokeDark_ && listBackground != CLR_INVALID && GetRValue(listBackground) < 50
                && GetGValue(listBackground) < 50 && GetBValue(listBackground) < 50;
        }
        SelectObject(memory, previous); DeleteDC(memory); ReleaseDC(window_, screen);
        Gdiplus::GdiplusStartupInput input; ULONG_PTR token{};
        bool success = false;
        if (printed && Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok) {
            {
                Gdiplus::Bitmap image(bitmap, nullptr);
                CLSID png{0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0, 0, 0xf8, 0x1e, 0xf3, 0x2e}};
                success = image.Save(output.c_str(), &png, nullptr) == Gdiplus::Ok;
            }
            Gdiplus::GdiplusShutdown(token);
        }
        DeleteObject(bitmap);
        return success;
    }
    bool StartSmokeSecondInstance() {
        std::wstring executable(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (!length || length >= executable.size()) return false;
        executable.resize(length);
        std::wstring command = L"\"" + executable + L"\" --smoke-second-instance";
        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) return false;
        CloseHandle(process.hThread);
        smokeSecondProcess_ = process.hProcess;
        smokeSecondStarted_ = GetTickCount64();
        return true;
    }
    void SmokeStep() {
        if constexpr (kSmokeBuild) {
            if (smokeStage_ == 0) {
                const auto containsHan = [](std::wstring_view text) {
                    return std::any_of(text.begin(), text.end(), [](wchar_t c) { return c >= 0x3400 && c <= 0x9fff; });
                };
                smokeEnglish_ = !containsHan(WindowText(window_));
                for (const auto& region : catalog_.regions) smokeEnglish_ = smokeEnglish_ && !containsHan(region.name);
                EnumChildWindows(window_, [](HWND child, LPARAM value) -> BOOL {
                    const auto text = WindowText(child);
                    if (std::any_of(text.begin(), text.end(), [](wchar_t c) { return c >= 0x3400 && c <= 0x9fff; }))
                        *reinterpret_cast<bool*>(value) = false;
                    return TRUE;
                }, reinterpret_cast<LPARAM>(&smokeEnglish_));
                smokeDark_ = ListView_GetBkColor(list_) == kSurface && ListView_GetTextBkColor(list_) == kSurface;
                const auto initialCount = activeRules_;
                const bool initialKnown = firewallKnown_, initialEnabled = firewallEnabled_;
                activeRules_ = 1; firewallKnown_ = true; firewallEnabled_ = false;
                UpdateBadge();
                smokeFirewallStatus_ = WindowText(badge_) == L"Firewall off";
                firewallKnown_ = false;
                UpdateBadge();
                smokeFirewallStatus_ = smokeFirewallStatus_ && WindowText(badge_) == L"State unknown";
                activeRules_ = initialCount; firewallKnown_ = initialKnown; firewallEnabled_ = initialEnabled;
                UpdateBadge();
                UpdateWindow(window_);
                smokeScreenshot_ = CaptureWindow();
                const auto originalCheck = ListView_GetCheckState(list_, 0);
                SetBusy(true, Job::Update);
                SetStatus(L"Downloading and validating the official AWS region data...");
                ListView_SetCheckState(list_, 0, !originalCheck);
                smokeBusyReadOnly_ = ListView_GetCheckState(list_, 0) == originalCheck;
                UpdateWindow(window_); UpdateWindow(list_);
                auto busyImage = options_.screenshot;
                if (!busyImage.empty()) busyImage = busyImage.parent_path() / (busyImage.stem().wstring() + L"-busy" + busyImage.extension().wstring());
                smokeBusyDark_ = CaptureWindow(busyImage) && smokeDark_ && IsWindowEnabled(list_);
                SetBusy(false, Job::Update);
                ListView_SetCheckState(list_, 0, !originalCheck);
                smokeReadyEditable_ = ListView_GetCheckState(list_, 0) != originalCheck;
                ListView_SetCheckState(list_, 0, originalCheck);
                SetStatus(L"Choose regions and select Apply lock. Restart Battlefield 6 after changing regions.");
                // Let initial layout, theme transitions, and screenshot work settle before idle sampling.
                smokeStage_ = 1;
                SetTimer(window_, kSmokeTimer, 1000, nullptr);
            } else if (smokeStage_ == 1) {
                PROCESS_MEMORY_COUNTERS memory{sizeof(memory)};
                if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))) smokeWorkingSet_ = memory.WorkingSetSize;
                smokeOriginalPriority_ = GetPriorityClass(GetCurrentProcess());
                smokeOriginalThreadPriority_ = ThreadBasePriority();
                MEMORY_PRIORITY_INFORMATION memoryPriority{};
                smokeMemoryReadable_ = GetProcessInformation(GetCurrentProcess(), ProcessMemoryPriority,
                    &memoryPriority, sizeof(memoryPriority)) != FALSE;
                smokeOriginalMemoryPriority_ = memoryPriority.MemoryPriority;
                smokeOriginalPower_.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
                smokePowerReadable_ = GetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                    &smokeOriginalPower_, sizeof(smokeOriginalPower_)) != FALSE;
                smokeCpuBaseline_ = CpuTime(); smokeStage_ = 2;
                SetTimer(window_, kSmokeTimer, 2500, nullptr);
            } else if (smokeStage_ == 2) {
                smokeVisibleCpu_ = CpuTime() - smokeCpuBaseline_;
                SendMessageW(window_, WM_SYSCOMMAND, SC_MINIMIZE, 0);
                smokeMinimized_ = !IsWindowVisible(window_) && trayAdded_;
                smokeTrayPriority_ = GetPriorityClass(GetCurrentProcess());
                smokeTrayThreadPriority_ = ThreadBasePriority();
                smokeBackground_ = resources_.Background() && resources_.Idle() && resources_.BackgroundError() == ERROR_SUCCESS
                    && smokeTrayPriority_ == IDLE_PRIORITY_CLASS;
                MEMORY_PRIORITY_INFORMATION memoryPriority{};
                if (GetProcessInformation(GetCurrentProcess(), ProcessMemoryPriority, &memoryPriority, sizeof(memoryPriority)))
                    smokeTrayMemoryPriority_ = memoryPriority.MemoryPriority;
                PROCESS_POWER_THROTTLING_STATE power{PROCESS_POWER_THROTTLING_CURRENT_VERSION};
                smokeEco_ = !resources_.PowerReadable() || (resources_.Eco()
                    && GetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &power, sizeof(power))
                    && (power.ControlMask & power.StateMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) != 0);
                smokeStage_ = 3;
                SetTimer(window_, kSmokeTimer, 250, nullptr);
            } else if (smokeStage_ == 3) {
                GetProcessIoCounters(GetCurrentProcess(), &smokeIoBaseline_);
                smokeCpuBaseline_ = CpuTime(); smokeStage_ = 4;
                SetTimer(window_, kSmokeTimer, 2500, nullptr);
            } else if (smokeStage_ == 4) {
                smokeHiddenCpu_ = CpuTime() - smokeCpuBaseline_;
                IO_COUNTERS io{};
                if (GetProcessIoCounters(GetCurrentProcess(), &io))
                    smokeHiddenIoBytes_ = io.ReadTransferCount + io.WriteTransferCount
                        - smokeIoBaseline_.ReadTransferCount - smokeIoBaseline_.WriteTransferCount;
                PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
                if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
                    smokeTrayWorkingSet_ = memory.WorkingSetSize;
                    smokePrivateBytes_ = memory.PrivateUsage;
                }
                ShowMain();
                smokeRestored_ = IsWindowVisible(window_) && !trayAdded_;
                PROCESS_POWER_THROTTLING_STATE power{PROCESS_POWER_THROTTLING_CURRENT_VERSION};
                MEMORY_PRIORITY_INFORMATION memoryPriority{};
                smokeResourceRestored_ = !resources_.Background() && !resources_.Idle() && !resources_.Eco()
                    && GetPriorityClass(GetCurrentProcess()) == smokeOriginalPriority_
                    && (!smokeMemoryReadable_ || (GetProcessInformation(GetCurrentProcess(), ProcessMemoryPriority,
                        &memoryPriority, sizeof(memoryPriority)) && memoryPriority.MemoryPriority == smokeOriginalMemoryPriority_))
                    && (!smokePowerReadable_ || (GetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &power, sizeof(power))
                        && power.ControlMask == smokeOriginalPower_.ControlMask && power.StateMask == smokeOriginalPower_.StateMask));
                ShowWindow(window_, SW_MAXIMIZE);
                SendMessageW(window_, WM_SYSCOMMAND, SC_MINIMIZE, 0);
                ShowMain();
                smokeMaximizedRestore_ = IsWindowVisible(window_) && IsZoomed(window_) && !trayAdded_;
                HideToTray();
                if (StartSmokeSecondInstance()) {
                    smokeStage_ = 5;
                    SetTimer(window_, kSmokeTimer, 100, nullptr);
                    return;
                }
                smokePassed_ = false;
                SendMessageW(window_, WM_CLOSE, 0, 0);
            } else {
                const DWORD wait = WaitForSingleObject(smokeSecondProcess_, 0);
                if (wait == WAIT_TIMEOUT && GetTickCount64() - smokeSecondStarted_ < 5000) {
                    SetTimer(window_, kSmokeTimer, 50, nullptr); return;
                }
                DWORD exitCode{};
                int windows = 0;
                EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
                    wchar_t name[128]{}; GetClassNameW(window, name, static_cast<int>(std::size(name)));
                    if (wcscmp(name, kClassName) == 0) ++*reinterpret_cast<int*>(parameter);
                    return TRUE;
                }, reinterpret_cast<LPARAM>(&windows));
                smokeSingleInstance_ = wait == WAIT_OBJECT_0 && GetExitCodeProcess(smokeSecondProcess_, &exitCode)
                    && exitCode == 0 && windows == 1 && smokeSecondRestored_ && IsWindowVisible(window_) && !trayAdded_;
                smokePassed_ = loaded_ && smokeScreenshot_ && smokeMinimized_ && smokeRestored_
                    && smokeDark_ && smokeEnglish_ && smokeMaximizedRestore_
                    && smokeBusyDark_ && smokeBusyReadOnly_ && smokeReadyEditable_ && smokeSingleInstance_
                    && smokeBackground_ && smokeEco_ && smokeResourceRestored_
                    && smokeFirewallStatus_
                    && smokeVisibleCpu_ <= 1000000 && smokeHiddenCpu_ <= 1000000;
                smokeStage_ = 6;
                RunJob(Job::Update, [](Result&, std::stop_token stop) {
                    std::mutex mutex;
                    std::unique_lock lock(mutex);
                    std::condition_variable_any condition;
                    condition.wait(lock, stop, [] { return false; });
                    throw std::runtime_error("Injected rollback failure after cancellation request");
                });
                worker_.request_stop();
            }
        }
    }
    void SaveSmokeReport() {
        if constexpr (kSmokeBuild) {
        if (options_.report.empty()) return;
        std::ostringstream json;
        json << "{\n  \"passed\": " << (smokePassed_ && smokeClosed_ ? "true" : "false")
            << ",\n  \"catalog_regions\": " << catalog_.regions.size()
            << ",\n  \"minimized_to_tray\": " << (smokeMinimized_ ? "true" : "false")
            << ",\n  \"restored_from_tray\": " << (smokeRestored_ ? "true" : "false")
            << ",\n  \"close_exited\": " << (smokeClosed_ ? "true" : "false")
            << ",\n  \"screenshot_saved\": " << (smokeScreenshot_ ? "true" : "false")
            << ",\n  \"english_ui\": " << (smokeEnglish_ ? "true" : "false")
            << ",\n  \"dark_background\": " << (smokeDark_ ? "true" : "false")
            << ",\n  \"maximized_restore\": " << (smokeMaximizedRestore_ ? "true" : "false")
            << ",\n  \"busy_dark_background\": " << (smokeBusyDark_ ? "true" : "false")
            << ",\n  \"busy_selection_read_only\": " << (smokeBusyReadOnly_ ? "true" : "false")
            << ",\n  \"ready_selection_editable\": " << (smokeReadyEditable_ ? "true" : "false")
            << ",\n  \"single_instance_restores_tray_window\": " << (smokeSingleInstance_ ? "true" : "false")
            << ",\n  \"tray_background_mode\": " << (smokeBackground_ ? "true" : "false")
            << ",\n  \"background_mode_error\": " << resources_.BackgroundError()
            << ",\n  \"priority_change_error\": " << resources_.PriorityError()
            << ",\n  \"power_throttling_error\": " << resources_.PowerError()
            << ",\n  \"power_throttling_query_available\": " << (smokePowerReadable_ ? "true" : "false")
            << ",\n  \"tray_priority_class\": " << smokeTrayPriority_
            << ",\n  \"visible_thread_base_priority\": " << smokeOriginalThreadPriority_
            << ",\n  \"tray_thread_base_priority\": " << smokeTrayThreadPriority_
            << ",\n  \"tray_memory_priority\": " << smokeTrayMemoryPriority_
            << ",\n  \"visible_memory_priority\": " << smokeOriginalMemoryPriority_
            << ",\n  \"tray_eco_qos_policy_readable\": " << (resources_.PowerReadable() ? "true" : "false")
            << ",\n  \"tray_eco_qos_verified_or_unavailable\": " << (smokeEco_ ? "true" : "false")
            << ",\n  \"foreground_resource_policy_restored\": " << (smokeResourceRestored_ ? "true" : "false")
            << ",\n  \"exit_leaves_background_mode\": " << (smokeExitNormal_ ? "true" : "false")
            << ",\n  \"cancellation_preserves_real_errors\": " << (smokeCancellationError_ ? "true" : "false")
            << ",\n  \"explicit_cancellation_recognized\": " << (smokeExplicitCancellation_ ? "true" : "false")
            << ",\n  \"firewall_status_not_overstated\": " << (smokeFirewallStatus_ ? "true" : "false")
            << ",\n  \"visible_idle_cpu_ms_over_2500ms\": " << static_cast<double>(smokeVisibleCpu_) / 10000.0
            << ",\n  \"tray_idle_cpu_ms_over_2500ms\": " << static_cast<double>(smokeHiddenCpu_) / 10000.0
            << ",\n  \"working_set_bytes\": " << smokeWorkingSet_
            << ",\n  \"tray_working_set_bytes\": " << smokeTrayWorkingSet_
            << ",\n  \"tray_private_bytes\": " << smokePrivateBytes_
            << ",\n  \"tray_idle_io_bytes_over_2500ms\": " << smokeHiddenIoBytes_
            << ",\n  \"firewall_mutations\": 0\n}\n";
        try { WriteFileAtomic(options_.report, json.str()); }
        catch (...) { smokePassed_ = false; }
        }
    }
    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        if (taskbarCreated_ && message == taskbarCreated_) {
            if (trayAdded_) { trayAdded_ = false; if (!AddTray()) ShowMain(); }
            return 0;
        }
        switch (message) {
        case WM_CREATE:
            try { CreateControls(); return 0; }
            catch (const std::exception& error) { MessageBoxEnglish(window_, ErrorText(error).c_str(), L"Cannot create window", MB_OK | MB_ICONERROR); return -1; }
        case WM_SIZE:
            if (wparam == SIZE_MINIMIZED) HideToTray(); else Layout();
            return 0;
        case WM_SYSCOMMAND:
            if ((wparam & 0xfff0) == SC_MINIMIZE) { HideToTray(); return 0; }
            if ((wparam & 0xfff0) == SC_CLOSE) { RequestExit(); return 0; }
            break;
        case WM_DPICHANGED:
            dpi_ = HIWORD(wparam); CreateFonts();
            CreateCheckImages();
            { const auto* rect = reinterpret_cast<const RECT*>(lparam);
              SetWindowPos(window_, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE); }
            Layout(); return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize = {S(960), S(650)}; return 0;
        case WM_COMMAND:
            if (HIWORD(wparam) != BN_CLICKED && lparam) break;
            switch (LOWORD(wparam)) {
            case Browse: if (!busy_) BrowseGame(); break;
            case Detect: if (!busy_) DetectGame(); break;
            case SelectAll: if (!busy_) SelectEvery(true); break;
            case SelectNone: if (!busy_) SelectEvery(false); break;
            case Apply: case IDOK: BeginApply(); break;
            case Unlock: BeginUnlock(); break;
            case Test: BeginProbe(); break;
            case Update: BeginUpdate(); break;
            case Cancel: if (worker_.joinable()) { worker_.request_stop(); EnableWindow(cancel_, FALSE); SetStatus(L"Canceling task..."); } break;
            case ShowWindowCommand:
                if constexpr (kSmokeBuild) {
                    if (smokeSecondProcess_) smokeSecondRestored_ = !IsWindowVisible(window_) && trayAdded_;
                }
                ShowMain(); break;
            case ExitCommand: RequestExit(); break;
            }
            return 0;
        case WM_NOTIFY:
            if (reinterpret_cast<NMHDR*>(lparam)->idFrom == RegionList && reinterpret_cast<NMHDR*>(lparam)->code == LVN_ITEMCHANGING) {
                const auto* item = reinterpret_cast<NMLISTVIEW*>(lparam);
                if (busy_ && !suppressChecks_ && (item->uChanged & LVIF_STATE)
                    && ((item->uOldState ^ item->uNewState) & LVIS_STATEIMAGEMASK)) return TRUE;
            }
            if (reinterpret_cast<NMHDR*>(lparam)->idFrom == RegionList && reinterpret_cast<NMHDR*>(lparam)->code == LVN_ITEMCHANGED) {
                const auto* item = reinterpret_cast<NMLISTVIEW*>(lparam);
                if (!suppressChecks_ && (item->uChanged & LVIF_STATE) && ((item->uOldState ^ item->uNewState) & LVIS_STATEIMAGEMASK)) SelectionChanged(true);
            }
            if (reinterpret_cast<NMHDR*>(lparam)->idFrom == RegionList && reinterpret_cast<NMHDR*>(lparam)->code == LVN_GETINFOTIP) {
                auto* tip = reinterpret_cast<NMLVGETINFOTIPW*>(lparam);
                if (tip->iItem >= 0 && static_cast<std::size_t>(tip->iItem) < probeDetails_.size() && tip->pszText && tip->cchTextMax > 0)
                    wcsncpy_s(tip->pszText, static_cast<std::size_t>(tip->cchTextMax), probeDetails_[static_cast<std::size_t>(tip->iItem)].c_str(), _TRUNCATE);
            }
            return 0;
        case WM_DRAWITEM: DrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam)); return TRUE;
        case WM_MEASUREITEM: {
            auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
            if (measure->CtlType == ODT_MENU) { measure->itemWidth = S(220); measure->itemHeight = S(32); return TRUE; }
            break;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wparam); HWND control = reinterpret_cast<HWND>(lparam);
            SetBkMode(dc, TRANSPARENT);
            COLORREF color = kMuted;
            if (control == title_ || control == gameLabel_ || control == regionsLabel_) color = kText;
            if (control == badge_) color = activeRules_ ? RGB(73, 188, 143) : RGB(139, 172, 226);
            if (control == path_) { SetTextColor(dc, kText); SetBkColor(dc, kSurface); return reinterpret_cast<LRESULT>(surface_); }
            SetTextColor(dc, color); return reinterpret_cast<LRESULT>(background_);
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: Paint(); return 0;
        case kJobDone: FinishJob(std::unique_ptr<Result>(reinterpret_cast<Result*>(lparam))); return 0;
        case kProbeDone: ShowProbe(std::unique_ptr<ProbeResult>(reinterpret_cast<ProbeResult*>(lparam))); return 0;
        case kTrayMessage:
            switch (LOWORD(lparam)) {
            case NIN_SELECT: case NIN_KEYSELECT: case WM_LBUTTONDBLCLK: ShowMain(); break;
            case WM_CONTEXTMENU: case WM_RBUTTONUP: TrayMenu(); break;
            }
            return 0;
        case kSmokeMessage: SmokeStep(); return 0;
        case WM_TIMER:
            if (wparam == kSmokeTimer && kSmokeBuild) { KillTimer(window_, kSmokeTimer); SmokeStep(); return 0; }
            break;
        case WM_QUERYENDSESSION: return TRUE;
        case WM_ENDSESSION:
            if (wparam) {
                resources_.SetBackground(false);
                // Stop/drain the writer before cleanup so it cannot recreate rules after shutdown cleanup.
                if (worker_.joinable()) { worker_.request_stop(); worker_.join(); }
                if constexpr (!kSmokeBuild) { try { RemoveOwnedRules(); } catch (...) {} }
                closing_ = true;
                DestroyWindow(window_);
            }
            return 0;
        case WM_CLOSE: RequestExit(); return 0;
        case WM_DESTROY:
            DeleteTray(); SaveSmokeReport(); PostQuitMessage(smokePassed_ ? 0 : 1); return 0;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }
};
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    // Keep standard dialogs in English without changing the user's Windows language.
    ULONG languageCount{};
    SetThreadPreferredUILanguages(MUI_LANGUAGE_NAME, L"en-US\0\0", &languageCount);
    SetThreadUILanguage(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HANDLE single = CreateMutexW(nullptr, FALSE, kSmokeBuild ? L"Local\\RegionLock.Native.Smoke.v1" : L"Local\\RegionLock.Native.Instance.v1");
    if (!single) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kClassName, nullptr)) {
            DWORD process{}; GetWindowThreadProcessId(existing, &process);
            if (process) AllowSetForegroundWindow(process);
            DWORD_PTR ignored{};
            SendMessageTimeoutW(existing, WM_COMMAND, ShowWindowCommand, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &ignored);
        }
        CloseHandle(single); return 0;
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    Options options;
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if constexpr (kSmokeBuild) {
        for (int i = 1; arguments && i < count; ++i) {
            const std::wstring_view arg(arguments[i]);
            if (arg == L"--report" && i + 1 < count) options.report = arguments[++i];
            else if (arg == L"--screenshot" && i + 1 < count) options.screenshot = arguments[++i];
        }
    }
    if (arguments) LocalFree(arguments);
    int code = 1;
    try {
        App app(std::move(options));
        if (app.Create(instance)) {
            ShowWindow(app.Window(), show); UpdateWindow(app.Window());
            MSG message{};
            BOOL result;
            while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
                if (!IsDialogMessageW(app.Window(), &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
            }
            code = result == -1 ? 1 : static_cast<int>(message.wParam);
        }
    } catch (const std::exception& error) { MessageBoxEnglish(nullptr, ErrorText(error).c_str(), L"Region Lock", MB_OK | MB_ICONERROR); }
    CloseHandle(single);
    return code;
}
