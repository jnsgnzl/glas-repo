# Eigener Kodi-Build für die Vero V (Live-Glas-Versuch)

Plan und Hintergrund: [docs/plan-kodi-blur.md](../../docs/plan-kodi-blur.md).

Gebaut wird wie bei OSMC: Debian Bullseye (amd64) in Docker. Darin liegt die armv7-Chroot aus dem Paket
`armv7-toolchain-osmc`, die über qemu läuft. Alles Große liegt im Docker-Volume `kodi-build`, nicht im Repo.

## Dateien

| Datei | Zweck |
|---|---|
| `Dockerfile` | Build-Rechner `glas-kodi-build`: Debian 11 (fester Snapshot), qemu, OSMC-Paketquelle |
| `osmc_repository.gpg` | öffentlicher Schlüssel der OSMC-Paketquelle (von der Vero) |
| `prepare-source.sh` | Kodi-Quelltext wie OSMC holen und OSMCs Patches als Git-Commits anwenden → `/build/kodi` (Zweige `vanilla`, `osmc`, `glas`) |
| `osmc-glas.sh` | OSMC-Repo `/build/osmc` auf den Stand der Vero (21.3.0-7, `2b5c995b4`) plus zwei Schalter: `GLAS_JOBS`, `GLAS_KODI_ONLY` |
| `run-build.sh` | Build starten (Log `/build/logs/build.log`) |

## Ablauf

```sh
docker build -t glas-kodi-build tools/kodi-build
docker volume create kodi-build
# Git Bash (PowerShell 5.1 verschluckt die Anführungszeichen):
docker run --rm -v kodi-build:/build -v "$PWD/tools/kodi-build:/tools:ro" glas-kodi-build bash -c 'tr -d "\r" < /tools/prepare-source.sh > /tmp/p.sh && bash /tmp/p.sh'
# Glas-Änderungen im Zweig glas von /build/kodi committen, dann als Patch für OSMC ablegen:
#   git -C /build/kodi format-patch osmc..glas --stdout > /build/osmc/package/mediacenter-osmc/patches/vero5-900-glas.patch
docker run --rm -v kodi-build:/build -v "$PWD/tools/kodi-build:/tools:ro" glas-kodi-build bash -c 'tr -d "\r" < /tools/osmc-glas.sh > /tmp/o.sh && bash /tmp/o.sh'
docker run -d --privileged --name glas-kodi -v kodi-build:/build -v kodi-tc:/opt/osmc-tc -v "$PWD/tools/kodi-build:/tools:ro" glas-kodi-build bash -c 'tr -d "\r" < /tools/run-build.sh > /tmp/r.sh && bash /tmp/r.sh'
```

- `GLAS_JOBS` (Umgebungsvariable, Vorgabe 6): parallele Jobs. Unter Emulation braucht ein C++-Job bis etwa 1 GB RAM.
- `GLAS_FULL=1`: voller OSMC-Build mit binären Add-ons, Sprachen und `.deb`-Paketen.
- Ohne `GLAS_FULL` endet der Build nach `make install` von Kodi. Ergebnis:
  `/build/osmc/package/mediacenter-osmc/files/usr/lib/kodi/kodi.bin`.

## Auf der Vero testen (nur kodi.bin)

```sh
sudo systemctl stop mediacenter
sudo cp -a /usr/lib/kodi/kodi.bin /usr/lib/kodi/kodi.bin.osmc   # einmalig sichern
sudo cp kodi.bin /usr/lib/kodi/kodi.bin
sudo systemctl start mediacenter
# zurück: sudo cp -a /usr/lib/kodi/kodi.bin.osmc /usr/lib/kodi/kodi.bin
```

Erkennbar am Startlog: `Glas: eigener Kodi-Build (skin.glas), Backdrop-Blur-Spike`.

## Schrittweise bauen (nur kodi.bin)

Voraussetzung ist ein vollständiger Lauf von `run-build.sh`. Danach in Git Bash (`MSYS_NO_PATHCONV=1`):

