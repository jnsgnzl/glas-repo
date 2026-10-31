#!/bin/bash
# Laeuft im Container: OSMC-Repository auf den Stand der Vero (21.3.0-7) bringen und zwei Glas-Schalter einbauen.
#   GLAS_JOBS        (Datei im OSMC-Wurzelordner) Zahl paralleler Jobs statt aller Kerne (Emulation, wenig RAM)
#   GLAS_KODI_ONLY   (Datei) nach "make install" von Kodi aufhoeren: keine binaeren Add-ons, Sprachen, Pakete
# Der Glas-Patch liegt als package/mediacenter-osmc/patches/vero5-900-glas.patch (aus /build/kodi, Zweig glas).
set -e
cd /build/osmc
git config user.email build@glas; git config user.name "Glas Build"
git checkout -q -B glas 2b5c995b4
grep -q GLAS_JOBS scripts/common.sh || perl -0pi -e 's/(export BUILD="make -j\$\{cores\}")/_glas="\$(dirname \$\{BASH_SOURCE[0]\})\/..\/GLAS_JOBS"; [ -f "\$_glas" ] && cores=\$(cat "\$_glas")
$1/' scripts/common.sh
grep -q GLAS_KODI_ONLY package/mediacenter-osmc/build.sh || perl -0pi -e 's/(	make install DESTDIR=\$\{out\}
	popd
)/$1	if [ -f ..\/..\/GLAS_KODI_ONLY ]; then echo "Glas: nur Kodi gebaut"; exit 0; fi
/' package/mediacenter-osmc/build.sh
grep -n GLAS scripts/common.sh package/mediacenter-osmc/build.sh
git add scripts/common.sh package/mediacenter-osmc/build.sh package/mediacenter-osmc/patches/vero5-900-glas.patch
git commit -qm "Glas: Jobs begrenzen, nur Kodi bauen, Glas-Patch" || true
git log --oneline -2
