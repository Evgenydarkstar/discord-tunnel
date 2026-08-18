// OS-level export forwarders for the 17 version.dll public functions.
//
// Each #pragma below tells the MSVC linker to emit a forwarder export
// in the PE header: callers binding to e.g. `version!GetFileVersionInfoA`
// are redirected at module-load time to
// `api-ms-win-core-version-l1-1-0!GetFileVersionInfoA`, which the
// Windows API Set Schema resolves to the real implementation (typically
// kernelbase / kernel32 in modern Windows).
//
// Why pragmas instead of a .def file?
//   The .def file syntax `Name = Module.Function` is documented as a
//   forwarder form, but in practice MSVC's linker in this configuration
//   treats it as a local-alias request and emits LNK2001 ("unresolved
//   external symbol"). The /EXPORT linker switch with the same target
//   syntax is unambiguous and works reliably.
//
// Why apisets instead of the literal system "version.dll"?
//   Forwarding to "version.dll" would create a recursion: our DLL is
//   *also* named version.dll, so the loader would forward calls back
//   to us. Apisets are uniquely named and resolve to the canonical
//   implementation DLL via the OS schema.

#pragma comment(linker, "/EXPORT:GetFileVersionInfoA=api-ms-win-core-version-l1-1-0.GetFileVersionInfoA,@1")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoByHandle=api-ms-win-core-version-l1-1-0.GetFileVersionInfoByHandle,@2")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoExA=api-ms-win-core-version-l1-1-0.GetFileVersionInfoExA,@3")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoExW=api-ms-win-core-version-l1-1-0.GetFileVersionInfoExW,@4")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeA=api-ms-win-core-version-l1-1-0.GetFileVersionInfoSizeA,@5")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeExA=api-ms-win-core-version-l1-1-0.GetFileVersionInfoSizeExA,@6")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeExW=api-ms-win-core-version-l1-1-0.GetFileVersionInfoSizeExW,@7")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeW=api-ms-win-core-version-l1-1-0.GetFileVersionInfoSizeW,@8")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoW=api-ms-win-core-version-l1-1-0.GetFileVersionInfoW,@9")
#pragma comment(linker, "/EXPORT:VerFindFileA=api-ms-win-core-version-l1-1-0.VerFindFileA,@10")
#pragma comment(linker, "/EXPORT:VerFindFileW=api-ms-win-core-version-l1-1-0.VerFindFileW,@11")
#pragma comment(linker, "/EXPORT:VerInstallFileA=api-ms-win-core-version-l1-1-0.VerInstallFileA,@12")
#pragma comment(linker, "/EXPORT:VerInstallFileW=api-ms-win-core-version-l1-1-0.VerInstallFileW,@13")
#pragma comment(linker, "/EXPORT:VerLanguageNameA=api-ms-win-core-version-l1-1-0.VerLanguageNameA,@14")
#pragma comment(linker, "/EXPORT:VerLanguageNameW=api-ms-win-core-version-l1-1-0.VerLanguageNameW,@15")
#pragma comment(linker, "/EXPORT:VerQueryValueA=api-ms-win-core-version-l1-1-0.VerQueryValueA,@16")
#pragma comment(linker, "/EXPORT:VerQueryValueW=api-ms-win-core-version-l1-1-0.VerQueryValueW,@17")

// This translation unit otherwise contains no code; the pragmas above
// are processed at compile time and embedded into the .obj's directive
// section, which the linker reads during the link step.
namespace { struct anchor {}; }
