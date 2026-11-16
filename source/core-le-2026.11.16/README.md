# Quelltext Live-Glas-Kern 2026.11.16 (LibreELEC Generic x86_64)

Eigener Kodi-Build für LibreELEC 12.2.1 „Generic“ (x86_64, GBM/GLES), Kodi 21.3-Omega mit den Kodi-Patches von
LibreELEC 12.2.1. Kodi steht unter der GPL 2.0; dies ist der vollständige Weg vom öffentlichen Quelltext zum Build:

1. LibreELEC <https://github.com/LibreELEC/LibreELEC.tv> beim Tag 12.2.1, daraus Kodi-Tag und Kodi-Patches
   (`kodi-build/libreelec/prepare-source.sh`).
2. Glas-Änderungen: Control-Typ `backdropblur` in der GLES-Fassung (`kodi-build/src/xbmc/guilib/GUIBackdropBlurControl.*`)
   und die Anpassungen in Kodis Dateien (`kodi-build/windows/apply-glas.ps1 -Target gbm`).
3. Bauen: `kodi-build/libreelec/README.md` (LibreELECs Bauumgebung in Docker, Paket `kodi`).
