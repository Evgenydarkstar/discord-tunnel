#include <Windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>

#include <filesystem>
#include <iterator>
#include <string>
#include <vector>
#include <cwctype>

#include "discord/runtime/include/discord_runtime.h"
#include "discord/shared/config.h"
#include "discord/shared/fs_utils.h"

namespace {

using namespace discord_client;

constexpr int kIdServer = 1001;
constexpr int kIdPort = 1002;
constexpr int kIdToken = 1003;
constexpr int kIdCaCert = 1004;
constexpr int kIdDiscordPath = 1005;
constexpr int kIdSkipVerify = 1006;
constexpr int kIdBrowseDiscordPath = 1007;
constexpr int kIdInstall = 1008;
constexpr int kIdUninstall = 1009;
constexpr int kIdStatus = 1010;
constexpr int kIdBrowseCaCert = 1011;

struct UiState {
    HWND title = nullptr;
    HWND subtitle = nullptr;
    HWND connection_heading = nullptr;
    HWND client_heading = nullptr;
    HWND helper_text = nullptr;
    HWND server = nullptr;
    HWND port = nullptr;
    HWND token = nullptr;
    HWND ca_cert = nullptr;
    HWND discord_path = nullptr;
    HWND skip_verify = nullptr;
    HWND status = nullptr;
};

UiState g_ui;

constexpr COLORREF kBackgroundColor = RGB(11, 17, 32);
constexpr COLORREF kCardColor = RGB(17, 24, 39);
constexpr COLORREF kEditColor = RGB(15, 23, 42);
constexpr COLORREF kHeaderColor = RGB(7, 11, 20);
constexpr COLORREF kTextColor = RGB(226, 232, 240);
constexpr COLORREF kMutedColor = RGB(148, 163, 184);
constexpr COLORREF kBorderColor = RGB(51, 65, 85);
constexpr COLORREF kAccentColor = RGB(88, 101, 242);
constexpr COLORREF kAccentPressedColor = RGB(71, 82, 196);
constexpr COLORREF kDangerColor = RGB(248, 113, 113);
constexpr COLORREF kDangerPressedColor = RGB(185, 28, 28);
constexpr COLORREF kStatusColor = RGB(17, 29, 53);

HBRUSH g_background_brush = nullptr;
HBRUSH g_card_brush = nullptr;
HBRUSH g_edit_brush = nullptr;
HBRUSH g_status_brush = nullptr;
HBRUSH g_border_brush = nullptr;
HFONT g_title_font = nullptr;
HFONT g_subtitle_font = nullptr;
HFONT g_heading_font = nullptr;
HFONT g_body_font = nullptr;
HFONT g_button_font = nullptr;

std::wstring window_text(HWND hwnd) {
    const int len = GetWindowTextLengthW(hwnd);
    std::wstring text(len + 1, L'\0');
    GetWindowTextW(hwnd, text.data(), len + 1);
    text.resize(len);
    return text;
}

void set_status(const std::wstring& message) {
    const auto line_break = message.find(L"\r\n");
    const std::wstring summary = line_break == std::wstring::npos
        ? message
        : message.substr(0, line_break) + L" Details are shown in the dialog.";
    SetWindowTextW(g_ui.status, summary.c_str());
}

std::filesystem::path browse_for_ca_cert(HWND owner) {
    wchar_t file_path[32768]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = L"PEM certificates (*.pem)\0*.pem\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = file_path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(file_path));
    dialog.lpstrTitle = L"Select CA certificate";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&dialog) == TRUE ? std::filesystem::path(file_path) : std::filesystem::path{};
}

std::filesystem::path browse_for_folder(HWND owner) {
    BROWSEINFOW info{};
    info.hwndOwner = owner;
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
    info.lpszTitle = L"Select Discord folder";
    PIDLIST_ABSOLUTE selected = SHBrowseForFolderW(&info);
    if (selected == nullptr) {
        return {};
    }

    wchar_t buffer[MAX_PATH];
    const bool ok = SHGetPathFromIDListW(selected, buffer) == TRUE;
    CoTaskMemFree(selected);
    if (!ok) {
        return {};
    }
    return std::filesystem::path(buffer);
}

