#define VER_H
#include <Windows.h>

#include <cwchar>
#include <filesystem>
#include <string>

#include "discord/shared/fs_utils.h"

namespace {

using namespace discord_client;

HMODULE g_system_version = nullptr;

#define EXPORT extern "C" __declspec(dllexport)

void ensure_system_version_loaded();
void launch_sidecar_runtime();

bool is_primary_discord_process() {
    const wchar_t* command_line = GetCommandLineW();
    if (command_line == nullptr) {
        return false;
    }

    return std::wcsstr(command_line, L" --type=") == nullptr;
}

template <typename T>
T load_symbol(const char* name) {
    ensure_system_version_loaded();
    return reinterpret_cast<T>(GetProcAddress(g_system_version, name));
}

std::filesystem::path system_version_path() {
    wchar_t system_dir[MAX_PATH];
    const UINT len = GetSystemDirectoryW(system_dir, MAX_PATH);
    return std::filesystem::path(std::wstring(system_dir, len)) / L"version.dll";
}

void ensure_system_version_loaded() {
    if (g_system_version != nullptr) {
        return;
    }
    g_system_version = LoadLibraryW(system_version_path().c_str());
}

DWORD WINAPI launch_sidecar_runtime_thread(void*) {
    try {
        launch_sidecar_runtime();
    } catch (...) {
        // An injected DLL must never let bootstrap failures terminate Discord.
    }
    return 0;
}

void launch_sidecar_runtime() {
    const auto app_dir = current_module_path().parent_path();
    const auto helper = helper_path_for_app_dir(app_dir);
    const auto config = config_path_for_app_dir(app_dir);
    std::error_code ec;
    const bool helper_exists = std::filesystem::is_regular_file(helper, ec);
    ec.clear();
    const bool config_exists = std::filesystem::is_regular_file(config, ec);
    if (!helper_exists || !config_exists) {
        return;
    }

    const std::wstring mutex_name = L"Local\\ManyserverDiscordRuntime-" + std::to_wstring(GetCurrentProcessId());
    HANDLE mutex = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
    if (mutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex != nullptr) {
            CloseHandle(mutex);
        }
        return;
    }

    std::wstring command = L"\"" + helper.wstring() + L"\" --runtime --discord-pid " +
        std::to_wstring(GetCurrentProcessId());

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(
            helper.c_str(),
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW | DETACHED_PROCESS,
            nullptr,
            app_dir.c_str(),
            &si,
            &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    CloseHandle(mutex);
}

}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        if (!is_primary_discord_process()) {
            return TRUE;
        }
        HANDLE thread = CreateThread(nullptr, 0, launch_sidecar_runtime_thread, nullptr, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}

EXPORT DWORD WINAPI GetFileVersionInfoSizeA(LPCSTR lptstrFilename, LPDWORD lpdwHandle) {
    using Fn = DWORD(WINAPI*)(LPCSTR, LPDWORD);
    return load_symbol<Fn>("GetFileVersionInfoSizeA")(lptstrFilename, lpdwHandle);
}

EXPORT DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR lptstrFilename, LPDWORD lpdwHandle) {
    using Fn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
    return load_symbol<Fn>("GetFileVersionInfoSizeW")(lptstrFilename, lpdwHandle);
}

EXPORT BOOL WINAPI GetFileVersionInfoA(LPCSTR lptstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData) {
    using Fn = BOOL(WINAPI*)(LPCSTR, DWORD, DWORD, LPVOID);
    return load_symbol<Fn>("GetFileVersionInfoA")(lptstrFilename, dwHandle, dwLen, lpData);
}

EXPORT BOOL WINAPI GetFileVersionInfoW(LPCWSTR lptstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData) {
    using Fn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
    return load_symbol<Fn>("GetFileVersionInfoW")(lptstrFilename, dwHandle, dwLen, lpData);
}

EXPORT BOOL WINAPI VerQueryValueA(LPCVOID pBlock, LPCSTR lpSubBlock, LPVOID* lplpBuffer, PUINT puLen) {
    using Fn = BOOL(WINAPI*)(LPCVOID, LPCSTR, LPVOID*, PUINT);
    return load_symbol<Fn>("VerQueryValueA")(pBlock, lpSubBlock, lplpBuffer, puLen);
}

EXPORT BOOL WINAPI VerQueryValueW(LPCVOID pBlock, LPCWSTR lpSubBlock, LPVOID* lplpBuffer, PUINT puLen) {
    using Fn = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    return load_symbol<Fn>("VerQueryValueW")(pBlock, lpSubBlock, lplpBuffer, puLen);
}

EXPORT DWORD WINAPI GetFileVersionInfoSizeExA(DWORD dwFlags, LPCSTR lpwstrFilename, LPDWORD lpdwHandle) {
    using Fn = DWORD(WINAPI*)(DWORD, LPCSTR, LPDWORD);
    return load_symbol<Fn>("GetFileVersionInfoSizeExA")(dwFlags, lpwstrFilename, lpdwHandle);
}

