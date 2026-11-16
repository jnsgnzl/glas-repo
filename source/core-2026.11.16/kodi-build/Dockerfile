# Build-Rechner fuer Kodi auf der Vero V wie bei OSMC: Debian Bullseye (amd64) mit OSMC-Paketquelle.
# OSMCs Skripte bauen in der armv7-Chroot aus dem Paket armv7-toolchain-osmc; die laeuft hier per qemu
# (binfmt von Docker Desktop). Siehe tools/kodi-build/README.md.
FROM debian:bullseye

# Debian 11 ist ausgelaufen (deb.debian.org liefert die Sicherheitspakete des Images nicht mehr):
# fester Snapshot, auf den das Image selbst verweist
RUN sed -i -e '/^deb /d' -e 's|^# deb http://snapshot|deb [check-valid-until=no] http://snapshot|' /etc/apt/sources.list \
    && apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates gnupg git make sudo dpkg-dev wget curl xz-utils patch \
        qemu-user-static binfmt-support procps \
    && rm -rf /var/lib/apt/lists/*

# OSMC-Paketquelle (Schluessel von der Vero: /etc/apt/trusted.gpg.d/osmc_repository.gpg)
COPY osmc_repository.gpg /etc/apt/trusted.gpg.d/osmc_repository.gpg
RUN echo "deb https://apt.osmc.tv bullseye main" > /etc/apt/sources.list.d/osmc.list

# armv7-Chroot von OSMC fest im Image (OSMCs Skripte installieren sie sonst bei jedem Lauf neu). Ihre
# Debian-Quellen zeigen auf deb.debian.org/security.debian.org, die fuer Bullseye 404 liefern: fester Snapshot.
# /opt/osmc-tc als Volume einhaengen (kodi-tc), dann bleiben die im Build nachinstallierten Pakete erhalten.
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends armv7-toolchain-osmc \
    && rm -rf /var/lib/apt/lists/* \
    && TC=/opt/osmc-tc/armv7-toolchain-osmc \
    && sed -i -e 's|^deb https://deb.debian.org/debian/\? bullseye-updates |deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/20260824T000000Z bullseye-updates |' \
              -e 's|^deb https://deb.debian.org/debian/\? bullseye |deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/20260824T000000Z bullseye |' \
              -e 's|^deb https://security.debian.org/\? bullseye-security |deb [check-valid-until=no] http://snapshot.debian.org/archive/debian-security/20260824T000000Z bullseye-security |' \
              $TC/etc/apt/sources.list \
    && grep -v '^#' $TC/etc/apt/sources.list

WORKDIR /build
