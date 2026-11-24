#!/bin/bash
# Glas: Kodi-Quellbaum fuer LibreELEC vorbereiten (PC, Git Bash). Zweig upstream = Kodi-Tag aus dem Kodi-Paket von
# LibreELEC <Tag>, Zweig vanilla = upstream + die Kodi-Patches von LibreELEC (nur der Grundordner; die Unterordner
# cec-framework, drmprime-filter und wayland gelten nicht fuer Generic). Danach:
#   powershell -ExecutionPolicy Bypass -File tools\kodi-build\windows\apply-glas.ps1 -KodiDir <Ziel>\kodi -Target gbm
# Der Patch landet in <Ziel>/patches; als glas.patch auf den NAS kopieren (README.md).
#
# Aufruf: bash tools/kodi-build/libreelec/prepare-source.sh [LE-Tag, Vorgabe 12.2.1] [Ziel, Vorgabe ~/glas-le]
set -euo pipefail
LE_TAG=${1:-12.2.1}
DEST=${2:-$HOME/glas-le}
RAW=https://raw.githubusercontent.com/LibreELEC/LibreELEC.tv/$LE_TAG/packages/mediacenter/kodi

KODI_TAG=$(curl -fsS "$RAW/package.mk" | sed -n 's/^PKG_VERSION="\(.*\)"$/\1/p')
[ -n "$KODI_TAG" ] || { echo "Kodi-Version von LibreELEC $LE_TAG nicht gefunden"; exit 1; }
echo "LibreELEC $LE_TAG baut Kodi $KODI_TAG"

mkdir -p "$DEST"
cd "$DEST"
if [ ! -d kodi/.git ]; then
    git clone -q --depth 1 --branch "$KODI_TAG" https://github.com/xbmc/xbmc.git kodi
    git -C kodi branch -q -f upstream HEAD
fi
cd kodi
# Zeilenenden wie im Kodi-Repo (sonst passen die Patches unter Windows nicht)
git config core.autocrlf false
git config core.eol lf
git rev-parse -q --verify upstream >/dev/null || { echo "Zweig upstream fehlt in $DEST/kodi"; exit 1; }

# Kodi-Patches von LibreELEC holen (frisch, damit ein anderer Tag keine Reste behaelt)
rm -rf ../le-patches
mkdir ../le-patches
names=$(curl -fsS "https://api.github.com/repos/LibreELEC/LibreELEC.tv/contents/packages/mediacenter/kodi/patches?ref=$LE_TAG" |
    grep -o '"name": "kodi-[^"]*\.patch"' | cut -d'"' -f4)
[ -n "$names" ] || { echo "Patch-Liste von LibreELEC $LE_TAG leer"; exit 1; }
for n in $names; do
    curl -fsS -o "../le-patches/$n" "$RAW/patches/$n"
done
echo "$(echo "$names" | wc -w) Kodi-Patches von LibreELEC"

# vanilla = upstream + LibreELEC-Patches (Reihenfolge wie im Bau: nach Namen). Zweig glas verwerfen: er hinge sonst am
# alten vanilla-Commit, und format-patch vanilla..glas naehme die LibreELEC-Patches noch einmal mit
git checkout -q -f -B vanilla upstream
git branch -q -D glas 2>/dev/null || true
git rm -rq --cached . >/dev/null
git reset -q --hard
for p in ../le-patches/*.patch; do
    # einige Patches haben Hunks ohne Kontext (wie patch -p1 im Bau)
    git apply --unidiff-zero --whitespace=nowarn "$p" || { echo "Patch passt nicht: $p"; exit 1; }
done
git add -A
git -c user.name=Glas -c user.email=glas@localhost commit -q -m "LibreELEC $LE_TAG: Kodi-Patches (packages/mediacenter/kodi/patches)"
git log --oneline -2
