# Discord Client

The Windows client proxies only Discord traffic and consists of two main
artifacts:

- `discord-tunnel.exe` - GUI for setup and configuration;
- `version.dll` - loadable Discord network shim with a built-in Rust runtime.

## Supported paths

- messages and files: local bridge -> QUIC/H3 CONNECT -> server;
- voice: raw UDP bridge -> QUIC DATAGRAM;
- camera and screen share: the same UDP path with fragmentation of large packets;
- under load, small realtime packets get priority over the video stream.

The server uses a single QUIC/H3 circuit with a token. TCP and UDP destinations
are restricted by the server allowlist.

## Build

Requirements:

- Windows 10/11 x64;
- Rust stable;
- Visual Studio 2022 Build Tools with C++ x64;
- CMake 3.20+;
- PowerShell 7.

```powershell
cd discord
./build.ps1 -Config Release
```

The resulting bundle is created in `discord/build/Release/`:

- `discord-tunnel.exe`;
- `version.dll`;
- `discord-tunnel.ini` - template without working credentials.

The source build also remains in `discord/discord-tunnel/build-output/` under the
name `discord-tunnel.exe`.

## Install

1. Fully close Discord, including the tray icon.
2. Run `discord-tunnel.exe`.
3. Enter `Server`, `Port` and `Token`.
4. Keep TLS checking enabled. If a self-signed certificate is used, install the
   server CA certificate (`ca-cert.pem`) on the machine (Certificates > Current
   User > Trusted Root Certification Authorities) or set `ca_cert_path` in
   `discord-tunnel.ini`.
5. Click `Install`, then start Discord manually.

`discord-tunnel.ini` with a real token must not be published or replaced with
the build template. When manually updating an installed client, replace only
`version.dll`.