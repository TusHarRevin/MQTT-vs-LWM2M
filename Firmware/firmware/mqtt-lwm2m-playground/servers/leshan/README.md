# Leshan

The playground uses the **Leshan demo server** 2.0.0-M18 (a single runnable jar
from Maven Central). It needs Java 17 or newer.

- Docker: `docker compose up leshan` (see `Dockerfile` here)
- Without Docker: `scripts/get-servers.ps1` / `scripts/get-servers.sh` download
  the jar, then `scripts/start-leshan.ps1` / `scripts/start-leshan.sh`

Web UI and REST API: http://127.0.0.1:8080 . Full walkthrough: `docs/server.md`.
