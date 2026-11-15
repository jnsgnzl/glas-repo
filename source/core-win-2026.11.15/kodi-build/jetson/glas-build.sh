#!/bin/bash
# Auf dem Jetson (Benutzer mit Docker-Rechten), Arbeitsordner ~/glas-build:
#   osmc/   OSMC-Repo auf dem Stand der Vero (2b5c995b4)        kodi/  Kodi-Quelltext (Zweige vanilla, osmc, glas)
#   tools/  Kopie von tools/kodi-build aus dem Glas-Repo       out/   kodi.bin, kodi.bin.debug      logs/  Protokolle
#
#   bash glas-build.sh image     Container-Image glas-kodi-armhf bauen (einmalig, laedt die Pakete)
#   bash glas-build.sh source    Kodi-Quelltext holen und OSMCs Patches anwenden (prepare-source.sh, einmalig)
#   bash glas-build.sh apply     Glas-Aenderungen aus tools/src in den Zweig glas uebernehmen (apply-glas.sh)
#   bash glas-build.sh build     kodi.bin bauen bzw. nur Geaendertes neu uebersetzen (jetson/build.sh)
#   bash glas-build.sh all       apply + build
#
# Rueksicht auf andere Dienste auf dem Jetson: hoechstens 3 Kerne, 3,5 GB Speicher (+1,5 GB Auslagerung), niedrige
# CPU-Gewichtung, Compiler mit nice 19. Aufraeumen: docker rmi glas-kodi-armhf; rm -rf ~/glas-build
set -e
W=$HOME/glas-build
IMAGE=glas-kodi-armhf
run() {
    docker run --rm --platform linux/arm/v7 --cpus 3 --cpu-shares 128 --memory 3500m --memory-swap 5000m \
        -e GLAS_JOBS="${GLAS_JOBS:-3}" -v "$W:/build" -v "$W/tools:/tools:ro" "$IMAGE" \
        bash -c "tr -d '\r' < /tools/$1 > /tmp/s.sh && bash /tmp/s.sh"
}
case "$1" in
    image)  docker build --platform linux/arm/v7 -t "$IMAGE" -f "$W/tools/jetson/Dockerfile" "$W/tools" ;;
    source) run prepare-source.sh ;;
    apply)  run apply-glas.sh ;;
    build)  run jetson/build.sh ;;
    all)    run apply-glas.sh && run jetson/build.sh ;;
    *)      sed -n '2,13p' "$0"; exit 1 ;;
esac
