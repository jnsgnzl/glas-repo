/*
 *  Copyright (C) 2026 Team Kodi / Glas
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

/*!
\file GlasVideoWindow.h
\brief Video-Glas nur, wenn das Video zu sehen ist (skin.glas, Pruefplan K1).

 Das Live-Glas nimmt ueber einem laufenden Film das Videobild (es liegt auf einer eigenen Ebene unter der Oberflaeche,
 ein Griff der Oberflaeche enthaelt es nicht). Vorher galt das, sobald irgendein Film lief - auch auf dem Homescreen,
 der den Film ganz abdeckt: Dock, Uhr und Kontrollzentrum zeigten den weichgezeichneten Film, und jedes Videobild
 zeichnete den ganzen Homescreen neu. Jetzt nur noch im Vollbild-Video oder wenn ein Videofenster (CGUIVideoControl)
 in den letzten 500 ms gezeichnet wurde. CGUIVideoControl::Render meldet sich hier (glas-patches.txt).
*/

#include <atomic>
#include <chrono>
#include <cstdint>

namespace GLAS_VIDEOWINDOW
{
inline int64_t NowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline std::atomic<int64_t> g_lastRender{0};

//! CGUIVideoControl::Render: ein Videofenster zeigt den laufenden Film
inline void NoteRender()
{
  g_lastRender = NowMs();
}

//! wurde in den letzten 500 ms ein Videofenster gezeichnet?
inline bool Seen()
{
  return NowMs() - g_lastRender.load() < 500;
}
} // namespace GLAS_VIDEOWINDOW
