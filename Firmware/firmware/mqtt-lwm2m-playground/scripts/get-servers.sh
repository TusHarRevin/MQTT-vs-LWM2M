#!/usr/bin/env sh
# Linux / macOS / WSL: installs Mosquitto from the package manager (if
# missing) and downloads the Leshan jar into servers/.local/.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST="$ROOT/servers/.local"
LESHAN_VERSION=2.0.0-M18
mkdir -p "$DEST"

if ! command -v mosquitto >/dev/null 2>&1; then
    if command -v apt-get >/dev/null 2>&1; then
        echo "Installing mosquitto (needs sudo)..."
        sudo apt-get update && sudo apt-get install -y mosquitto mosquitto-clients
        # Debian/Ubuntu start a system broker on 1883; stop it so ours can bind.
        sudo systemctl disable --now mosquitto 2>/dev/null || true
    elif command -v brew >/dev/null 2>&1; then
        brew install mosquitto
    else
        echo "Please install mosquitto with your package manager." >&2
    fi
fi
if [ ! -f "$DEST/leshan-demo-server.jar" ]; then
    echo "Downloading Leshan $LESHAN_VERSION..."
    curl -fL -o "$DEST/leshan-demo-server.jar" \
        "https://repo1.maven.org/maven2/org/eclipse/leshan/leshan-demo-server/$LESHAN_VERSION/leshan-demo-server-$LESHAN_VERSION-jar-with-dependencies.jar"
fi
command -v java >/dev/null 2>&1 || echo "Leshan needs Java 17+: e.g. sudo apt-get install -y openjdk-21-jre-headless"
echo "Done. Start the servers with scripts/start-mosquitto.sh and scripts/start-leshan.sh"
