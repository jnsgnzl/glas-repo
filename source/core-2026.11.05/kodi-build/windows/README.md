# Eigener Kodi-Build für Windows (Live-Glas, DirectX 11)

> **Stand 01.11.2026:** Durchstich und Live-Glas (DX11, `frosted` und `liquid`, Video-Glas über SDR-Filmen) unter
> Windows 11 gebaut und am PC getestet (Build in `C:\Users\KI\glas-win`, AMD Radeon, 16:9 und 21:9). 10 Bit und
> begrenzter Farbbereich geprüft. Der PQ/HDR-Weg ist eingebaut und nachgerechnet, aber auf einem HDR-Bildschirm noch
> nicht angesehen (der PC hat keinen). Stereo ist offen. Plan, Architektur und offene Fragen: [docs/plan-liveglas-windows.md](../../../docs/plan-liveglas-windows.md).
> Wie du das gebaute Kodi startest: [Abschnitt 0](#0-gebautes-kodi-starten-kurzanleitung).

Grundlage ist das unveränderte Kodi 21.3 (Tag `21.3-Omega`, Commit `a3a448d`). Auf demselben Commit baut OSMC für die
Vero. OSMC-Patches gibt es unter Windows nicht. Die Glas-Änderungen kommen aus `../src/` (gemeinsam mit der Vero) und
aus `src/` in diesem Ordner (DX11-Fassungen, sobald es sie gibt).

## Dateien

| Datei | Zweck | Gegenstück (Vero) |
|---|---|---|
| `check-env.ps1` | prüft Werkzeuge und Plattenplatz, installiert nichts | – |
| `prepare-source.ps1` | Kodi 21.3 klonen, Zweig `vanilla`, LF und lange Pfade | `../prepare-source.sh` |
| `apply-glas.ps1` | Glas-Änderungen in den Zweig `glas` übernehmen, Commit, Patches nach `<WorkDir>\patches` | `../apply-glas.sh`, `../overdraw.sh` |
| `src/xbmc/guilib/GUIBackdropBlurControl.cpp` | DX11-Fassung des Controls (gleicher Header wie die Vero); ihr Vorhandensein schaltet `apply-glas.ps1` auf „Live-Glas“ | `../src/xbmc/guilib/GUIBackdropBlurControl.cpp` (GLES) |

`apply-glas.ps1` kennt zwei Stufen:
- **Durchstich** (solange `src/xbmc/guilib/GUIBackdropBlurControl.cpp` fehlt): nur plattformfreie Teile, also
  Schrift in Zielgröße, Stereo-Texturen und Overdraw. Die Startlog-Zeile heißt
  `Glas: Windows-Durchstich ohne Live-Glas (skin.glas)`. Damit erkennt `script.glas.helper` keinen Live-Glas-Kern, und
  der Skin bietet die Einstellung „Live-Glas“ nicht an.
- **Live-Glas**: zusätzlich Control `backdropblur`, Fabrik, CMake, Meldungen aus `CGUIControl::DoProcess`, Haken in
  `CApplication::Render`, `GUIWindowManager`/`GUIWindowFullScreen` wie auf der Vero. Die Startlog-Zeile lautet
  `Glas: eigener Kodi-Build (skin.glas), Backdrop-Blur-Spike, Stereo-Texturen, Windows-DX11`. Mit `-VideoGlass`
  kommt `, Video-Glas` hinzu; erst setzen, wenn Video-Glas wirklich geht.

AML-Teile der Vero (`ScreenshotAML.cpp`, `AMLUtils.cpp`, `WinSystemAmlogic.cpp`) lässt es weg.

## Schritt für Schritt

### 0. Gebautes Kodi starten (Kurzanleitung)

Der fertige Build liegt in `C:\Users\KI\glas-win\build\Release\kodi.exe` (samt DLLs, `system\`, `addons\`). Er
benutzt **dasselbe Profil** wie das installierte Kodi (`%APPDATA%\Kodi`): Bibliothek, Skin „Glas“ und Glas Helper
sind also gleich da.

1. Installiertes Kodi beenden. **Nie zwei Kodi gleichzeitig** (gleiches Profil, gleiche Datenbanken).
2. Starten, am einfachsten per Doppelklick auf `kodi.exe` im Explorer, oder in PowerShell:
   ```powershell
   Start-Process C:\Users\KI\glas-win\build\Release\kodi.exe -WorkingDirectory C:\Users\KI\glas-win\build\Release
   ```
   Für eine Verknüpfung auf dem Desktop: Ziel wie oben, „Ausführen in“ = `C:\Users\KI\glas-win\build\Release`.
3. Erkennung prüfen: `%APPDATA%\Kodi\kodi.log` enthält
   `Glas: eigener Kodi-Build (skin.glas), Backdrop-Blur-Spike, Video-Glas, Stereo-Texturen, Windows-DX11`. Der Glas Helper
   setzt daraufhin `Window(Home).Property(glas.corebuild)`, und in den Skin-Einstellungen erscheint **„Live-Glas“**.
4. „Live-Glas“ einschalten (Skin-Einstellungen, oder `Skin.SetBool(glas_coreblur)`). Seitenleiste, Kontrollzentrum,
   Dock und Dialoge zeigen dann echtes, weichgezeichnetes Glas statt der statischen Glasbilder.
5. Zurück zum installierten Kodi geht ohne Umschalten: Der Skin nutzt Live-Glas nur, wenn der laufende Kern es meldet
   (`glas.corebuild`), sonst die statischen Glasbilder (behoben 01.11.2026, vorher fehlte dort alles Glas).

Der Build meldet „Video-Glas“ (gebaut mit `-VideoGlass`): Über laufenden Filmen zeigt das Glas das bewegte Video
(unter DX11 liegt es schon im Backbuffer, der Griff sieht es mit). Geprüft mit SDR-Filmen. Kodis eigenes
Bildschirmfoto zeigt unter Windows kein Video, für Video-Glas ein Windows-Bildschirmfoto nehmen.

**Farbraum (Plan 3.5, Phase 4):** Griff und Pyramide haben Format und Kodierung des Backbuffers (8 oder 10 Bit, voller
oder begrenzter Bereich, SDR oder PQ). Beim Zusammensetzen rechnet das Glas erst auf SDR-Gamma im vollen Bereich
zurück, wendet dann das Material an (Tönung, Weiß, Abdunklung) und kodiert danach genau wie Kodis GUI-Shader
(`tonemapHDR(adjustColorRange(c))`). Die Werte kommen aus denselben Quellen wie in `CGUIShaderDX`
(`UseLimitedColor`, `IsTransferPQ`, `GetGuiSdrPeakLuminance`). HDR-Lichter über dem SDR-Weiß dämpft es weich auf
höchstens das 1,5-Fache (linear). Stand 01.11.2026:
- **10 Bit** (`videoscreen.10bitsurfaces` = 2, „Immer“; auf AMD steht „Auto“ auf 8 Bit): Log
  `10 bit swapchain … R10G10B10A2_UNORM`. Das Glas weicht von 8 Bit im Mittel um 0,5 und höchstens um 2 von 255 ab.
- **Begrenzter Farbbereich** (`videoscreen.limitedrange`): Glasflächen = 16 + voll·219/255 bis auf ±2 (Seitenleiste
  im Mittel 84,2 bei erwarteten 84,2), also keine doppelte Umrechnung.
- **PQ/HDR-Ausgabe:** nur nachgerechnet (Python, gleiche Formeln): SDR-Farbe → PQ → Glas → zurück entspricht dem
  SDR-Glas bis auf float-Rundung (≤ 1,3/255). Mit 10-Bit-Rundung des Griffs sind es im Mittel 0,5/255 und bei
  gesättigten Farben am Gamut-Rand bis 8 PQ-Codes. Ohne die Korrektur wären es 40–90/255. Messschalter
  `C:\tmp\glas-force-pq` erzwingt den PQ-Zweig auf dem SDR-Backbuffer. Das ist nur ein Lauftest: keine Fehler, das
  Bild ist dabei absichtlich falsch (Glas heller). **Wie es auf einem echten HDR-Bildschirm aussieht, ist noch nicht
  geprüft.**
- **HDR-Film auf SDR-Bildschirm** (geprüft mit „Guardians of the Galaxy“, HEVC 10 Bit, HDR10, `ff-hevc` in Software):
  Kodi rechnet das Video auf SDR herunter, der Backbuffer bleibt SDR (Glas meldet `Ausgabe SDR`). Kontrollzentrum und
  Player-Menü zeigen das bewegte Video im Glas, Glas-GPU 0,3–0,9 ms je Bild. Verspätete Bilder gibt es dabei durch die
  4K-Software-Dekodierung, nicht durch das Glas. Mit DXVA stürzt das PC-Kodi bei HEVC/HDR ab, deshalb zum Testen
  `videoplayer.usedxva2` aus.
- **Leistung nach Phase 4** (Raster scrollen, 1600×900): Glas im Mittel 0,06–0,15 ms GPU je Bild, ein Griff etwa 0,7 ms,
  keine zusätzlichen verspäteten Bilder. Das entspricht dem Stand vor Phase 4.

Portabel mit eigenem Profil (`kodi.exe -p`, Profil in `portable_data\`): siehe Schritt 7.

Gemessen am PC (01.11.2026, AMD Radeon, Fenster 1600×900 und 1920×800, 60 Hz): Ein Griff samt Pyramide und
Zusammensetzen kostet etwa 0,7 ms GPU, im Mittel je Bild 0,1–0,5 ms, CPU je Griff unter 0,1 ms. Verspätete Bilder
beim Scrollen gleich wie ohne Live-Glas (Streuung 1–4 %). Über laufendem Video wird jedes Bild neu gegriffen
(0,7 ms GPU je Bild), keine verspäteten Bilder.

Neu bauen nach Änderungen an den Glas-Quellen (PowerShell, dauert inkrementell wenige Minuten):

```powershell
powershell -ExecutionPolicy Bypass -File C:\Users\janis\Projekte\Kodi-tvOS\tools\kodi-build\windows\apply-glas.ps1 -KodiDir C:\Users\KI\glas-win\kodi -VideoGlass
$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$env:PATH = ($env:PATH -split ';' | Where-Object { $_ -notmatch 'WinLibs' }) -join ';'
& $cmake --build C:\Users\KI\glas-win\build --config Release -- /m /v:minimal
```

### 1. Werkzeuge (einmalig, installiert der Nutzer selbst)

Stand am PC (01.11.2026): VS 2022 Community 17.12 ist da, aber **ohne C++-Workload**. Windows SDK und `fxc.exe` fehlen,
CMake im `PATH` ist 4.4.4 (WinLibs). Git 2.53 und Java 8 sind vorhanden.

1. **Visual Studio Installer** öffnen, „Visual Studio Community 2022“ → *Ändern*:
   - Workload **„Desktopentwicklung mit C++“** anhaken. Das bringt MSVC v143, das Windows 11 SDK (mit `fxc.exe`) und
     „C++-CMake-Tools für Windows“.
   - Bei der Gelegenheit auf die neueste 17.x aktualisieren.
   - Etwa 8 GB.
2. **CMake 3.31.x** (Kitware, x64-Installer). Alternativ das CMake aus dem VS-Workload. CMake 4.x ist für Kodi 21 nicht
   erprobt: Es lehnt `cmake_minimum_required` unter 3.5 ab, wie es ältere Abhängigkeiten nutzen. Notbehelf:
   Umgebungsvariable `CMAKE_POLICY_VERSION_MINIMUM=3.5`.
3. Java (vorhanden, JRE 8 genügt: Kodi verlangt keine Mindestversion) und Git (vorhanden).
4. NSIS nur für einen Installer.
5. Prüfen:
   ```powershell
   powershell -ExecutionPolicy Bypass -File tools\kodi-build\windows\check-env.ps1 -WorkDir D:\glas-win
   ```

**Plattenplatz:** Quelltext, Abhängigkeiten, MSYS2 und Build-Ordner brauchen etwa 25–35 GB. Auf `C:` sind nur etwa
42 GB frei. Den Arbeitsordner besser auf ein anderes Laufwerk legen und kurz halten (Pfadlängen), z. B. `D:\glas-win`.

### 2. Shell

Gebaut wird in der **„Developer PowerShell for VS 2022“** (bzw. „x64 Native Tools Command Prompt“). Die
WinLibs-MinGW-Werkzeuge (`gcc`, `ninja`, `cmake` 4.x) dort für diese Sitzung aus dem `PATH` nehmen, damit CMake nicht
die falschen findet:

```powershell
$env:PATH = ($env:PATH -split ';' | Where-Object { $_ -notmatch 'WinLibs' }) -join ';'
```

### 3. Quelltext

```powershell
powershell -ExecutionPolicy Bypass -File tools\kodi-build\windows\prepare-source.ps1 -WorkDir D:\glas-win
```

Ergebnis: `D:\glas-win\kodi` (Git, Zweig `vanilla` = 21.3-Omega).

### 4. Abhängigkeiten (Kodis eigene Skripte, laden aus dem Netz)

```bat
cd /d D:\glas-win\kodi\tools\buildsteps\windows\x64
download-dependencies.bat
download-msys2.bat
make-mingwlibs.bat
```

- Meldet `download-dependencies.bat` „Not all formed packages are ready!“, einfach noch einmal starten.
- `make-mingwlibs.bat` baut FFmpeg in Kodis eigenem MSYS2 (unter `project\BuildDependencies`) und dauert lange.

### 5. Zuerst unverändert bauen (Durchstich)

Erst prüfen, dass Kodi 21.3 selbst baut, bevor Glas-Änderungen dazukommen:

```bat
mkdir D:\glas-win\build && cd /d D:\glas-win\build
cmake -G "Visual Studio 17 2022" -A x64 -T host=x64 D:\glas-win\kodi
cmake --build . --config Release -- /m
```

Ergebnis: `D:\glas-win\build\Release\kodi.exe` samt DLLs, `system\`, `addons\`.

### 6. Glas-Änderungen einspielen und neu bauen

```powershell
powershell -ExecutionPolicy Bypass -File tools\kodi-build\windows\apply-glas.ps1 -KodiDir D:\glas-win\kodi
cmake --build D:\glas-win\build --config Release -- /m
```

`apply-glas.ps1` ist wiederholbar. Es setzt die berührten Dateien auf `vanilla` zurück und patcht sie neu. Dateien mit
gleichem Inhalt bekommen ihren alten Zeitstempel zurück, damit MSBuild nur Geändertes übersetzt. Fehlt ein Anker (andere
Kodi-Fassung), bricht es mit Meldung ab. Ohne Commit: `-NoCommit`.

### 7. Testen am PC (das installierte Kodi bleibt unberührt)

Der eigene Build läuft **portabel**. Profil und Add-ons liegen dann in `portable_data\` neben `kodi.exe`, nicht in
`%APPDATA%\Kodi`.

```powershell
cd D:\glas-win\build\Release
.\kodi.exe -p
# einmal nach dem ersten Start: Skin und Helper aus dem Repo verknüpfen
cmd /c mklink /J portable_data\addons\skin.glas C:\Users\janis\Projekte\Kodi-tvOS\skin.glas
cmd /c mklink /J portable_data\addons\script.glas.helper C:\Users\janis\Projekte\Kodi-tvOS\script.glas.helper
```

- Startlog prüfen: `portable_data\kodi.log` enthält die Glas-Zeile (siehe oben).
- Immer **16:9 und 21:9** prüfen: Fenster bzw. Auflösung 1920×1080 und 2560×1080.
- Testplan: [docs/plan-liveglas-windows.md](../../../docs/plan-liveglas-windows.md), Abschnitt 9.

**Messschalter:** Der Kern prüft Dateien `/tmp/glas-*`. Unter Windows sind das `C:\tmp\glas-*` (Laufwerk, von dem
Kodi läuft; am 01.11.2026 mit `C:\tmp\glas-timing` geprüft). Beispiele:

```powershell
New-Item -ItemType Directory -Force C:\tmp | Out-Null
New-Item C:\tmp\glas-frames -ItemType File    # GLAS_FRAMES im Log (Bilder je Sekunde, verspätete Bilder)
New-Item C:\tmp\glas-timing -ItemType File    # GLAS_TIMING: CPU je Bild, GPU je Bild und davon Glas (DX11-Zeitstempel)
New-Item C:\tmp\glas-overdraw -ItemType File  # GLAS_OVERDRAW im Log
New-Item C:\tmp\glas-font-off -ItemType File  # Schrift wieder per GPU verkleinern (Vergleich)
New-Item C:\tmp\glas-pyramid-off -ItemType File # Glas zeigt den unverwischten Griff (Lage prüfen)
```

Weitere Schalter der DX11-Fassung wie auf der Vero: `glas-diag`, `glas-grablog`, `glas-grabscale` (Zahl),
`glas-lens-old`, `glas-liquid-inner-off`, dazu nur unter Windows `glas-force-pq` (PQ-Zweig des Glases auf dem
SDR-Backbuffer erzwingen, nur Lauftest: das Bild ist dabei falsch). Die Mali-Schalter (Zwischenbild, Quelle, Ring, Stereo-Finish) gibt es unter
Windows nicht. Hinweis: Der Glas Helper legt bei erkanntem Kern `C:\tmp\glas-stereo-finish-off` an (`display3d.py`);
unter Windows wirkungslos.

Zum Ansehen der DX11-Pässe (Griff, Pyramide, Zusammensetzen) eignet sich RenderDoc, für Bildzeiten PresentMon (beides
extern, bei Bedarf selbst installieren).

### 8. Verteilen über den Glas Helper (wie auf der Vero)

Unter Windows unterscheidet sich nur `kodi.exe` vom offiziellen Kodi 21.3 (alle DLLs gleich, die Glas-Änderungen
stecken nur in Kodis C++-Code). Deshalb verteilt der Glas Helper den Kern wie auf der Vero (`script.glas.helper/core.py`,
Skin-Einstellung „Live-Glas-Kern“):

1. Veröffentlichen: `python tools/publish-repo.py core-win build\Release\kodi.exe --id <id> --base "C:/Program Files/Kodi/kodi.exe" --release`
   packt `kodi.exe` mit xz, trägt den Build mit `device: "win64"` und der Prüfsumme der offiziellen `kodi.exe`
   (`kodi_sha256`) ins selbe `core/manifest.json` ein, legt den Quelltext nach `source/core-win-<id>/` und das Release
   `core-win-<id>` mit `kodi.exe.xz` an. Danach `publish-repo.py push`.
2. Beim Tester: Der Helper vergleicht seine `kodi.exe` mit `kodi_sha256`, lädt den passenden Build, prüft die
   Prüfsumme und startet `resources/core/glas-core.ps1`. Liegt Kodi unter Programme, fragt Windows einmal nach
   Administratorrechten. Das Skript sichert das Original als `kodi.exe.orig`, tauscht nach dem Ende von Kodi die
   Datei und startet Kodi mit normalen Rechten neu.
3. Entfernen stellt `kodi.exe.orig` wieder her. Ein Kodi-Update über den offiziellen Installer ersetzt den Kern; der
   Helper bietet ihn neu an, sobald ein Build zur neuen `kodi.exe` passt. Einen automatischen Rückweg nach Abstürzen
   wie den Starter der Vero gibt es nicht.

Von Python geladene Dateien tragen keine Download-Markierung, SmartScreen meldet sich deshalb nicht.

## Was noch fehlt

- HDR/PQ-Ausgabe auf einem echten HDR-Bildschirm ansehen (Windows-HDR an, bzw. Kodis HDR-Umschaltung bei einem
  HDR-Film): Glas nicht überstrahlt, Material wie bei SDR, `sdrPeakLum` (SDR-Helligkeit in Windows) verstellen.
  Bisher nur nachgerechnet und mit `glas-force-pq` als Lauftest geprüft (siehe Abschnitt 0, „Farbraum“).
- HDR-Lichter unter dem Glas: weiche Grenze bei 1,5× SDR-Weiß (`PQ_HEAD`) ist geschätzt, am HDR-Bildschirm abstimmen.
- Stereo-3D unter DX11 (SBS/TAB, Hardware-Stereo): Code-Pfad vorhanden (Projektion, Array-Slice aus dem RTV),
  ungetestet.
- Glas mit eigener Quelle (`<source>`, Dock): greift unter Windows wie jedes andere Glas (gleiches Bild).
- Über dem Film sieht das Glas die Abdunklung des Player-Menüs mit (Vero: nur das Videobild); Plan 3.4.
- GPU-Zeitstempel: es wird nur ein Teil der Bilder gemessen (Ergebnisse des verzögerten Kontexts kommen spät).
- Weg A (gemeinsame Logik, zwei Backends) statt doppelter `Process`-/Griff-Logik.
- Ein Build-Skript (Schritte 4–6 in einem).
