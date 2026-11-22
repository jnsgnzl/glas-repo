/*
 *  Copyright (C) 2026 Team Kodi / Glas
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

/*!
\file GlasTopFade.h
\brief Skin-Tag <topfade>40</topfade> an senkrechten Listen und Grouplists (skin.glas): gerollter Inhalt laeuft oben
weich aus statt hart abgeschnitten zu werden.

 Vorbild (Einstellungen, gemessen 14.11.2026, docs/<Abgleich> Nachtrag 7): ueber der Oberkante der
 Liste liegt ein Band von ~40 px, in dem der gerollte Inhalt zum Grund hin ausblendet (13 px ueber der Kante ~45 %,
 29 px ~15 %, ab ~39 px nichts). Kodi schneidet Listen hart an ihrem Rechteck ab und kann nicht weich maskieren. Hier
 zeichnet die Liste nach dem normalen Durchgang den Inhalt, der ins Band ragt, noch einmal in waagerechten Streifen,
 jeder mit eigener Deckkraft (unten voll, nach oben ausblendend, Kurve t^1,7). Darunter bleibt der echte Hintergrund
 stehen (Fanart, Verlauf) - kein Farbband, das nicht passt. Damit etwas ins Band ragt, haelt die Liste eine Zeile vor
 der ersten sichtbaren bereit (GetCacheOffsets).

 Waehrend der Streifen zeichnet Live-Glas nicht (GUIBackdropBlurControl::Render fragt Active()): sonst griffe es je
 Streifen neu. Ohne diesen Build kennt Kodi den Tag nicht, die Liste schneidet wie bisher.
 Ablage ohne neue Felder in Kodis Kopfdateien (wie GlasEdge.h): die Fabrik merkt sich Liste und Hoehe hier.
*/

#include "ServiceBroker.h"
#include "utils/Geometry.h"
#include "utils/TransformMatrix.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace GLAS_TOPFADE
{
struct State
{
  std::mutex lock;
  std::unordered_map<const void*, float> table;
  std::atomic<size_t> entries{0};
  // laufender Streifen (nur im GUI-Faden)
  bool active = false;
  CRect strip;
  float alpha = 1.0f;
};

inline State& Get()
{
  static State* state = new State; // nie zerstoeren: Controls koennen nach den statischen Objekten sterben
  return *state;
}

inline void Set(const void* control, float band)
{
  State& s = Get();
  std::unique_lock<std::mutex> guard(s.lock);
  s.table[control] = band;
  s.entries = s.table.size();
}

inline bool Find(const void* control, float& band)
{
  State& s = Get();
  if (s.entries == 0)
    return false;
  std::unique_lock<std::mutex> guard(s.lock);
  auto i = s.table.find(control);
  if (i == s.table.end())
    return false;
  band = i->second;
  return band > 0.0f;
}

inline void Forget(const void* control)
{
  State& s = Get();
  if (s.entries == 0)
    return;
  std::unique_lock<std::mutex> guard(s.lock);
  s.table.erase(control);
  s.entries = s.table.size();
}

//! Laeuft gerade ein Streifen? (Live-Glas zeichnet dann nicht)
inline bool Active()
{
  return Get().active;
}

//! Deckkraft des laufenden Streifens. CGUIControl::DoRender rechnet sie in jede Transformation ein: Controls setzen
//! beim Zeichnen ihre vorab berechnete Transformation fest (SetTransform), ein Fader der Liste ginge sonst verloren
inline float Alpha()
{
  return Get().alpha;
}

//! Statt SetClipRegion der Liste: im Streifen dessen Rechteck, sonst das der Liste
inline bool Clip(float x, float y, float w, float h)
{
  CGraphicContext& ctx = CServiceBroker::GetWinSystem()->GetGfxContext();
  const State& s = Get();
  if (s.active)
    return ctx.SetClipRegion(s.strip.x1, s.strip.y1, s.strip.Width(), s.strip.Height());
  return ctx.SetClipRegion(x, y, w, h);
}

//! Eintrag bzw. Kind (Oberkante y, Hoehe h) ausserhalb des laufenden Streifens? Dann nicht zeichnen. Etwas Rand fuer
//! den Fokus-Zoom
inline bool Outside(float y, float h)
{
  const State& s = Get();
  if (!s.active)
    return false;
  const float margin = 0.25f * h;
  return y - margin >= s.strip.y2 || y + h + margin <= s.strip.y1;
}

//! Eine Zeile vor der ersten sichtbaren bereithalten (verarbeiten und zeichnen), damit etwas ins Band ragt
inline void Cache(const void* container, int& cacheBefore)
{
  float band;
  if (Find(container, band))
    cacheBefore = (std::max)(cacheBefore, 1);
}

//! Nach dem normalen Durchgang: Band ueber der Oberkante (x, top, Breite w) in Streifen nachzeichnen. render() ist der
//! Render der Liste selbst (zeichnet dann nur im Streifen, s. Clip und Outside)
template<class F>
void Run(const void* control, float x, float top, float w, F render)
{
  State& s = Get();
  float band;
  if (s.active || !Find(control, band))
    return;
  CGraphicContext& ctx = CServiceBroker::GetWinSystem()->GetGfxContext();
  const int strips = (std::clamp)(static_cast<int>(band / 4.0f + 0.5f), 2, 12);
  const float h = band / strips;
  for (int i = 0; i < strips; ++i)
  {
    const float t = (static_cast<float>(i) + 0.5f) / strips; // 0 oben .. 1 unten
    const float alpha = std::pow(t, 1.7f);
    if (alpha < 0.02f)
      continue;
    const float y0 = top - band + i * h;
    if (!ctx.SetClipRegion(x, y0, w, h))
      continue;
    s.strip = CRect(x, y0, x + w, y0 + h);
    s.alpha = alpha;
    s.active = true;
    render();
    s.active = false;
    ctx.RestoreClipRegion();
  }
}
} // namespace GLAS_TOPFADE
