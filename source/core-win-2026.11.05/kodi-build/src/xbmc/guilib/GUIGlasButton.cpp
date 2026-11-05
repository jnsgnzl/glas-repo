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
#include "GUIControlFactory.h"
#include "GUIImage.h"
#include "GUIInfoManager.h"
#include "GUILabel.h"
#include "GUISettingsSliderControl.h"
#include "GUISpinControlEx.h"
#include "ServiceBroker.h"
#include "utils/StringUtils.h"
#include "utils/XBMCTinyXML.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

CGlasButtonGlass::CGlasButtonGlass() = default;

// Kopie (Kodi klont die Vorgabe-Controls fuer jede Einstellungszeile): eigenes Glas-Control und eigener Schatten je Zeile
CGlasButtonGlass::CGlasButtonGlass(const CGlasButtonGlass& from)
  : m_glass(from.m_glass ? std::make_unique<CGUIBackdropBlurControl>(*from.m_glass) : nullptr),
    m_condition(from.m_condition),
    m_onFocus(from.m_onFocus),
    m_shadow(from.m_shadow ? std::make_unique<CGUIImage>(*from.m_shadow) : nullptr),
    m_shadowCondition(from.m_shadowCondition),
    m_shadowLeft(from.m_shadowLeft),
    m_shadowTop(from.m_shadowTop),
    m_shadowRight(from.m_shadowRight),
    m_shadowBottom(from.m_shadowBottom)
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
    m_shadow = from.m_shadow ? std::make_unique<CGUIImage>(*from.m_shadow) : nullptr;
    m_shadowCondition = from.m_shadowCondition;
    m_shadowLeft = from.m_shadowLeft;
    m_shadowTop = from.m_shadowTop;
    m_shadowRight = from.m_shadowRight;
    m_shadowBottom = from.m_shadowBottom;
    m_shadowShown = false;
  }
  return *this;
}

CGlasButtonGlass::~CGlasButtonGlass() = default;

void CGlasButtonGlass::FromXml(CGUIControl* control, const TiXmlElement* node, int parentID)
{
  const TiXmlElement* glass = node ? node->FirstChildElement("glass") : nullptr;
  const TiXmlElement* shadow = node ? node->FirstChildElement("focusshadow") : nullptr;
  const TiXmlElement* textColor2 = node ? node->FirstChildElement("textcolor2") : nullptr;
  const TiXmlElement* focusedColor2 = node ? node->FirstChildElement("focusedcolor2") : nullptr;
  if (!control || (!glass && !shadow && !textColor2 && !focusedColor2))
    return;

  // Knopf finden: selbst einer (auch radiobutton, colorbutton, edit) oder der innere Knopf von Drehfeld/Regler
  CGUIButtonControl* button = dynamic_cast<CGUIButtonControl*>(control);
  CGUILabel* value = button ? &button->GlasLabel2() : nullptr; // Wert rechts: label2 bzw. Text von Drehfeld/Regler
  if (!button)
  {
    if (auto* spin = dynamic_cast<CGUISpinControlEx*>(control))
    {
      button = &spin->GlasButton();
      value = &spin->GlasValueLabel();
    }
    else if (auto* slider = dynamic_cast<CGUISettingsSliderControl*>(control))
    {
      button = &slider->GlasButton();
      value = &slider->GlasValueLabel();
    }
  }
  if (!button)
  {
    CLog::Log(LOGWARNING, "Glas: <glass>/<focusshadow>/<textcolor2>/<focusedcolor2> gehen nur an Knoepfen (Control {})",
              control->GetID());
    return;
  }

  // Farben des Werts rechts (label2, Text von Drehfeld und Regler): Kodi faerbt ihn wie label (Hell: 105 statt schwarz)
  if ((textColor2 || focusedColor2) && value)
  {
    CLabelInfo& info2 = value->GetLabelInfo();
    if (textColor2)
      CGUIControlFactory::GetInfoColor(node, "textcolor2", info2.textColor, parentID);
    if (focusedColor2)
      CGUIControlFactory::GetInfoColor(node, "focusedcolor2", info2.focusedColor, parentID);
  }

  CGlasButtonGlass& target = button->Glas();
  auto& infoManager = CServiceBroker::GetGUI()->GetInfoManager();

  if (glass)
  {
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

    target.m_glass = std::move(blur);
    const char* focus = glass->Attribute("focus");
    target.m_onFocus = focus && StringUtils::EqualsNoCase(focus, "true");
    const char* condition = glass->Attribute("condition");
    target.m_condition = (condition && *condition) ? infoManager.Register(condition, parentID) : nullptr;
    target.m_shown = false;
  }

  if (shadow)
  {
    CTextureInfo texture;
    if (!CGUIControlFactory::GetTexture(shadow, "texture", texture) || texture.filename.empty())
    {
      CLog::Log(LOGWARNING, "Glas: <focusshadow> ohne <texture> (Control {})", control->GetID());
      return;
    }
    target.m_shadow = std::make_unique<CGUIImage>(parentID, 0, 0.0f, 0.0f, 0.0f, 0.0f, texture);
    shadow->QueryFloatAttribute("left", &target.m_shadowLeft);
    shadow->QueryFloatAttribute("top", &target.m_shadowTop);
    shadow->QueryFloatAttribute("right", &target.m_shadowRight);
    shadow->QueryFloatAttribute("bottom", &target.m_shadowBottom);
    const char* condition = shadow->Attribute("condition");
    target.m_shadowCondition =
        (condition && *condition) ? infoManager.Register(condition, parentID) : nullptr;
    target.m_shadowShown = false;
  }
}

