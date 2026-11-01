#!/bin/bash
# Laeuft im Container: Glas-Aenderungen (tools/kodi-build/src, unter /tools/src eingehaengt) in /build/kodi, Zweig glas,
# uebernehmen, Control "backdropblur" bei Kodi anmelden, committen; Patch nach /build/glas.patch.
set -e
cd /build/kodi
# frischer Quellbaum (prepare-source.sh): Zweig glas auf dem OSMC-Stand anlegen
git rev-parse -q --verify glas >/dev/null || git branch glas osmc
git checkout -q glas
# Zeitstempel aller Dateien merken: Dateien, die dieses Skript frisch aus osmc holt und neu patcht, bekommen ihn am
# Ende zurueck, wenn ihr Inhalt gleich geblieben ist - sonst uebersetzt make jedes Mal alles neu, was sie einbindet
# (GUIWindowManager.h: Hunderte Dateien)
git ls-files -z xbmc | xargs -0 stat -c '%Y %n' > /tmp/glas-mtimes 2>/dev/null || true
for f in $(cd /tools/src && find . -type f); do
    mkdir -p "$(dirname "$f")"
    if ! tr -d '\r' < "/tools/src/$f" | cmp -s - "$f"; then
        tr -d '\r' < "/tools/src/$f" > "$f"
    fi
done

G=xbmc/guilib
# Dateien, die fruehere Versuche gepatcht haben und die heute unveraendert bleiben: auf den OSMC-Stand zuruecksetzen
# (Vorab-Durchgang, 29.10.2026 wieder ausgebaut; docs/plan-leistung.md 4b)
git checkout -q osmc -- xbmc/rendering/gles/RenderSystemGLES.cpp $G/GUIWindowManager.h
grep -q GUICONTROL_BACKDROPBLUR $G/GUIControl.h || \
    perl -0pi -e 's/(    GUICONTROL_VISUALISATION,\n)/$1    GUICONTROL_BACKDROPBLUR,\n/' $G/GUIControl.h

F=$G/GUIControlFactory.cpp
grep -q GUIBackdropBlurControl.h $F || perl -0pi -e 's/(#include "GUIImage.h"\n)/#include "GUIBackdropBlurControl.h"\n$1/' $F
grep -q '"backdropblur"' $F || perl -0pi -e 's/(static const ControlMapping controls\[\] = \{\n)/$1    {"backdropblur", CGUIControl::GUICONTROL_BACKDROPBLUR},\n/' $F
grep -q 'case CGUIControl::GUICONTROL_BACKDROPBLUR' $F || perl -0pi -e 's/(    case CGUIControl::GUICONTROL_VISUALISATION:\n)/    case CGUIControl::GUICONTROL_BACKDROPBLUR:
    {
      CGUIBackdropBlurControl* blur = new CGUIBackdropBlurControl(parentID, id, posX, posY, width, height);
      float value;
      if (XMLUtils::GetFloat(pControlNode, "blurradius", value))
        blur->SetBlurRadius(value);
      if (XMLUtils::GetFloat(pControlNode, "cornerradius", value))
        blur->SetCornerRadius(value);
      if (XMLUtils::GetFloat(pControlNode, "saturation", value))
        blur->SetSaturation(value);
      GUIINFO::CGUIInfoColor tint;
      if (GetInfoColor(pControlNode, "tintcolor", tint, parentID))
        blur->SetTint(tint);
      std::string refresh;
      if (XMLUtils::GetString(pControlNode, "refresh", refresh))
        blur->SetAlwaysRefresh(refresh == "always");
      bool timing = false;
      if (XMLUtils::GetBoolean(pControlNode, "timing", timing))
        blur->SetTiming(timing);
      int grabScale = 4;
      if (XMLUtils::GetInt(pControlNode, "grabscale", grabScale))
        blur->SetGrabScale(grabScale);
      control = blur;
      break;
    }
