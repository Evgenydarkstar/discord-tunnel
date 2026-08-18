#include <Windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <filesystem>
#include <string>
#include <vector>

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
constexpr int kIdBrowse = 1007;
constexpr int kIdInstall = 1008;
constexpr int kIdUninstall = 1009;
constexpr int kIdStatus = 1010;

struct UiState {
    HWND server = nullptr;
    HWND port = nullptr;
    HWND token = nullptr;
    HWND ca_cert = nullptr;
    HWND discord_path = nullptr;
    HWND skip_verify = nullptr;
    HWND status = nullptr;
};

UiState g_ui;

std::wstring window_text(HWND hwnd) {
    const int len = GetWindowTextLengthW(hwnd);
    std::wstring text(len + 1, L'\0');
    GetWindowTextW(hwnd, text.data(), len + 1);
    text.resize(len);
    return text;
}

void set_status(const std::wstring& message) {
    SetWindowTextW(g_ui.status, message.c_str());
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
    const auto config = load_config(config_path_for_app_dir(app_dirs.front()));
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
    if (config.discord_root.empty()) {
        *error = L"Discord Path is required.";
        return false;
    }
    if (!std::filesystem::is_directory(config.discord_root)) {
        *error = L"Discord Path does not exist.";
        return false;
    }
    if (!config.ca_cert_path.empty() && !std::filesystem::is_regular_file(config.ca_cert_path)) {
        *error = L"CA Cert path does not exist.";
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

void create_label(HWND parent, const wchar_t* text, int x, int y, int w, int h) {
    CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, parent, nullptr, nullptr, nullptr);
}

HWND create_edit(HWND parent, int id, int x, int y, int w, int h, DWORD extra_style = 0) {
    return CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extra_style,
        x,
        y,
        w,
        h,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        nullptr,
        nullptr);
}

void launch_runtime_mode(int argc, wchar_t** argv) {
    DWORD parent_pid = 0;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--discord-pid" && i + 1 < argc) {
            parent_pid = static_cast<DWORD>(std::wcstoul(argv[++i], nullptr, 10));
        }
    }

    if (parent_pid == 0) {
        Sleep(5000);
        return;
    }
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parent_pid);
    if (parent == nullptr) {
        Sleep(5000);
        return;
    }
    WaitForSingleObject(parent, INFINITE);
    CloseHandle(parent);
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
        case WM_CREATE: {
            create_label(hwnd, L"Server", 20, 20, 120, 20);
            create_label(hwnd, L"Port", 20, 60, 120, 20);
            create_label(hwnd, L"Token", 20, 100, 120, 20);
            create_label(hwnd, L"CA Cert", 20, 140, 120, 20);
            create_label(hwnd, L"Discord Path", 20, 180, 120, 20);

            g_ui.server = create_edit(hwnd, kIdServer, 140, 18, 340, 24);
            g_ui.port = create_edit(hwnd, kIdPort, 140, 58, 120, 24);
            g_ui.token = create_edit(hwnd, kIdToken, 140, 98, 340, 24);
            g_ui.ca_cert = create_edit(hwnd, kIdCaCert, 140, 138, 340, 24);
            g_ui.discord_path = create_edit(hwnd, kIdDiscordPath, 140, 178, 280, 24);
            g_ui.skip_verify = CreateWindowW(
                L"BUTTON",
                L"Skip TLS verify",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                140,
                214,
                180,
                24,
                hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSkipVerify)),
                nullptr,
                nullptr);
            CreateWindowW(
                L"BUTTON",
                L"Browse",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                430,
                178,
                80,
                24,
                hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBrowse)),
                nullptr,
                nullptr);
            CreateWindowW(
                L"BUTTON",
                L"Install",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                140,
                255,
                120,
                32,
                hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdInstall)),
                nullptr,
                nullptr);
            CreateWindowW(
                L"BUTTON",
                L"Uninstall",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                270,
                255,
                120,
                32,
                hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdUninstall)),
                nullptr,
                nullptr);
            g_ui.status = CreateWindowW(
                L"STATIC",
                L"Ready",
                WS_CHILD | WS_VISIBLE,
                20,
                305,
                490,
                60,
                hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdStatus)),
                nullptr,
                nullptr);

            SetWindowTextW(g_ui.port, L"443");
            const auto root = default_discord_root();
            if (!root.empty()) {
                SetWindowTextW(g_ui.discord_path, root.wstring().c_str());
                load_existing_config_into_ui(root);
            }
            return 0;
        }
        case WM_COMMAND: {
            switch (LOWORD(w_param)) {
                case kIdBrowse: {
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
            launch_runtime_mode(argc, argv);
            LocalFree(argv);
            CoUninitialize();
            return 0;
        }
    }

    INITCOMMONCONTROLSEX controls{sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    const wchar_t kClassName[] = L"ManyserverDiscordInstaller";
    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        0,
        kClassName,
        L"Manyserver Discord Client",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        550,
        430,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (hwnd == nullptr) {
        LocalFree(argv);
        return 1;
    }

    ShowWindow(hwnd, show_command);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    LocalFree(argv);
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
