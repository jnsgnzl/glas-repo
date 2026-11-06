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
| `apply-glas.sh` | Glas-Änderungen (`src/`, dazu Eingriffe per perl) in den Zweig `glas` übernehmen; legt den Zweig auf frischem Baum an |
| `glas-patches.txt`, `glas-patches.pl` | Eingriffe, die Vero und Windows gleich brauchen, als Anker und Einsatz in einer Datei; `apply-glas.sh` wendet sie mit dem perl-Skript an, `windows/apply-glas.ps1` liest sie selbst |
| `jetson/` | Bauen auf dem Jetson: `Dockerfile` (armhf), `build.sh` (CMake wie OSMC, `make kodi`), `glas-build.sh` (Steuerung) |

## Stand der Umgebung (29.10.2026)

Gebaut wird jetzt auf dem **Jetson** (siehe nächster Abschnitt). Die PC-Umgebung (Docker-Volume `kodi-build`, Image
`glas-kodi-build`) gibt es nicht mehr; der Ablauf darunter bleibt als Weg über den PC. Veröffentlicht ist Kern
2026.10.17 (`tools/publish-repo.py core`, Glas-Repository), gebaut aus den Kernquellen von `100ea13`.

## Bauen auf dem Jetson (nativ, ohne Emulation)

Der Jetson Orin Nano (ARM64, per SSH erreichbar) führt 32-Bit-ARM-Programme
direkt aus. Statt OSMCs amd64-Container mit armv7-Chroot unter qemu läuft der Build in einem armhf-Container mit Debian
Bullseye und OSMCs Paketquelle (`jetson/Dockerfile`, Pakete wie `package/mediacenter-osmc/build.sh` für `vero5`).
Konfiguriert wird mit OSMCs CMake-Optionen für die Vero V (`jetson/build.sh`).

Arbeitsordner `~/glas-build` auf dem Jetson: `osmc/` (OSMC-Repo auf `2b5c995b4`), `kodi/` (Quelltext, Zweige `vanilla`,
`osmc`, `glas`; Build-Baum `kodi/kodi-build`), `tools/` (Kopie dieses Ordners), `out/` (`kodi.bin`, `kodi.bin.debug`),
`logs/` (`build.log`, `make.log`).

```sh
# vom PC aus: Werkzeuge kopieren (Git Bash, im Ordner tools)
tar czf - kodi-build | ssh <benutzer>@<jetson> \
  'cd ~/glas-build && rm -rf tools && mkdir tools && tar xzf - -C tools --strip-components=1'
# auf dem Jetson
bash ~/glas-build/tools/jetson/glas-build.sh image    # einmalig: Image glas-kodi-armhf (1,4 GB)
bash ~/glas-build/tools/jetson/glas-build.sh source   # einmalig: Quelltext + OSMC-Patches
bash ~/glas-build/tools/jetson/glas-build.sh all      # Glas-Änderungen übernehmen und bauen
```

Rücksicht auf das, was sonst auf dem Jetson läuft: Der Container bekommt höchstens 3 Kerne und 3,5 GB Speicher, eine
niedrige CPU-Gewichtung, der Compiler läuft mit `nice 19`. Aufräumen: `docker rmi glas-kodi-armhf` und
`rm -rf ~/glas-build` (Dateien gehören root: über einen Container löschen).

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

## Untertitel-Streifen (Bild-Untertitel im Scope-Modus)

Startlog: `…, Glas-Knoepfe, Untertitel-Streifen`. Bild-Untertitel (PGS, DVD, DVB), die aus dem sichtbaren Streifen einer
21:9-Leinwand ragen, rücken vor dem Zeichnen hinein (`src/xbmc/cores/VideoPlayer/VideoRenderers/GlasSubtitleStrip.h`,
drei Zeilen in `OverlayRenderer.cpp`). Den Streifen meldet der Glas Helper als `Window(Home).Property(glas.substrip)` =
„oben,unten“ in Zeilen von 1080 (leer = aus). Nicht in Disc-Menüs, nicht bei Bildern über 40 % der Höhe; Text und ASS
bleiben unberührt (die rückt der Helper über `subtitles.marginvertical`). Hintergrund und Messungen:
[docs/plan-scope.md](../../docs/plan-scope.md), „Bild-Untertitel“.

