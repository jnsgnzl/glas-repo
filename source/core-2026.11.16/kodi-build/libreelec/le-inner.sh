#!/bin/bash
# Glas: Teil von le-build.sh, laeuft im Container (Benutzer docker) mit dem NAS-Ordner unter /work.
set -euo pipefail
cd /work
LE=/work/LibreELEC.tv

if [ ! -d "$LE/.git" ]; then
    git clone -q https://github.com/LibreELEC/LibreELEC.tv.git "$LE"
fi
cd "$LE"
git fetch -q --tags origin
# Glas-Patch und lokale Einstellungen sind nicht eingecheckt; checkout -f wirft nur Aenderungen an Dateien von LibreELEC weg
git checkout -q -f "$LE_TAG"
echo "LibreELEC $(git describe --tags)"

cp /work/glas.patch packages/mediacenter/kodi/patches/kodi-999.90-glas.patch

# Lokale Einstellungen (config/options liest .libreelec/options nach den Vorgaben von LibreELEC)
mkdir -p .libreelec
cat > .libreelec/options <<EOF
CONCURRENCY_MAKE_LEVEL=$JOBS
LTO_SUPPORT="$LTO"
EOF

# Nur das Kodi-Paket (mit allen Abhaengigkeiten), kein Abbild
start=$(date +%s)
if ! scripts/build kodi; then
    # Ninja bricht beim ersten Fehler ab: die uebrigen Kodi-Dateien weiter uebersetzen und alle Fehler auf einmal zeigen
    kb=$(ls -d build.*-"$DEVICE"."$ARCH"-*/build/kodi-*/.*-linux-gnu 2>/dev/null | head -1)
    nj=$(ls -1 build.*-"$DEVICE"."$ARCH"-*/toolchain/bin/ninja 2>/dev/null | head -1)
    [ -n "$nj" ] || nj=$(command -v ninja || true)
    if [ -n "$kb" ] && [ -f "$kb/build.ninja" ] && [ -n "$nj" ]; then
        echo "=== Alle Kodi-Fehler (ninja -k 0) ==="
        "$nj" -C "$kb" -k 0 -j"$JOBS" 2>&1 | grep -E -A3 "error:|^FAILED:" || true
    fi
    exit 1
fi
echo "Bau: $(( ($(date +%s) - start) / 60 )) min"

bin=$(ls -1 build.*-"$DEVICE"."$ARCH"-*/install_pkg/kodi-*/usr/lib/kodi/kodi.bin 2>/dev/null | head -1)
[ -n "$bin" ] || { echo "kodi.bin nicht gefunden"; exit 1; }
out=/work/out/$LE_TAG-$DEVICE
mkdir -p "$out"
cp "$bin" "$out/kodi.bin"
(cd "$out" && sha256sum kodi.bin | tee kodi.bin.sha256)
file "$out/kodi.bin" || true
ls -l "$out"
