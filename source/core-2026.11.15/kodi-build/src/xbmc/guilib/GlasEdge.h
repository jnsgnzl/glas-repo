/*
 *  Copyright (C) 2026 Team Kodi / Glas
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

/*!
\file GlasEdge.h
\brief Skin-Tag <edgefade>a1,a2</edgefade> an Listen (skin.glas): Eintraege am Rand blenden aus.

 Vorbild (Popover mit langer Liste, gemessen 11.11.2026): ist die Liste in eine Richtung gerollt, liegt am Rand eine
 angeschnittene Zeile mit blasser Schrift (Deckkraft ~0,35), die Zeile davor ist noch leicht aufgehellt (~0,87), alles
 weiter innen voll. Ganz oben bzw. ganz unten (nichts mehr dahinter) blendet nichts aus. Kodi kann Listen nicht weich
 maskieren; hier bekommt jeder Eintrag eine Deckkraft nach dem Abstand seiner Mitte zum Rand (in Zeilen):
   Mitte bis 0,5 Zeilen vom Rand  a1
   1,5 Zeilen                     a2
   ab 2,5 Zeilen                  1
 dazwischen linear. Die Staerke je Rand waechst mit dem, was dahinter liegt (bis eine Zeile: voll), damit das
 Ausblenden beim Losrollen nicht springt.

   <control type="list"> <height>600</height> <edgefade>0.35,0.87</edgefade> ... </control>

 Kodi ohne diesen Build kennt den Tag nicht: die Liste zeichnet wie bisher.
 Ablage ohne neue Felder in Kodis Kopfdateien (wie GlasFit.h): die Fabrik merkt sich Liste und Werte hier.
*/

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace GLAS_EDGE
{
struct State
{
  std::mutex lock;
  std::unordered_map<const void*, std::pair<float, float>> table;
  std::atomic<size_t> entries{0};
};

inline State& Get()
{
  static State* state = new State; // nie zerstoeren: Controls koennen nach den statischen Objekten sterben
  return *state;
}

inline void Set(const void* container, float a1, float a2)
{
  State& s = Get();
  std::unique_lock<std::mutex> guard(s.lock);
  s.table[container] = {a1, a2};
  s.entries = s.table.size();
}

inline bool Find(const void* container, float& a1, float& a2)
{
  State& s = Get();
  if (s.entries == 0)
    return false;
  std::unique_lock<std::mutex> guard(s.lock);
  auto i = s.table.find(container);
  if (i == s.table.end())
    return false;
  a1 = i->second.first;
  a2 = i->second.second;
  return true;
}

inline void Forget(const void* container)
{
  State& s = Get();
  if (s.entries == 0)
    return;
  std::unique_lock<std::mutex> guard(s.lock);
  s.table.erase(container);
  s.entries = s.table.size();
}

//! Deckkraft nach dem Abstand d (in Zeilen) der Eintragsmitte zum Rand
inline float Curve(float d, float a1, float a2)
{
  if (d <= 0.5f)
    return a1;
  if (d <= 1.5f)
    return a1 + (a2 - a1) * (d - 0.5f);
  if (d <= 2.5f)
    return a2 + (1.0f - a2) * (d - 1.5f);
  return 1.0f;
}

/*! Deckkraft eines Eintrags (1 = unveraendert)
    start/size: Lage und Groesse des Eintrags in Rollrichtung; viewStart/viewSize: die Liste;
    before/after: wie viel Inhalt vor bzw. nach dem sichtbaren Bereich liegt (Pixel); row: Zeilenhoehe */
inline float Alpha(const void* container, float start, float size, float viewStart, float viewSize, float before,
                   float after, float row)
{
  float a1, a2;
  if (row <= 0.0f || !Find(container, a1, a2))
    return 1.0f;
  const float center = start + size / 2.0f;
  const float strengthBefore = std::clamp(before / row, 0.0f, 1.0f);
  const float strengthAfter = std::clamp(after / row, 0.0f, 1.0f);
  const float top = 1.0f - strengthBefore * (1.0f - Curve((center - viewStart) / row, a1, a2));
  const float bottom = 1.0f - strengthAfter * (1.0f - Curve((viewStart + viewSize - center) / row, a1, a2));
  return std::clamp(top * bottom, 0.0f, 1.0f);
}
} // namespace GLAS_EDGE
