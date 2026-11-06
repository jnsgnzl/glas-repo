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
# Listen-Layouts pruefen die Typen ihrer Controls: ohne Eintrag warnt Kodi bei jedem Glas in einer Liste
# ("Trying to add unsupported control type", fuegt es aber ein)
grep -q GUICONTROL_BACKDROPBLUR $G/GUIListGroup.cpp || \
    perl -0pi -e 's/(    CGUIControl::GUICONTROL_BORDEREDIMAGE,\n)/    CGUIControl::GUICONTROL_BACKDROPBLUR,\n$1/' $G/GUIListGroup.cpp
grep -q GUICONTROL_BACKDROPBLUR $G/GUIListGroup.cpp || { echo "Patch GUIListGroup fehlgeschlagen"; exit 1; }
# Kontextmenue "Inhalt anzeigen" (Browse into) nicht bei Disc-Abbildern: zeigt nur die rohe Disc-Struktur
# (BDMV, VIDEO_TS, ANY!); abgespielt wird ueber Wiedergabe bzw. "Titel waehlen"
VC=xbmc/video/ContextMenus.cpp
git checkout -q osmc -- $VC
perl -0pi -e 's/(bool CVideoBrowse::IsVisible\(const CFileItem& item\) const\n\{\n)/$1  \/\/ Glas: Disc-Abbilder nicht oeffnen (rohe Disc-Struktur ergibt in der Oberflaeche keinen Sinn)\n  if (item.IsDiscImage() || URIUtils::IsDiscImage(item.GetDynPath()))\n    return false;\n/' $VC
grep -q "item.IsDiscImage() || URIUtils::IsDiscImage" $VC || { echo "Patch ContextMenus fehlgeschlagen"; exit 1; }

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
# luma (Helligkeit hinter dem Glas als Window(Home).Property(glas.glass.<name>.luma)) nachruesten
grep -q 'blur->SetLuma' $F || perl -0pi -e 's/(        blur->SetSource\(glasSource\);\n)/$1      std::string glasLuma;\n      if (XMLUtils::GetString(pControlNode, "luma", glasLuma))\n        blur->SetLuma(glasLuma);\n/' $F
for tag in grabscale updaterate grabprobe style; do grep -q "\"$tag\"" $F || { echo "Fabrik: $tag fehlt"; exit 1; }; done
grep -q 'blur->SetSource' $F || { echo "Fabrik: source fehlt"; exit 1; }
grep -q 'blur->SetLuma' $F || { echo "Fabrik: luma fehlt"; exit 1; }

# Schrift in Zielgroesse bei verkleinertem Text (src/xbmc/guilib/GUIFont.cpp, GlasScaledFont): die Schrift merkt sich
# Datei, Seitenverhaeltnis und Rand, damit CGUIFont eine kleinere Fassung laden kann
T=$G/GUIFontTTF
grep -q m_glasPath $T.h || perl -0pi -e 's/(  float m_height\{0\.0f\};\n)/$1  \/\/ Glas: Datei, Seitenverhaeltnis und Rand fuer verkleinerte Fassungen (CGUIFont::GlasScaledFont)\n  std::string m_glasPath;\n  float m_glasAspect\{1.0f\};\n  bool m_glasBorder\{false\};\n/' $T.h
grep -q m_glasPath $T.cpp || perl -0pi -e 's/(  m_face = g_freeTypeLibrary\.GetFont\(strFilename, height, aspect, m_fontFileInMemory\);\n)/  m_glasPath = strFilename;\n  m_glasAspect = aspect;\n  m_glasBorder = border;\n$1/' $T.cpp
grep -q m_glasPath $T.h && grep -q m_glasPath $T.cpp || { echo "GUIFontTTF-Patch fehlgeschlagen"; exit 1; }

C=$G/CMakeLists.txt
grep -q GUIBackdropBlurControl $C || perl -0pi -e 's/^set\(SOURCES DDSImage.cpp\n/set(SOURCES DDSImage.cpp\n            GUIBackdropBlurControl.cpp\n/m; s/^set\(HEADERS DDSImage.h\n/set(HEADERS DDSImage.h\n            GUIBackdropBlurControl.h\n/m' $C

