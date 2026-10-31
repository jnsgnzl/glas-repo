#!/bin/bash
# Laeuft im Container (privileged, Volumes kodi-build und kodi-tc): Glas-Aenderungen in den Build-Baum des letzten
# OSMC-Builds einspielen und nur neu uebersetzen, was sich geaendert hat (kodi.bin).
# Voraussetzung: ein vollstaendiger Lauf von run-build.sh (Baum unter package/mediacenter-osmc/src/xbmc-*).
# Ergebnis: /build/out/kodi.bin (gestrippt) und kodi.bin.debug.
set -e
set -o pipefail
mkdir -p /build/logs /build/out
exec > >(tee -a /build/logs/incremental.log) 2>&1
echo "=== Glas-Inkrementell $(date -Is)"
mountpoint -q /proc/sys/fs/binfmt_misc || mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc
update-binfmts --enable qemu-arm 2>/dev/null || true

TC=/opt/osmc-tc/armv7-toolchain-osmc
PKG=/build/osmc/package/mediacenter-osmc
SRC=$(ls -d $PKG/src/xbmc-* | head -1)
[ -d "$SRC/kodi-build" ] || { echo "Kein Build-Baum unter $SRC"; exit 1; }

# Quelltext des Build-Baums auf den Stand des Zweigs glas bringen: Dateien, die sich zwischen osmc und glas
# unterscheiden, aus /build/kodi kopieren (der Baum enthaelt bereits alle OSMC-Patches und den ersten Glas-Patch)
cd /build/kodi
for f in $(git diff --name-only osmc glas); do
    mkdir -p "$SRC/$(dirname "$f")"
    if ! cmp -s "$f" "$SRC/$f"; then cp "$f" "$SRC/$f"; echo "aktualisiert: $f"; fi
done

# Chroot wie OSMCs build_in_env: Quelltext unter /mnt
mountpoint -q $TC/mnt || mount --bind /build/osmc $TC/mnt
mountpoint -q $TC/proc || mount -t proc proc $TC/proc
cp /etc/resolv.conf $TC/etc/resolv.conf
REL=${SRC#/build/osmc/}
JOBS=${GLAS_JOBS:-6}
time chroot $TC /bin/bash -c "set -o pipefail; cd /mnt/$REL/kodi-build && make -j$JOBS kodi 2>&1 | tee /mnt/make.log | grep -E '^\[|error|Error' "
cp "$SRC/kodi-build/kodi.bin" /build/out/kodi.bin.debug
cp "$SRC/kodi-build/kodi.bin" "$SRC/kodi-build/kodi.bin.glas"
chroot $TC strip -s "/mnt/$REL/kodi-build/kodi.bin.glas"
mv "$SRC/kodi-build/kodi.bin.glas" /build/out/kodi.bin
umount $TC/proc || true
umount $TC/mnt || true
ls -la /build/out/
echo "=== Ende $(date -Is)"
