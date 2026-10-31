# Quelltext Live-Glas-Kern 2026.10.29

Eigener Kodi-Build für die Vero V (OSMC), passend zu OSMCs Paket `vero5-mediacenter-osmc` 21.3.0-7.
Kodi steht unter der GPL 2.0; dies ist der vollständige Weg vom öffentlichen Quelltext zum Build:

1. OSMC-Repository <https://github.com/osmc/osmc> auf dem Stand des Pakets 21.3.0-7 (siehe `kodi-build/osmc-glas.sh`),
   darin Kodi 21.3 mit OSMCs Patches (`kodi-build/prepare-source.sh`).
2. Glas-Änderungen: neuer Control-Typ `backdropblur` (`kodi-build/src/xbmc/guilib/GUIBackdropBlurControl.*`) und
   die Anpassungen in Kodis Dateien (`kodi-build/apply-glas.sh`, `kodi-build/overdraw.sh`).
3. Bauen: `kodi-build/README.md` (Docker, Debian 11, OSMCs armv7-Toolchain).
