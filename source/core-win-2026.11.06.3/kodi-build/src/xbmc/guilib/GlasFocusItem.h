/*
 *  Copyright (C) 2026 Team Kodi / Glas
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

/*!
\file GlasFocusItem.h
\brief Lage des fokussierten Eintrags einer Liste (skin.glas): fuer das Kontextmenue neben der Kachel.

 Kodi setzt das Kontextmenue mittig auf das fokussierte Control - bei einer Liste also auf die ganze Liste, nicht auf
 die Kachel. Der Skin konnte das nur ueber Tabellen je Spalte und Reihe ausgleichen, und nur dort, wo Container.Position
 im Dialog etwas liefert (nicht in Fenstern mit mehreren Reihen wie der App TV). CGUIBaseContainer::ProcessItem merkt
 sich hier die Zelle des fokussierten Eintrags (focusedlayout, relativ zur Liste), CGUIDialogContextMenu setzt das
 Menue daneben (rechts, solange die Zelle links der Bildmitte liegt, sonst links; oben buendig mit der Zelle) und meldet
 die Seite als Fenster-Eigenschaft glas.ctx.side (right/left) sowie die Zellgroesse (glas.ctx.w, glas.ctx.h). Nur
 wenn das Kontextmenue es will: Control 9900 darin ist sichtbar (Bedingung im Skin).
*/

#include "utils/Geometry.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace GLAS_FOCUSITEM
{
// gesperrt: Controls werden auch in anderen Faeden zerstoert (Forget)
struct State
{
  std::mutex lock;
  std::unordered_map<const void*, CRect> table;
  std::atomic<size_t> entries{0};
};

inline State& Get()
{
  static State state;
  return state;
}

inline void Set(const void* container, const CRect& cell)
{
  State& s = Get();
  std::unique_lock<std::mutex> guard(s.lock);
  s.table[container] = cell;
  s.entries = s.table.size();
}

inline bool Get(const void* container, CRect& cell)
{
  State& s = Get();
  std::unique_lock<std::mutex> guard(s.lock);
  auto i = s.table.find(container);
  if (i == s.table.end())
    return false;
  cell = i->second;
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
} // namespace GLAS_FOCUSITEM
