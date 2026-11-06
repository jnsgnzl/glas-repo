/*
 *  Copyright (C) 2026 Team Kodi / Glas
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

/*!
\file GlasBand.h
\brief Gummiband am Ende einer Liste (skin.glas): ueber das Ende hinaus federt die Liste kurz nach wie in tvOS.

 Kodi bleibt am Ende einer Liste oder Grouplist stumm stehen. Hier federt sie: Hoch/Runter (Links/Rechts bei
 waagerechten Listen) am Ende, wenn der Fokus nirgendwohin wandert, und das Mausrad, wenn die Liste nicht weiter rollt.
 Es federt die aeusserste Grouplist gleicher Richtung um den Eintrag (die ganze Spalte, z. B. Einstellungen und die
 Mediathek-Leiste), sonst die Liste selbst; quer zur Liste nichts. 18 px in Skin-Koordinaten hinaus (110 ms), dann
 zurueck (380 ms). Gezeichnet in CGUIControl::Animate als Verschiebung des ganzen Controls, wie es der Skin vorher mit
 einer Slide-Animation an Liste 5700 tat. Mit Skin.HasSetting(glas_reduce_motion) nichts.
*/

#include "GUIComponent.h"
#include "GUIControl.h"
#include "GUIControlGroupList.h"
#include "GUIInfoManager.h"
#include "ServiceBroker.h"
#include "guilib/GUIAction.h"
#include "input/actions/ActionIDs.h"
#include "interfaces/info/Info.h"
#include "interfaces/info/InfoBool.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/TimeUtils.h"
#include "utils/TransformMatrix.h"

#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace GLAS_BAND
{
constexpr float AMPLITUDE = 18.0f; // Skin-Pixel
constexpr unsigned int OUT_MS = 110;
constexpr unsigned int BACK_MS = 380;
constexpr unsigned int AGAIN_MS = 250; // erneuter Anstoss erst danach (gehaltene Taste federt wieder, nicht stehend)

struct Entry
{
  unsigned int start;
  float dx;
  float dy;
};

// gesperrt: Controls werden auch in anderen Faeden zerstoert (Forget)
struct State
{
  std::mutex lock;
  std::unordered_map<const void*, Entry> table;
  std::atomic<size_t> entries{0};
};

inline State& Get()
{
  static State state;
  return state;
}

inline bool ReduceMotion()
{
  CGUIComponent* gui = CServiceBroker::GetGUI();
  if (!gui)
    return false;
  static INFO::InfoPtr reduce = gui->GetInfoManager().Register("Skin.HasSetting(glas_reduce_motion)");
  return reduce && reduce->Get(INFO::DEFAULT_CONTEXT);
}

//! Mit dem Glas-Skin laufen Listen und Grouplists ohne eigenes Ziel nicht im Kreis (tvOS kennt das nicht; Kodi springt
//! sonst vom Anfang ans Ende und umgekehrt, und es gaebe kein Ende zum Federn). Ein ausdrueckliches Ziel auf sich selbst
//! (<onup>eigene id</onup>) laeuft weiter im Kreis; andere Skins bleiben bei Kodi.
inline bool GlasSkin()
{
  const auto component = CServiceBroker::GetSettingsComponent();
  const auto settings = component ? component->GetSettings() : nullptr;
  return settings && settings->GetString(CSettings::SETTING_LOOKANDFEEL_SKIN) == "skin.glas";
}

inline bool NoWrap()
{
  return GlasSkin();
}

//! Ersatz fuer Kodis "!action.HasActionsMeetingCondition() || action.GetNavigation() == id"
inline bool Wraps(const CGUIAction& action, int id)
{
  return action.GetNavigation() == id || (!action.HasActionsMeetingCondition() && !NoWrap());
}

//! Anstossen: dx/dy = Richtung der Auslenkung (-1, 0, 1)
inline void Kick(const CGUIControl* control, float dx, float dy)
{
  if (!control || ReduceMotion())
    return;
  const unsigned int now = CTimeUtils::GetFrameTime();
  State& s = Get();
  std::unique_lock<std::mutex> guard(s.lock);
  auto i = s.table.find(control);
  if (i != s.table.end() && now - i->second.start < AGAIN_MS && i->second.dx == dx && i->second.dy == dy)
    return;
  s.table[control] = Entry{now, dx, dy};
  s.entries = s.table.size();
}

//! Nach Hoch/Runter/Links/Rechts: hat der Fokus das Control nicht verlassen, federt die passende Liste.
//! ownVertical: 1 = senkrechte Liste, 0 = waagerechte Liste, -1 = kein Container (nur umgebende Grouplists)
inline void AfterMove(CGUIControl* control, int action, int ownVertical)
{
  if (!control || !control->HasFocus())
    return;
  const bool vertical = action == ACTION_MOVE_UP || action == ACTION_MOVE_DOWN;
  if (!vertical && action != ACTION_MOVE_LEFT && action != ACTION_MOVE_RIGHT)
    return;
  const CGUIControl* target = nullptr;
  if (ownVertical >= 0 && (ownVertical == 1) == vertical)
    target = control;
  for (CGUIControl* p = control->GetParentControl(); p; p = p->GetParentControl())
  {
    const CGUIControlGroupList* list = dynamic_cast<const CGUIControlGroupList*>(p);
    if (list && (list->GetOrientation() == VERTICAL) == vertical)
      target = list;
  }
  if (!target)
    return;
  const float sign = (action == ACTION_MOVE_UP || action == ACTION_MOVE_LEFT) ? 1.0f : -1.0f;
  Kick(target, vertical ? 0.0f : sign, vertical ? sign : 0.0f);
}

//! In CGUIControl::Animate: Verschiebung anhaengen; true, solange es federt (und im Bild danach, zum Aufraeumen)
inline bool Apply(const CGUIControl* control, TransformMatrix& transform, unsigned int now)
{
  State& s = Get();
  if (s.entries == 0)
    return false;
  std::unique_lock<std::mutex> guard(s.lock);
  auto i = s.table.find(control);
  if (i == s.table.end())
    return false;
  const unsigned int t = now - i->second.start;
  if (t >= OUT_MS + BACK_MS || now < i->second.start)
  {
    s.table.erase(i);
    s.entries = s.table.size();
    return true;
  }
  float d;
  if (t < OUT_MS)
    d = std::sin(static_cast<float>(t) / OUT_MS * 1.5707964f);
  else
  {
    const float u = 1.0f - static_cast<float>(t - OUT_MS) / BACK_MS;
    d = u * u * u;
  }
  transform *= TransformMatrix::CreateTranslation(i->second.dx * d * AMPLITUDE, i->second.dy * d * AMPLITUDE);
  return true;
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
} // namespace GLAS_BAND
