#!/usr/bin/env sh
# Starts the Leshan demo server: web/REST 8080, CoAP 5683/udp, CoAP+DTLS 5684/udp.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
JAR="$ROOT/servers/.local/leshan-demo-server.jar"
[ -f "$JAR" ] || { echo "Leshan jar missing - run scripts/get-servers.sh" >&2; exit 1; }
echo "Leshan: http://127.0.0.1:${1:-8080} (Ctrl+C to stop)"
exec java -Xmx256m -jar "$JAR" -wp "${1:-8080}"