# <glass> an Knoepfen: Live-Glas unter der Platte (src/xbmc/guilib/GUIGlasButton.*), z. B. Einstellungszeilen, die
# Kodi selbst aus den Vorgabe-Controls baut (dort kann der Skin kein backdropblur unterlegen)
grep -q GUIGlasButton.cpp $C || perl -0pi -e 's/(            GUIBackdropBlurControl\.cpp\n)/$1            GUIGlasButton.cpp\n/; s/(            GUIBackdropBlurControl\.h\n)/$1            GUIGlasButton.h\n/' $C
grep -q GUIGlasButton.h $F || perl -0pi -e 's/(#include "GUIImage.h"\n)/#include "GUIGlasButton.h"\n$1/' $F
grep -q 'CGlasButtonGlass::FromXml' $F || perl -0pi -e 's/(  \/\/ things that apply to all controls\n  if \(control\)\n)/  CGlasButtonGlass::FromXml(control, pControlNode, parentID); \/\/ Glas: <glass> an Knoepfen\n\n$1/' $F
BC=$G/GUIButtonControl
git checkout -q osmc -- $BC.h $BC.cpp $G/GUISpinControlEx.h $G/GUISettingsSliderControl.h
perl -0pi -e 's/(#include "GUIControl.h"\n)/$1#include "GUIGlasButton.h"\n/; s/(  CGUIButtonControl\* Clone\(\) const override \{ return new CGUIButtonControl\(\*this\); \}\n)/$1  CGlasButtonGlass& Glas() { return m_glas; } \/\/ Glas: <glass> (GUIGlasButton.h)\n  CGUILabel& GlasLabel2() { return m_label2; } \/\/ Glas: <textcolor2>, <focusedcolor2>\n/; s/(  bool m_bSelected;\n)/$1  CGlasButtonGlass m_glas; \/\/ Glas: Live-Glas unter der Platte\n/' $BC.h
perl -0pi -e 's/    m_bSelected\(control.m_bSelected\)\n\{\n/    m_bSelected(control.m_bSelected),\n    m_glas(control.m_glas)\n{\n/; s/(void CGUIButtonControl::Process\(unsigned int currentTime, CDirtyRegionList &dirtyregions\)\n\{\n)/$1  \/\/ Glas: <glass> - Glas vor den Texturen verarbeiten (die Liste enthaelt bis hier nur, was dahinter liegt)\n  \/\/ grow (glas-patches.txt, GlasGrow.h): die gewachsene Flaeche\n  const CPoint glasGrowGlass = GLAS_GROW::Peek(this);\n  if (m_glas.Process(*this, m_posX - glasGrowGlass.x, m_posY - glasGrowGlass.y, GetWidth() + 2 * glasGrowGlass.x,\n                     m_height + 2 * glasGrowGlass.y, currentTime, dirtyregions))\n    MarkDirtyRegion();\n\n/; s/(void CGUIButtonControl::Render\(\)\n\{\n)/$1  m_glas.Render(); \/\/ Glas: unter den Texturen\n/; s/(  m_imgNoFocus->FreeResources\(immediately\);\n)/$1  m_glas.FreeResources(immediately);\n/' $BC.cpp
perl -0pi -e 's/(  CGUISpinControlEx\* Clone\(\) const override \{ return new CGUISpinControlEx\(\*this\); \}\n)/$1  CGUIButtonControl& GlasButton() { return m_buttonControl; } \/\/ Glas: <glass> (GUIGlasButton.h)\n  CGUILabel& GlasValueLabel() { return m_label; } \/\/ Glas: <textcolor2>, <focusedcolor2>\n/' $G/GUISpinControlEx.h
perl -0pi -e 's/(  CGUISettingsSliderControl \*Clone\(\) const override \{ return new CGUISettingsSliderControl\(\*this\); \}\n)/$1  CGUIButtonControl& GlasButton() { return m_buttonControl; } \/\/ Glas: <glass> (GUIGlasButton.h)\n  CGUILabel& GlasValueLabel() { return m_label; } \/\/ Glas: <textcolor2>, <focusedcolor2>\n/' $G/GUISettingsSliderControl.h
[ "$(grep -c m_glas $BC.h)" = 2 ] && [ "$(grep -c m_glas $BC.cpp)" = 4 ] && grep -q GlasButton $G/GUISpinControlEx.h && \
    grep -q GlasButton $G/GUISettingsSliderControl.h && grep -q 'CGlasButtonGlass::FromXml' $F && grep -q GUIGlasButton.cpp $C || \
    { echo "Knopf-Glas-Patch fehlgeschlagen"; exit 1; }

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
# Startlog: Faehigkeit <glass> an Knoepfen melden (Glas Helper setzt daraufhin Window(Home).Property(glas.corebutton))
grep -q "Stereo-Texturen, Glas-Knoepfe" xbmc/application/Application.cpp || perl -pi -e 's/, Stereo-Texturen"\);/, Stereo-Texturen, Glas-Knoepfe");/' xbmc/application/Application.cpp
grep -q "Stereo-Texturen, Glas-Knoepfe" xbmc/application/Application.cpp || { echo "Startlog-Markierung Glas-Knoepfe fehlt"; exit 1; }
# Startlog: Faehigkeit Untertitel-Streifen melden (Glas Helper setzt daraufhin Window(Home).Property(glas.substrip))
grep -q "Glas-Knoepfe, Untertitel-Streifen" xbmc/application/Application.cpp || perl -pi -e 's/, Glas-Knoepfe"\);/, Glas-Knoepfe, Untertitel-Streifen");/' xbmc/application/Application.cpp
grep -q "Glas-Knoepfe, Untertitel-Streifen" xbmc/application/Application.cpp || { echo "Startlog-Markierung Untertitel-Streifen fehlt"; exit 1; }

