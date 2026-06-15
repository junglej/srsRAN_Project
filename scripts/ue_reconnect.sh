#!/bin/bash
# UE modem reconnect helper for Quectel RM530N-GL + ModemManager.
# Run on the UE machine (192.168.5.2) as root or via sudo.
set -euo pipefail

INTERFACE="wwan0"
APN=""           # leave empty to use the existing connection APN
CONNECTION_NAME="" # leave empty to auto-detect the ModemManager connection

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*"
}

# --- 1. Find the modem ---
MODEM_INDEX=$(mmcli -L 2>/dev/null | awk '/\/org\/freedesktop\/ModemManager1\/Modem\// {gsub(/[^0-9]/,"",$1); print $1; exit}')
if [ -z "$MODEM_INDEX" ]; then
    log "ERROR: No ModemManager modem found. Is the module powered on?"
    exit 1
fi
log "Found modem: $MODEM_INDEX"

# --- 2. Check registration state ---
STATE=$(mmcli -m "$MODEM_INDEX" 2>/dev/null | awk -F: '/state/ {gsub(/^ +| +$/,"",$2); print $2; exit}')
log "Modem state: ${STATE:-unknown}"

# --- 3. Enable modem if disabled ---
if [ "$STATE" = "disabled" ]; then
    log "Modem is disabled, enabling..."
    mmcli -m "$MODEM_INDEX" --enable
    sleep 2
fi

# --- 4. Set 5G/4G allowed modes (adjust as needed) ---
log "Setting allowed modes to 5G preferred..."
mmcli -m "$MODEM_INDEX" --set-allowed-modes="5g|4g" 2>/dev/null || true

# --- 5. Check data connection / bearer ---
BEARERS=$(mmcli -m "$MODEM_INDEX" 2>/dev/null | grep -c "Bearer path" || true)
if [ "$BEARERS" -gt 0 ]; then
    log "Existing bearer found, checking connectivity..."
else
    log "No active bearer."
fi

# --- 6. Test connectivity ---
if ping -c 2 -W 3 10.45.0.1 >/dev/null 2>&1; then
    log "Ping to 10.45.0.1 OK. Connection is alive."
    exit 0
fi
log "Ping failed. Attempting reconnect..."

# --- 7. Disconnect / reset bearer ---
log "Disconnecting existing bearers..."
mmcli -m "$MODEM_INDEX" --simple-disconnect 2>/dev/null || true
sleep 1

# --- 8. Reset modem if still not working (hard reset via AT) ---
log "Resetting modem..."
mmcli -m "$MODEM_INDEX" --reset 2>/dev/null || true
sleep 10

# --- 9. Re-enable and reconnect via NetworkManager ---
log "Re-enabling modem..."
mmcli -m "$MODEM_INDEX" --enable 2>/dev/null || true
sleep 3

if [ -n "$CONNECTION_NAME" ]; then
    log "Activating NetworkManager connection: $CONNECTION_NAME"
    nmcli connection up "$CONNECTION_NAME" || true
else
    # Auto-detect the first ModemManager (gsm) connection
    AUTO_CONN=$(nmcli -t -f NAME,TYPE connection show --active 2>/dev/null | grep gsm | head -1 | cut -d: -f1)
    if [ -n "$AUTO_CONN" ]; then
        log "Reactivating connection: $AUTO_CONN"
        nmcli connection down "$AUTO_CONN" 2>/dev/null || true
        sleep 1
        nmcli connection up "$AUTO_CONN" || true
    else
        log "No active gsm connection found. Trying simple connect..."
        mmcli -m "$MODEM_INDEX" --simple-connect="apn=${APN:-internet},ip-type=ipv4" 2>/dev/null || true
    fi
fi

sleep 5

# --- 10. Verify ---
if ping -c 2 -W 3 10.45.0.1 >/dev/null 2>&1; then
    log "Reconnect successful."
    exit 0
else
    log "Reconnect failed. Manual intervention may be needed."
    exit 1
fi
