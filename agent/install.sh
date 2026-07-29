#!/usr/bin/env bash
#
# FleetPanel / WikiStats telemetry agent installer.
#
# Supports Debian, Ubuntu and Raspberry Pi OS (bookworm and newer; anything with
# Python >= 3.11 and systemd).
#
#   curl -fsSL https://raw.githubusercontent.com/WikiZell/wikiStats/main/agent/install.sh | sudo bash
#
#   sudo ./install.sh                 # interactive, safe defaults
#   sudo ./install.sh --yes           # non-interactive
#   sudo ./install.sh --port 9000     # override the HTTP port
#   sudo ./install.sh --token auto    # enable bearer auth with a generated token
#   sudo ./install.sh --no-service    # install but do not run at boot
#
set -euo pipefail

APP_NAME="fleetpanel-agent"
INSTALL_DIR="/opt/${APP_NAME}"
CONFIG_DIR="/etc/${APP_NAME}"
CONFIG_FILE="${CONFIG_DIR}/config.toml"
SERVICE_FILE="/etc/systemd/system/${APP_NAME}.service"
SERVICE_USER="fleetpanel"
SERVICE_GROUP="fleetpanel"
MIN_PY_MINOR=11

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

ASSUME_YES=0
PORT=""
TOKEN=""
START_SERVICE=1
ENABLE_SERVICE=""   # empty = ask; "yes" / "no" once decided
MQTT_HOST=""

REPO_URL="${WIKISTATS_REPO:-https://github.com/WikiZell/wikiStats}"
REPO_REF="${WIKISTATS_REF:-main}"

RED=$'\033[0;31m'; GREEN=$'\033[0;32m'; YELLOW=$'\033[0;33m'; BOLD=$'\033[1m'; NC=$'\033[0m'
if [ ! -t 1 ]; then RED=""; GREEN=""; YELLOW=""; BOLD=""; NC=""; fi

info()  { printf '%s==>%s %s\n' "${GREEN}" "${NC}" "$*"; }
warn()  { printf '%s==>%s %s\n' "${YELLOW}" "${NC}" "$*" >&2; }
die()   { printf '%serror:%s %s\n' "${RED}" "${NC}" "$*" >&2; exit 1; }

usage() {
    cat <<EOF
Usage: sudo ./install.sh [options]

  --yes, -y           Do not prompt; keep an existing config after backing it up
  --port PORT         HTTP port to write into a freshly created config (default 8770)
  --token VALUE|auto  Enable bearer authentication. "auto" generates a 48-char token
  --mqtt-host HOST    Pre-fill and enable MQTT publishing to this broker
  --service           Run at boot (systemd). Default; skips the question
  --no-service        Install only; do not enable or start anything
  --no-start          Enable the service for next boot but do not start it now
  --help, -h          This message

Environment:
  WIKISTATS_REPO      Repository to fetch when piped from curl
  WIKISTATS_REF       Branch or tag to fetch (default: main)
EOF
}

# Kept verbatim so the bootstrap path can hand them to the downloaded copy.
ORIGINAL_ARGS=("$@")

while [ $# -gt 0 ]; do
    case "$1" in
        -y|--yes)      ASSUME_YES=1 ;;
        --port)        PORT="${2:?--port needs a value}"; shift ;;
        --token)       TOKEN="${2:?--token needs a value}"; shift ;;
        --mqtt-host)   MQTT_HOST="${2:?--mqtt-host needs a value}"; shift ;;
        --service)     ENABLE_SERVICE="yes" ;;
        --no-service)  ENABLE_SERVICE="no" ;;
        --no-start)    START_SERVICE=0 ;;
        -h|--help)     usage; exit 0 ;;
        *)             die "unknown option: $1 (try --help)" ;;
    esac
    shift
done

