#!/bin/bash
# Laeuft im Container glas-kodi-armhf (Jetson, /build = ~/glas-build): kodi.bin aus /build/kodi (Zweig glas) bauen,
# mit OSMCs CMake-Optionen fuer die Vero V (package/mediacenter-osmc/build.sh, Ziel vero5). Der Build-Baum bleibt in
# /build/kodi/kodi-build liegen, folgende Laeufe uebersetzen nur Geaendertes.
# Ergebnis: /build/out/kodi.bin (gestrippt) und /build/out/kodi.bin.debug.
set -e
set -o pipefail
mkdir -p /build/logs /build/out
exec > >(tee -a /build/logs/build.log) 2>&1
echo "=== Glas-Kodi-Build (Jetson) $(date -Is)"
cd /build/kodi
git checkout -q glas
grep -qx kodi-build .git/info/exclude || echo kodi-build >> .git/info/exclude
mkdir -p kodi-build
cd kodi-build
# wie OSMCs build_in_env mit BUILD_OPTION_DEFAULTS (LANGC, O3, NOFP, PREFER_LIBOSMC; package/common.sh)
export LANG=C
export LD_LIBRARY_PATH="/usr/osmc/lib" PKG_CONFIG_PATH="/usr/osmc/lib/pkgconfig"
if [ ! -f CMakeCache.txt ]; then
    BUILD_FLAGS="-O3 -fomit-frame-pointer -I/usr/osmc/include -L/usr/osmc/lib "
    COMPFLAGS="-march=armv7-a -mfloat-abi=hard -O3 -marm -mfpu=neon -fomit-frame-pointer "
    COMPFLAGS+="-I/opt/vero5/include -L/opt/vero5/lib -L/usr/osmc/lib -Wl,-rpath=/usr/osmc/lib"
    export CFLAGS="${COMPFLAGS} ${BUILD_FLAGS}" CXXFLAGS="${COMPFLAGS} ${BUILD_FLAGS}" CPPFLAGS="${COMPFLAGS} ${BUILD_FLAGS}"
    export LDFLAGS="-L/opt/vero5/lib"
    export LIBRARY_PATH="/opt/vero5/lib"
    cmake -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_INSTALL_LIBDIR=/usr/lib \
        -DCMAKE_PREFIX_PATH=/opt/vero5 \
        -DCMAKE_INCLUDE_PATH=/opt/vero5/include \
        -DCMAKE_LIBRARY_PATH=/usr/osmc/lib \
        -DOPENGLES_gl_LIBRARY=/opt/vero5/lib/libEGL.so \
        -DENABLE_AML=ON \
        -DAPP_RENDER_SYSTEM=gles \
        -DASS_INCLUDE_DIR=/usr/osmc/lib \
        -DAML_INCLUDE_DIR=/opt/vero5/include \
        -DSHAIRPLAY_INCLUDE_DIR=/usr/osmc/include/shairplay/ \
        -DENABLE_OPENGLES=ON \
        -DENABLE_OPENGL=OFF \
        -DENABLE_OPTICAL=1 \
        -DENABLE_DVDCSS=1 \
        -DWITH_ARCH=arm \
        -DWITH_CPU="cortex-a55" \
        -DCORE_PLATFORM_NAME=aml \
        -DCORE_SYSTEM_NAME=linux \
        -DENABLE_APP_AUTONAME=OFF \
        -DENABLE_INTERNAL_FMT=OFF \
        -DENABLE_INTERNAL_FLATBUFFERS=OFF \
        -DENABLE_INTERNAL_SPDLOG=OFF \
        -DENABLE_INTERNAL_UDFREAD=OFF \
        -DENABLE_MDNS=OFF \
        -DENABLE_BLUETOOTH=OFF \
        -DENABLE_PULSEAUDIO=OFF \
        -DENABLE_LCMS2=OFF \
        -DENABLE_SNDIO=OFF \
        -DENABLE_MARIADBCLIENT=ON \
        -DENABLE_INTERNAL_DAV1D=ON \
        -DADDONS_CONFIGURE_AT_STARTUP=OFF \
        ../
fi
JOBS=${GLAS_JOBS:-3}
start=$(date +%s)
# Compiler-Ausgabe vollstaendig in make.log (Fortschritt: grep '^\[' /build/logs/make.log | tail -1)
if ! nice -n 19 make -j"$JOBS" kodi > /build/logs/make.log 2>&1; then
    grep -E -B2 -A8 'error|Error' /build/logs/make.log | tail -60
    echo "Build fehlgeschlagen nach $(( $(date +%s) - start )) s"
    exit 1
fi
echo "make: $(( $(date +%s) - start )) s"
cp kodi.bin /build/out/kodi.bin.debug
cp kodi.bin /build/out/kodi.bin
strip -s /build/out/kodi.bin
ls -la /build/out/
echo "=== Ende $(date -Is)"