## Hauptfilm (Blu-ray-Abbild direkt starten)

Startlog: `…, Untertitel-Streifen, Hauptfilm`. Kodi zeigt vor einer ISO die Titelliste (`disc.playback` 0) bzw. nimmt
die längste Playlist (2, `GetTitleLongest`); bei vielen Discs ist das nicht der Film. Steht in der NFO des Films
`<mainplaylist>BDMV/PLAYLIST/00800.mpls</mainplaylist>` (im `<movie>`; schreibt z. B. Kodex aus seiner Disc-Analyse,
geht auch von Hand; Kodi selbst übergeht den Tag), wählt der Kern (`src/xbmc/dialogs/GlasMainPlaylist.h`, drei Zeilen
am Anfang von `CGUIDialogSimpleMenu::ShowPlaySelection`) diese Playlist so, wie es die Auswahl in der Titelliste täte
(`SetDynPath` auf `bluray://…/BDMV/PLAYLIST/xxxxx.mpls`, `original_listitem_url`): Mediathek, Fortsetzen und Gesehen
bleiben am Eintrag. Die NFO liest er beim Start der ISO selbst (wie Kodi: `<Name>.nfo`, sonst `movie.nfo`); kein Glas
Helper, kein Kodex nötig. Ohne Tag, bei „Disc-Menü anzeigen“ (1) und wenn die Playlist auf dem Abbild fehlt: wie bei
Kodi eingestellt. Fortsetzen läuft wie bisher über den gespeicherten Zustand (Kodi ruft die Auswahl dabei nicht auf).

Befund 04.11.2026 (Kodex, alle 298 ISOs der Vero): bei 127 ist die längste Playlist nicht der Film, meist Schleifen
oder „Alles abspielen“ der Extras (Deadpool 310 min statt 108, Pitch Black 1576 min); nur Terminator 2 ist als
Attrappen-Disc markiert. Auf der Vero geprüft (erster Stand mit einer Liste des Helpers statt NFO, gleicher Weg in
Kodi): Deadpool startet aus der Mediathek ohne Titelliste mit `00800.mpls` (1:48:06 wie die Laufzeit), Haltepunkt
landet am Mediathek-Eintrag, Fortsetzen läuft im selben Titel weiter.
Achtung, Kodi selbst: wird ein Film gestartet, während die 3D-Frage (`CStereoscopicsManager::OnStreamChange`) eines
anderen noch offen ist, verklemmen sich Player und Oberfläche (beide warten aufeinander, `CloseFile`); nicht durch Glas.

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

## Federkurven (`spring`) und Rückfall-Effekte (`glasfallback`)

tvOS bewegt fast alles mit Federn. Kodi kennt nur feste Kurven, `back` schwingt immer um 10 % des Wegs über.
`src/xbmc/guilib/GlasTween.h` ergänzt eine Feder, `VisibleEffect.cpp` liest sie in `CAnimEffect::GetTweener`. Das gilt
für Animationen und für `scrolltime`:

```xml
<effect type="zoom" start="82" end="100" time="220" tween="cubic" easing="out" spring="0.32" />
<scrolltime tween="cubic" easing="out" spring="0">300</scrolltime>
```

- `spring` ist `bounce` wie bei Apple: 0 ohne Überschwingen, 0,3 etwa 5 %, höchstens 0,9. Die Feder kommt genau am
  Ende von `time` zur Ruhe (Spitze bei 0,3 nach ≈ 45 % der Zeit). `easing` wie bei Kodi.
- Kodi ohne diesen Build kennt `spring` nicht und nimmt `tween`. Deshalb nie `tween` weglassen.
- `glasfallback="true"` an einem `<effect>`: Der Glas-Kern lässt den Effekt aus, jedes andere Kodi spielt ihn. So bleibt
  ohne Kern eine Nachahmung (Dock-Kachel: zwei Zoom-Effekte für das Überschwingen, `GlasTileLift`).
