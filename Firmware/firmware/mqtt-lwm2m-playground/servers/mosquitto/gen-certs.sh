#!/usr/bin/env sh
# Generates a throw-away lab CA and a broker certificate for "localhost" /
# 127.0.0.1. LAB USE ONLY - the CA key is committed to the repo on purpose so
# the first TLS experiment works out of the box. Never reuse these anywhere.
set -e
cd "$(dirname "$0")/certs"
export MSYS_NO_PATHCONV=1   # Git Bash on Windows: don't mangle "/CN=..."
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -sha256 \
    -keyout ca.key -out ca.crt -subj "/CN=MQTT LwM2M Playground Lab CA"
openssl req -newkey rsa:2048 -nodes -sha256 \
    -keyout server.key -out server.csr -subj "/CN=localhost"
printf 'subjectAltName=DNS:localhost,IP:127.0.0.1,DNS:mosquitto\nbasicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n' > server.ext
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
    -days 3650 -sha256 -out server.crt -extfile server.ext
rm -f server.csr server.ext ca.srl
chmod 644 server.key   # mosquitto in Docker runs as uid 1883 and must read it
echo "Certificates written to $(pwd)"