$1/' $F
# Einlesen spaeterer Tags nachruesten (nach dem Anlegen des Falls oben, damit es auch auf frischem Baum greift)
grep -q '"grabscale"' $F || perl -0pi -e 's/(        blur->SetTiming\(timing\);
)/$1      int grabScale = 4;
      if (XMLUtils::GetInt(pControlNode, "grabscale", grabScale))
        blur->SetGrabScale(grabScale);
/' $F
# updaterate / grabprobe nachruesten
grep -q '"updaterate"' $F || perl -0pi -e 's/(        blur->SetGrabScale\(grabScale\);\n)/$1      float updateRate = 15.0f;\n      if (XMLUtils::GetFloat(pControlNode, "updaterate", updateRate))\n        blur->SetUpdateRate(updateRate);\n      bool grabProbe = false;\n      if (XMLUtils::GetBoolean(pControlNode, "grabprobe", grabProbe))\n        blur->SetGrabProbe(grabProbe);\n/' $F
# style (frosted|liquid) nachruesten
grep -q '"style"' $F || perl -0pi -e 's/(        blur->SetGrabProbe\(grabProbe\);\n)/$1      std::string glasStyle;\n      if (XMLUtils::GetString(pControlNode, "style", glasStyle))\n        blur->SetLiquid(glasStyle == "liquid");\n/' $F
# source (Control hinter dem Glas, am Anfang des Bildes weichgezeichnet) nachruesten
grep -q 'blur->SetSource' $F || perl -0pi -e 's/(        blur->SetLiquid\(glasStyle == "liquid"\);\n)/$1      int glasSource = 0;\n      if (XMLUtils::GetInt(pControlNode, "source", glasSource))\n        blur->SetSource(glasSource);\n/' $F
for tag in grabscale updaterate grabprobe style; do grep -q "\"$tag\"" $F || { echo "Fabrik: $tag fehlt"; exit 1; }; done
grep -q 'blur->SetSource' $F || { echo "Fabrik: source fehlt"; exit 1; }

# Schrift in Zielgroesse bei verkleinertem Text (src/xbmc/guilib/GUIFont.cpp, GlasScaledFont): die Schrift merkt sich
# Datei, Seitenverhaeltnis und Rand, damit CGUIFont eine kleinere Fassung laden kann
T=$G/GUIFontTTF
grep -q m_glasPath $T.h || perl -0pi -e 's/(  float m_height\{0\.0f\};\n)/$1  \/\/ Glas: Datei, Seitenverhaeltnis und Rand fuer verkleinerte Fassungen (CGUIFont::GlasScaledFont)\n  std::string m_glasPath;\n  float m_glasAspect\{1.0f\};\n  bool m_glasBorder\{false\};\n/' $T.h
grep -q m_glasPath $T.cpp || perl -0pi -e 's/(  m_face = g_freeTypeLibrary\.GetFont\(strFilename, height, aspect, m_fontFileInMemory\);\n)/  m_glasPath = strFilename;\n  m_glasAspect = aspect;\n  m_glasBorder = border;\n$1/' $T.cpp
grep -q m_glasPath $T.h && grep -q m_glasPath $T.cpp || { echo "GUIFontTTF-Patch fehlgeschlagen"; exit 1; }

C=$G/CMakeLists.txt
grep -q GUIBackdropBlurControl $C || perl -0pi -e 's/^set\(SOURCES DDSImage.cpp\n/set(SOURCES DDSImage.cpp\n            GUIBackdropBlurControl.cpp\n/m; s/^set\(HEADERS DDSImage.h\n/set(HEADERS DDSImage.h\n            GUIBackdropBlurControl.h\n/m' $C

