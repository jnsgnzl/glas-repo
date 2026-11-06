/*
 *  Copyright (C) 2026 Team Kodi / Glas
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

/*!
\file GlasLayer.h
\brief Live-Glas ueber Live-Glas (skin.glas): Ebene je Glasflaeche, damit jede Ebene ihren eigenen Griff hat.

 Alle Glasflaechen eines Fensters teilen sich einen Griff des Bildschirms (hoechstens einer je Bild). Liegt Glas auf
 Glas (Kreis "Hinzufuegen" auf der Glaskarte des Kontrollzentrums), braucht das obere einen Griff, der das untere
 schon enthaelt. Mit einem gemeinsamen Zwischenspeicher nahm es je nach Bild den Griff ohne die Karte (dunkel) oder
 den mit ihr (hell) - es flackerte (05.11.2026). Jetzt bekommt jede Glasflaeche die Ebene 1 + die hoechste Ebene der
 Flaechen, die in diesem Bild schon gezeichnet waren und wirklich darunter liegen (mindestens 25 % der kleineren
 Flaeche, wie OverlapsMuch: Nachbarn, die sich beim Fokus-Zoom beruehren, zaehlen nicht). Jede Ebene hat einen
 eigenen Griff und Zwischenspeicher (CBackdropBlurShared mit Schluessel Fenster + Ebene * STEP).

 Gilt je Fenster (GlasWindowID); ueber Video bleibt alles auf Ebene 0 (das Video ist ohnehin jedes Bild neu).
 Nur der Render-Faden benutzt die Liste; mehrere Zeichendurchgaenge im selben Bild (Bereiche, Stereo-Augen) behalten
 die Ebene des ersten.
*/

#include <algorithm>
#include <vector>

namespace GLAS_LAYER
{
constexpr int STEP = 1 << 20; //!< Schluessel des Griffs: Fenster + Ebene * STEP (Fenster-IDs sind kleiner)
constexpr int MAX = 2;

inline bool Below(const float a[4], const float b[4])
{
  const float w = std::min(a[2], b[2]) - std::max(a[0], b[0]);
  const float h = std::min(a[3], b[3]) - std::max(a[1], b[1]);
  if (w <= 0 || h <= 0)
    return false;
  const float areaA = (a[2] - a[0]) * (a[3] - a[1]);
  const float areaB = (b[2] - b[0]) * (b[3] - b[1]);
  return w * h >= 0.25f * std::min(areaA, areaB);
}

//! Ebene der Glasflaeche control (rect in Pixeln) im Fenster window fuer das Bild frame
inline int Of(const void* control, const float rect[4], int window, unsigned int frame)
{
  struct Entry
  {
    const void* control;
    int window;
    float rect[4];
    int layer;
  };
  static std::vector<Entry> s_entries;
  static unsigned int s_frame = 0;
  if (frame != s_frame)
  {
    s_entries.clear();
    s_frame = frame;
  }
  int layer = 0;
  for (const Entry& e : s_entries)
  {
    if (e.control == control)
      return e.layer;
    if (e.window == window && Below(rect, e.rect))
      layer = std::max(layer, e.layer + 1);
  }
  layer = std::min(layer, MAX);
  s_entries.push_back({control, window, {rect[0], rect[1], rect[2], rect[3]}, layer});
  return layer;
}
} // namespace GLAS_LAYER
