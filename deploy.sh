#!/usr/bin/env bash
#
# discord-tunnel deploy script
# Deploys the Discord tunnel server on any Linux distribution using Docker.
#
# Flow:
#   1. If run standalone (this script copied outside the project), fetch the
#      project from GitHub and re-run from the downloaded checkout
#   2. Detect the Linux distribution
#   3. Check Docker availability and offer to install it
#   4. Collect server settings (port, public IP, token)
#   5. Start the server with docker compose
#   6. Export the CA certificate for the client
#
# Run with "uninstall" to stop the server and remove deployment files.
#
set -euo pipefail

SERVER_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SERVER_DIR"

# GitHub repository that hosts this project. Used to fetch the project when
# this script is run standalone: the user copies only this script and it pulls
# the rest of the project from GitHub.
REPO_OWNER="Evgenydarkstar"
REPO_NAME="discord-tunnel"
REPO_BRANCH="main"
REPO_URL="https://github.com/$REPO_OWNER/$REPO_NAME"

step()  { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
info()  { printf '\033[0;37m%s\033[0m\n' "$*"; }
ok()    { printf '\033[0;32m%s\033[0m\n' "$*"; }
warn()  { printf '\033[0;33mWARNING: %s\033[0m\n' "$*" >&2; }
die()   { printf '\033[0;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

prompt() {
    # prompt <question> <default>  -> echoes the chosen value (question goes to stderr)
    local question="$1" default="$2" answer=""
    printf '%s [%s]: ' "$question" "$default" >&2
    IFS= read -r answer || true
    answer="${answer:-$default}"
    printf '%s' "$answer"
}

confirm() {
    # confirm <question> <default-yes> -> true/false
    local question="$1" default="$2" answer=""
    printf '%s [%s]: ' "$question" "$default"
    IFS= read -r answer || true
    case "${answer:-$default}" in
        y|Y|yes|YES) return 0 ;;
        *) return 1 ;;
    esac
}

is_root() { [ "$(id -u)" -eq 0 ]; }

is_full_checkout() {
    # True when the script lives inside the project checkout (server/ present).
    [ -d "$SERVER_DIR/server" ]
}

bootstrap_from_github() {
    # Fetch the project from GitHub into a local install directory and re-run
    # the deploy script from there. Called only when this script is run
    # standalone (copied outside the project).
    step "Fetching the project from GitHub"
    local install_dir="${DISCORD_TUNNEL_DIR:-$HOME/.discord-tunnel}"
    local tmp tar
    tmp="$(mktemp -d)"
    tar="$tmp/$REPO_NAME.tar.gz"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --max-time 120 -o "$tar" "$REPO_URL/archive/refs/heads/$REPO_BRANCH.tar.gz" \
            || die "Failed to download the project from $REPO_URL (branch $REPO_BRANCH)."
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$tar" "$REPO_URL/archive/refs/heads/$REPO_BRANCH.tar.gz" \
            || die "Failed to download the project from $REPO_URL (branch $REPO_BRANCH)."
    else
        die "Neither curl nor wget is available; cannot fetch the project."
    fi
    mkdir -p "$install_dir"
    tar -xzf "$tar" -C "$tmp"
    cp -a "$tmp/$REPO_NAME-$REPO_BRANCH/." "$install_dir/"
    rm -rf "$tmp"
    ok "Project downloaded to $install_dir"
    info "Re-running the deploy script from the downloaded project..."
    exec bash "$install_dir/deploy.sh" "$@"
    exit 0  # unreachable: exec replaces this process
}

require_root() {
    if ! is_root; then
        die "This script must be run as root (sudo ./deploy.sh)."
    fi
}

detect_distro() {
    if [ -r /etc/os-release ]; then
        # shellcheck disable=SC1091
        . /etc/os-release
        distro_id="${ID:-unknown}"
        distro_like="${ID_LIKE:-}"
        distro_name="${NAME:-$distro_id}"
    elif [ -r /etc/redhat-release ]; then
        distro_id="rhel"
        distro_like=""
        distro_name="$(cat /etc/redhat-release)"
    else
        distro_id="unknown"
        distro_like=""
        distro_name="unknown Linux"
    fi
    echo "Detected distribution: $distro_name ($distro_id)"
}