```sh
docker run --rm -v kodi-build:/build -v "$PWD/tools/kodi-build:/tools:ro" glas-kodi-build bash -c 'tr -d "\r" < /tools/apply-glas.sh > /tmp/a.sh && bash /tmp/a.sh'
docker run --rm --privileged -v kodi-build:/build -v kodi-tc:/opt/osmc-tc -v "$PWD/tools/kodi-build:/tools:ro" glas-kodi-build bash -c 'tr -d "\r" < /tools/build-incremental.sh > /tmp/i.sh && bash /tmp/i.sh'
```

`apply-glas.sh` ist wiederholbar: Kerndateien, die es ganz neu patcht (`GUIControl.cpp`, `GUITexture.h/.cpp`,
`ScreenshotAML.cpp`), holt es vorher frisch aus dem Zweig `osmc`; `GUITexture.cpp` bekommt danach das
Mess-Werkzeug Overdraw wieder (`overdraw.sh` mit `GLAS_NO_COMMIT=1`). Ergebnis: `/build/out/kodi.bin`.

## Stereo-3D: Stereo-Texturen und Glas in 3D

Startlog: `Glas: eigener Kodi-Build (skin.glas), Backdrop-Blur-Spike, Video-Glas, Stereo-Texturen`. Der Glas
Helper setzt daraufhin `Window(Home).Property(glas.corestereo)`.

**Stereo-Texturen** (`<texture stereo="sbs">pfad</texture>`, ebenso an `texturefocus`, `texturenofocus`,
`texturebg` usw. – überall, wo Kodi eine Textur liest, `CGUIControlFactory::GetTexture`):

- Die Bilddatei enthält links das Bild fürs linke, rechts das fürs rechte Auge (Side-by-Side, doppelt so breit).
- Kodi zählt nur eine Hälfte: Breite der Bildquelle halbiert (`aspectratio`, `width="auto"`, Ränder `border`).
- Beim Zeichnen nimmt das rechte Auge (`GetStereoView() == RENDER_STEREO_VIEW_RIGHT`) die rechte Hälfte, sonst
  die linke – auch in 2D und bei `MONO`. Ein Vanilla-Kodi kennt das Attribut nicht und zeigt das ganze Doppelbild;
  der Skin nutzt solche Bilder deshalb nur mit `glas.corestereo`.
- `diffuse=` (Maske, z. B. runde Ecken) bleibt über der ganzen Fläche, sie wird nicht halbiert.
- Das Attribut gehört zum Control, nicht zum Bild: ein Control mit `stereo="sbs"` halbiert jedes Bild, das es zeigt.
  Für normale Poster ein eigenes Control nehmen (nicht per `$VAR` zwischen SBS- und normalem Bild wechseln).
- Umsetzung: `CTextureInfo::stereoSbs` (bool hinter `m_infill`, liegt im Füllbyte – Größe und Lage der Struktur
  bleiben), `CGUITexture::AllocResources` (Breite halbieren) und `CGUITexture::Render` (Bildkoordinaten des
  rechten Auges um eine Hälfte verschieben; gilt für alle Renderer, auch GLES).

**Live-Glas in 3D** (`GUIBackdropBlurControl.cpp`): Kodi zeichnet die Oberfläche je Auge einmal, verschiebt jedes
Control um `<depth>` × `lookandfeel.stereostrength` (Vero: ±10 px bei Tiefe 1) über die Kamera.

- Griff und Weichzeichnung je Auge: zweiter Zwischenspeicher für das rechte Auge (wird erst in 3D angelegt),
  Merker „Hintergrund geändert“ und „Nachholen“ je Auge.
- Das Glas-Rechteck geht in 3D durch dieselbe Projektion wie die Texturen (`CRenderSystemGLES::Project`); das Glas
  liegt so in derselben Tiefe wie sein Control.
