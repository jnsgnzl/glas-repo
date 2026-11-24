# Live-Glas-Kern für LibreELEC 12 (Generic x86_64)

Stand 16.11.2026: Auf dem NAS übersetzt (`out/12.2.1-Generic/kodi.bin`, 26 MB, Startlog-Zeile mit `LibreELEC-GBM`)
und auf einem Gerät gelaufen (Ryzen 7 5700U mit Radeon-Grafik, Mesa 25.1.9, LibreELEC 12.2.1 im Live-Modus vom
Stick): von Hand eingebunden und über den Glas Helper installiert, jeweils rund eine Minute ohne Glas-Fehler.
Noch nicht veröffentlicht.

LibreELEC baut mit GCC 13 und `-Werror=double-promotion`/`sign-compare`/`missing-field-initializers`: Standard-Header
direkt einbinden (libstdc++ 13 bringt z. B. `<string>` nicht mehr über Umwege mit) und `float`→`double` ausdrücklich
umwandeln – sonst scheitert der Bau, obwohl Windows und Vero durchgehen.

## Warum es geht

LibreELEC 12 „Generic“ baut Kodi mit **GBM + GLES** über Mesa (`projects/Generic/devices/gbm/options`:
`OPENGL="no"`, `OPENGLES="mesa"`, `DISPLAYSERVER="no"` → im Kodi-Paket `-DCORE_PLATFORM_NAME=gbm
-DAPP_RENDER_SYSTEM=gles`). Damit läuft die GLES-Fassung des Glases (`../src`, wie auf der Vero) ohne Portierung.
„Generic-legacy“ (X11, Desktop-OpenGL, für Nvidia) geht damit **nicht**.

LibreELEC 12.2.1 baut Kodi 21.3-Omega – derselbe Stand wie Windows (`../windows`). Der Patch entsteht deshalb mit
`../windows/apply-glas.ps1 -Target gbm`: gleiche Textanker, aber GLES-Quellen aus `../src` statt DX11.

## Bewusst weggelassen (nur Amlogic/Vero)

- Video-Glas über `/dev/amvideocap0`: Der Kern meldet im Startlog **kein** „Video-Glas“. Fehlt das Gerät, schaltet die
  GLES-Fassung ab (Warnung `GLAS_VIDEOGLASS` im Log). Offen: Mit der Rendermethode „EGL“ zeichnet Kodi das Video selbst
  ins GLES-Bild, dann könnte das Glas es direkt greifen – ungeprüft.
- Fester Takt und Leerlauf ohne Tausch (`WinSystemAmlogicGLESContext.cpp`), Bildschirmfoto (`ScreenshotAML.cpp`),
  Grenze der Oberflächengröße (`AMLUtils.cpp`, `WinSystemAmlogic.cpp`). Der feste Takt gilt nur auf Amlogic
  (Erkennung über `/dev/amvideocap0`): `/dev/fb0` mit `FBIO_WAITFORVSYNC` gibt es auch am PC (fbdev des
  DRM-Treibers), der erste Kern vom 16.11.2026 begrenzte die Oberfläche deshalb auf 30 Bilder/s. Im Log steht jetzt
  `GLAS_TAKT: … kein Amlogic: ohne festen Takt`.

Startlog-Zeile endet mit `LibreELEC-GBM` (statt `Windows-DX11`).

## Bauen

### 1. Patch erzeugen (PC, Git Bash + PowerShell)

```bash
bash tools/kodi-build/libreelec/prepare-source.sh 12.2.1 ~/glas-le
```

```powershell
powershell -ExecutionPolicy Bypass -File tools\kodi-build\windows\apply-glas.ps1 -KodiDir $env:USERPROFILE\glas-le\kodi -Target gbm
```

`prepare-source.sh` holt den Kodi-Tag aus dem Kodi-Paket von LibreELEC und legt `vanilla` = Kodi + die Kodi-Patches von
LibreELEC (nur der Grundordner; `cec-framework`, `drmprime-filter`, `wayland` gelten nicht für Generic) an. Ergebnis:
`~/glas-le/patches/0001-Glas-LibreELEC-GBM-GLES-Live-Glas.patch`.

### 2. Auf den NAS kopieren

Freigegebener Ordner `glas-build` auf der DS923+ (Container Manager installiert). Per SMB kopieren – die Dateien
gehören dann dem Benutzer der Freigabe, unter dessen UID der Bau läuft:

```powershell
Copy-Item $env:USERPROFILE\glas-le\patches\0001-*.patch \\<NAS>\glas-build\glas.patch
Copy-Item tools\kodi-build\libreelec\le-*.sh \\<NAS>\glas-build\
```

### 3. Bauen (NAS)

