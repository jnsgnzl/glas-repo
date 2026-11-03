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
*/

#include "DirtyRegion.h"
#include "interfaces/info/InfoBool.h"

#include <memory>

class CGUIBackdropBlurControl;
class CGUIControl;
class TiXmlElement;

class CGlasButtonGlass
{
public:
  CGlasButtonGlass();
  CGlasButtonGlass(const CGlasButtonGlass& from);
  CGlasButtonGlass& operator=(const CGlasButtonGlass& from);
  ~CGlasButtonGlass();

  //! <glass> of a control node: hang the glass onto the button (factory, after the control was created)
  static void FromXml(CGUIControl* control, const TiXmlElement* node, int parentID);

  /*! Before the button's textures: follow position and size, show depending on condition and focus.
      Returns true if the glass appeared or disappeared (the button marks itself dirty). */
  bool Process(const CGUIControl& owner, float posX, float posY, float width, float height,
               unsigned int currentTime, CDirtyRegionList& dirtyregions);
  void Render();
  void FreeResources(bool immediately);

private:
  std::unique_ptr<CGUIBackdropBlurControl> m_glass;
  INFO::InfoPtr m_condition;
  bool m_onFocus = false;
  bool m_shown = false;
};