# Reads one line from the terminal even when this script arrived on stdin from
# `curl | sudo bash`. Falls back to the supplied default when there is no tty,
# which is what happens in CI and in cloud-init.
ask() {
    local prompt="$1" fallback="$2" reply=""
    if [ "$ASSUME_YES" -eq 1 ] || [ ! -r /dev/tty ]; then
        # The prompt goes to stderr so that only the answer lands in $( ).
        printf '%s %s (non-interactive)\n' "$prompt" "$fallback" >&2
        printf '%s' "$fallback"
        return 0
    fi
    printf '%s%s%s ' "${BOLD}" "$prompt" "${NC}" >&2
    read -r reply </dev/tty || reply=""
    printf '%s' "${reply:-$fallback}"
}

# --------------------------------------------------------------- preflight

[ "$(id -u)" -eq 0 ] || die "run as root: sudo ./install.sh"

command -v systemctl >/dev/null 2>&1 || die "systemd is required (systemctl not found)"

if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    case "${ID:-}${ID_LIKE:-}" in
        *debian*|*ubuntu*|*raspbian*) : ;;
        *) warn "untested distribution '${PRETTY_NAME:-unknown}'; continuing anyway" ;;
    esac
    info "detected ${PRETTY_NAME:-unknown}"
fi

PYTHON=""
for candidate in python3.13 python3.12 python3.11 python3; do
    if command -v "$candidate" >/dev/null 2>&1; then
        minor="$("$candidate" -c 'import sys; print(sys.version_info[1] if sys.version_info[0]==3 else 0)' 2>/dev/null || echo 0)"
        if [ "$minor" -ge "$MIN_PY_MINOR" ]; then PYTHON="$(command -v "$candidate")"; break; fi
    fi
done
[ -n "$PYTHON" ] || die "Python 3.${MIN_PY_MINOR}+ not found. Install it with: apt install python3 python3-venv"
info "using $PYTHON ($("$PYTHON" --version 2>&1))"

if ! "$PYTHON" -c 'import venv' >/dev/null 2>&1; then
    die "the venv module is missing. Install it with: apt install python3-venv"
fi

# ------------------------------------------------------------ bootstrap
#
# Piped from curl there is no repository next to the script, only the script. Fetch
# the source tarball into a temporary directory and hand over to the copy inside it,
# so the rest of this file can assume the tree is present either way.

if [ ! -f "${SCRIPT_DIR}/pyproject.toml" ]; then
    command -v curl >/dev/null 2>&1 || die "curl is required to bootstrap (apt install curl)"
    command -v tar  >/dev/null 2>&1 || die "tar is required to bootstrap"

    BOOTSTRAP_DIR="$(mktemp -d)"
    # shellcheck disable=SC2064 - expand BOOTSTRAP_DIR now, not at trap time
    trap "rm -rf '${BOOTSTRAP_DIR}'" EXIT

    info "fetching ${REPO_URL} (${REPO_REF})"
    if ! curl -fsSL "${REPO_URL}/archive/refs/heads/${REPO_REF}.tar.gz" \
            | tar -xz -C "${BOOTSTRAP_DIR}"; then
        die "could not download ${REPO_URL} at ref ${REPO_REF}"
    fi

    # The tarball's top-level directory is named after the repo and ref, and the
    # exact casing is not worth guessing at.
    BOOTSTRAP_AGENT="$(find "${BOOTSTRAP_DIR}" -mindepth 2 -maxdepth 3 -type f \
        -name pyproject.toml -path '*/agent/*' -print -quit)"
    [ -n "${BOOTSTRAP_AGENT}" ] || die "downloaded archive does not contain agent/pyproject.toml"

    exec bash "$(dirname "${BOOTSTRAP_AGENT}")/install.sh" "${ORIGINAL_ARGS[@]}"
fi

# ------------------------------------------------------------ service user

if id -u "$SERVICE_USER" >/dev/null 2>&1; then
    info "system user '${SERVICE_USER}' already exists"
else
    info "creating system user '${SERVICE_USER}'"
    # No shell, no home directory contents, no login. The agent needs none of them.
    useradd --system --no-create-home --home-dir /nonexistent \
            --shell /usr/sbin/nologin --comment "FleetPanel telemetry agent" \
            "$SERVICE_USER"
fi
getent group "$SERVICE_GROUP" >/dev/null 2>&1 || groupadd --system "$SERVICE_GROUP"