# Map distro to package manager
pm_install_docker() {
    case "$distro_id" in
        debian|ubuntu|linuxmint|raspbian)
            apt-get update
            DEBIAN_FRONTEND=noninteractive apt-get install -y docker.io docker-compose-v2 || \
                DEBIAN_FRONTEND=noninteractive apt-get install -y docker.io docker-compose
            ;;
        fedora)
            dnf -y install docker docker-compose-plugin || dnf -y install docker docker-compose
            systemctl enable --now docker
            ;;
        centos|rhel|rocky|almalinux|amzn)
            dnf -y install docker docker-compose-plugin || yum -y install docker docker-compose-plugin
            systemctl enable --now docker
            ;;
        opensuse*|sles)
            zypper --non-interactive install docker docker-compose-plugin || \
                zypper --non-interactive install docker docker-compose
            systemctl enable --now docker
            ;;
        arch|manjaro)
            pacman -Syu --noconfirm docker docker-compose
            systemctl enable --now docker
            ;;
        alpine)
            apk add --no-cache docker docker-cli-compose
            rc-update add docker default
            service docker start
            ;;
        *)
            return 1
            ;;
    esac
}

install_docker() {
    step "Installing Docker"
    if ! is_root; then
        die "Root privileges are required to install Docker. Re-run as root."
    fi
    if pm_install_docker; then
        ok "Docker installed via the distribution package manager."
        return 0
    fi
    warn "Package manager not recognized; falling back to the official install script."
    if confirm "Download and run get.docker.com?" "y"; then
        curl -fsSL https://get.docker.com | sh
        ok "Docker installed via the official script."
        return 0
    fi
    die "Docker installation was declined."
}

check_docker() {
    step "Checking Docker"
    if ! command -v docker >/dev/null 2>&1; then
        warn "Docker is not installed."
        if confirm "Install Docker now?" "y"; then
            install_docker
        else
            die "Docker is required to deploy the server."
        fi
    fi
    if docker compose version >/dev/null 2>&1; then
        docker_compose="docker compose"
    elif command -v docker-compose >/dev/null 2>&1; then
        docker_compose="docker-compose"
    else
        warn "The 'docker compose' plugin is not available."
        if confirm "Install Docker again with Compose support?" "y"; then
            install_docker
        else
            die "Docker Compose is required."
        fi
    fi
    if ! docker info >/dev/null 2>&1; then
        warn "Docker daemon is not running or not accessible."
        if command -v systemctl >/dev/null 2>&1; then
            systemctl start docker || true
        fi
        sleep 2
    fi
    docker info >/dev/null 2>&1 || die "Cannot reach the Docker daemon."
    ok "Docker is available ($docker_compose)."
}

valid_port() {
    [ "$1" -ge 1 ] && [ "$1" -le 65535 ]
}

port_in_use() {
    # returns 0 if the port is already bound
    if [ -d /proc/net ]; then
        grep -q ":$1 " /proc/net/tcp /proc/net/tcp6 2>/dev/null && return 0
        grep -q ":$1 " /proc/net/udp /proc/net/udp6 2>/dev/null && return 0
    fi
    return 1
}

detect_public_ip() {
    # Prefer the local interface address; fall back to an external service.
    local ip=""
    ip="$(ip route get 1.1.1.1 2>/dev/null | awk '{for (i=1;i<=NF;i++) if ($i=="src") {print $(i+1); exit}}')"
    if [ -z "$ip" ] && command -v curl >/dev/null 2>&1; then
        ip="$(curl -fsS --max-time 10 https://api.ipify.org 2>/dev/null || true)"
    fi
    printf '%s' "$ip"
}

generate_token() {
    if command -v openssl >/dev/null 2>&1; then
        openssl rand -hex 24
    else
        od -An -N24 -tx1 /dev/urandom | tr -d ' \n'
    fi
}