- Video-Glas in 3D: das Videogerät liefert auch bei 3D-Filmen ein normales Bild (geprüft 17.10.2026). Ob das Glas
  über einem 3D-Film das Videobild zeigt, entscheidet die Skin (`glas_stereo_video`); `/tmp/glas-stereo-video-off`
  schaltet es im Kern ab. Über Video nimmt das rechte Auge das Ergebnis des linken (ein Griff je Bild statt zwei).
- Das eigene Zwischenbild gilt auch in 3D (`/tmp/glas-guitarget-stereo-off` schaltet zurück
  zum Fenster). Gemessen über einem 3D-Film (1080p23.98 FP, Glas mit Videobild): ohne Zwischenbild 12–18
  Bilder/s und bis 177 ms je Bild (Film und Oberfläche stockten), mit Zwischenbild 24–26 Bilder/s.
- In 3D wartet Kodi am Ende jedes Bildes auf die GPU (`glFinish` vor und nach der Kopie des Zwischenbilds; Schalter
  `/tmp/glas-stereo-finish-off`). Gemessen über einem 3D-Film mit Live-Video-Glas: ohne 1–8 Bilder je Sekunde um
  einen Bildwechsel verspätet, mit 0 (Zeichnen ~27 ms + Kopie ~12 ms von 41,7 ms). Video-Glas in 3D läuft live
  (jedes Videobild); `/tmp/glas-stereo-video-ms` drosselt zum Messen.
  In den 3D-Menüs ohne Film bremst das Warten nur (24.10.2026: Mediathek 74 statt 18 %, Homescreen 64 statt 27 %
  verspätete Bilder). Bis der Kern selbst nur mit Video wartet, legt der Glas Helper den Schalter ohne Video an und
  entfernt ihn mit Video (`script.glas.helper/display3d.py`, `Watch`). Achtung: `/tmp/glas-timing` erzwingt
  `glFinish` ebenfalls, Vergleiche mit und ohne Warten nur mit `/tmp/glas-frames` messen.
- Liquid-Shader geteilt: das Innere einer Glasflaeche zeichnet ein schlanker Shader (`LIQUID_INNER_SHADER`, ein
  Texturzugriff), nur die Randstreifen (Breite wie der Lichtbrechungsrand, mindestens der Eckradius) der volle
  `LIQUID_SHADER` - gleiches Bild. Gemessen in 3D an derselben Filmstelle: Untertitelmenue 6-12 -> 0-5, Einstellungs-
  Popover 4-8 -> 0-2 verspaetete Bilder/s. Eine Verzweigung im Shader half nur halb (Mali reserviert Register fuer den
  langen Weg). Schalter `/tmp/glas-liquid-inner-off`.
- Weitere Messschalter (Vorgabe aus bzw. wie Kodi, halfen nicht messbar): `/tmp/glas-partial-on` (nur markierte
  Bereiche neu zeichnen), `/tmp/glas-afterrender-skip` (Kodis Fenster-Markierung bei Dialog-Aenderung weglassen),
  `/tmp/glas-fsmark-skip`, `/tmp/glas-stereo-preframe-window`, `/tmp/glas-capture-ms`.
- Vergleiche immer an derselben Filmstelle messen (z. B. Player.Seek auf 25:00): die Last haengt stark von der Szene ab
  (3D-Decoder und Oberflaeche teilen sich die Speicherbandbreite).
- Zeitmessung `/tmp/glas-timing` (Log `GLAS_TIMING`: Zeichnen und Kopie je Sekunde, dazu Griff/Zusammensetzen je
  Glasfläche).
- 2D und `MONO` laufen unverändert über den bisherigen Weg (ein Zwischenspeicher, ohne Projektion).

## Eigenes Zwischenbild (GUI-Rendertarget)