void load_existing_config_into_ui(const std::filesystem::path& root) {
    const auto app_dirs = find_discord_app_dirs(root);
    if (app_dirs.empty()) {
        return;
    }
    const auto config = load_config(config_path_for_app_dir(app_dirs.back()));
    if (!config) {
        return;
    }
    SetWindowTextW(g_ui.server, config->server.c_str());
    SetWindowTextW(g_ui.port, std::to_wstring(config->port).c_str());
    SetWindowTextW(g_ui.token, config->token.c_str());
    SetWindowTextW(g_ui.ca_cert, config->ca_cert_path.c_str());
    SetWindowTextW(g_ui.discord_path, config->discord_root.c_str());
    SendMessageW(g_ui.skip_verify, BM_SETCHECK, config->skip_tls_verify ? BST_CHECKED : BST_UNCHECKED, 0);
}

AppConfig collect_config() {
    AppConfig config;
    config.server = window_text(g_ui.server);
    config.token = window_text(g_ui.token);
    config.ca_cert_path = window_text(g_ui.ca_cert);
    config.discord_root = window_text(g_ui.discord_path);
    config.skip_tls_verify = SendMessageW(g_ui.skip_verify, BM_GETCHECK, 0, 0) == BST_CHECKED;
    try {
        const auto port_text = window_text(g_ui.port);
        const unsigned long port = std::stoul(port_text.empty() ? L"443" : port_text);
        if (port >= 1 && port <= 65535) {
            config.port = static_cast<std::uint16_t>(port);
        }
    } catch (...) {
        config.port = 0;
    }
    return config;
}

bool validate_config(const AppConfig& config, std::wstring* error) {
    if (config.server.empty()) {
        *error = L"Server is required.";
        return false;
    }
    if (config.port == 0) {
        *error = L"Port must be between 1 and 65535.";
        return false;
    }
    if (config.token.empty()) {
        *error = L"Token is required.";
        return false;
    }
    if (config.ca_cert_path.empty()) {
        *error = L"CA certificate is required.";
        return false;
    }
    if (!std::filesystem::is_regular_file(config.ca_cert_path)) {
        *error = L"CA certificate path does not exist.";
        return false;
    }
    if (config.discord_root.empty()) {
        *error = L"Discord Path is required.";
        return false;
    }
    if (!std::filesystem::is_directory(config.discord_root)) {
        *error = L"Discord Path does not exist.";
        return false;
    }
    return true;
}

bool install_files(const AppConfig& config, std::wstring* message) {
    const auto source_dir = current_module_path().parent_path();
    const auto source_exe = current_module_path();
    const auto source_dll = source_dir / L"version.dll";
    if (!std::filesystem::is_regular_file(source_dll)) {
        *message = L"version.dll was not found next to the installer exe.";
        return false;
    }

    const auto app_dirs = find_discord_app_dirs(config.discord_root);
    if (app_dirs.empty()) {
        *message = L"No Discord app-* folders were found in the selected path.";
        return false;
    }

    std::vector<std::wstring> updated;
    for (const auto& app_dir : app_dirs) {
        if (!copy_file_overwrite(source_exe, helper_path_for_app_dir(app_dir))) {
            *message = L"Failed to copy helper exe into " + app_dir.wstring();
            return false;
        }
        if (!copy_file_overwrite(source_dll, dll_path_for_app_dir(app_dir))) {
            *message = L"Failed to copy version.dll into " + app_dir.wstring();
            return false;
        }
        if (!save_config(config_path_for_app_dir(app_dir), config)) {
            *message = L"Failed to write config into " + app_dir.wstring();
            return false;
        }
        updated.push_back(app_dir.wstring());
    }

    *message = L"Installed into:\r\n" + join_lines(updated);
    return true;
}