# Aenderungen melden: pauschale Fenstermarkierungen erkennen (Glas ignoriert sie); Diagnose /tmp/glas-diag
GC=$G/GUIControl.cpp
git checkout -q osmc -- $GC
perl -0pi -e 's/(void CGUIControl::DoProcess\(unsigned int currentTime, CDirtyRegionList &dirtyregions\)\n\{\n  CRect dirtyRegion = m_renderRegion;\n)/namespace GLAS_GUITARGET\n{\nbool Diag();\nvoid NoteChange(const CGUIControl* control, const CRect& region, int why, unsigned int currentTime);\n}\n\n$1  int glasWhy = ((m_controlDirtyState & DIRTY_STATE_CONTROL) ? 1 : 0) | ((m_bInvalidated && IsVisible()) ? 2 : 0);\n/; s/(  if \(Animate\(currentTime\)\)\n)(    MarkDirtyRegion\(\);\n)/$1  {\n$2    glasWhy |= 4;\n  }\n/; s/(      dirtyRegion.Union\(m_renderRegion\);\n)/$1      glasWhy |= 8;\n/; s/(  changed \|= \(m_controlDirtyState & DIRTY_STATE_CONTROL\) != 0;\n)/  if (m_controlDirtyState & DIRTY_STATE_CONTROL)\n    glasWhy |= 16;\n$1/; s/(  if \(changed\)\n  \{\n)(    dirtyregions.emplace_back\(dirtyRegion\);\n)/$1    GLAS_GUITARGET::NoteChange(this, dirtyRegion, glasWhy, currentTime);\n$2/' $GC
grep -c "glasWhy" $GC
# Startlog: eigener Build (auf frischem Baum anlegen; der Glas Helper erkennt den Kern an dieser Zeile)
grep -q "Glas: eigener Kodi-Build" xbmc/application/Application.cpp || perl -0pi -e 's/(  CLog::Log\(LOGINFO, "Starting \{\} \(\{\}\)\. Platform)/  CLog::Log(LOGINFO, "Glas: eigener Kodi-Build (skin.glas), Backdrop-Blur-Spike");\n$1/' xbmc/application/Application.cpp
# Startlog: Faehigkeit Video-Glas melden
grep -q "Backdrop-Blur-Spike, Video-Glas" xbmc/application/Application.cpp || perl -pi -e 's/Backdrop-Blur-Spike"\);/Backdrop-Blur-Spike, Video-Glas");/' xbmc/application/Application.cpp
# Startlog: Faehigkeit Stereo-Texturen melden (Glas Helper setzt daraufhin Window(Home).Property(glas.corestereo))
grep -q "Video-Glas, Stereo-Texturen" xbmc/application/Application.cpp || perl -pi -e 's/, Video-Glas"\);/, Video-Glas, Stereo-Texturen");/' xbmc/application/Application.cpp
grep -q "Video-Glas, Stereo-Texturen" xbmc/application/Application.cpp || { echo "Startlog-Markierung fehlt"; exit 1; }

