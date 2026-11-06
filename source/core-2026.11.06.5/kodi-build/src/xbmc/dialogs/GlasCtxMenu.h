/*
 *  Copyright (C) 2026 Team Kodi / Glas
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

/*!
\file GlasCtxMenu.h
\brief Kontextmenue wie tvOS (skin.glas): Symbol links vor jedem Eintrag, zerstoerende Eintraege rot.

 tvOS 27.2 zeigt im Kontext- und Popover-Menue links ein Symbol je Eintrag, zerstoerende Aktionen in roter Schrift und im
 Fokus als rote Kapsel mit weisser Schrift (docs/tvos27-simulator-messwerte.md, Popover-Menues). Kodi kennt das nicht.
 CGUIDialogContextMenu::SetupButtons fragt hier nach jedem Eintrag: das Symbol erkennt der Kern an Kodis Text-IDs (in
 jeder Sprache), es steht als Fenster-Eigenschaft glas.ctx.icon.<n> (n ab 0) fuer den Skin bereit (Bild
 glas/ctx-<name>.png, Grouplist 997 in DialogContextMenu.xml), glas.ctx.icons = 1, sobald ein Eintrag eines hat. Der Text
 rueckt dann bei allen Eintraegen hinter das Symbol. Zerstoerende Eintraege (Loeschen, Entfernen, aus der Bibliothek, aus
 den Favoriten, Quelle entfernen) faerbt der Kern selbst: Schrift rot, im Fokus weiss auf roter Kapsel. Nur mit dem
 Glas-Skin; bei mehr Eintraegen als sichtbaren Zeilen (Liste rollt) keine Symbole.
*/

#include "guilib/GUIButtonControl.h"
#include "guilib/GUIWindow.h"
#include "guilib/GlasBand.h"
#include "guilib/LocalizeStrings.h"
#include "utils/StringUtils.h"
#include "utils/Variant.h"

#include <string>
#include <utility>
#include <vector>

namespace GLAS_CTXMENU
{
constexpr unsigned int MAX_ROWS = 11;   // DialogContextMenu.xml: Grouplist 996 hoechstens 764 hoch (Zeilen 66)
constexpr float TEXT_SHIFT = 47.0f;     // Text 34 -> 81 vom Rand der Flaeche (Symbol bei 37, 30 breit)
constexpr unsigned int MAX_PROPS = 40;

struct Kind
{
  const char* icon;
  bool destructive;
};

inline Kind Classify(const std::string& label)
{
  auto is = [&label](std::initializer_list<int> ids) {
    for (int id : ids)
    {
      const std::string& s = g_localizeStrings.Get(id);
      if (!s.empty() && label == s)
        return true;
    }
    return false;
  };
  auto starts = [&label](int id) {
    std::string s = g_localizeStrings.Get(id);
    const size_t p = s.find('{');
    if (p != std::string::npos)
      s = s.substr(0, p);
    StringUtils::TrimRight(s);
    return !s.empty() && StringUtils::StartsWith(label, s);
  };
  if (is({117, 1210, 15015, 646, 522}))
    return {"trash", true};
  if (is({14077}))
    return {"star", true};
  if (is({208, 13412, 13434, 13404}) || starts(12022))
    return {"play", false};
  if (is({12021, 38209}))
    return {"restart", false};
  if (is({10008}))
    return {"next", false};
  if (is({13347}))
    return {"queue", false};
  if (is({19033, 14116, 13346, 20351, 20352, 22081, 10524}))
    return {"info", false};
  if (is({16103}))
    return {"watched", false};
  if (is({16104}))
    return {"unwatched", false};
  if (is({14076}))
    return {"star", false};
  if (is({16106}))
    return {"manage", false};
  if (is({118, 21435, 1027}))
    return {"edit", false};
  if (is({184, 13349}))
    return {"refresh", false};
  return {"", false};
}

//! Vor dem Anlegen der Knoepfe: Eigenschaften zuruecksetzen; true, wenn Symbole gezeigt werden
template<class Buttons>
inline bool Begin(CGUIWindow& dialog, const Buttons& buttons)
{
  for (unsigned int i = 0; i < MAX_PROPS; i++)
  {
    dialog.SetProperty("glas.ctx.icon." + std::to_string(i), "");
    dialog.SetProperty("glas.ctx.red." + std::to_string(i), "");
  }
  dialog.SetProperty("glas.ctx.icons", "");
  if (!GLAS_BAND::GlasSkin() || buttons.size() > MAX_ROWS)
    return false;
  bool any = false;
  for (const auto& b : buttons)
    any |= *Classify(b.second).icon != '\0';
  dialog.SetProperty("glas.ctx.icons", any ? "1" : "");
  return any;
}

//! Je Knopf nach SetLabel
inline void Decorate(CGUIWindow& dialog, CGUIButtonControl& button, unsigned int index, const std::string& label,
                     bool icons)
{
  if (!GLAS_BAND::GlasSkin())
    return;
  const Kind kind = Classify(label);
  if (icons && index < MAX_PROPS)
    dialog.SetProperty("glas.ctx.icon." + std::to_string(index), std::string(kind.icon));
  if (icons)
    button.GlasLabel().GetLabelInfo().offsetX += TEXT_SHIFT;
  if (kind.destructive)
  {
    if (index < MAX_PROPS)
      dialog.SetProperty("glas.ctx.red." + std::to_string(index), "1"); // Symbol rot bzw. im Fokus weiss
    button.GlasLabel().GetLabelInfo().textColor = 0xFFFF4F53;     // tvOS: rote Schrift (Dunkel #FF4F53)
    button.GlasLabel().GetLabelInfo().focusedColor = 0xFFFFFFFF;  // im Fokus weiss
    // auf roter Kapsel (#FF4245): ueber die Farbe des Controls, die Kodi in jedem Bild an beide Texturen gibt
    // (CGUIButtonControl::UpdateColors); die ruhende Textur ist leer
    button.SetColorDiffuse(KODI::GUILIB::GUIINFO::CGUIInfoColor(0xFFFF4245));
  }
}
} // namespace GLAS_CTXMENU