bool uninstall_files(const std::filesystem::path& root, std::wstring* message) {
    const auto app_dirs = find_discord_app_dirs(root);
    if (app_dirs.empty()) {
        *message = L"No Discord app-* folders were found in the selected path.";
        return false;
    }

    std::vector<std::wstring> updated;
    for (const auto& app_dir : app_dirs) {
        const bool managed_install =
            std::filesystem::exists(helper_path_for_app_dir(app_dir)) ||
            std::filesystem::exists(config_path_for_app_dir(app_dir));
        if (managed_install) {
            remove_if_exists(dll_path_for_app_dir(app_dir));
        }
        remove_if_exists(helper_path_for_app_dir(app_dir));
        remove_if_exists(config_path_for_app_dir(app_dir));
        remove_if_exists(log_path_for_app_dir(app_dir));
        updated.push_back(app_dir.wstring());
    }

    *message = L"Removed client files from:\r\n" + join_lines(updated);
    return true;
}

HFONT create_font(int points, int weight) {
    HDC screen = GetDC(nullptr);
    const int height = -MulDiv(points, GetDeviceCaps(screen, LOGPIXELSY), 72);
    ReleaseDC(nullptr, screen);
    return CreateFontW(
        height,
        0,
        0,
        0,
        weight,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
}

void initialize_theme() {
    g_background_brush = CreateSolidBrush(kBackgroundColor);
    g_card_brush = CreateSolidBrush(kCardColor);
    g_edit_brush = CreateSolidBrush(kEditColor);
    g_status_brush = CreateSolidBrush(kStatusColor);
    g_border_brush = CreateSolidBrush(kBorderColor);
    g_title_font = create_font(22, FW_SEMIBOLD);
    g_subtitle_font = create_font(10, FW_NORMAL);
    g_heading_font = create_font(11, FW_SEMIBOLD);
    g_body_font = create_font(10, FW_NORMAL);
    g_button_font = create_font(10, FW_SEMIBOLD);
}

void cleanup_theme() {
    DeleteObject(g_background_brush);
    DeleteObject(g_card_brush);
    DeleteObject(g_edit_brush);
    DeleteObject(g_status_brush);
    DeleteObject(g_border_brush);
    DeleteObject(g_title_font);
    DeleteObject(g_subtitle_font);
    DeleteObject(g_heading_font);
    DeleteObject(g_body_font);
    DeleteObject(g_button_font);
}

void enable_dark_title_bar(HWND hwnd) {
    constexpr DWORD kUseImmersiveDarkMode = 20;
    BOOL enabled = TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd, kUseImmersiveDarkMode, &enabled, sizeof(enabled)))) {
        constexpr DWORD kUseImmersiveDarkModeBefore20H1 = 19;
        DwmSetWindowAttribute(hwnd, kUseImmersiveDarkModeBefore20H1, &enabled, sizeof(enabled));
    }
}

void set_control_font(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HWND create_label(
    HWND parent,
    const wchar_t* text,
    int x,
    int y,
    int w,
    int h,
    HFONT font = nullptr) {
    HWND label = CreateWindowW(
        L"STATIC",
        text,
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        x,
        y,
        w,
        h,
        parent,
        nullptr,
        nullptr,
        nullptr);
    set_control_font(label, font == nullptr ? g_body_font : font);
    return label;
}

HWND create_edit(HWND parent, int id, int x, int y, int w, int h, DWORD extra_style = 0) {
    HWND edit = CreateWindowExW(
        0,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | extra_style,
        x,
        y,
        w,
        h,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        nullptr,
        nullptr);
    set_control_font(edit, g_body_font);
    SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(9, 9));
    return edit;
}

HWND create_button(HWND parent, const wchar_t* text, int id, int x, int y, int w, int h) {
    HWND button = CreateWindowW(
        L"BUTTON",
        text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        x,
        y,
        w,
        h,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        nullptr,
        nullptr);
    set_control_font(button, g_button_font);
    return button;
}

void paint_rounded_rect(HDC dc, const RECT& rect, int radius, HBRUSH fill, HBRUSH border) {
    HRGN region = CreateRoundRectRgn(rect.left, rect.top, rect.right + 1, rect.bottom + 1, radius, radius);
    FillRgn(dc, region, fill);
    if (border != nullptr) {
        FrameRgn(dc, region, border, 1, 1);
    }
    DeleteObject(region);
}

