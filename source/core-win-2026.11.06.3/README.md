# Quelltext Live-Glas-Kern 2026.11.06.3 (Windows x64)

Eigener Kodi-Build für Windows x64 (DirectX 11), passend zum offiziellen Kodi 21.3 (Tag `21.3-Omega`).
Kodi steht unter der GPL 2.0; dies ist der vollständige Weg vom öffentlichen Quelltext zum Build:

1. Kodi 21.3-Omega von <https://github.com/xbmc/xbmc> (`kodi-build/windows/prepare-source.ps1`).
2. Glas-Änderungen: Control-Typ `backdropblur` in der DirectX-11-Fassung
   (`kodi-build/windows/src/xbmc/guilib/GUIBackdropBlurControl.cpp`) und die Anpassungen in Kodis Dateien
   (`kodi-build/windows/apply-glas.ps1`).
3. Bauen: `kodi-build/windows/README.md` (Visual Studio 2022, CMake).
