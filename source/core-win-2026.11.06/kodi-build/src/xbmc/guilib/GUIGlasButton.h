/*
 *  Copyright (C) 2026 Team Kodi / Glas
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

/*!
\file GUIGlasButton.h
\brief Skin tag <glass> on buttons: Live-Glas (CGUIBackdropBlurControl) under the button's own plate, for skin.glas.

 Works on button, radiobutton, colorbutton, edit, spincontrolex and sliderex (the last two through their inner button).
 Meant for rows Kodi builds itself from the default controls (settings categories, add-on settings), where the skin
 cannot put a backdropblur control underneath.

   <glass cornerradius="20" blurradius="48" saturation="1.5" style="liquid" focus="false" condition="..." />

 Defaults: blurradius 48, saturation 1.5, style liquid, focus false (no glass while the button has focus: the focus
 texture covers the row). condition is an info bool (attribute "condition", so $EXP[] works); empty = always.
 Kodi without this build ignores the unknown tag.

 Skin tag <focusshadow> on the same buttons: a texture under the focused button that may reach beyond it (shadow of
 the focused settings row, tvOS 27.2 Hell). left/top/right/bottom = how far it reaches out on each side.

   <focusshadow left="40" top="34" right="40" bottom="40" condition="...">
     <texture border="71" colordiffuse="row_shadow">glas/shadow-landscape-20.png</texture>
   </focusshadow>

 Kodi gives every settings row the full width of its grouplist and the grouplist clips to its rectangle, so the shadow
 is drawn without the innermost clip region (the grouplist's); outer ones (a dialog) stay.

 Skin tags <textcolor2> and <focusedcolor2> on the same buttons: colors of label2 (the value on the right of a
 settings row; on spincontrolex and sliderex their value text), which Kodi otherwise draws in the colors of label (tvOS 27.2 Hell: values 105 grey, also on the
 focused row). Kodi without this build ignores them.
*/

#include "DirtyRegion.h"
#include "interfaces/info/InfoBool.h"

#include <memory>

class CGUIBackdropBlurControl;
class CGUIControl;
class CGUIImage;
class TiXmlElement;

class CGlasButtonGlass
{
public:
  CGlasButtonGlass();
  CGlasButtonGlass(const CGlasButtonGlass& from);
  CGlasButtonGlass& operator=(const CGlasButtonGlass& from);
  ~CGlasButtonGlass();

  //! <glass> and <focusshadow> of a control node: hang them onto the button (factory, after the control was created)
  static void FromXml(CGUIControl* control, const TiXmlElement* node, int parentID);

  /*! Before the button's textures: follow position and size, show depending on condition and focus.
      Returns true if the glass appeared or disappeared (the button marks itself dirty). */
  bool Process(const CGUIControl& owner, float posX, float posY, float width, float height,
               unsigned int currentTime, CDirtyRegionList& dirtyregions);
  //! Shadow, then glass - both under the button's textures
  void Render();
  void FreeResources(bool immediately);

private:
  void RenderShadow();

  std::unique_ptr<CGUIBackdropBlurControl> m_glass;
  INFO::InfoPtr m_condition;
  bool m_onFocus = false;
  bool m_shown = false;

  std::unique_ptr<CGUIImage> m_shadow;
  INFO::InfoPtr m_shadowCondition;
  float m_shadowLeft = 0.0f;
  float m_shadowTop = 0.0f;
  float m_shadowRight = 0.0f;
  float m_shadowBottom = 0.0f;
  bool m_shadowShown = false;
};