- Startlog: `…, Hauptfilm, Federkurven, …`. Unter Windows geprüft (05.11.2026, Dock-Kachel mit übertriebenen Werten:
  gedämpfte Schwingung 152 → 340 → 297 → 309 → 306 px). Vero: Patch in `apply-glas.sh`, an Kodis `VisibleEffect.cpp`
  getestet, noch nicht gebaut.

## Wachsen um feste Pixel (Effekt `grow`)

Kodis `zoom` skaliert ein ganzes Control samt Schrift und wächst prozentual. tvOS vergrößert Kapseln und Zeilen um
feste 4 px je Seite, die Schrift bleibt gleich groß. `src/xbmc/guilib/GlasGrow.h` und die Blöcke in
`glas-patches.txt` ergänzen dafür einen Effekt ohne Transformation:

```xml
<animation type="Focus">
  <effect type="grow" start="0" end="4" time="100" tween="cubic" easing="out" />
  <effect type="zoom" start="100" end="104" center="auto" time="100" glasfallback="true" />
</animation>
```

- `start`/`end` in Pixeln je Seite, `"x,y"` für getrennte Achsen. Wirkt an `image` und an Knöpfen samt Ableitungen,
  bei Drehfeld und Regler über den inneren Knopf. Gewachsen werden die Texturen sowie `<glass>` und `<focusshadow>` des
  Knopfs. Die Beschriftung rückt mit der Kante, bei `width auto` bleibt sie in der Mitte.
- Ein gewachsener Knopf zeichnet über den innersten Ausschnitt hinaus, aber nur um das Wachstum. Sonst schnitte die
  Grouplist die Ränder der Einstellungszeilen ab, weil Kodi jede Zeile so breit macht wie sie.
- Ohne neue Felder in Kodis Kopfdateien: `CGUIControl::Animate` sammelt die Werte, Bild und Knopf holen sie in `Process`.
- Kodi ohne diesen Build kennt `grow` nicht und spielt nur den `glasfallback`-Zoom. Im Skin: Include `GlasFocusGrowPx`.
- Unter Windows gemessen (05.11.2026): Kategorie 400 × 72 → 408 × 80, Einstellungszeile 1280 → 1288 breit,
  Suchkapsel mit `width auto` 66 → 74 hoch.

## Listenhöhe nach den Einträgen (`<fititems>`)

`<fititems>true</fititems>` an einer Liste: so hoch, waagrecht so breit, wie ihre sichtbaren Einträge, höchstens
`height` bzw. `width` aus dem Skin. Die Grouplist darüber rückt im selben Bild nach (Seitenleiste der Mediathek).
Kodi ohne diesen Build lässt die Liste in ihrer festen Größe. Unter Windows geprüft: Orte-Liste mit `height` 608 und
einem sichtbaren Eintrag, die Überschrift darunter steht wie bei 76.

## Glas über Glas (`GlasLayer.h`)

Alle Glasflächen eines Fensters teilten sich einen Griff. Liegt Glas auf Glas (Kreis „Hinzufügen“ auf der Glaskarte
des Kontrollzentrums), nahm das obere je Bild mal den Griff ohne die Karte und mal den mit ihr. Es flackerte hell und
dunkel. Jetzt bekommt jede Fläche eine Ebene: 1 + die höchste Ebene der Flächen, die im selben Bild schon gezeichnet
waren und zu mindestens 25 % darunter liegen. Jede Ebene hat einen eigenen Griff. Über Video bleibt alles auf Ebene 0.
Beide Fassungen (DX11, GLES) sind geändert. Startlog `…, Glas-Ebenen, …`, der Glas Helper setzt `glas.corelayers`
(Skin: `$EXP[glas_corelayers]`). Unter Windows: 24 Bildschirmbilder hintereinander gleich.

## Kleinigkeiten

- **„…“ beim Kürzen** (`GUIFontTTF.cpp`): eine Glyphe U+2026 statt drei Punkten, wenn die Schrift sie hat. In Inter
  sehen beide gleich aus (gleich breit), in anderen Schriften ist „…“ enger.
