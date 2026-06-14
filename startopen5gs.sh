#!/bin/bash
set -e

# === Kill any existing Open5GS processes ===
echo "Stopping any existing Open5GS processes..."
sudo killall -9 open5gs-nrfd open5gs-scpd open5gs-amfd open5gs-smfd open5gs-upfd \
    open5gs-ausfd open5gs-udmd open5gs-udrd open5gs-pcfd open5gs-nssfd \
    open5gs-bsfd open5gs-mmed open5gs-sgwcd open5gs-sgwud open5gs-hssd \
    open5gs-pcrfd 2>/dev/null || true
# Also kill the test app
sudo killall -9 app 2>/dev/null || true
sleep 2

# === Setup ogstun interface (idempotent) ===
if ! ip link show ogstun &>/dev/null; then
    sudo ip tuntap add name ogstun mode tun
fi
sudo ip addr add 10.45.0.1/16 dev ogstun 2>/dev/null || true
sudo ip link set ogstun up

# === Check MongoDB ===
if ! pgrep -x "mongod" > /dev/null; then
    echo "WARNING: MongoDB is not running. WebUI will fail."
    echo "Start it with: sudo systemctl start mongod"
fi

# === Start Open5GS core ===
cd /home/ran/Documents/open5gs/build/tests/app
./app &
APP_PID=$!
echo "Open5GS core started (PID: $APP_PID)"

# === Start WebUI ===
cd /home/ran/Documents/open5gs/webui
npm run dev &
WEBUI_PID=$!
echo "Open5GS WebUI started (PID: $WEBUI_PID)"

echo ""
echo "Core:   AMF N2 at 127.0.0.5:38412"
echo "WebUI:  http://localhost:9999"
echo ""
echo "Default login: admin / 1423"
echo ""
echo "Press Ctrl+C to stop all services"

# Trap Ctrl+C to clean up
trap 'echo "Stopping..."; kill $WEBUI_PID $APP_PID 2>/dev/null; exit' INT

# Wait for both processes
wait
