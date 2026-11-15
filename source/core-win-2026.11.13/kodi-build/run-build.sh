#!/bin/bash
# Laeuft im Container (privileged, Volume kodi-build unter /build): Kodi fuer die Vero V bauen wie OSMC.
# Aufruf: docker run -d --privileged --name glas-kodi -v kodi-build:/build -v kodi-tc:/opt/osmc-tc -v <tools>:/tools:ro glas-kodi-build \
#           bash -c 'tr -d "\r" < /tools/run-build.sh > /tmp/r.sh && bash /tmp/r.sh'
# Log: /build/logs/build.log; Ergebnis: /build/osmc/package/mediacenter-osmc/files/usr/lib/kodi/kodi.bin
set -o pipefail
mkdir -p /build/logs
exec > >(tee -a /build/logs/build.log) 2>&1
echo "=== Glas-Kodi-Build $(date -Is)"
# ARM-Programme per qemu ausfuehren (Registrierung gilt fuer die ganze Docker-VM)
mountpoint -q /proc/sys/fs/binfmt_misc || mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc
update-binfmts --enable qemu-arm 2>/dev/null || true
cat /proc/sys/fs/binfmt_misc/qemu-arm | head -3
cd /build/osmc
echo "${GLAS_JOBS:-6}" > GLAS_JOBS
if [ "${GLAS_FULL:-0}" = "1" ]; then rm -f package/mediacenter-osmc/GLAS_KODI_ONLY; else touch package/mediacenter-osmc/GLAS_KODI_ONLY; fi
cd package/mediacenter-osmc
time make vero5
echo "=== Ende $(date -Is), Status $?"
ls -la files/usr/lib/kodi/kodi.bin 2>/dev/null