# ------------------------------------------------------------ application

info "installing application to ${INSTALL_DIR}"
install -d -m 0755 -o root -g root "${INSTALL_DIR}"
rm -rf "${INSTALL_DIR}/src" "${INSTALL_DIR}/pyproject.toml" "${INSTALL_DIR}/README.md"
cp -r "${SCRIPT_DIR}/src" "${INSTALL_DIR}/src"
cp "${SCRIPT_DIR}/pyproject.toml" "${INSTALL_DIR}/pyproject.toml"
[ -f "${SCRIPT_DIR}/README.md" ] && cp "${SCRIPT_DIR}/README.md" "${INSTALL_DIR}/README.md"
# Ship the uninstaller alongside the application: someone removing this in two
# years should not have to find the repository it came from.
if [ -f "${SCRIPT_DIR}/uninstall.sh" ]; then
    cp "${SCRIPT_DIR}/uninstall.sh" "${INSTALL_DIR}/uninstall.sh"
    chmod 0755 "${INSTALL_DIR}/uninstall.sh"
fi

if [ ! -x "${INSTALL_DIR}/venv/bin/python" ]; then
    info "creating virtual environment"
    "$PYTHON" -m venv "${INSTALL_DIR}/venv"
fi

info "installing Python dependencies (this can take a few minutes on a Pi)"
"${INSTALL_DIR}/venv/bin/pip" install --quiet --upgrade pip setuptools wheel
"${INSTALL_DIR}/venv/bin/pip" install --quiet "${INSTALL_DIR}"

chown -R root:root "${INSTALL_DIR}"
chmod -R go-w "${INSTALL_DIR}"

# ---------------------------------------------------------------- config

install -d -m 0750 -o root -g "${SERVICE_GROUP}" "${CONFIG_DIR}"

write_config=1
if [ -f "${CONFIG_FILE}" ]; then
    backup="${CONFIG_FILE}.$(date +%Y%m%d%H%M%S).bak"
    cp -a "${CONFIG_FILE}" "${backup}"
    info "existing configuration backed up to ${backup}"
    reply="$(ask "Overwrite ${CONFIG_FILE} with a fresh default config? [y/N]" 'n')"
    case "$reply" in [yY]*) write_config=1 ;; *) write_config=0 ;; esac
    [ "$write_config" -eq 0 ] && info "keeping existing configuration"
fi

if [ "$write_config" -eq 1 ]; then
    info "writing ${CONFIG_FILE}"
    cp "${SCRIPT_DIR}/packaging/config.example.toml" "${CONFIG_FILE}"

    if [ -n "$PORT" ]; then
        sed -i "s/^port = 8770$/port = ${PORT}/" "${CONFIG_FILE}"
        info "HTTP port set to ${PORT}"
    fi

    if [ -n "$TOKEN" ]; then
        if [ "$TOKEN" = "auto" ]; then
            TOKEN="$(head -c 24 /dev/urandom | od -An -tx1 | tr -d ' \n')"
        fi
        sed -i "s|^auth_mode = \"none\"$|auth_mode = \"bearer\"|" "${CONFIG_FILE}"
        sed -i "s|^api_token = \"\"$|api_token = \"${TOKEN}\"|" "${CONFIG_FILE}"
        info "bearer authentication enabled"
    fi

    if [ -n "$MQTT_HOST" ]; then
        # Only the [mqtt] block has `enabled = false`; anchor on it to stay safe.
        sed -i "0,/^enabled = false$/s//enabled = true/" "${CONFIG_FILE}"
        sed -i "s|^host = \"\"$|host = \"${MQTT_HOST}\"|" "${CONFIG_FILE}"
        info "MQTT publishing enabled to ${MQTT_HOST}"
    fi
fi

chown root:"${SERVICE_GROUP}" "${CONFIG_FILE}"
# The config may hold an API token and an MQTT password: group-readable only.
chmod 0640 "${CONFIG_FILE}"

info "validating configuration"
if ! "${INSTALL_DIR}/venv/bin/fleetpanel-agent" --config "${CONFIG_FILE}" --check; then
    die "configuration failed validation; nothing was started. Fix ${CONFIG_FILE} and re-run."
