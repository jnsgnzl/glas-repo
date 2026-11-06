/*
 *  Copyright (C) 2026 Team Kodi / Glas
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

/*!
\file GlasFit.h
\brief Skin-Tag <fititems>true</fititems> an Listen (skin.glas): so hoch (waagrecht: so breit) wie die Eintraege.

 Kodi zeichnet eine Liste immer in ihrer festen Groesse. In einer Grouplist (Seitenleiste der Mediathek: Orte, darunter
 Genres) bleibt dann unter wenigen Eintraegen eine Luecke - bisher mit festen Hoehenstufen je Bibliothek umgangen.
 Mit fititems richtet sich die Groesse nach den sichtbaren Eintraegen (bei festen Inhalten nach ihren <visible>,
 sonst nach dem Inhalt): (Anzahl - 1) x itemlayout + focusedlayout, hoechstens height bzw. width aus dem Skin (dann
 rollt sie wie bisher). Die Grouplist darueber rueckt die folgenden Controls im selben Bild nach.

   <control type="list"> <height>608</height> <fititems>true</fititems> ... </control>

 Kodi ohne diesen Build kennt den Tag nicht: die Liste bleibt height hoch.
 Ablage ohne neue Felder in Kodis Kopfdateien: die Fabrik merkt sich Liste und Hoechstmass hier.
*/

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace GLAS_FIT
{
// gesperrt: Controls werden auch in anderen Faeden zerstoert (Forget); leer (fast immer) ohne Sperre
struct State
{
  std::mutex lock;
  std::unordered_map<const void*, float> table;
  std::atomic<size_t> entries{0};
};

inline State& Get()
{
  static State state;
  return state;
}

inline void Set(const void* container, float maxSize)
{
  State& s = Get();
  std::unique_lock<std::mutex> guard(s.lock);
  s.table[container] = maxSize;
  s.entries = s.table.size();
}

//! Hoechstmass (0 = kein fititems)
inline float Max(const void* container)
{
  State& s = Get();
  if (s.entries == 0)
    return 0.0f;
  std::unique_lock<std::mutex> guard(s.lock);
  auto i = s.table.find(container);
  return i != s.table.end() ? i->second : 0.0f;
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
} // namespace GLAS_FIT