# Stereo-Texturen: <texture stereo="sbs">. Bilddatei links fuers linke, rechts fuers rechte Auge; Kodi zeigt je Auge
# die passende Haelfte (2D/MONO: links), Seitenverhaeltnis wie eine Haelfte, diffuse ueber der ganzen Flaeche.
# CTextureInfo bekommt ein bool hinter m_infill (liegt im Fuellbyte vor orientation: Groesse und Lage bleiben).
GT=$G/GUITexture
git checkout -q osmc -- $GT.h $GT.cpp
perl -0pi -e 's/(  int        orientation;     \/\/ orientation)/  bool stereoSbs{false}; \/\/ Glas: stereo="sbs" - links Bild fuers linke, rechts fuers rechte Auge\n$1/' $GT.h
perl -0pi -e 's/(  m_frameHeight = \(float\)m_texture.m_height;\n)/$1\n  \/\/ Glas: Stereo-Textur (stereo="sbs"): je Auge eine Haelfte, Groesse und Seitenverhaeltnis wie eine Haelfte\n  if (m_info.stereoSbs)\n    m_frameWidth *= 0.5f;\n/; s/(  OrientateTexture\(texture, u3, v3, orientation\);\n)/$1\n  \/\/ Glas: Stereo-Textur - rechtes Auge liest die rechte Haelfte, sonst (auch 2D\/MONO) die linke. Nur die\n  \/\/ Bildkoordinaten verschieben: die Maske (diffuse) bleibt ueber der ganzen Flaeche\n  if (m_info.stereoSbs \&\&\n      CServiceBroker::GetWinSystem()->GetGfxContext().GetStereoView() == RENDER_STEREO_VIEW_RIGHT)\n  {\n    texture.x1 += u3;\n    texture.x2 += u3;\n  }\n/' $GT.cpp
[ "$(grep -c stereoSbs $GT.h)" = 1 ] && [ "$(grep -c stereoSbs $GT.cpp)" = 2 ] || { echo "Stereo-Patch GUITexture fehlgeschlagen"; exit 1; }
grep -q 'Attribute("stereo")' $F || perl -0pi -e 's/(    image.useLarge = true;\n)/$1  \/\/ Glas: stereo="sbs" - Side-by-Side-Bild, je Auge eine Haelfte (2D\/MONO: linke)\n  const char* stereo = pNode->Attribute("stereo");\n  image.stereoSbs = stereo \&\& StringUtils::EqualsNoCase(stereo, "sbs");\n/' $F
grep -q 'Attribute("stereo")' $F || { echo "Stereo-Patch GUIControlFactory fehlgeschlagen"; exit 1; }
# GUITexture.cpp kam frisch aus osmc: Mess-Werkzeug Overdraw wieder einbauen (ohne eigenen Commit)
tr -d '\r' < /tools/overdraw.sh > /tmp/overdraw.sh
GLAS_NO_COMMIT=1 bash /tmp/overdraw.sh
# Bildschirmfoto (Amlogic): Videogeraet nicht gleichzeitig mit dem Video-Glas lesen
SA=xbmc/utils/ScreenshotAML.cpp
git checkout -q osmc -- $SA
# und denselben dauerhaft offenen Zugriff benutzen: der Treiber gibt beim Schliessen seine 32 MB nicht frei
perl -0pi -e 's/(#include <sys\/ioctl.h>\n)/$1#include <mutex>\n\nnamespace GLAS_VIDEODEVICE\n{\nstd::mutex& Mutex();\nint Fd();\n}\n/; s/  int captureFd = open\(CAPTURE_DEVICEPATH, O_RDWR, 0\);\n/  std::unique_lock<std::mutex> glasDevice(GLAS_VIDEODEVICE::Mutex());\n  int captureFd = GLAS_VIDEODEVICE::Fd();\n/; s/\n    close\(captureFd\);\n/\n/' $SA
grep -q "GLAS_VIDEODEVICE::Fd()" $SA && ! grep -q "close(captureFd)" $SA || { echo "ScreenshotAML-Patch fehlgeschlagen"; exit 1; }
# Nur geaenderte Bereiche zeichnen, solange das eigene Zwischenbild aktiv ist (es behaelt den Rest des Bildes):
# Kodis Vorgabe (algorithmdirtyregions 3) zeichnet bei jeder Aenderung den ganzen Bildschirm - ueber einem Film mit
# Glas also jedes Bild komplett, in 3D fuer beide Augen. Ohne Zwischenbild bleibt alles wie bei Kodi.
WM=$G/GUIWindowManager.cpp
git checkout -q osmc -- $WM
perl -0pi -e 's/(bool CGUIWindowManager::Render\(\)\n\{)/namespace GLAS_GUITARGET\n{\nbool Active();\nbool Regions(CDirtyRegionList& regions);\n}\n\n$1/; s/(  CDirtyRegionList dirtyRegions = m_tracker.GetDirtyRegions\(\);\n)/$1  \/\/ Glas: im eigenen Zwischenbild nur die markierten Bereiche (vereinigt) neu zeichnen\n  CDirtyRegionList glasRegions = m_tracker.GetMarkedRegions();\n  const bool glasPartial = GLAS_GUITARGET::Active() \&\& GLAS_GUITARGET::Regions(glasRegions);\n/; s/(  else if \(CServiceBroker::GetSettingsComponent\(\)->GetAdvancedSettings\(\)->m_guiAlgorithmDirtyRegions == DIRTYREGION_SOLVER_FILL_VIEWPORT_ON_CHANGE)\)\n/$1 \&\& !glasPartial)\n/; s/(  else\n  \{\n    for \(const auto& i : )dirtyRegions(\))/$1(glasPartial ? glasRegions : dirtyRegions)$2/' $WM
grep -qF "glasPartial ? glasRegions : dirtyRegions" $WM && grep -qF "FILL_VIEWPORT_ON_CHANGE && !glasPartial)" $WM || { echo "GUIWindowManager-Patch fehlgeschlagen"; exit 1; }
# AfterRender: aendert sich etwas in einem Dialog, markiert Kodi das ganze Fenster darunter (Vorsicht wegen
# Sichtbarkeitsbedingungen). Ueber einem Film mit Glas also staendig ganzer Bildschirm neu (in 3D beide Augen).
# Solange das Zwischenbild in Gebrauch ist, genuegen die eigenen Markierungen der Controls (Sichtbarkeitswechsel
# markieren sich selbst). Vorgabe wie Kodi; Messschalter /tmp/glas-afterrender-skip laesst die Markierung weg (half nicht messbar).
perl -0pi -e 's/(void CGUIWindowManager::AfterRender\(\)\n\{)/namespace GLAS_GUITARGET\n{\nbool KeepDialogMark();\n}\n\n$1/; s/(      if \(pWindow && window->IsControlDirty\(\))\)\n        pWindow->MarkDirtyRegion\(\);\n/$1 \&\& GLAS_GUITARGET::KeepDialogMark())\n        pWindow->MarkDirtyRegion();\n/' $WM
grep -qF "IsControlDirty() && GLAS_GUITARGET::KeepDialogMark())" $WM || { echo "AfterRender-Patch fehlgeschlagen"; exit 1; }
# Vollbild-Video: Kodi erklaert das ganze Fenster fuer geaendert, sobald ein Overlay (Untertitel, Blu-ray-Menuegrafik)
# im Spiel ist - bei Blu-ray-ISOs mehrmals je Sekunde, dann zeichnet Kodi alles neu (in 3D fuer beide Augen).
# Messschalter /tmp/glas-fsmark-skip laesst diese Markierung aus (nur zum Messen, Untertitel wuerden nicht nachgezogen)
FS=xbmc/video/windows/GUIWindowFullScreen.cpp
git checkout -q osmc -- $FS
perl -0pi -e 's/(void CGUIWindowFullScreen::Process\(unsigned int currentTime, CDirtyRegionList &dirtyregion\)\n\{)/namespace GLAS_GUITARGET\n{\nbool SkipFullscreenMark();\n}\n\n$1/; s/(  if \(appPlayer->IsRenderingGuiLayer\(\))\)\n    MarkDirtyRegion\(\);\n/$1 \&\& !GLAS_GUITARGET::SkipFullscreenMark())\n    MarkDirtyRegion();\n/' $FS
grep -qF "IsRenderingGuiLayer() && !GLAS_GUITARGET::SkipFullscreenMark())" $FS || { echo "GUIWindowFullScreen-Patch fehlgeschlagen"; exit 1; }
# 4K-Ausgabe: Oberflaeche kleiner zeichnen, der Video-Prozessor skaliert hoch (docs/plan-leistung.md, Schritt B).
# OSMC setzt fuer die Vero V die Oberflaeche auf die volle Ausgabe (3840 x 2160, zu viel fuer die Mali-G31), aeltere Veros
# auf 1080 mit Hochskalieren (aml_handle_scale). Glas beachtet Kodis Einstellung videoscreen.limitguisize wie GBM
# (1 = 720, 2 = 1080 bzw. 720 ueber 30 Hz, 3 = 1080, 4 = unbegrenzt bzw. 1080 ueber 30 Hz; 0 = unbegrenzt wie bisher)
# und macht sie unter AML sichtbar (System > Anzeige). Wirkt beim naechsten Einlesen der Aufloesungen (Kodi-Start).
AU=xbmc/utils/AMLUtils.cpp
git checkout -q osmc -- $AU
perl -0pi -e 's/(#include "OSMCSecureOS.h"\n)/$1#include "ServiceBroker.h"\n#include "settings\/Settings.h"\n#include "settings\/SettingsComponent.h"\n/; s/(        res->iWidth = \(width > 4096\) \? 1920 : width;\n        res->iHeight= \(height > 2160\) \? 1080 : height;\n)/$1        \/\/ Glas: Kodis "Groesse der Oberflaeche begrenzen" (videoscreen.limitguisize) wie bei GBM\n        if (width > 1920 \&\& height > 1080)\n        {\n          int glasLimit = 0;\n          const auto glasSettings = CServiceBroker::GetSettingsComponent();\n          if (glasSettings \&\& glasSettings->GetSettings())\n            glasLimit = glasSettings->GetSettings()->GetInt("videoscreen.limitguisize");\n          const bool glasFast = rrate > 30;\n          if (glasLimit == 1 || (glasLimit == 2 \&\& glasFast))\n          {\n            res->iWidth = 1280;\n            res->iHeight = 720;\n          }\n          else if (glasLimit == 3 || (glasLimit == 2 \&\& !glasFast) || (glasLimit == 4 \&\& glasFast))\n          {\n            res->iWidth = 1920;\n            res->iHeight = 1080;\n          }\n        }\n/' $AU
grep -q 'GetInt("videoscreen.limitguisize")' $AU || { echo "AMLUtils-Patch fehlgeschlagen"; exit 1; }
WA=xbmc/windowing/amlogic/WinSystemAmlogic.cpp
git checkout -q osmc -- $WA
perl -0pi -e 's/(  aml_set_framebuffer_resolution\(4096, 2205, m_framebuffer_name\);\n)/$1\n  \/\/ Glas: Groesse der Oberflaeche begrenzen (AMLUtils.cpp, Vero V) in den Einstellungen zeigen\n  if (const auto glasSetting = m_settings->GetSetting("videoscreen.limitguisize"))\n    glasSetting->SetVisible(true);\n/' $WA
grep -q 'GetSetting("videoscreen.limitguisize")' $WA || { echo "WinSystemAmlogic-Patch fehlgeschlagen"; exit 1; }
grep -q '"settings/lib/Setting.h"' $WA || perl -0pi -e 's/(#include "WinSystemAmlogic.h"\n)/$1#include "settings\/lib\/Setting.h"\n/' $WA
# Zwischenbild in CApplication::Render einhaengen
A=xbmc/application/Application.cpp
grep -q GLAS_GUITARGET $A || perl -0pi -e 's/(void CApplication::Render\(\)\n\{\n)/namespace GLAS_GUITARGET\n{\nvoid Begin();\nvoid End(bool rendered);\n}\n\n$1/; s/(  if \(!CServiceBroker::GetRenderSystem\(\)->BeginRender\(\)\)\n    return;\n)/$1\n  GLAS_GUITARGET::Begin(); \/\/ Glas: eigenes Zwischenbild, solange Live-Glas zu sehen ist\n/; s/(\n  CServiceBroker::GetRenderSystem\(\)->EndRender\(\);\n\n  \/\/ reset our info cache)/\n  GLAS_GUITARGET::End(hasRendered);$1/' $A
# Inhalt unveraendert gegenueber dem letzten Glas-Stand: alten Zeitstempel zurueck (s. oben)
git ls-files -z xbmc | xargs -0 stat -c '%Y %n' > /tmp/glas-mtimes-now 2>/dev/null || true
diff /tmp/glas-mtimes /tmp/glas-mtimes-now | sed -n 's/^< //p' | while read -r mtime file; do
    [ -f "$file" ] && git diff --quiet HEAD -- "$file" 2>/dev/null && touch -d "@$mtime" "$file" || true
done
git add -A xbmc
git diff --cached --stat
git commit -qm "Glas: Control backdropblur (Live-Weichzeichner, GLES)" || echo "keine Aenderung"
# Patch fuer schrittweise Builds (build-tree) bzw. spaeter als OSMC-Patch (GLAS_TO_OSMC=1)
git format-patch osmc..glas --stdout > /build/glas.patch
[ "${GLAS_TO_OSMC:-0}" = "1" ] && cp /build/glas.patch /build/osmc/package/mediacenter-osmc/patches/vero5-900-glas.patch
grep -c '^+' /build/glas.patch
