/*
 *  Copyright (C) 2026 Team Kodi / Glas
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

/*!
\file GlasGrow.h
\brief Animationseffekt grow (skin.glas): Flaeche um feste Pixel wachsen lassen, ohne die Schrift zu strecken.

 tvOS vergroessert fokussierte Kapseln und Zeilen um feste Pixel je Seite (Einstellungszeilen 780 x 66 -> 788 x 74), die
 Schrift bleibt gleich gross und rueckt mit der Kante. Kodis zoom skaliert das ganze Control samt Text und waechst
 prozentual (lange Kapseln mehr als kurze; an Knoepfen mit width auto gar nicht sauber, weil der Zoom die Breite beim
 Laden nimmt):

   <animation type="Focus">
     <effect type="grow" start="0" end="4" time="100" tween="cubic" easing="out" />
     <effect type="zoom" start="100" end="104" center="auto" time="100" glasfallback="true" />
   </animation>

 start/end: Pixel je Seite, "x,y" fuer getrennte Achsen. Wirkt an image (Textur), button und davon abgeleiteten
 Knoepfen (Texturen, <glass>, <focusshadow>; die Beschriftung rueckt mit, waechst aber nicht), spincontrolex und
 sliderex (ueber ihren inneren Knopf). Ein gewachsener Knopf zeichnet seine Texturen ueber den innersten Ausschnitt
 (Grouplist, Liste) hinaus, aber nur um das Wachstum: sonst schnitte Kodis Grouplist die Kanten ab (jede
 Einstellungszeile ist so breit wie sie). Kodi ohne diesen Build kennt den Effekt nicht und spielt nur die
 glasfallback-Effekte.

 Umsetzung ohne neue Felder in Kodis Kopfdateien: CGUIControl::Animate sammelt die grow-Effekte seiner Animationen
 (CAnimation::RenderAnimation meldet sie) und legt das Ergebnis hier je Control ab; Bild und Knopf holen es in Process.
 Animate und Process laufen im GUI-Faden, Controls werden aber auch in anderen Faeden zerstoert (Listeneintraege samt
 Layouts, z. B. beim Start eines Films): die Tabelle ist deshalb gesperrt; solange nichts waechst (fast immer), gehen
 alle Aufrufe ohne Sperre an einem Zaehler vorbei.
*/

#include "utils/Geometry.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace GLAS_GROW
{
struct State
{
  std::mutex lock;
  std::unordered_map<const void*, CPoint> grow; //!< Wachstum je Control (nur ungleich 0)
  std::unordered_map<const void*, bool> applied; //!< Verbraucher, der zuletzt gewachsen gezeichnet hat
  std::unordered_map<const void*, const void*> alias; //!< innerer Knopf -> aeusseres Control (spincontrolex)
  std::atomic<size_t> entries{0}; //!< grow + applied + alias, fuer den Weg ohne Sperre
  CPoint sum; //!< nur im GUI-Faden (Animate)

  void Count() { entries = grow.size() + applied.size() + alias.size(); }
};

inline State& Get()
{
  static State state;
  return state;
}

//! CGUIControl::Animate: Beginn der Animationen eines Controls
inline void Begin()
{
  Get().sum = CPoint();
}

//! CAnimation::RenderAnimation: aktueller Wert eines grow-Effekts
inline void Add(float x, float y)
{
  Get().sum.x += x;
  Get().sum.y += y;
}

//! CGUIControl::Animate: Ende, Ergebnis fuer dieses Control ablegen
inline void End(const void* control)
{
  State& s = Get();
  const bool grown = s.sum.x != 0.0f || s.sum.y != 0.0f;
  if (!grown && s.entries == 0)
    return;
  std::unique_lock<std::mutex> guard(s.lock);
  if (grown)
    s.grow[control] = s.sum;
  else
    s.grow.erase(control);
  s.Count();
}

//! Inneren Knopf an das aeussere Control haengen (dessen Animationen gelten); nur solange das aeussere waechst
//! (Process des aeusseren, nach dessen Animate)
inline void Alias(const void* inner, const void* outer)
{
  State& s = Get();
  if (s.entries == 0)
    return;
  std::unique_lock<std::mutex> guard(s.lock);
  if (s.grow.count(outer))
    s.alias[inner] = outer;
  else
    s.alias.erase(inner);
  s.Count();
}

//! Wachstum je Seite (0, 0 = keins)
inline CPoint Peek(const void* control)
{
  State& s = Get();
  if (s.entries == 0)
    return CPoint();
  std::unique_lock<std::mutex> guard(s.lock);
  if (s.grow.empty())
    return CPoint();
  auto a = s.alias.find(control);
  auto g = s.grow.find(a != s.alias.end() ? a->second : control);
  return g != s.grow.end() ? g->second : CPoint();
}

//! Muss der Verbraucher seine Texturen setzen? Ja, solange er waechst, und einmal danach (zurueck auf die Ruhegroesse)
inline bool NeedsApply(const void* consumer, const CPoint& grow)
{
  State& s = Get();
  const bool grown = grow.x != 0.0f || grow.y != 0.0f;
  if (!grown && s.entries == 0)
    return false;
  std::unique_lock<std::mutex> guard(s.lock);
  bool result = true;
  if (grown)
    s.applied[consumer] = true;
  else
    result = s.applied.erase(consumer) > 0;
  s.Count();
  return result;
}

//! ~CGUIControl (beliebiger Faden): Eintraege vergessen (die Adresse kann wiederverwendet werden)
inline void Forget(const void* control)
{
  State& s = Get();
  if (s.entries == 0)
    return;
  std::unique_lock<std::mutex> guard(s.lock);
  s.grow.erase(control);
  s.applied.erase(control);
  s.alias.erase(control);
  s.Count();
}
} // namespace GLAS_GROW
