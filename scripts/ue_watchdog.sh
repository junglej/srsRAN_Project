#!/bin/bash
# UE modem watchdog for Quectel RM530N-GL + ModemManager.
# Restarts the data connection if 10.45.0.1 is unreachable.
set -euo pipefail

LOG_FILE="/var/log/ue_watchdog.log"
TARGET="10.45.0.1"
CONNECTION_UUID="5219b607-cfa8-4785-9edf-95c3e1a9df10"

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "$LOG_FILE"
}

# Only run if a ModemManager modem exists
MODEM_INDEX=$(mmcli -L 2>/dev/null | awk '/\/org\/freedesktop\/ModemManager1\/Modem\// {gsub(/[^0-9]/,"",$1); print $1; exit}')
if [ -z "$MODEM_INDEX" ]; then
    log "No modem found, skipping."
    exit 0
fi

# Test connectivity
if ping -c 2 -W 3 "$TARGET" >/dev/null 2>&1; then
    exit 0
fi

log "Ping to $TARGET failed. Reconnecting..."

# Disconnect any active bearer
mmcli -m "$MODEM_INDEX" --simple-disconnect 2>/dev/null || true
sleep 2

# Restart the NetworkManager connection
nmcli connection down "$CONNECTION_UUID" 2>/dev/null || true
sleep 1
nmcli connection up "$CONNECTION_UUID" 2>/dev/null || true

sleep 8

# Verify
if ping -c 2 -W 3 "$TARGET" >/dev/null 2>&1; then
    log "Reconnected successfully."
else
    log "Reconnect failed. Will retry on next run."
fi