collect_settings() {
    step "Server settings"

    local port="" public_ip="" token="" allowed_hosts="" custom_hosts=""
    local default_allowed_hosts="discord.com,discord.gg,discord.media,discordapp.com,discordapp.net,discord-attachments-uploads-prd.storage.googleapis.com"

    while :; do
        port="$(prompt "Port for the tunnel (UDP)" "443")"
        case "$port" in
            ''|*[!0-9]*) warn "Invalid port: $port (must be a number 1-65535)" ;;
            *) if valid_port "$port"; then break; else warn "Invalid port: $port (must be 1-65535)"; fi ;;
        esac
    done
    if port_in_use "$port"; then
        warn "Port $port appears to be already in use."
    fi

    local auto_ip=""
    auto_ip="$(detect_public_ip)"
    if [ -n "$auto_ip" ]; then
        info "Detected public IP: $auto_ip"
        public_ip="$(prompt "Public IP of this server" "$auto_ip")"
    else
        public_ip="$(prompt "Public IP of this server" "")"
    fi
    [ -n "$public_ip" ] || die "Public IP cannot be empty."

    token="$(generate_token)"
    info "Generated token: $token"
    if confirm "Use this token for the client?" "y"; then
        :
    else
        token="$(prompt "Enter your own token" "")"
        [ -n "$token" ] || die "Token cannot be empty."
    fi

    info "Allowed hosts restrict which domains the tunnel can reach."
    if confirm "Restrict to the default Discord host list?" "y"; then
        allowed_hosts="$default_allowed_hosts"
    else
        allowed_hosts="$(prompt "Allowed hosts (comma-separated)" "")"
        [ -n "$allowed_hosts" ] || die "Allowed hosts cannot be empty."
    fi

    server_port="$port"
    server_ip="$public_ip"
    server_token="$token"
    server_allowed_hosts="$allowed_hosts"
    echo "Server IP  : $server_ip"
    echo "Port       : $server_port"
    echo "Allowed    : $(echo "$server_allowed_hosts" | tr ',' '\n' | grep -c .) host(s)"
}

write_env() {
    step "Writing .env"
    local env_file="$SERVER_DIR/.env"
    if [ -f "$env_file" ]; then
        if confirm "A .env file already exists. Overwrite it?" "n"; then
            cp "$env_file" "$env_file.bak"
            info "Backup saved to .env.bak"
        else
            die "Deployment aborted (existing .env kept)."
        fi
    fi
    cat > "$env_file" <<EOF
VPN_TYPE=discord
VPN_PORT=$server_port
PUBLIC_IP=$server_ip
DISCORD_HTTP3_TOKEN=$server_token
DISCORD_HTTP3_ALLOWED_HOSTS=$server_allowed_hosts
WORKER_ID=local-worker
WORKER_TOKEN=$server_token
EOF
    ok ".env written."
}

write_compose() {
    step "Writing docker-compose.deploy.yml"
    cat > "$SERVER_DIR/docker-compose.deploy.yml" <<EOF
services:
  discord-http3-runtime:
    build:
      context: .
      dockerfile: server/Dockerfile.discord
    restart: unless-stopped
    entrypoint: ["/app/scripts/start-discord-http3.sh"]
    ports:
      - "${server_port}:${server_port}/udp"
    env_file:
      - .env
    environment:
      DISCORD_HTTP3_CERT_PATH: "/data/run/discord-http3/tls.crt"
      DISCORD_HTTP3_KEY_PATH: "/data/run/discord-http3/tls.key"
    volumes:
      - ./data:/data
EOF
    ok "docker-compose.deploy.yml written."
}

start_server() {
    step "Building and starting the server"
    $docker_compose -f "$SERVER_DIR/docker-compose.deploy.yml" up -d --build
    ok "Server is starting."
}

