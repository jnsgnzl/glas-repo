/*
 *  Copyright (C) 2026 Team Kodi / Glas
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

/*!
\file GlasLoadHidden.h
\brief Skin-Tag <loadhidden>true</loadhidden> an Listen (skin.glas, Pruefplan LS9): die Liste laedt ihren Inhalt auch
dann nach, wenn die Gruppe um sie herum ausgeblendet ist.

 Die Suche blendet eine Ergebnisreihe (Gruppe aus Kopf und Liste in der Grouplist 9200) aus, solange sie nichts
 findet. Kodi verarbeitet ausgeblendete Gruppen nicht, die Liste darin fragt bei einem neuen Begriff also nie neu ab
 - eine Reihe kaeme nach einem leeren Ergebnis nicht wieder. Bisher zaehlten deshalb fuenf unsichtbare Lade-Listen
 dieselben Pfade noch einmal (neun Abfragen je Tastendruck statt vier). Mit diesem Tag ruft die Grouplist fuer
 ausgeblendete Gruppen UpdateVisibility der markierten Listen darin selbst auf (CGUIBaseContainer laedt dort nach).
 Den Fokus bekommen sie nicht: die Gruppe bleibt ausgeblendet und damit nicht fokussierbar.

 Andere Wege scheiterten an Kodi: allowhiddenfocus an der Liste laedt zwar nach, doch eine leere Liste haelt noch
 den Eintrag ".." und naehme ausgeblendet den Fokus an; als direktes Kind der Grouplist ueberschreibt diese ausserdem
 ihre Navigation (CGUIControlGroupList::AddControl).
 Ablage ohne neue Felder in Kodis Kopfdateien (wie GlasTopFade.h): die Fabrik merkt sich die Liste hier.
*/

#include "GUIControlGroup.h"

#include <atomic>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace GLAS_LOADHIDDEN
{
struct State
{
  std::mutex lock;
  std::unordered_set<const void*> lists;
  std::atomic<size_t> entries{0};
};

inline State& Get()
{
  static State* state = new State; // nie zerstoeren: Controls koennen nach den statischen Objekten sterben
  return *state;
}

inline void Set(const void* list)
{
  State& s = Get();
  std::unique_lock<std::mutex> guard(s.lock);
  s.lists.insert(list);
  s.entries = s.lists.size();
}

inline bool Has(const void* list)
{
  State& s = Get();
  if (s.entries == 0)
    return false;
  std::unique_lock<std::mutex> guard(s.lock);
  return s.lists.count(list) != 0;
}

inline void Forget(const void* list)
{
  State& s = Get();
  if (s.entries == 0)
    return;
  std::unique_lock<std::mutex> guard(s.lock);
  s.lists.erase(list);
  s.entries = s.lists.size();
}

//! Zugriff auf die Kinder einer Gruppe (geschuetzt in CGUIControlGroup)
struct Children : public CGUIControlGroup
{
  static const std::vector<CGUIControl*>& Of(const CGUIControlGroup& group)
  {
    return static_cast<const Children&>(group).m_children;
  }
};

inline void Walk(CGUIControl* control)
{
  for (CGUIControl* child : Children::Of(*static_cast<CGUIControlGroup*>(control)))
  {
    if (child->IsContainer())
    {
      if (Has(child))
        child->UpdateVisibility(nullptr);
    }
    else if (child->IsGroup())
      Walk(child);
  }
}

//! Aus CGUIControlGroupList::Process nach UpdateVisibility eines Kindes: ausgeblendete Gruppe - markierte Listen
//! darin trotzdem nachladen lassen
inline void Update(CGUIControl* control)
{
  if (Get().entries == 0 || !control->IsGroup() || control->IsContainer() || control->IsVisible())
    return;
  Walk(control);
}
} // namespace GLAS_LOADHIDDEN
