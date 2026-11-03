/*
 *  Copyright (C) 2026 Team Kodi / Glas
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GUIGlasButton.h"

#include "GUIBackdropBlurControl.h"
#include "GUIButtonControl.h"
#include "GUIComponent.h"
#include "GUIInfoManager.h"
#include "GUISettingsSliderControl.h"
#include "GUISpinControlEx.h"
#include "ServiceBroker.h"
#include "utils/StringUtils.h"
#include "utils/XBMCTinyXML.h"
#include "utils/log.h"

CGlasButtonGlass::CGlasButtonGlass() = default;

// Kopie (Kodi klont die Vorgabe-Controls fuer jede Einstellungszeile): eigenes Glas-Control je Zeile
CGlasButtonGlass::CGlasButtonGlass(const CGlasButtonGlass& from)
  : m_glass(from.m_glass ? std::make_unique<CGUIBackdropBlurControl>(*from.m_glass) : nullptr),
    m_condition(from.m_condition),
    m_onFocus(from.m_onFocus)
{
}

CGlasButtonGlass& CGlasButtonGlass::operator=(const CGlasButtonGlass& from)
{
  if (this != &from)
  {
    m_glass = from.m_glass ? std::make_unique<CGUIBackdropBlurControl>(*from.m_glass) : nullptr;
    m_condition = from.m_condition;
    m_onFocus = from.m_onFocus;
    m_shown = false;
  }
  return *this;
}

CGlasButtonGlass::~CGlasButtonGlass() = default;

void CGlasButtonGlass::FromXml(CGUIControl* control, const TiXmlElement* node, int parentID)
{
  const TiXmlElement* glass = node ? node->FirstChildElement("glass") : nullptr;
  if (!control || !glass)
    return;

  // Knopf finden: selbst einer (auch radiobutton, colorbutton, edit) oder der innere Knopf von Drehfeld/Regler
  CGUIButtonControl* button = dynamic_cast<CGUIButtonControl*>(control);
  if (!button)
  {
    if (auto* spin = dynamic_cast<CGUISpinControlEx*>(control))
      button = &spin->GlasButton();
    else if (auto* slider = dynamic_cast<CGUISettingsSliderControl*>(control))
      button = &slider->GlasButton();
  }
  if (!button)
  {
    CLog::Log(LOGWARNING, "Glas: <glass> geht nur an Knoepfen (Control {})", control->GetID());
    return;
  }

  auto blur = std::make_unique<CGUIBackdropBlurControl>(parentID, 0, 0.0f, 0.0f, 0.0f, 0.0f);
  blur->SetBlurRadius(48.0f);
  blur->SetSaturation(1.5f);
  blur->SetLiquid(true);
  float value;
  if (glass->QueryFloatAttribute("cornerradius", &value) == TIXML_SUCCESS)
    blur->SetCornerRadius(value);
  if (glass->QueryFloatAttribute("blurradius", &value) == TIXML_SUCCESS)
    blur->SetBlurRadius(value);
  if (glass->QueryFloatAttribute("saturation", &value) == TIXML_SUCCESS)
    blur->SetSaturation(value);
  if (const char* style = glass->Attribute("style"))
    blur->SetLiquid(StringUtils::EqualsNoCase(style, "liquid"));

  CGlasButtonGlass& target = button->Glas();
  target.m_glass = std::move(blur);
  const char* focus = glass->Attribute("focus");
  target.m_onFocus = focus && StringUtils::EqualsNoCase(focus, "true");
  const char* condition = glass->Attribute("condition");
  target.m_condition = (condition && *condition)
                           ? CServiceBroker::GetGUI()->GetInfoManager().Register(condition, parentID)
                           : nullptr;
  target.m_shown = false;
}

bool CGlasButtonGlass::Process(const CGUIControl& owner, float posX, float posY, float width, float height,
                               unsigned int currentTime, CDirtyRegionList& dirtyregions)
{
  if (!m_glass)
    return false;
  const bool shown = (m_onFocus || !owner.HasFocus()) &&
                     (!m_condition || m_condition->Get(owner.GetParentID()));
  const bool changed = shown != m_shown;
  m_shown = shown;
  if (!shown)
    return changed;
  // Lage des Knopfs (gleiches Koordinatensystem: wird im selben Ursprung und mit derselben Transformation gezeichnet)
  m_glass->SetPosition(posX, posY);
  m_glass->SetWidth(width);
  m_glass->SetHeight(height);
  m_glass->DoProcess(currentTime, dirtyregions);
  return changed;
}

void CGlasButtonGlass::Render()
{
  if (m_glass && m_shown)
    m_glass->DoRender();
}

void CGlasButtonGlass::FreeResources(bool immediately)
{
  if (m_glass)
    m_glass->FreeResources(immediately);
}
