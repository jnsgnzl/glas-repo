#!/bin/bash
# Glas: Live-Glas-Kern fuer LibreELEC bauen - laeuft auf dem NAS (Synology DS923+, Container Manager) als root im
# Ordner, in dem dieses Skript, le-inner.sh und glas.patch liegen. Normalerweise ueber le-watch.sh (DSM-Aufgabenplaner),
# von Hand:  sudo bash /volume1/glas-build/le-build.sh
# Umgebung: LE_TAG, PROJECT, DEVICE, ARCH, JOBS (Vorgabe: nach RAM), LTO (yes/no, Vorgabe nach RAM)
# Ergebnis: out/<LE_TAG>-<DEVICE>/kodi.bin (+ .sha256). Erster Lauf baut Toolchain, Mesa und LLVM mit (viele Stunden);
# danach baut ccache nur noch Kodi neu.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")" && pwd)
export LE_TAG=${LE_TAG:-12.2.1}
export PROJECT=${PROJECT:-Generic} DEVICE=${DEVICE:-Generic} ARCH=${ARCH:-x86_64}
echo "=== Start $(date '+%F %T')"

command -v docker >/dev/null || { echo "docker fehlt (DSM: Container Manager installieren)"; exit 1; }
[ -f "$ROOT/glas.patch" ] || { echo "glas.patch fehlt in $ROOT (README.md, Schritt 2)"; exit 1; }
[ -f "$ROOT/le-inner.sh" ] || { echo "le-inner.sh fehlt in $ROOT"; exit 1; }

# Parallelitaet nach RAM: Kodi und LLVM brauchen ~1,5 GB je Uebersetzer; LTO beim Binden von Kodi deutlich mehr
mem_gb=$(awk '/MemTotal/ { printf "%d", ($2 + 524288) / 1048576 }' /proc/meminfo)
cpus=$(nproc)
if [ -z "${JOBS:-}" ]; then
    JOBS=$(( mem_gb / 2 ))
    [ "$JOBS" -lt 1 ] && JOBS=1
    [ "$JOBS" -gt "$cpus" ] && JOBS=$cpus
fi
if [ -z "${LTO:-}" ]; then
    if [ "$mem_gb" -ge 12 ]; then LTO=yes; else LTO=no; fi
fi
export JOBS LTO
echo "NAS: $cpus Threads, $mem_gb GB RAM -> $JOBS Jobs, LTO=$LTO; LibreELEC $LE_TAG $PROJECT/$DEVICE $ARCH"
df -h "$ROOT" | tail -1

# Bauabbild aus dem Dockerfile von LibreELEC (Ubuntu 24.04). LibreELEC baut nie als root; der Bau laeuft unter der
# UID, der die Dateien im Ordner gehoeren (Besitzer von le-build.sh, also der Benutzer der Freigabe) - so bleiben
# Rechte und ACL der Freigabe unveraendert und alles ist per SMB lesbar
uid=$(stat -c %u "$ROOT/le-build.sh")
[ "$uid" -ne 0 ] || { echo "le-build.sh gehoert root - per SMB kopieren, nicht als root anlegen"; exit 1; }
mkdir -p "$ROOT/docker"
curl -fsS -o "$ROOT/docker/Dockerfile" \
    "https://raw.githubusercontent.com/LibreELEC/LibreELEC.tv/$LE_TAG/tools/docker/noble/Dockerfile"
docker build -q -t glas-le "$ROOT/docker"
printf 'FROM glas-le\nUSER root\nRUN useradd -o -u %s -g users -G sudo -m -s /bin/bash glas\nUSER glas\n' "$uid" |
    docker build -q -t glas-le-user -

mkdir -p "$ROOT/out"
docker run --rm --log-driver none -v "$ROOT":/work -w /work \
    -e LE_TAG -e PROJECT -e DEVICE -e ARCH -e JOBS -e LTO \
    glas-le-user bash /work/le-inner.sh
echo "=== Ende $(date '+%F %T')"