- **Textbox ohne Rollen** (kein `autoscroll`, keine `pagecontrol`): Die letzte ganz sichtbare Zeile endet mit „…“, wenn
  noch Text folgt. Gebaut, mangels passender Textbox im Skin noch nicht im Bild gesehen.
- **`<textcolor2>`, `<focusedcolor2>`** an Knöpfen (`GUIGlasButton.cpp`): Farben des Werts rechts, also `label2`, bei
  Drehfeld und Regler deren Wert. Kodi färbt ihn sonst wie den Text. Einstellungszeilen in Hell: Werte 105, auch in
  der Fokuszeile.

## Kontextmenü neben der Kachel (`GlasFocusItem.h`)

Kodi setzt das Kontextmenü mittig auf das fokussierte Control, bei einer Liste also auf die ganze Liste. Der Skin hat
das bisher mit Tabellen je Spalte und Reihe ausgeglichen. Das ging nur dort, wo `Container.Position` im Dialog etwas
liefert, also nicht in der App TV mit ihren Reihen.

- `CGUIBaseContainer::ProcessItem` merkt sich die Zelle des fokussierten Eintrags in Bildschirm-Pixeln, so wie sie
  gezeichnet wird: mit Grouplist-Rollen und Animationen der Eltern.
- `CGUIDialogContextMenu::PositionAtCurrentFocus` setzt das Menü daneben, wenn **Control 9900** im Kontextmenü sichtbar
  ist (Bedingung im Skin): rechts, solange die Zelle links der Bildmitte liegt, sonst links, oben bündig. Zellen breiter
  als die halbe Breite (Titelbild) bleiben bei Kodis Mitte.
- Fenster-Eigenschaften des Menüs: `glas.ctx.side` (`right`/`left`), `glas.ctx.w`, `glas.ctx.h`. Der Skin legt je
  Ansicht nur noch eine feste Feinlage darauf (`tools/dev/gen_views.py`, `ctx_core`).
- Startlog `…, Kontextmenue-Lage, …`, Glas Helper `glas.corectx`, Skin `$EXP[glas_corectx]`.

## Ja/Nein-Rückfrage: Rolle

`CGUIDialogYesNo::OnInitWindow` setzt vor den `onload`-Aktionen `glas.yesno = true` und `glas.destructive = true`,
wenn Titel, Text oder Ja-Knopf „Löschen“ bzw. „Entfernen“ (117/15015 in der Sprache der Oberfläche) enthalten. Der Skin
färbt den Ja-Knopf dann rot und setzt sonst den Fokus auf die Hauptaktion (Ja), wie tvOS. Kodi wählt sonst immer Nein.

## Richtung des Fensterwechsels

`CGUIWindowManager` setzt `Window(Home).Property(glas.nav)` auf `back` (PreviousWindow, auch wenn es intern
`ActivateWindow` aufruft) oder `forward`, bevor Fenster geschlossen und geöffnet werden. `Glas_OpenClose`: vorwärts
250 ms, zurück 700 ms `quadratic out` (tvOS: 50 % nach ≈ 200 ms, lang auslaufend). Ohne Kern bleibt der Kompromiss 300 ms.

## Überblenden mit Kurve (`GlasFade.h`)

`<fadetime tween="sine" easing="inout">400</fadetime>`: Bildwechsel folgen derselben Kurve wie Animationen, `spring`
geht auch. Kodi ohne Kern blendet linear. Gilt je Bild-Control aus dem Skin, Kopien in Listen-Layouts blenden linear.

## Vorschaubild beim Spulen (`glasseek://`)

Bildadresse `glasseek://<Datei>/<Zeit>` (Zeit wie `Player.SeekTime` oder Sekunden). Sie läuft über denselben Weg wie
Kodis Kapitelbilder (`chapter://`): `TextureCacheJob` erkennt die Adresse, `VideoChapterImageFileLoader` holt das Bild
mit `GlasSeekThumb` (`DVDFileInfo.cpp`) an der Zeit. `DialogSeekBar.xml` zeigt es beim Spulen (`Player.Seeking`)
400 × 225 über der Leiste, mittig auf der Spulstelle. Die Bilder landen wie alle Bilder im Bild-Cache
(`userdata/Thumbnails`). Startlog `…, Spulvorschau, …`, `glas.coreseek`.

