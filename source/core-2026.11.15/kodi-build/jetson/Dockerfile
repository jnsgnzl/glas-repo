# Build-Rechner fuer Kodi auf der Vero V, nativ auf dem Jetson (ARM64, fuehrt 32-Bit-ARM direkt aus):
# Debian Bullseye armhf wie OSMCs Toolchain-Chroot armv7-toolchain-osmc, aber ohne Chroot und ohne qemu.
# Pakete wie OSMCs package/mediacenter-osmc/build.sh (handle_dep, Ziel vero5). Siehe tools/kodi-build/README.md.
FROM --platform=linux/arm/v7 debian:bullseye

# Debian 11 ist ausgelaufen: fester Snapshot wie beim PC-Build (tools/kodi-build/Dockerfile, Toolchain-Chroot)
RUN printf '%s\n' \
      'deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/20260824T000000Z bullseye main contrib non-free' \
      'deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/20260824T000000Z bullseye-updates main contrib non-free' \
      'deb [check-valid-until=no] http://snapshot.debian.org/archive/debian-security/20260824T000000Z bullseye-security main contrib non-free' \
      > /etc/apt/sources.list \
    && apt-get -o Acquire::Retries=5 update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends ca-certificates gnupg wget \
    && rm -rf /var/lib/apt/lists/*

# OSMC-Paketquelle (Schluessel von der Vero: /etc/apt/trusted.gpg.d/osmc_repository.gpg)
COPY osmc_repository.gpg /etc/apt/trusted.gpg.d/osmc_repository.gpg
RUN echo "deb https://apt.osmc.tv bullseye main" > /etc/apt/sources.list.d/osmc.list

RUN apt-get -o Acquire::Retries=5 update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential git patch perl procps xz-utils bzip2 file \
        autopoint automake bison make curl cvs default-jre-headless gawk gettext gperf liblirc-dev libasound2-dev \
        libbz2-dev libcap-dev libcdio-dev libcurl4-openssl-dev libdbus-1-dev libfontconfig1-dev libfreetype6-dev \
        libfribidi-dev libfstrcmp-dev libgtest-dev libinput-dev libgif-dev libiso9660-dev libjpeg62-turbo-dev liblzo2-dev \
        libmad0-dev libmicrohttpd-dev libmodplug-dev libmariadbd-dev libpcre3-dev libplist-dev libpng-dev \
        libsmbclient-dev libssh-dev libavahi-client-dev libssl-dev libtinyxml-dev libtinyxml2-dev libtool libudev-dev \
        libvorbis-dev libxml2-dev libxmu-dev libxslt1-dev libxt-dev libyajl-dev libxkbcommon-dev nasm python3-dev \
        python3-pil swig unzip yasm zip zlib1g-dev libtag1-dev libsamplerate0-dev libltdl-dev libgnutls28-dev uuid-dev \
        libcrossguid-dev cmake rapidjson-dev libgcrypt20-dev libnfs-dev libass-dev libunistring-dev xmlstarlet meson \
        libflatbuffers-dev libspdlog-dev libfmt-dev libudfread-dev libdisplay-info-dev \
        vero5-libcec-dev-osmc vero5-userland-dev-osmc armv7-libshairplay-dev-osmc armv7-librtmp-dev-osmc \
        armv7-libplatform-dev-osmc armv7-libbluray-dev-osmc armv7-libsqlite-dev-osmc libamcodec-dev-osmc \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
