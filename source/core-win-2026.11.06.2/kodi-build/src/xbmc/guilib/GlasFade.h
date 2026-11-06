/*
 *  Copyright (C) 2026 Team Kodi / Glas
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

/*!
\file GlasFade.h
\brief Kurve der Ueberblendung von Bildern (skin.glas): <fadetime tween="sine" easing="inout">400</fadetime>.

 Kodi blendet beim Bildwechsel (fadetime) immer linear. Mit tween/easing (oder spring, GlasTween.h) am Tag fadetime
 folgt die Ueberblendung derselben Kurve wie eine Animation. Kodi ohne diesen Build blendet weiter linear. Ablage ohne
 neue Felder in Kodis Kopfdateien (je Bild-Control, von der Fabrik gesetzt; Kopien in Listen-Layouts blenden linear).
*/

#include "Tween.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace GLAS_FADE
{
// gesperrt: Controls werden auch in anderen Faeden zerstoert (Forget); leer ohne Sperre
struct State
{
  std::mutex lock;
  std::unordered_map<const void*, std::shared_ptr<Tweener>> table;
  std::atomic<size_t> entries{0};
};

inline State& Get()
{
  static State state;
  return state;
}

inline void Set(const void* image, std::shared_ptr<Tweener> tweener)
{
  if (!tweener)
    return;
  State& s = Get();
  std::unique_lock<std::mutex> guard(s.lock);
  s.table[image] = std::move(tweener);
  s.entries = s.table.size();
}

//! Kurve des Bilds (leer = linear); geteilt, damit sie auch bei gleichzeitigem Forget gueltig bleibt
inline std::shared_ptr<Tweener> Get(const void* image)
{
  State& s = Get();
  if (s.entries == 0)
    return {};
  std::unique_lock<std::mutex> guard(s.lock);
  auto i = s.table.find(image);
  return i != s.table.end() ? i->second : std::shared_ptr<Tweener>();
}

inline void Forget(const void* image)
{
  State& s = Get();
  if (s.entries == 0)
    return;
  std::unique_lock<std::mutex> guard(s.lock);
  s.table.erase(image);
  s.entries = s.table.size();
}
} // namespace GLAS_FADE