- **Film offen halten (05.11.2026):** `GlasSeekThumb` hält Datei, Demuxer und Software-Decoder des gespulten Films offen
  und springt nur noch (Decoder zurücksetzen, erstes ganzes Bild). Eine Anfrage, hinter der schon eine neuere wartet,
  entfällt. Geschlossen wird bei `Player/OnStop` (die Ankündigung wartet nie auf ein laufendes Bild, `try_lock`), bei
  einem anderen Film, nach einem Fehler und vor dem Abbau der Netzfreigaben (`CApplication::CloseNetworkShares` ruft
  `GlasSeekClose`, sonst griffe der Film nach dem Ruhezustand auf eine abgebaute NFS-Verbindung zu). Geöffnet wird nur,
  solange etwas spielt. Braucht ein Bild über 1,5 s, gibt es für den Film bis zum Stopp keine mehr. Gemessen: Vero
  1080p 80–360 ms je Bild (vorher ≈ 1,1 s), PC 55–140 ms. Log `Glas-Spulvorschau: …` (Öffnen und Bilder über 400 ms
  als Info, sonst Debug).
- **Nur das Schlüsselbild (05.11.2026, abends):** dekodiert wird direkt mit libavcodec statt mit Kodis Decoder: ein
  Paket am Sprungziel, Decoder leeren (Drain), ohne Deblocking, Slice-Threads, 10 Bit direkt beim Verkleinern nach
  BGRA. Kodis Decoder gab das Bild erst nach vollem Umsortier-Puffer heraus (4K-HEVC: 3–5 Bilder zu je ≈ 1 s). Klappt
  der erste Durchgang nicht (Blu-ray: nach dem Sprung nicht immer ein Schlüsselbild), sucht ein zweiter das nächste
  I-Bild (`AVDISCARD_NONINTRA`, `SHOW_ALL`, nur Bilder vom Typ I). Der Decoder lebt nur für ein Bild, auch 4K bleibt
  offen. Gleiches Schlüsselbild wie zuletzt → Kopie; war das letzte Bild langsam (über 0,5 s), gilt es für Anfragen
  bis 3 s daneben weiter (der Film läuft beim Spulen weiter, der Skin fragt jede Sekunde nach). Gemessen (Vero):
  4K-HEVC Dolby Vision 0,15–1,2 s je Bild (vorher 5–6 s, aus), Blu-ray-ISO ≈ 1,0–1,2 s (vorher ≈ 5 s, aus; erstes
  Bild mit Öffnen der Titelliste ≈ 4 s), PC 1080p 24–81 ms.
- **Bild-Cache:** Kodi legt jedes `glasseek://`-Bild dauerhaft ab. Der Glas Helper entfernt sie nach jedem Stopp und
  beim Start (`seekthumbs.py`, `Textures.RemoveTexture`).
- **Blu-ray-ISOs:** `CanExtract` lässt eine feste Titelliste (`bluray://…/xxxxx.mpls`) zu, Kodis Blu-ray-Eingang bekommt
  beim Bildziehen einen Platzhalter-Player; bei einer laufenden ISO nimmt die Vorschau die gespielte Titelliste.
- **Erst Auswählen springt** (`SeekHandler.cpp`, mit `skin.glas`): Video-Schritte (Links/Rechts) halten das Ziel, statt
  nach `videoplayer.seekdelay` selbst zu springen. `CApplication::OnAction` fragt zuerst
  `CSeekHandler::GlasConfirmAction`: Auswählen, OSD, Play, Pause springen (aus der Pause spielt der Film weiter), Zurück
  und Vollbild-Umschalten verwerfen, Stopp verwirft und stoppt. Stopp ohne Taste oder ein anderer Film verwerfen in
  `FrameMove`. Musik, analoges Spulen und andere Skins bleiben wie bei Kodi.