Docker braucht auf dem NAS root; `sudo` fragt per SSH nach dem Passwort. Deshalb einmalig im DSM-Aufgabenplaner
(Systemsteuerung › Aufgabenplaner › Erstellen › Geplante Aufgabe › Benutzerdefiniertes Skript): Benutzer **root**,
täglich alle 5 Minuten, Befehl:

```bash
for d in /volume*/glas-build; do [ -f "$d/le-watch.sh" ] && bash "$d/le-watch.sh"; done
```

Einen Bau startet dann eine Datei `start.txt` im Ordner (leer oder mit `LE_TAG=…`, `JOBS=…`, `LTO=…` je Zeile).
`le-watch.sh` löscht sie, ruft `le-build.sh` auf und schreibt `status.txt` und `bau.log`. Wer in `glas-build`
schreiben darf, kann damit als root bauen lassen – den Ordner nur dem eigenen Benutzer freigeben.

- `le-build.sh` baut das Abbild aus dem Dockerfile von LibreELEC (`tools/docker/noble`) plus einen Benutzer mit der UID
  des Ordnerbesitzers (LibreELEC baut nie als root), setzt Jobs und LTO nach RAM (unter 12 GB ohne LTO) und startet
  `le-inner.sh` im Container.
- `le-inner.sh` holt LibreELEC beim Tag, legt den Patch als `packages/mediacenter/kodi/patches/kodi-999.90-glas.patch`
  ab und baut nur das Paket `kodi` (mit Toolchain, Mesa, LLVM usw.), kein Abbild.
- Erster Lauf: gut 8 Stunden bis Kodi (DS923+, 4 Threads mit LTO); LLVM braucht beim Binden viel RAM. Danach baut
  ccache nur Kodi neu (rund 20 Minuten).
- Scheitert Kodi, übersetzt `le-inner.sh` mit `ninja -k 0` weiter und schreibt alle Fehler auf einmal ans Ende von
  `bau.log` (Abschnitt „Alle Kodi-Fehler“).
- Ergebnis: `out/12.2.1-Generic/kodi.bin` und `kodi.bin.sha256`.

Umgebung: `LE_TAG`, `PROJECT`/`DEVICE`/`ARCH` (Vorgabe Generic/Generic/x86_64), `JOBS`, `LTO`.

## Ausprobieren (ohne Helper, verschwindet beim Neustart)

LibreELEC ist schreibgeschützt; ein Bind-Mount legt den Kern nur bis zum nächsten Neustart darüber:

```bash
mkdir -p /storage/glas && cp kodi.bin /storage/glas/ && chmod +x /storage/glas/kodi.bin
systemctl stop kodi
mount --bind /storage/glas/kodi.bin /usr/lib/kodi/kodi.bin
systemctl start kodi
grep "Glas: eigener Kodi-Build" /storage/.kodi/temp/kodi.log
```

Zurück: `reboot` (oder `systemctl stop kodi; umount /usr/lib/kodi/kodi.bin; systemctl start kodi`).

Ohne SSH macht dasselbe das Add-on `script.glas.kerntest` (als ZIP mit `addon.xml`, `default.py` und `kodi.bin` im
Ordner `script.glas.kerntest/` packen, von der Startpartition des Sticks installieren). Im Live-Modus liegt `/storage`
im Arbeitsspeicher, die Logs sind nach dem Ausschalten weg. Das Add-on sichert sie deshalb auf die Startpartition
(`/flash`, FAT, unter Windows lesbar) nach `glas-logs/<log|start>-<Zeit>/`: über den Menüpunkt „Logs auf den Stick
sichern“ und von selbst 45 Sekunden nach dem Einbinden. Gesichert werden `kodi.log`, `kodi.old.log`, Absturzlogs,
Bildschirmfotos aus `/storage/screenshots`, das Journal, `dmesg`, die Prüfsumme des laufenden Kerns sowie
`systemctl cat kodi` und der Zustand unter `/storage/.config/glas-core` (Weg über den Glas Helper).

### Testpaket auf den Stick (vor einer Veröffentlichung)

```bash
python tools/kodi-build/libreelec/le-testpaket.py <kodi.bin> --id 2026.11.16 --le 12.2.1 --base <LibreELECs kodi.bin> --out D:/
```

Legt das Add-on-ZIP mit diesem Kern sowie `glas-test/kodi.bin.xz` und `glas-test/manifest.json` (Adresse
`file:///flash/glas-test/…`) auf die Startpartition. LibreELECs eigenes `kodi.bin` für `--base` steckt im Abbild
`SYSTEM` auf dem Stick (`7z e D:\SYSTEM usr/lib/kodi/kodi.bin`). Unter LibreELEC stellt der Menüpunkt „Glas Helper
testen“ des Add-ons den Glas Helper auf dieses Manifest um (`addon_data/script.glas.helper/core/manifest_url`); danach
den Kern über die Glas-Einstellungen installieren, **ohne** ihn vorher im Add-on einzubinden, und am Ende die Logs
sichern. So läuft der Helper-Weg wie später bei Testern, nur ohne Netz und ohne Release.