# Bild-Untertitel (PGS, DVD, DVB) im Scope-Modus in den sichtbaren Streifen der Leinwand ruecken
# (src/xbmc/cores/VideoPlayer/VideoRenderers/GlasSubtitleStrip.h; Streifen vom Glas Helper, subpos.py)
OR=xbmc/cores/VideoPlayer/VideoRenderers/OverlayRenderer.cpp
git checkout -q osmc -- $OR
perl -0pi -e 's/(#include "OverlayRendererUtil.h"\n)/$1#include "GlasSubtitleStrip.h"\n/; s/(      std::shared_ptr<COverlay> o = Convert\(\*\(it->overlay_dvd\), it->pts\);\n)/$1      \/\/ Glas: Bild-Untertitel (nicht Text\/ASS) duerfen in den Streifen der Leinwand ruecken (GlasSubtitleStrip.h)\n      GLAS_SUBSTRIP::CurrentIsImage() = it->overlay_dvd->IsOverlayType(DVDOVERLAY_TYPE_IMAGE) ||\n                                        it->overlay_dvd->IsOverlayType(DVDOVERLAY_TYPE_SPU);\n/; s/(  state.x \+= GetStereoscopicDepth\([^\n]*\);\n)/  GLAS_SUBSTRIP::Clamp(state.y, state.height, o->m_pos == COverlay::POSITION_RELATIVE, m_rv); \/\/ Glas: Scope-Modus\n$1/' $OR
[ "$(grep -c GLAS_SUBSTRIP $OR)" = 2 ] && grep -q '"GlasSubtitleStrip.h"' $OR || { echo "OverlayRenderer-Patch fehlgeschlagen"; exit 1; }

# Blu-ray-Abbild direkt mit dem Hauptfilm aus der NFO starten (<mainplaylist>, schreibt z. B. Kodex) statt Titelliste bzw.
# laengster Playlist
# (src/xbmc/dialogs/GlasMainPlaylist.h)
grep -q "Untertitel-Streifen, Hauptfilm" xbmc/application/Application.cpp || perl -pi -e 's/, Untertitel-Streifen"\);/, Untertitel-Streifen, Hauptfilm");/' xbmc/application/Application.cpp
grep -q "Untertitel-Streifen, Hauptfilm" xbmc/application/Application.cpp || { echo "Startlog-Markierung Hauptfilm fehlt"; exit 1; }
SM=xbmc/dialogs/GUIDialogSimpleMenu.cpp
git checkout -q osmc -- $SM
perl -0pi -e 's/(#include "GUIDialogSimpleMenu.h"\n)/$1#include "GlasMainPlaylist.h"\n/; s/(bool CGUIDialogSimpleMenu::ShowPlaySelection\(CFileItem& item\)\n\{\n)/$1  \/\/ Glas: Hauptfilm aus der NFO direkt waehlen (GlasMainPlaylist.h), auch statt "Hauptfilm abspielen" (laengste Playlist)\n  if (GLAS_MAINPLAYLIST::Apply(item))\n    return true;\n\n/' $SM
grep -q '"GlasMainPlaylist.h"' $SM && grep -q 'GLAS_MAINPLAYLIST::Apply(item)' $SM || { echo "GUIDialogSimpleMenu-Patch fehlgeschlagen"; exit 1; }

