#!/usr/bin/env sh
# Starts Mosquitto with the playground config.  Usage: start-mosquitto.sh [port] [tls-port]
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PORT=${1:-1883}
TLS_PORT=${2:-8883}
CONF=$(mktemp)
sed -e "s/^listener 1883$/listener $PORT/" -e "s/^listener 8883$/listener $TLS_PORT/" \
    "$ROOT/servers/mosquitto/mosquitto.conf" > "$CONF"
cd "$ROOT/servers/mosquitto"   # certs/ paths are relative to here
echo "Mosquitto: MQTT on $PORT, MQTT/TLS on $TLS_PORT (Ctrl+C to stop)"
exec mosquitto -c "$CONF" -v
