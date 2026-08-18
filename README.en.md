# Discord Tunnel

[Русский](README.md) | [**English**](README.en.md)

Discord Tunnel is a self-hosted tunnel for Discord consisting of a Linux server
and a Windows client application. The client redirects Discord network traffic
only; it does not create a system-wide VPN or proxy for other applications.

[Download the Windows client](https://github.com/Evgenydarkstar/discord-tunnel/releases/latest/download/discord-tunnel-windows-x64.zip)

## Features

- messages, attachments, and other TCP traffic over QUIC/HTTP/3 CONNECT;
- voice, camera, and screen sharing over QUIC Datagram;
- prioritization of small voice packets over video traffic under load;
- token-based client authentication;
- a server allowlist restricting accessible destinations to Discord hosts;
- automated Docker server deployment and a Windows GUI installer.

## How it works

```text
Discord for Windows
        |
        | version.dll intercepts Discord connections only
        v
Embedded client (Rust) === QUIC/HTTP/3, UDP ===> Discord Tunnel Server
                                                       |
                                                       v
                                                Discord services
```

The Windows client installs a network module into the Discord directory. The
server accepts an encrypted QUIC connection, validates its token, and permits
access only to configured destinations. Multiple client computers can use one
server when configured with the same token.

## Server installation

### Requirements

- a Linux server with a public IP address;
- an open inbound UDP port, `443` by default;
- `root` privileges or `sudo` access;
- `curl` or `wget`.

### Network ports

Only one inbound port is required between the client and the server:

| Direction | Protocol | Port | Purpose |
| --- | --- | --- | --- |
| Inbound | UDP | `443` or the port selected during setup | QUIC/HTTP/3 between client and server |
| Inbound | TCP | not required | The tunnel does not accept TCP connections from clients |

QUIC and HTTP/3 run over UDP, so you do not need to expose or forward inbound
`TCP 443`. The server still needs outbound access to Discord services: TCP for
HTTPS connections and UDP for voice, camera, and screen sharing. If outbound
traffic is restricted by a firewall, allow TCP and UDP to the Discord hosts and
ports covered by the configured destination allowlist.

### Quick installation

Download and run the interactive installer:

```bash
curl -fsSLO https://raw.githubusercontent.com/Evgenydarkstar/discord-tunnel/main/deploy.sh
chmod +x deploy.sh
sudo ./deploy.sh
```

The installer will:

1. install Docker and Docker Compose when needed;
2. ask for the UDP port and the server's public IP address;
3. generate an access token;
4. offer a safe allowlist of Discord domains;
5. build and start the container;
6. export the client CA certificate to `ca-cert.pem`.

After installation, save the `Server`, `Port`, and `Token` values and transfer
`ca-cert.pem` to the client computer over a secure channel. The token grants
access to the tunnel, so do not publish it in a repository, message, or
screenshot.

If the server is behind NAT, forward the selected UDP port. Allow the same port
through the server firewall and the cloud provider's security group.

### Installation from a repository clone

```bash
git clone https://github.com/Evgenydarkstar/discord-tunnel.git
cd discord-tunnel
sudo ./deploy.sh
```

The installer prints the relevant management commands when it finishes. For a
standard installation, they are:

```bash
docker compose -f docker-compose.deploy.yml logs -f discord-tunnel
docker compose -f docker-compose.deploy.yml restart discord-tunnel
sudo ./deploy.sh uninstall
```

## Windows client installation

### Requirements

- Windows 10 or Windows 11 x64;
- the official Discord Desktop application;
- the connection settings and `ca-cert.pem` produced during server setup.

### Installation steps

1. Download the [latest Windows release](https://github.com/Evgenydarkstar/discord-tunnel/releases/latest/download/discord-tunnel-windows-x64.zip).
2. Extract the entire ZIP into a separate folder. `discord-tunnel.exe` and
   `version.dll` must remain next to each other.
3. Fully close Discord, including its system tray icon.
4. Run `discord-tunnel.exe`.
5. Enter the server IP address or hostname in `Server`, without `https://`.
6. Enter the `Port` and `Token` shown by the server installer.
7. Select the `ca-cert.pem` copied from the server in `CA Cert`.
8. Check the Discord path. It is normally detected automatically as
   `%LOCALAPPDATA%\Discord`.
9. Leave `Skip TLS verify` disabled and click `Install`.
10. Start Discord normally after the installation succeeds.

The build is not currently signed with a commercial Windows certificate, so
SmartScreen may display a warning. Download the archive only from the project's
[Releases](https://github.com/Evgenydarkstar/discord-tunnel/releases) page.

### Updating and removing the client

To update, fully close Discord, extract the new release, and click `Install`
again. To remove the client module, close Discord, run `discord-tunnel.exe`,
check the Discord path, and click `Uninstall`.

A Discord update may create a new `app-*` directory. If the tunnel stops working
after an update, run the client installer again and click `Install`.

## Building the client from source

The build requires Windows x64, Rust stable, Visual Studio Build Tools with C++,
CMake 3.20+, and PowerShell 7:

```powershell
cd client/discord
./build.ps1 -Config Release
```

The resulting archive is written to
`client/discord/build/discord-tunnel-windows-x64.zip`. See
[`client/discord/README.md`](client/discord/README.md) for additional details.

## Project structure

- `server/` - Python QUIC/HTTP/3 server and Docker configuration;
- `client/discord/` - Windows GUI, native network module, and Rust runtime;
- `deploy.sh` - interactive server installation and removal;
- `.github/workflows/windows-release.yml` - Windows archive build and GitHub
  Release publication for `v*` tags.

## Security

- do not disable TLS verification unless necessary;
- do not publish the `Token`, `.env`, or an installed `discord-tunnel.ini`;
- keep the destination allowlist restricted to Discord domains;
- regularly update the server and client from the trusted repository.

This project is intended for use on your own devices and servers. You are
responsible for complying with applicable laws and the terms of the services
you use.
