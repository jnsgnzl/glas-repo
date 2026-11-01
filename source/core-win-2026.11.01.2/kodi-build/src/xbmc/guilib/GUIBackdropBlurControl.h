/*
 *  Copyright (C) 2026 Team Kodi / Glas
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

/*!
\file GUIBackdropBlurControl.h
\brief Skin control <control type="backdropblur">: blurs what has already been drawn behind it
       (backdrop blur / frosted glass). Spike for skin.glas, GLES only.

 Tags: blurradius (skin px, default 40), cornerradius (skin px), saturation (default 1.0),
       tintcolor (ARGB, info colors allowed), refresh (auto|always), timing (true logs cost),
       grabscale (2 or 4, default 4: screen is read at 1/grabscale; all controls share one read per frame),
       updaterate (Hz, default 15: re-blur at most this often while the background keeps changing; 0 = every frame),
       grabprobe (true: read only a 64x64 corner, measurement of the fixed cost of a read),
       style (frosted|liquid, default frosted: liquid draws Liquid-Glass material itself - adaptive dark tint,
       refraction at the rim, specular edge light),
       source (id of a control in the same window that is all there is behind the glass, e.g. the Top Shelf
       background behind the dock: drawn small at the start of the frame and blurred there instead of grabbing the
       screen mid-frame; 2D without video only, otherwise it grabs as usual).

 Stereo 3D (GUI drawn once per eye): grab and blur per eye (own cache for the right eye), the glass rectangle
 follows the stereo shift of the control (<depth> x stereostrength) like the textures do. Video glass is off
 in stereo (switch /tmp/glas-stereo-video turns it on for tests).
*/

#include "GUIControl.h"
#include "guilib/guiinfo/GUIInfoColor.h"
#include "utils/Geometry.h"


class CGUIBackdropBlurControl : public CGUIControl
{
public:
  CGUIBackdropBlurControl(int parentID, int controlID, float posX, float posY, float width, float height);
  CGUIBackdropBlurControl(const CGUIBackdropBlurControl& from);
  ~CGUIBackdropBlurControl() override;
  CGUIBackdropBlurControl* Clone() const override { return new CGUIBackdropBlurControl(*this); }

  void Process(unsigned int currentTime, CDirtyRegionList& dirtyregions) override;
  void Render() override;
  void FreeResources(bool immediately = false) override;
  bool CanFocus() const override { return false; }

  void SetBlurRadius(float radius) { m_blurRadius = radius; }
  void SetCornerRadius(float radius) { m_cornerRadius = radius; }
  void SetSaturation(float saturation) { m_saturation = saturation; }
  void SetTint(const KODI::GUILIB::GUIINFO::CGUIInfoColor& tint) { m_tint = tint; }
  void SetAlwaysRefresh(bool always) { m_alwaysRefresh = always; }
  void SetTiming(bool timing) { m_timing = timing; }
  void SetGrabScale(int scale) { m_grabScale = scale; }
  void SetUpdateRate(float rate) { m_updateRate = rate; }
  void SetGrabProbe(bool probe) { m_grabProbe = probe; }
  void SetLiquid(bool liquid) { m_liquid = liquid; }
  void SetSource(int id) { m_sourceId = id; }

private:
  float m_blurRadius = 40.0f;
  float m_cornerRadius = 0.0f;
  float m_saturation = 1.0f;
  KODI::GUILIB::GUIINFO::CGUIInfoColor m_tint;
  bool m_alwaysRefresh = false;
  bool m_timing = false;
  bool m_backgroundDirty[2] = {true, true}; //!< je Auge (0 = 2D/links, 1 = rechts)
  unsigned int m_lastProcess = 0;
  unsigned int m_frame = 0;
  int m_grabScale = 4;
  float m_updateRate = 15.0f;
  bool m_grabProbe = false;
  bool m_pendingRecompute[2] = {false, false}; //!< je Auge
  bool m_liquid = false;
  int m_sourceId = 0;
  bool m_fromSource = false; //!< im vorigen Bild aus der Quelle gezeichnet
  unsigned int m_videoSeq = 0;
  CRect m_screenRect;
};
