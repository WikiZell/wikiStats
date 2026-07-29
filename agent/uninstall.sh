#!/usr/bin/env bash
#
# Remove the FleetPanel / WikiStats telemetry agent.
#
#   sudo ./uninstall.sh            # stop, disable, remove the application; KEEP config
#   sudo ./uninstall.sh --purge    # also delete /etc/fleetpanel-agent and the service user
#
set -euo pipefail

APP_NAME="fleetpanel-agent"
INSTALL_DIR="/opt/${APP_NAME}"
CONFIG_DIR="/etc/${APP_NAME}"
SERVICE_FILE="/etc/systemd/system/${APP_NAME}.service"
SERVICE_USER="fleetpanel"

PURGE=0
ASSUME_YES=0

GREEN=$'\033[0;32m'; YELLOW=$'\033[0;33m'; RED=$'\033[0;31m'; NC=$'\033[0m'
if [ ! -t 1 ]; then GREEN=""; YELLOW=""; RED=""; NC=""; fi
info() { printf '%s==>%s %s\n' "${GREEN}" "${NC}" "$*"; }
warn() { printf '%s==>%s %s\n' "${YELLOW}" "${NC}" "$*" >&2; }
die()  { printf '%serror:%s %s\n' "${RED}" "${NC}" "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --purge)  PURGE=1 ;;
        -y|--yes) ASSUME_YES=1 ;;
        -h|--help)
            echo "Usage: sudo ./uninstall.sh [--purge] [--yes]"
            echo "  --purge  also remove ${CONFIG_DIR} and the '${SERVICE_USER}' system user"
            exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
    shift
done

[ "$(id -u)" -eq 0 ] || die "run as root: sudo ./uninstall.sh"

if [ "$PURGE" -eq 1 ] && [ "$ASSUME_YES" -eq 0 ]; then
    printf '%sThis deletes %s including any API token and MQTT password. Continue? [y/N]%s ' \
        "${YELLOW}" "${CONFIG_DIR}" "${NC}"
    read -r reply </dev/tty || reply="n"
    case "$reply" in [yY]*) : ;; *) die "aborted" ;; esac
fi

if systemctl list-unit-files 2>/dev/null | grep -q "^${APP_NAME}\.service"; then
    info "stopping and disabling ${APP_NAME}"
    systemctl stop "${APP_NAME}" 2>/dev/null || true
    systemctl disable "${APP_NAME}" 2>/dev/null || true
fi

if [ -f "${SERVICE_FILE}" ]; then
    info "removing ${SERVICE_FILE}"
    rm -f "${SERVICE_FILE}"
    systemctl daemon-reload
    systemctl reset-failed "${APP_NAME}" 2>/dev/null || true
fi

if [ -d "${INSTALL_DIR}" ]; then
    info "removing ${INSTALL_DIR}"
    rm -rf "${INSTALL_DIR}"
fi

if [ "$PURGE" -eq 1 ]; then
    if [ -d "${CONFIG_DIR}" ]; then
        info "removing ${CONFIG_DIR}"
        rm -rf "${CONFIG_DIR}"
    fi
    if id -u "${SERVICE_USER}" >/dev/null 2>&1; then
        info "removing system user '${SERVICE_USER}'"
        userdel "${SERVICE_USER}" 2>/dev/null || warn "could not remove user '${SERVICE_USER}'"
    fi
    info "purge complete"
else
    if [ -d "${CONFIG_DIR}" ]; then
        info "configuration preserved at ${CONFIG_DIR} (use --purge to remove it)"
    fi
    info "uninstall complete"
fi
