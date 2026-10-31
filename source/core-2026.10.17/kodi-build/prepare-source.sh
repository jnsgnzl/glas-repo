#!/bin/bash
# Laeuft im Container glas-kodi-build (Volume kodi-build unter /build):
# Kodi-Quelltext wie OSMC holen und OSMCs Patches in derselben Reihenfolge als Git-Commits anwenden.
# Ergebnis: /build/kodi (Git, Zweig osmc = OSMC-Stand, darauf entsteht der Glas-Zweig).
set -e
OSMC=/build/osmc
COMMIT=$(grep -o 'xbmc/archive/[0-9a-f]*' $OSMC/package/mediacenter-osmc/build.sh | head -1 | cut -d/ -f3)
mkdir -p /build/cache
TAR=/build/cache/xbmc-$COMMIT.tar.gz
[ -f $TAR ] || wget -q -O $TAR https://github.com/xbmc/xbmc/archive/$COMMIT.tar.gz
if [ ! -d /build/kodi/.git ]; then
    rm -rf /build/kodi /build/xbmc-$COMMIT
    tar -xzf $TAR -C /build
    mv /build/xbmc-$COMMIT /build/kodi
    cd /build/kodi
    git init -q && git config user.email build@glas && git config user.name "Glas Build"
    git add -A && git commit -qm "Kodi $COMMIT (Omega 21.3)"
    git branch -m vanilla
    git checkout -qb osmc
    P=$OSMC/package/mediacenter-osmc/patches
    for group in all arm vero5; do
        for patch in $(find $P -name "${group}-*.patch" -printf '%P\n' | sort); do
            if grep -q "GIT binary patch" $P/$patch; then git apply $P/$patch; else patch -s -p1 --ignore-whitespace < $P/$patch; fi
            git add -A && git commit -qm "OSMC: $patch"
        done
    done
    cp -ar $P/resource.language.*/ addons/ 2>/dev/null || true
    git add -A && git commit -qm "OSMC: Sprachdateien" || true
fi
cd /build/kodi && git log --oneline | wc -l && git log --oneline -3