void paint_window(HWND hwnd) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd, &paint);
    RECT client{};
    GetClientRect(hwnd, &client);
    FillRect(dc, &client, g_background_brush);

    RECT header{0, 0, client.right, 104};
    HBRUSH header_brush = CreateSolidBrush(kHeaderColor);
    FillRect(dc, &header, header_brush);
    DeleteObject(header_brush);

    HBRUSH accent_brush = CreateSolidBrush(kAccentColor);
    RECT mark{28, 28, 42, 68};
    paint_rounded_rect(dc, mark, 8, accent_brush, nullptr);
    DeleteObject(accent_brush);

    paint_rounded_rect(dc, RECT{28, 128, 732, 320}, 16, g_card_brush, g_border_brush);
    paint_rounded_rect(dc, RECT{28, 340, 732, 536}, 16, g_card_brush, g_border_brush);
    paint_rounded_rect(dc, RECT{28, 612, 732, 680}, 14, g_status_brush, nullptr);

    HBRUSH status_dot = CreateSolidBrush(kAccentColor);
    HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
    HGDIOBJ old_brush = SelectObject(dc, status_dot);
    Ellipse(dc, 46, 638, 56, 648);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(status_dot);
    EndPaint(hwnd, &paint);
}

void draw_button(const DRAWITEMSTRUCT& item) {
    wchar_t text[128]{};
    GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));

    const bool action_button = item.CtlID == kIdInstall || item.CtlID == kIdUninstall;
    FillRect(item.hDC, &item.rcItem, action_button ? g_background_brush : g_card_brush);

    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    COLORREF fill = kCardColor;
    COLORREF foreground = kTextColor;
    COLORREF border = kBorderColor;
    if (item.CtlID == kIdInstall) {
        fill = pressed ? kAccentPressedColor : kAccentColor;
        foreground = RGB(255, 255, 255);
        border = fill;
    } else if (item.CtlID == kIdUninstall) {
        fill = pressed ? kDangerPressedColor : kCardColor;
        foreground = pressed ? RGB(255, 255, 255) : kDangerColor;
        border = pressed ? kDangerPressedColor : RGB(127, 29, 29);
    } else if (pressed) {
        fill = RGB(30, 41, 59);
    }

    HBRUSH fill_brush = CreateSolidBrush(fill);
    HBRUSH border_brush = CreateSolidBrush(border);
    paint_rounded_rect(item.hDC, item.rcItem, 10, fill_brush, border_brush);
    DeleteObject(fill_brush);
    DeleteObject(border_brush);

    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, foreground);
    HGDIOBJ old_font = SelectObject(item.hDC, g_button_font);
    RECT text_rect = item.rcItem;
    DrawTextW(item.hDC, text, -1, &text_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(item.hDC, old_font);

    if ((item.itemState & ODS_FOCUS) != 0) {
        RECT focus = item.rcItem;
        InflateRect(&focus, -4, -4);
        DrawFocusRect(item.hDC, &focus);
    }
}

int start_embedded_runtime(const std::filesystem::path& app_dir) {
    const auto config = load_config(config_path_for_app_dir(app_dir));
    if (!config) {
        return 2;
    }

    const std::string server = wide_to_utf8(config->server);
    const std::string token = wide_to_utf8(config->token);
    const std::string ca_cert_path = wide_to_utf8(config->ca_cert_path);
    const char* ca_cert = ca_cert_path.empty() ? nullptr : ca_cert_path.c_str();
    const int status = dt_embedded_start(
        server.c_str(),
        config->port,
        token.c_str(),
        ca_cert,
        config->skip_tls_verify ? 1 : 0,
        0);
    return status == DT_OK ? 0 : 10 + (-status);
}

std::wstring runtime_mutex_name(const std::filesystem::path& app_dir) {
    std::wstring name = L"Local\\DiscordTunnelRuntime-";
    for (const wchar_t ch : app_dir.wstring()) {
        name.push_back(std::iswalnum(static_cast<wint_t>(ch)) ? ch : L'_');
    }
    return name;
}