export_ca_certificate() {
    step "Exporting the CA certificate"
    local ca_file="$SERVER_DIR/ca-cert.pem"
    local ca_path=""
    for _ in $(seq 1 30); do
        ca_path="$($docker_compose -f "$SERVER_DIR/docker-compose.deploy.yml" exec -T discord-http3-runtime cat /data/certs/_default/ca-cert.pem 2>/dev/null || true)"
        if [ -n "$ca_path" ]; then
            break
        fi
        sleep 2
    done
    if [ -n "$ca_path" ]; then
        printf '%s\n' "$ca_path" > "$ca_file"
        chmod 644 "$ca_file"
        ok "CA certificate exported to $ca_file"
    else
        warn "Could not read the CA certificate yet. It will be created on first run."
    fi
}

show_summary() {
    step "Deployment summary"
    echo
    echo "The Discord tunnel server is running."
    echo
    echo "Client settings (Windows app discord-tunnel.exe):"
    echo "  Server : $server_ip"
    echo "  Port   : $server_port"
    echo "  Token  : $server_token"
    if [ -f "$SERVER_DIR/ca-cert.pem" ]; then
        echo "  CA cert: $SERVER_DIR/ca-cert.pem  (install it on the client machine)"
    fi
    echo
    echo "Make sure UDP port $server_port is reachable:"
    echo "  - open it in the firewall/security group"
    echo "  - forward it if the server is behind NAT"
    echo
    info "Commands:"
    info "  $docker_compose -f $SERVER_DIR/docker-compose.deploy.yml logs -f discord-http3-runtime"
    info "  $docker_compose -f $SERVER_DIR/docker-compose.deploy.yml down"
    echo
}

compose_down() {
    # compose_down <compose-file>  -> best-effort stop+remove of the stack and its locally-built image
    local compose_file="$1"
    if ! command -v docker >/dev/null 2>&1; then
        return 1
    fi
    if docker compose version >/dev/null 2>&1; then
        if docker compose -f "$compose_file" down --rmi local >/dev/null 2>&1; then
            return 0
        fi
    fi
    if command -v docker-compose >/dev/null 2>&1; then
        if docker-compose -f "$compose_file" down --rmi local >/dev/null 2>&1; then
            return 0
        fi
    fi
    return 1
}

uninstall() {
    step "Uninstalling the Discord tunnel"
    local compose_file="$SERVER_DIR/docker-compose.deploy.yml"

    if [ -f "$compose_file" ]; then
        if confirm "Stop and remove the tunnel container and its image?" "y"; then
            if compose_down "$compose_file"; then
                ok "Container and image removed."
            else
                warn "Could not stop the stack (no container running or Docker unavailable)."
            fi
        fi
    else
        info "No docker-compose.deploy.yml found; no container to stop."
    fi

    # Best-effort cleanup of containers from a manual compose deployment.
    if command -v docker >/dev/null 2>&1; then
        docker rm -f discord-http3-runtime discord-http3 openconnect-control >/dev/null 2>&1 || true
    fi

    step "Removing deployment files"
    for f in docker-compose.deploy.yml .env ca-cert.pem; do
        if [ -f "$SERVER_DIR/$f" ]; then
            if confirm "Remove $f?" "y"; then
                rm -f "$SERVER_DIR/$f"
                ok "Removed $f"
            fi
        fi
    done

    if [ -d "$SERVER_DIR/data" ]; then
        if confirm "Remove the server data directory ($SERVER_DIR/data)? This deletes certificates, configs and the database." "n"; then
            rm -rf "$SERVER_DIR/data"
            ok "Removed data directory."
        fi
    fi

    echo
    ok "Uninstall complete."
    info "Docker was left installed. Remove it with your distribution's package manager if desired."
}

main() {
    # Standalone mode: this script was copied outside the project, so fetch
    # the project from GitHub first and deploy from the downloaded checkout.
    if ! is_full_checkout; then
        bootstrap_from_github "$@"
    fi

    case "${1:-}" in
        uninstall|remove|-u|--uninstall)
            uninstall
            ;;
        *)
            require_root
            detect_distro
            check_docker
            collect_settings
            write_env
            write_compose
            start_server
            export_ca_certificate
            show_summary
            ;;
    esac
}

main "$@"