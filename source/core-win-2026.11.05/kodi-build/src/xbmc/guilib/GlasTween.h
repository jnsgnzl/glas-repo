/*
 *  Copyright (C) 2026 Team Kodi / Glas
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

/*!
\file GlasTween.h
\brief Federkurve fuer Animationen und Rollen (skin.glas): spring="<bounce>" neben tween/easing.

 tvOS bewegt fast alles mit Federn (Fokus, Popover, Seitenleiste). Kodi kennt nur feste Kurven; "back" schwingt immer
 um 10 % der Strecke ueber. Die Feder ist ein gedaempfter Schwinger, der genau am Ende der Animation (time) zur Ruhe
 kommt:

   <animation effect="zoom" end="110" time="200" tween="cubic" easing="out" spring="0.3">Focus</animation>
   <scrolltime tween="cubic" easing="out" spring="0">300</scrolltime>

 spring = bounce wie bei Apple (SwiftUI spring(duration:bounce:)): 0 = ohne Ueberschwingen (kritisch gedaempft),
 0,3 = etwa 5 % Ueberschwingen, hoechstens 0,9. Daempfung zeta = 1 - bounce. easing wie bei Kodi: out (Standard)
 = Feder am Ende, in = gespiegelt, inout = beides. Kodi ohne diesen Build kennt das Attribut nicht und nimmt tween.
*/

#include "Tween.h"

#include <algorithm>
#include <cmath>

class GlasSpringTweener : public Tweener
{
public:
  explicit GlasSpringTweener(float bounce)
  {
    m_zeta = 1.0f - std::clamp(bounce, 0.0f, 0.9f);
    if (m_zeta >= 0.999f)
    {
      // kritisch gedaempft: e^-w (1 + w) = 0,001 bei w = 9,233
      m_omega = 9.233f;
    }
    else
    {
      // Huelle e^(-zeta w t) bei t = 1 auf 0,1 %: zeta w = ln 1000
      m_omega = 6.9078f / m_zeta;
      m_omegaD = m_omega * std::sqrt(1.0f - m_zeta * m_zeta);
    }
    m_end = Raw(1.0f);
  }

  float Tween(float time, float start, float change, float duration) override
  {
    const float t = duration > 0.0f ? std::clamp(time / duration, 0.0f, 1.0f) : 1.0f;
    float p;
    switch (m_tweenerType)
    {
      case EASE_IN:
        p = 1.0f - Curve(1.0f - t);
        break;
      case EASE_INOUT:
        p = t < 0.5f ? (1.0f - Curve(1.0f - 2.0f * t)) * 0.5f : 0.5f + Curve(2.0f * t - 1.0f) * 0.5f;
        break;
      default:
        p = Curve(t);
        break;
    }
    return start + change * p;
  }

  bool HasResumePoint() const override { return false; }

private:
  //! Auslenkung des Schwingers (0 -> 1), am Ende auf 0,1 % genau
  float Raw(float t) const
  {
    if (m_omegaD == 0.0f)
      return 1.0f - std::exp(-m_omega * t) * (1.0f + m_omega * t);
    const float decay = std::exp(-m_zeta * m_omega * t);
    return 1.0f - decay * (std::cos(m_omegaD * t) +
                           m_zeta / std::sqrt(1.0f - m_zeta * m_zeta) * std::sin(m_omegaD * t));
  }

  //! Den Rest von 0,1 % gleichmaessig verteilen: endet genau bei 1 (kein Sprung am Ende der Animation)
  float Curve(float t) const { return Raw(t) + (1.0f - m_end) * t; }

  float m_zeta = 1.0f;
  float m_omega = 9.233f;
  float m_omegaD = 0.0f;
  float m_end = 1.0f;
};