EXPORT DWORD WINAPI GetFileVersionInfoSizeExW(DWORD dwFlags, LPCWSTR lpwstrFilename, LPDWORD lpdwHandle) {
    using Fn = DWORD(WINAPI*)(DWORD, LPCWSTR, LPDWORD);
    return load_symbol<Fn>("GetFileVersionInfoSizeExW")(dwFlags, lpwstrFilename, lpdwHandle);
}

EXPORT BOOL WINAPI GetFileVersionInfoExA(DWORD dwFlags, LPCSTR lpwstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData) {
    using Fn = BOOL(WINAPI*)(DWORD, LPCSTR, DWORD, DWORD, LPVOID);
    return load_symbol<Fn>("GetFileVersionInfoExA")(dwFlags, lpwstrFilename, dwHandle, dwLen, lpData);
}

EXPORT BOOL WINAPI GetFileVersionInfoExW(DWORD dwFlags, LPCWSTR lpwstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData) {
    using Fn = BOOL(WINAPI*)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID);
    return load_symbol<Fn>("GetFileVersionInfoExW")(dwFlags, lpwstrFilename, dwHandle, dwLen, lpData);
}

EXPORT DWORD WINAPI VerLanguageNameA(DWORD wLang, LPSTR szLang, DWORD cchLang) {
    using Fn = DWORD(WINAPI*)(DWORD, LPSTR, DWORD);
    return load_symbol<Fn>("VerLanguageNameA")(wLang, szLang, cchLang);
}

EXPORT DWORD WINAPI VerLanguageNameW(DWORD wLang, LPWSTR szLang, DWORD cchLang) {
    using Fn = DWORD(WINAPI*)(DWORD, LPWSTR, DWORD);
    return load_symbol<Fn>("VerLanguageNameW")(wLang, szLang, cchLang);
}

EXPORT DWORD WINAPI VerFindFileA(DWORD uFlags, LPSTR szFileName, LPSTR szWinDir, LPSTR szAppDir, LPSTR szCurDir, PUINT lpuCurDirLen, LPSTR szDestDir, PUINT lpuDestDirLen) {
    using Fn = DWORD(WINAPI*)(DWORD, LPSTR, LPSTR, LPSTR, LPSTR, PUINT, LPSTR, PUINT);
    return load_symbol<Fn>("VerFindFileA")(uFlags, szFileName, szWinDir, szAppDir, szCurDir, lpuCurDirLen, szDestDir, lpuDestDirLen);
}

EXPORT DWORD WINAPI VerFindFileW(DWORD uFlags, LPWSTR szFileName, LPWSTR szWinDir, LPWSTR szAppDir, LPWSTR szCurDir, PUINT lpuCurDirLen, LPWSTR szDestDir, PUINT lpuDestDirLen) {
    using Fn = DWORD(WINAPI*)(DWORD, LPWSTR, LPWSTR, LPWSTR, LPWSTR, PUINT, LPWSTR, PUINT);
    return load_symbol<Fn>("VerFindFileW")(uFlags, szFileName, szWinDir, szAppDir, szCurDir, lpuCurDirLen, szDestDir, lpuDestDirLen);
}

EXPORT DWORD WINAPI VerInstallFileA(DWORD uFlags, LPSTR szSrcFileName, LPSTR szDestFileName, LPSTR szSrcDir, LPSTR szDestDir, LPSTR szCurDir, LPSTR szTmpFile, PUINT lpuTmpFileLen) {
    using Fn = DWORD(WINAPI*)(DWORD, LPSTR, LPSTR, LPSTR, LPSTR, LPSTR, LPSTR, PUINT);
    return load_symbol<Fn>("VerInstallFileA")(uFlags, szSrcFileName, szDestFileName, szSrcDir, szDestDir, szCurDir, szTmpFile, lpuTmpFileLen);
}

EXPORT DWORD WINAPI VerInstallFileW(DWORD uFlags, LPWSTR szSrcFileName, LPWSTR szDestFileName, LPWSTR szSrcDir, LPWSTR szDestDir, LPWSTR szCurDir, LPWSTR szTmpFile, PUINT lpuTmpFileLen) {
    using Fn = DWORD(WINAPI*)(DWORD, LPWSTR, LPWSTR, LPWSTR, LPWSTR, LPWSTR, LPWSTR, PUINT);
    return load_symbol<Fn>("VerInstallFileW")(uFlags, szSrcFileName, szDestFileName, szSrcDir, szDestDir, szCurDir, szTmpFile, lpuTmpFileLen);
}

EXPORT DWORD WINAPI GetFileVersionInfoByHandle(DWORD dwFlags, HANDLE hFile, LPDWORD lpdwHandle, DWORD dwLen, LPVOID lpData) {
    using Fn = DWORD(WINAPI*)(DWORD, HANDLE, LPDWORD, DWORD, LPVOID);
    return load_symbol<Fn>("GetFileVersionInfoByHandle")(dwFlags, hFile, lpdwHandle, dwLen, lpData);
}
