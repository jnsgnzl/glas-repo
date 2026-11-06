/*
 *  Glas (skin.glas): Bild-Untertitel (PGS, DVD, DVB) in den sichtbaren Streifen einer 21:9-Leinwand legen
 *  (Scope-Modus, docs/plan-scope.md). Nur in OverlayRenderer.cpp eingebunden (apply-glas.sh / apply-glas.ps1).
 *
 *  Kodi setzt Bild-Untertitel genau dorthin, wo die Datei sie vorsieht. Manche Dateien legen sie an den unteren Rand
 *  des 16:9-Bildes (gemessen: "Cold Storage" y 888-1012, "Mortal Kombat II" englisch y 933-1060 bei 1080 Zeilen) -
 *  im Scope-Modus unter den unteren Balken. Text-Untertitel verschiebt der Glas Helper ueber den Abstand
 *  (subtitles.marginvertical, subpos.py); fuer Bild-Untertitel gibt es in Kodi keine Einstellung.
 *
 *  Der Glas Helper setzt Window(Home).Property(glas.substrip) = "oben,unten" in Zeilen von 1080 (2.40: "140,940"),
 *  leer = aus. Ragt ein Untertitel unten aus dem Streifen, rueckt er um den Ueberstand plus GAP nach oben (oben
 *  entsprechend nach unten). Nicht in Disc-Menues (Blu-ray- und DVD-Menues kommen auf demselben Weg), nicht bei Bildern
 *  ueber MAX_HEIGHT der Hoehe und nicht, wenn der Untertitel hoeher als der Streifen ist.
 */
#pragma once

#include "ServiceBroker.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindow.h"
#include "guilib/GUIWindowManager.h"
#include "guilib/WindowIDs.h"
#include "utils/Geometry.h"
#include "utils/Variant.h"

#include <cstdio>
#include <string>

namespace GLAS_SUBSTRIP
{
constexpr float GAP = 0.02f;        // Abstand zur Leinwandkante, Anteil der Bildhoehe (22 px bei 1080)
constexpr float MAX_HEIGHT = 0.4f;  // groessere Bilder sind keine Untertitel (Menues, Grafiken)

//! Ist das gerade gezeichnete Overlay ein Bild-Untertitel (nicht Text/ASS)? Setzt CRenderer::Render(int) je Eintrag.
inline bool& CurrentIsImage()
{
  static bool image = false;
  return image;
}

//! Streifen als Anteil der Bildhoehe aus Window(Home).Property(glas.substrip); false = aus.
inline bool Strip(float& top, float& bottom)
{
  static std::string last;
  static bool valid = false;
  static float stripTop = 0.0f;
  static float stripBottom = 1.0f;
  auto* gui = CServiceBroker::GetGUI();
  if (!gui)
    return false;
  CGUIWindow* home = gui->GetWindowManager().GetWindow(WINDOW_HOME);
  if (!home)
    return false;
  const std::string value = home->GetProperty("glas.substrip").asString();
  if (value != last)
  {
    last = value;
    int t = 0;
    int b = 0;
    valid = !value.empty() && std::sscanf(value.c_str(), "%d,%d", &t, &b) == 2 && t >= 0 && b <= 1080 &&
            b - t >= 540 && (t > 0 || b < 1080);
    stripTop = t / 1080.0f;
    stripBottom = b / 1080.0f;
  }
  top = stripTop;
  bottom = stripBottom;
  return valid;
}

//! y/height wie SRenderState (relative: y = Mitte, sonst Oberkante), view = Bildflaeche (CRenderer::m_rv).
inline void Clamp(float& y, float height, bool centered, const CRect& view)
{
  if (!CurrentIsImage())
    return;
  float top = 0.0f;
  float bottom = 1.0f;
  if (!Strip(top, bottom))
    return;
  const float h = view.Height();
  if (h <= 0.0f || height <= 0.0f || height > MAX_HEIGHT * h)
    return;
  const auto appPlayer = CServiceBroker::GetAppComponents().GetComponent<CApplicationPlayer>();
  if (appPlayer && appPlayer->IsInMenu())
    return;
  const float gap = GAP * h;
  const float minY = view.y1 + top * h + gap;
  const float maxY = view.y1 + bottom * h - gap;
  if (height > maxY - minY)
    return;
  const float y1 = centered ? y - height * 0.5f : y;
  const float y2 = y1 + height;
  if (y2 > maxY)
    y -= y2 - maxY;
  else if (y1 < minY)
    y += minY - y1;
}
} // namespace GLAS_SUBSTRIP