# Federkurve: spring="<bounce>" neben tween/easing an Animationen und scrolltime (src/xbmc/guilib/GlasTween.h);
# Kodi ohne diesen Build nimmt tween
VE=$G/VisibleEffect.cpp
git checkout -q osmc -- $VE
perl -0pi -e 's/(#include "Tween.h"\n)/$1#include "GlasTween.h" \/\/ Glas: spring="<bounce>"\n/; s/(  float accel = 0;\n  pAnimationNode->QueryFloatAttribute\("acceleration", &accel\);\n)/  \/\/ Glas: Federkurve, wenn spring="<bounce>" gesetzt ist (Kodi ohne diesen Build nimmt tween); easing wie oben\n  float glasBounce = 0.0f;\n  if (pAnimationNode->QueryFloatAttribute("spring", &glasBounce) == TIXML_SUCCESS)\n  {\n    m_pTweener = std::make_shared<GlasSpringTweener>(glasBounce);\n    const char* glasEasing = pAnimationNode->Attribute("easing");\n    if (glasEasing && StringUtils::CompareNoCase(glasEasing, "in") == 0)\n      m_pTweener->SetEasing(EASE_IN);\n    else if (glasEasing && StringUtils::CompareNoCase(glasEasing, "inout") == 0)\n      m_pTweener->SetEasing(EASE_INOUT);\n  }\n\n$1/' $VE
# glasfallback="true" an einem <effect>: nur fuer Kodi ohne diesen Build (z. B. nachgeahmtes Ueberschwingen statt spring)
perl -0pi -e 's/(    std::string type = XMLUtils::GetAttribute\(effect, "type"\);\n)    AddEffect\(type, effect, rect\);\n/$1    \/\/ Glas: glasfallback="true" - Effekt nur fuer Kodi ohne Glas-Kern (z. B. nachgeahmtes Ueberschwingen statt spring)\n    const char* glasFallback = effect->Attribute("glasfallback");\n    if (!glasFallback || !StringUtils::EqualsNoCase(glasFallback, "true"))\n      AddEffect(type, effect, rect);\n/' $VE
grep -q '"GlasTween.h"' $VE && grep -q 'GlasSpringTweener' $VE && grep -q 'glasFallback' $VE || { echo "VisibleEffect-Patch (Federkurve) fehlgeschlagen"; exit 1; }
grep -q "Hauptfilm, Federkurven" xbmc/application/Application.cpp || perl -pi -e 's/, Hauptfilm"\);/, Hauptfilm, Federkurven");/' xbmc/application/Application.cpp
grep -q "Hauptfilm, Federkurven" xbmc/application/Application.cpp || { echo "Startlog-Markierung Federkurven fehlt"; exit 1; }

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
# Gemeinsame Patches aus glas-patches.txt (wie windows/apply-glas.ps1): Wachsen um feste Pixel (Effekt grow,
# src/xbmc/guilib/GlasGrow.h), Listenhoehe nach den Eintraegen (<fititems>, GlasFit.h), "…" beim Kuerzen und in
# Textboxen, Kontextmenue neben der Kachel (GlasFocusItem.h), Rolle der Ja/Nein-Rueckfrage, Richtung des
# Fensterwechsels, Kurve von fadetime (GlasFade.h), Vorschaubild beim Spulen (glasseek://). Laeuft zuletzt, nach allen
# anderen Patches (GUIWindowManager.cpp holt der Teil oben frisch aus osmc). Dateien, die nur diese Patches beruehren,
# vorher auf den OSMC-Stand; die uebrigen sind oben schon frisch gepatcht, schon vorhandene Bloecke ueberspringt das Skript
git checkout -q osmc -- $G/GUIImage.cpp $G/GUISpinControlEx.cpp $G/GUISettingsSliderControl.cpp $G/GUIBaseContainer.cpp \
    $G/GUITextBox.cpp xbmc/dialogs/GUIDialogContextMenu.cpp xbmc/dialogs/GUIDialogYesNo.cpp xbmc/TextureCacheJob.cpp \
    xbmc/video/VideoChapterImageFileLoader.cpp xbmc/cores/VideoPlayer/DVDFileInfo.cpp xbmc/SeekHandler.h xbmc/SeekHandler.cpp     xbmc/interfaces/builtins/GUIContainerBuiltins.cpp xbmc/guilib/GUISliderControl.h xbmc/guilib/GUISliderControl.cpp \
    xbmc/guilib/GUIControlGroupList.cpp xbmc/guilib/GUIPanelContainer.cpp xbmc/dialogs/GUIDialogSelect.cpp \
    xbmc/video/windows/GUIWindowVideoNav.cpp