Mit `--addons` kommen Glas Helper und Glas aus dem Arbeitsbaum als Test-ZIPs dazu (Version mit angehängter `.1`).
Nötig, solange der veröffentlichte Glas Helper LibreELEC nicht kennt: Am 16.11.2026 zeigte Glas mit Helper 0.13.0 aus
dem Repository keinen Eintrag „Live-Glas-Kern“, das Umstellen des Manifests blieb deshalb ohne Wirkung. Reihenfolge
auf dem Gerät: Glas aus dem Repository installieren (holt die Abhängigkeiten), dann die Test-ZIPs von Helper und Skin
darüber, dann das Kerntest-Add-on.

Frisches System: Kodi lädt die Liste seines eigenen Repositorys nicht von selbst nach, wenn das Netzwerk erst nach
dem Start eingerichtet wurde. Bis dahin scheitert die Installation von Glas („Abhängigkeit … nicht erfüllbar“, in
der Liste „Nicht verfügbar“). Abhilfe: unter Add-ons › Aus Repository installieren das Kodi-Repository einmal öffnen.

## Über den Glas Helper (Installation am 16.11.2026 auf einem Gerät geprüft)

`script.glas.helper/core.py` erkennt LibreELEC an `/etc/os-release` (`ID=libreelec`, `LIBREELEC_ARCH=Generic.x86_64`,
Gerät im Manifest `le-generic.x86_64`) und nimmt den Build mit `libreelec` = `VERSION` (z. B. `12.2.1`).
`resources/core/glas-core-le.sh` (Kodi läuft unter LibreELEC als root, kein sudo):

- **install**: prüft – wenn das Manifest `kodi_sha256` hat – LibreELECs eigenes `kodi.bin` (liegt der Build schon
  darüber, über ein zweites Einhängen von `/` ohne Untermounts), legt Build, Zustand (`build`, `base` = Gerät, Version,
  `BUILD_ID`, `starts`) und eine Kopie des Skripts nach `/storage/.config/glas-core/` und das Drop-in
  `/storage/.config/system.d/kodi.service.d/glas-core.conf` (`ExecStartPre=-/bin/sh …/glas-core.sh prestart`).
- **prestart** (vor jedem Start von `kodi.service`, auch beim Neustart nach einem Absturz – `Restart=always`): löst
  alte Mounts, legt den Build per Bind-Mount über `/usr/lib/kodi/kodi.bin` – nur wenn `base` zum System passt, kein
  Rückfall gesetzt ist und der Build nicht dreimal hintereinander in der ersten Minute endete (gleiche Prüfung über
  `kodi.log` wie der Starter der Vero). Ebenso Rückfall ab drei Abstürzen in 15 Minuten in LibreELECs Zähler
  `/run/libreelec/crashes.dat`: bei fünf schaltet `kodi.sh` in den abgesicherten Modus (benennt `.kodi` um).
- **remove**: Drop-in und `/storage/.config/glas-core` weg, Mount gelöst (`umount -l`, Kodi läuft noch); nach dem
  Neustart läuft LibreELECs Kodi.

Veröffentlichen: `python tools/publish-repo.py core-le out/12.2.1-Generic/kodi.bin --id <Kennung> --le 12.2.1
--base <kodi.bin aus dem offiziellen LibreELEC 12.2.1>`.

Geprüft nur auf dem PC mit nachgebauter Umgebung (Attrappen für mount/umount/systemctl): Installation mit falscher und
richtiger Prüfsumme, Rückfall nach drei Fehlstarts, gesunder Lauf, Absturzzähler, Versionswechsel, Entfernen.
Auf dem Gerät geprüft (16.11.2026, Testpaket vom Stick, Helper 0.13.0.1 aus dem Arbeitsbaum): `install` mit
Prüfsumme von LibreELECs `kodi.bin` endet mit 0, LibreELEC liest das Drop-in (`systemctl cat kodi` zeigt es), nach dem
Neustart von Kodi liegt der Build über `/usr/lib/kodi/kodi.bin` (Prüfsumme des laufenden Kerns = Build), Zustand
`build`, `base`, `starts` = 1.
Weiter ungeprüft auf einem Gerät: Aktualisieren und Entfernen, Rückfall nach Fehlstarts (BusyBox-`date -d` mit Kodis
Zeitstempeln), `mount --bind /` für die Prüfsumme bei liegendem Build, und ob der Kern einen Neustart des Rechners
übersteht (im Live-Modus liegt `/storage` im Arbeitsspeicher).

## Offen

- Auf einer fest installierten LibreELEC-Fassung prüfen: Neustart des Rechners, Aktualisieren, Entfernen, Rückfall.
- Video-Glas über die EGL-Rendermethode prüfen.