bool CGlasButtonGlass::Process(const CGUIControl& owner, float posX, float posY, float width, float height,
                               unsigned int currentTime, CDirtyRegionList& dirtyregions)
{
  bool changed = false;

  if (m_shadow)
  {
    // Schatten nur unter dem fokussierten Knopf. Immer verarbeiten, auch versteckt: dann meldet das Bild seine
    // Flaeche (die ueber den Knopf hinausreicht) beim Verschwinden selbst als geaendert.
    const bool shadowShown =
        owner.HasFocus() && (!m_shadowCondition || m_shadowCondition->Get(owner.GetParentID()));
    if (shadowShown && !m_shadowShown)
      m_shadow->AllocResources();
    m_shadowShown = shadowShown;
    m_shadow->SetVisible(shadowShown);
    m_shadow->SetPosition(posX - m_shadowLeft, posY - m_shadowTop);
    m_shadow->SetWidth(width + m_shadowLeft + m_shadowRight);
    m_shadow->SetHeight(height + m_shadowTop + m_shadowBottom);
    m_shadow->DoProcess(currentTime, dirtyregions);
  }

  if (!m_glass)
    return changed;
  const bool shown = (m_onFocus || !owner.HasFocus()) &&
                     (!m_condition || m_condition->Get(owner.GetParentID()));
  changed = shown != m_shown;
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

void CGlasButtonGlass::RenderShadow()
{
  if (!m_shadow || !m_shadowShown)
    return;
  // Ohne den innersten Ausschnitt zeichnen (den der Grouplist: Kodi macht jede Einstellungszeile so breit wie die
  // Grouplist, dort wuerde der Schatten seitlich und unter der letzten Zeile abgeschnitten). Der Ausschnitt ist in
  // denselben Koordinaten wie die Controls (Kodi beschneidet in Software); GetClipRegion zieht den Ursprung ab,
  // SetClipRegion zaehlt ihn wieder dazu - so liegt danach genau derselbe Ausschnitt wieder oben auf dem Stapel.
  // Ohne Ausschnitt liefert GetClipRegion den ganzen Bildschirm, dann bleibt alles, wie es ist.
  CGraphicContext& gfx = CServiceBroker::GetWinSystem()->GetGfxContext();
  const CRect clip = gfx.GetClipRegion();
  const bool clipped = clip != CRect(0, 0, gfx.GetWidth(), gfx.GetHeight());
  if (clipped)
    gfx.RestoreClipRegion();
  m_shadow->DoRender();
  if (clipped)
    gfx.SetClipRegion(clip.x1, clip.y1, clip.Width(), clip.Height());
}

void CGlasButtonGlass::Render()
{
  RenderShadow();
  if (m_glass && m_shown)
    m_glass->DoRender();
}

void CGlasButtonGlass::FreeResources(bool immediately)
{
  if (m_glass)
    m_glass->FreeResources(immediately);
  if (m_shadow)
    m_shadow->FreeResources(immediately);
}