Solange ein `backdropblur`-Control zu sehen ist (und bis 3 s danach), zeichnet Kodi jedes Bild in ein eigenes
Framebuffer-Objekt und kopiert es am Ende einmal auf den Bildschirm (`GLAS_GUITARGET` in
`GUIBackdropBlurControl.cpp`, eingehängt in `CApplication::Render` durch `apply-glas.sh`). Das Glas liest dann
aus diesem Zwischenbild. Beim Lesen aus dem Fenster-Framebuffer mitten im Bild ließ der Mali-Treiber die CPU bis
zum Bildwechsel warten (17–35 ms). Bei Stereo-3D bleibt alles beim Fenster.

Messschalter auf der Vero (je Sekunde geprüft): `touch /tmp/glas-guitarget-always` (immer Zwischenbild),
`touch /tmp/glas-guitarget-off` (nie). Log: `GLAS_GUITARGET`.

Messung 1080p60 (Glas 15 Hz, `refresh=always`): Seitenleiste 47 → 58,5 fps, Kontrollzentrum 24 → 48 fps;
das Zwischenbild allein kostet nichts Messbares. Im Alltag (Navigieren im Kontrollzentrum) 31 → 46 fps.
Rest: Jeder Griff teilt das Bild; die Kachel-GPU schreibt das bis dahin Gezeichnete zurück und lädt es danach
wieder (Speicherbandbreite, etwa 6 ms).

## Nur bei echten Änderungen neu weichzeichnen

Kodi markiert ein ganzes Fenster als geändert, sobald sich in einem Dialog darüber etwas tut
(`CGUIWindowManager::AfterRender`), ebenso bei `MarkDirty`. Das Glas hielt deshalb jede Bewegung im
Kontrollzentrum und sein eigenes Nachholen für eine Änderung des Hintergrunds. Das ergab eine Schleife mit 60
Bildern und 15 Neuberechnungen je Sekunde bei stillem Bild. `apply-glas.sh` meldet jetzt aus
`CGUIControl::DoProcess` jede eingetragene Änderung; reine Fenstermarkierungen (ohne Animation, Verschiebung
oder Ungültigkeit) ignoriert das Glas. Das Zwischenbild hängt an der Sichtbarkeit des Glases (`Process`), nicht
mehr am Zeichnen.

Ergebnis 1080p60: Kontrollzentrum still 0 statt 60 Bilder je Sekunde; beim Navigieren keine Neuberechnung,
5 % Bilder über 20 ms (ohne Live-Glas 4 %).

Diagnose-Schalter auf der Vero:

| Datei | Log | Inhalt |
|---|---|---|
| `/tmp/glas-diag` | `GLAS_DIAG` | je Sekunde: Controls mit Änderungen samt Grund; Griffe nach Grund; schwarze Bilder und Helligkeitseinbrüche in der Glasmitte (liest Pixel zurück, bremst) |
| `/tmp/glas-frames` | `GLAS_FRAMES` | gezeichnete Bilder in Bewegung, Bilder über 20 ms, längstes Bild |
| `/tmp/glas-overdraw` | `GLAS_OVERDRAW` | gezeichnete Bildschirmflächen je Bild, größte Verursacher |

## Liquid Glass (`<style>liquid</style>`)

Eigenes Shader-Programm neben dem bisherigen (`frosted`, Vorgabe), angelehnt an tvOS 26:

- **Lichtbrechung am Rand:** in einem Randband (bis 22 Skin-Pixel) holt der Shader den Hintergrund von weiter
  außen, teils aus dem schärferen Griff (erste Stufe, 1/4) – wie durch eine dicke Glaskante.
- **Material hell wie tvOS 26:** 9 % Weiss (im Apple-Video gemessen: Glas über dunklem Inhalt heller als der
  Hintergrund); erst über sehr hellem Hintergrund bis 28 % dunkler, damit weisser Text lesbar bleibt.
- **Lichtkante:** dünne helle Linie, oben links am stärksten, gegenüber schwächer, dazu weicher Schein innen.

Mit `liquid` zeichnet der Skin keine eigene Material-/Kantentextur mehr darüber (`GlasMaterial`, neues Include
`GlasLiveGlass` für Dialoge, deren deckende Fläche ohne Live-Glas als Rückfall bleibt).