int launch_runtime_mode(int argc, wchar_t** argv) {
    DWORD parent_pid = 0;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--discord-pid" && i + 1 < argc) {
            parent_pid = static_cast<DWORD>(std::wcstoul(argv[++i], nullptr, 10));
        }
    }

    const auto app_dir = current_module_path().parent_path();
    HANDLE mutex = CreateMutexW(nullptr, FALSE, runtime_mutex_name(app_dir).c_str());
    if (mutex == nullptr) {
        return 4;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }

    const int runtime_status = start_embedded_runtime(app_dir);
    if (runtime_status != 0) {
        CloseHandle(mutex);
        return runtime_status;
    }

    if (parent_pid == 0) {
        CloseHandle(mutex);
        return 0;
    }
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parent_pid);
    if (parent == nullptr) {
        CloseHandle(mutex);
        return 3;
    }
    WaitForSingleObject(parent, INFINITE);
    CloseHandle(parent);
    CloseHandle(mutex);
    return 0;
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
        case WM_CREATE: {
            g_ui.title = create_label(hwnd, L"Discord Tunnel", 58, 22, 500, 36, g_title_font);
            g_ui.subtitle = create_label(
                hwnd,
                L"Private, low-latency connectivity for Discord voice and media",
                58,
                62,
                620,
                22,
                g_subtitle_font);
            g_ui.connection_heading = create_label(hwnd, L"Connection", 48, 148, 240, 24, g_heading_font);
            g_ui.client_heading = create_label(hwnd, L"Client setup", 48, 360, 240, 24, g_heading_font);

            create_label(hwnd, L"Server address", 48, 182, 300, 20);
            create_label(hwnd, L"Port", 568, 182, 144, 20);
            create_label(hwnd, L"Access token", 48, 250, 300, 20);
            create_label(hwnd, L"Discord installation", 48, 398, 300, 20);
            create_label(hwnd, L"CA certificate", 48, 466, 300, 20);

            g_ui.server = create_edit(hwnd, kIdServer, 48, 204, 500, 32);
            g_ui.port = create_edit(hwnd, kIdPort, 568, 204, 144, 32, ES_NUMBER);
            g_ui.token = create_edit(hwnd, kIdToken, 48, 272, 664, 32);
            g_ui.discord_path = create_edit(hwnd, kIdDiscordPath, 48, 420, 548, 32);
            g_ui.ca_cert = create_edit(hwnd, kIdCaCert, 48, 488, 548, 32);

            SendMessageW(g_ui.server, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"vpn.example.com"));
            SendMessageW(g_ui.token, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Paste the token from your server"));
            SendMessageW(
                g_ui.discord_path,
                EM_SETCUEBANNER,
                TRUE,
                reinterpret_cast<LPARAM>(L"Select the Discord installation folder"));
            SendMessageW(
                g_ui.ca_cert,
                EM_SETCUEBANNER,
                TRUE,
                reinterpret_cast<LPARAM>(L"Use the certificate exported by deploy.sh"));

            g_ui.skip_verify = CreateWindowW(
                L"BUTTON",
                L"Skip TLS certificate verification",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                472,
                356,
                240,
                26,
                hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSkipVerify)),
                nullptr,
                nullptr);
            set_control_font(g_ui.skip_verify, g_body_font);

            create_button(hwnd, L"Browse", kIdBrowseDiscordPath, 610, 419, 102, 34);
            create_button(hwnd, L"Browse", kIdBrowseCaCert, 610, 487, 102, 34);
            g_ui.helper_text = create_label(
                hwnd,
                L"Settings are applied to every detected Discord app version.",
                48,
                563,
                360,
                28,
                g_subtitle_font);
            create_button(hwnd, L"Uninstall", kIdUninstall, 424, 552, 136, 40);
            create_button(hwnd, L"Install client", kIdInstall, 576, 552, 136, 40);
            g_ui.status = CreateWindowW(
                L"STATIC",
                L"Ready. Enter your connection details to continue.",
                WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                68,
                625,
                640,
                42,
                hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdStatus)),
                nullptr,
                nullptr);
            set_control_font(g_ui.status, g_body_font);

            SetWindowTextW(g_ui.port, L"443");
            const auto root = default_discord_root();
            if (!root.empty()) {
                SetWindowTextW(g_ui.discord_path, root.wstring().c_str());
                load_existing_config_into_ui(root);
            }
            return 0;
        }
        case WM_PAINT:
            paint_window(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(l_param);
            if (item != nullptr && item->CtlType == ODT_BUTTON) {
                draw_button(*item);
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(w_param);
            HWND control = reinterpret_cast<HWND>(l_param);
            SetBkMode(dc, TRANSPARENT);
            if (control == g_ui.title) {
                SetTextColor(dc, RGB(255, 255, 255));
            } else if (control == g_ui.subtitle) {
                SetTextColor(dc, kMutedColor);
            } else if (control == g_ui.connection_heading || control == g_ui.client_heading) {
                SetTextColor(dc, kTextColor);
            } else if (control == g_ui.status) {
                SetTextColor(dc, RGB(147, 197, 253));
                SetBkColor(dc, kStatusColor);
                SetBkMode(dc, OPAQUE);
                return reinterpret_cast<LRESULT>(g_status_brush);
            } else {
                SetTextColor(dc, kMutedColor);
            }
            return reinterpret_cast<LRESULT>(GetStockObject(HOLLOW_BRUSH));
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(w_param);
            SetTextColor(dc, kTextColor);
            SetBkColor(dc, kEditColor);
            return reinterpret_cast<LRESULT>(g_edit_brush);
        }
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(w_param);
            SetTextColor(dc, kMutedColor);
            SetBkColor(dc, kCardColor);
            return reinterpret_cast<LRESULT>(g_card_brush);
        }
        case WM_COMMAND: {
            switch (LOWORD(w_param)) {
                case kIdBrowseCaCert: {
                    const auto selected = browse_for_ca_cert(hwnd);
                    if (!selected.empty()) {
                        SetWindowTextW(g_ui.ca_cert, selected.wstring().c_str());
                    }
                    return 0;
                }
                case kIdBrowseDiscordPath: {
                    const auto selected = browse_for_folder(hwnd);
                    if (!selected.empty()) {
                        SetWindowTextW(g_ui.discord_path, selected.wstring().c_str());
                        load_existing_config_into_ui(selected);
                    }
                    return 0;
                }
                case kIdInstall: {
                    const auto config = collect_config();
                    std::wstring status;
                    if (!validate_config(config, &status)) {
                        set_status(status);
                        MessageBoxW(hwnd, status.c_str(), L"Install failed", MB_ICONERROR);
                        return 0;
                    }
                    if (!install_files(config, &status)) {
                        set_status(status);
                        MessageBoxW(hwnd, status.c_str(), L"Install failed", MB_ICONERROR);
                        return 0;
                    }
                    set_status(status);
                    MessageBoxW(hwnd, status.c_str(), L"Install complete", MB_ICONINFORMATION);
                    return 0;
                }
                case kIdUninstall: {
                    const auto root = std::filesystem::path(window_text(g_ui.discord_path));
                    std::wstring status;
                    if (!uninstall_files(root, &status)) {
                        set_status(status);
                        MessageBoxW(hwnd, status.c_str(), L"Uninstall failed", MB_ICONERROR);
                        return 0;
                    }
                    set_status(status);
                    MessageBoxW(hwnd, status.c_str(), L"Uninstall complete", MB_ICONINFORMATION);
                    return 0;
                }
            }
            break;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, message, w_param, l_param);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--runtime") {
            const int exit_code = launch_runtime_mode(argc, argv);
            LocalFree(argv);
            CoUninitialize();
            return exit_code;
        }
    }

    INITCOMMONCONTROLSEX controls{sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    initialize_theme();

    const wchar_t kClassName[] = L"ManyserverDiscordInstaller";
    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    if (RegisterClassW(&wc) == 0) {
        cleanup_theme();
        LocalFree(argv);
        CoUninitialize();
        return 1;
    }

    RECT window_rect{0, 0, 760, 700};
    const DWORD window_style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&window_rect, window_style, FALSE);

    HWND hwnd = CreateWindowExW(
        0,
        kClassName,
        L"Manyserver Discord Client",
        window_style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        window_rect.right - window_rect.left,
        window_rect.bottom - window_rect.top,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (hwnd == nullptr) {
        cleanup_theme();
        LocalFree(argv);
        CoUninitialize();
        return 1;
    }

    enable_dark_title_bar(hwnd);
    ShowWindow(hwnd, show_command);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    LocalFree(argv);
    cleanup_theme();
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