fi

# --------------------------------------------------------------- service

info "installing systemd unit"
cp "${SCRIPT_DIR}/packaging/${APP_NAME}.service" "${SERVICE_FILE}"
chmod 0644 "${SERVICE_FILE}"
systemctl daemon-reload

if [ -z "$ENABLE_SERVICE" ]; then
    reply="$(ask 'Start WikiStats automatically at boot? [Y/n]' 'y')"
    case "$reply" in
        [nN]*) ENABLE_SERVICE="no" ;;
        *)     ENABLE_SERVICE="yes" ;;
    esac
fi

if [ "$ENABLE_SERVICE" = "yes" ]; then
    systemctl enable "${APP_NAME}" >/dev/null
    info "service enabled; it will start on every boot"
else
    systemctl disable "${APP_NAME}" >/dev/null 2>&1 || true
    info "service installed but NOT enabled; it will not start at boot"
fi

if [ "$ENABLE_SERVICE" = "yes" ] && [ "$START_SERVICE" -eq 1 ]; then
    info "starting ${APP_NAME}"
    systemctl restart "${APP_NAME}"
    sleep 2
    if ! systemctl is-active --quiet "${APP_NAME}"; then
        warn "service did not stay up. Recent log:"
        journalctl -u "${APP_NAME}" -n 30 --no-pager || true
        die "installation finished but the service is not running"
    fi
elif [ "$ENABLE_SERVICE" = "yes" ]; then
    info "service enabled but not started now (--no-start)"
fi

# ---------------------------------------------------------------- summary

CONF_PORT="$(awk -F'= *' '/^port *=/ {print $2; exit}' "${CONFIG_FILE}" | tr -d ' ')"
CONF_PORT="${CONF_PORT:-8770}"

# Address on the interface holding the default route - the one a panel will use.
IP_ADDR="$(ip -4 route get 192.0.2.1 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i=="src"){print $(i+1); exit}}')"
[ -n "${IP_ADDR:-}" ] || IP_ADDR="$(hostname -I 2>/dev/null | awk '{print $1}')"
[ -n "${IP_ADDR:-}" ] || IP_ADDR="127.0.0.1"

DEVICE_ID="$("${INSTALL_DIR}/venv/bin/python" -c \
    'import sys; sys.path.insert(0,"'"${INSTALL_DIR}"'/src"); from fleetpanel_agent.identity import derive_device_id; print(derive_device_id())' 2>/dev/null || echo "?")"

cat <<EOF

${BOLD}FleetPanel agent installed.${NC}

  Device ID     ${DEVICE_ID}
  Telemetry     http://${IP_ADDR}:${CONF_PORT}/api/v1/telemetry
  Health        http://${IP_ADDR}:${CONF_PORT}/api/v1/health
  API docs      http://${IP_ADDR}:${CONF_PORT}/docs
  mDNS          _fleetpanel._tcp.local.
  Config        ${CONFIG_FILE}

  Test it:
    curl -s http://${IP_ADDR}:${CONF_PORT}/api/v1/health

  Manage it:
    sudo systemctl status ${APP_NAME}
    sudo journalctl -u ${APP_NAME} -f
    sudo systemctl restart ${APP_NAME}

EOF

if [ "$ENABLE_SERVICE" = "yes" ]; then
    printf '  Runs at boot: %syes%s\n\n' "${GREEN}" "${NC}"
else
    cat <<EOF
  Runs at boot: ${YELLOW}no${NC}

  Start it once:
    sudo systemctl start ${APP_NAME}

  Or make it permanent later:
    sudo systemctl enable --now ${APP_NAME}

EOF
fi

if [ -n "$TOKEN" ] && [ "$write_config" -eq 1 ]; then
    cat <<EOF
${YELLOW}API token (store it now; it is only shown here):${NC}
  ${TOKEN}

  curl -s -H "Authorization: Bearer ${TOKEN}" http://${IP_ADDR}:${CONF_PORT}/api/v1/telemetry

EOF
fi