tr -d '\r' < /tools/glas-patches.pl > /tmp/glas-patches.pl
perl /tmp/glas-patches.pl /tools/glas-patches.txt
grep -q 'CGlasGrowEffect' $VE && grep -q 'GLAS_GROW::Peek' $BC.cpp && grep -q 'GLAS_FIT::Max' $G/GUIBaseContainer.cpp && \
    grep -q 'GlasLabel2' $BC.h && grep -q 'GlasSetNav' $WM && grep -q 'glasseek' xbmc/TextureCacheJob.cpp || \
    { echo "Gemeinsame Patches (glas-patches.txt) fehlgeschlagen"; exit 1; }
grep -q "Federkurven, Wachsen" $A || perl -pi -e 's/, Federkurven"\);/, Federkurven, Wachsen, Listenhoehe, Glas-Ebenen, Kontextmenue-Lage, Spulvorschau");/' $A
grep -q "Glas-Ebenen, Kontextmenue-Lage, Spulvorschau" $A || { echo "Startlog-Markierung Wachsen/Spulvorschau fehlt"; exit 1; }
grep -q "Spulvorschau, Buchstaben-Sprung" $A || perl -pi -e 's/, Spulvorschau"\);/, Spulvorschau, Buchstaben-Sprung");/' $A
grep -q "Spulvorschau, Buchstaben-Sprung" $A || { echo "Startlog-Markierung Buchstaben-Sprung fehlt"; exit 1; }
grep -q "Buchstaben-Sprung, Helligkeit" $A || perl -pi -e 's/, Buchstaben-Sprung"\);/, Buchstaben-Sprung, Helligkeit");/' $A
grep -q "Buchstaben-Sprung, Helligkeit" $A || { echo "Startlog-Markierung Helligkeit fehlt"; exit 1; }
grep -q "Helligkeit, Maus-Spulvorschau" $A || perl -pi -e 's/, Helligkeit"\);/, Helligkeit, Maus-Spulvorschau");/' $A
grep -q "Helligkeit, Maus-Spulvorschau" $A || { echo "Startlog-Markierung Maus-Spulvorschau fehlt"; exit 1; }
grep -q "GLAS_BAND::Apply" $G/GUIControl.cpp && grep -q "GLAS_BAND::AfterMove" $G/GUIBaseContainer.cpp && \
    grep -q "GLAS_BAND::AfterMove" $G/GUIPanelContainer.cpp && grep -q "GLAS_BAND::Wraps" $G/GUIControlGroupList.cpp && \
    grep -q "GLAS_BAND::GlasSkin" xbmc/dialogs/GUIDialogSelect.cpp && grep -q "GLAS_CTXMENU::Decorate" xbmc/dialogs/GUIDialogContextMenu.cpp || { echo "Gummiband-Patch fehlt"; exit 1; }
grep -q "Maus-Spulvorschau, Gummiband" $A || perl -pi -e 's/, Maus-Spulvorschau"\);/, Maus-Spulvorschau, Gummiband");/' $A
grep -q "Maus-Spulvorschau, Gummiband" $A || { echo "Startlog-Markierung Gummiband fehlt"; exit 1; }
grep -q "Gummiband, Kontextmenue-Symbole" $A || perl -pi -e 's/, Gummiband"\);/, Gummiband, Kontextmenue-Symbole");/' $A
grep -q "Gummiband, Kontextmenue-Symbole" $A || { echo "Startlog-Markierung Kontextmenue-Symbole fehlt"; exit 1; }
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
