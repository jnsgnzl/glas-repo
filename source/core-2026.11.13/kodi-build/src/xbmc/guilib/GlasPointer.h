/*
 *  Copyright (C) 2026 Team Kodi / Glas
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

/*!
\file GlasPointer.h
\brief Mauszeiger als Kreis, der in die Fokusflaeche fliesst (skin.glas, A8 im Pruefplan).

 Kodi zeigt im Fenster Pointer.xml je Zustand ein Bild an der Mausposition: 1 = normal, 2 = ueber einem fokussierbaren
 Control (CGUIControl::OnMouseOver). Glas setzt den Fokus beim Darueberfahren, die Fokusflaeche zeigt also schon, wo man
 steht; deshalb ist Bild 2 in Glas leer und der Zeiger verschwindet ueber Knoepfen, Kacheln und Zeilen.

 Hier der Uebergang dazu: Hat Pointer.xml ein Bild mit id 5 (9-Slice-Kreis, Rand = halbe Breite), laesst der Kern es
 beim Wechsel normal -> Fokus vom Kreis auf die Flaeche des Controls wachsen und dabei ausblenden (IN_MS), beim Wechsel
 zurueck von der Flaeche auf den Kreis schrumpfen und einblenden (OUT_MS); Kreis 1 ist solange verborgen. Die Flaeche:
 bei Listen die Kachel im fokussierten Eintrag (NoteShape: das groesste sichtbare Bild unter dem Zeiger, ohne Schatten
 und Leuchten - Texturen mit "shadow" oder "glow" im Namen - und hoechstens 1,6-mal so gross wie die Zelle; die Zelle
 selbst enthaelt Abstaende und Titel), ohne solches Bild die Zelle (GlasFocusItem.h); sonst der Zeichenbereich des
 Controls beim letzten OnMouseOver. Knoepfe ohne Fokusbild (<texturefocus />: Mausfaenger, unsichtbare Fokusflaechen
 der Detailseite, Uhr und Zurueck-Kapseln) zeichnen ihren Fokus, wenn ueberhaupt, mit anderen Controls. Liegt im selben
 Fenster ein sichtbares Bild oder ein Knopf mit "hover" im Texturnamen unter dem Zeiger (Glas: weisse Kapsel der Uhr
 und der Zurueck-Kapseln, Titelkapseln, unsichtbare Form des Banners "Jetzt laeuft"; hover-pill-*.png), gilt es als
 Fokusflaeche (FindHover, hoechstens alle SEARCH_MS gesucht); sonst bleibt der Kreis stehen, denn sonst waere der
 Zeiger weg. Ohne Bild 5 (Estuary, andere Skins) oder solange seine Sichtbarkeitsbedingung nicht gilt (Glas:
 "Mauszeiger: Klassisch") bleibt alles wie bei Kodi.

 Koordinaten: Das Zeigerfenster rechnet ohne Skalierung in Bildschirm-Pixeln (m_needsScaling = false), sein Ursprung
 steht auf der Mausposition; GetRenderRegion und GlasFocusItem liefern ebenfalls Bildschirm-Pixel.

 Messschalter: Zahl in /tmp/glas-pointer-slow (unter Windows <Laufwerk>:\tmp\...) verlangsamt den Uebergang um diesen
 Faktor (fuer Bildschirmfotos mitten im Uebergang), beim Beginn jedes Uebergangs gelesen; dann schreibt der Kern jeden
 Uebergang und jedes Bild darin als GLAS_POINTER ins Kodi-Protokoll.
*/

#include "GlasFocusItem.h"
#include "ServiceBroker.h"
#include "guilib/GUIButtonControl.h"
#include "guilib/GUIControl.h"
#include "guilib/GUIControlGroup.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIImage.h"
#include "guilib/GUIWindow.h"
#include "guilib/GUIWindowManager.h"
#include "input/InputManager.h"
#include "guilib/guiinfo/GUIInfoColor.h"
#include "input/mouse/MouseStat.h"
#include "utils/Geometry.h"
#include "utils/log.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace GLAS_POINTER
{
constexpr int ID_CIRCLE = 1; // Kreis im Zustand normal (Pointer.xml)
constexpr int ID_MORPH = 5; // Uebergangsbild (Pointer.xml), nur der Kern zeigt es
constexpr unsigned int IN_MS = 220; // Kreis -> Flaeche
constexpr unsigned int OUT_MS = 180; // Flaeche -> Kreis
constexpr unsigned int GAP_MS = 300; // laenger nicht verarbeitet = Zeiger war weg, kein Uebergang
constexpr unsigned int SEARCH_MS = 50; // Suche nach Hover-Bildern ueber Knoepfen ohne Fokusbild

//! Letztes Control unter dem Zeiger (nur als Schluessel fuer GlasFocusItem, nie dereferenziert)
struct Hover
{
  const void* key = nullptr;
  bool container = false;
  CRect rect;
  bool valid = false;
  bool unseen = false; // Knopf ohne Fokusbild: Kreis bleibt, ausser ueber einem Hover-Bild
  int window = 0; // Fenster des Knopfs (fuer die Suche nach Hover-Bildern)
  CRect hoverImage;
  bool haveHoverImage = false;
  unsigned int lastSearch = 0;
  CRect shape; // Kachel im fokussierten Eintrag einer Liste (NoteShape)
  bool haveShape = false;
};

inline Hover& LastHover()
{
  static Hover hover;
  return hover;
}

//! Aus CGUIControl::OnMouseOver, sobald das Control fokussierbar ist
inline void Note(const CGUIControl& control)
{
  Hover& h = LastHover();
  if (h.key != &control)
  {
    h.haveShape = false;
    h.haveHoverImage = false;
    h.lastSearch = 0;
  }
  h.key = &control;
  h.container = control.IsContainer();
  h.unseen = control.GetControlType() == CGUIControl::GUICONTROL_BUTTON &&
             static_cast<const CGUIButtonControl&>(control).GlasNoFocusTexture();
  if (h.unseen)
  {
    const CGUIControl* root = &control;
    while (root->GetParentControl())
      root = root->GetParentControl();
    h.window = root->GetID();
  }
  h.rect = control.GetRenderRegion();
  h.valid = true;
}

inline bool Target(CRect& rect)
{
  const Hover& h = LastHover();
  if (!h.valid)
    return false;
  if (h.unseen)
  {
    rect = h.hoverImage;
    return h.haveHoverImage;
  }
  if (h.container && h.haveShape)
  {
    rect = h.shape;
    return true;
  }
  if (h.container && GLAS_FOCUSITEM::Get(h.key, rect) && !rect.IsEmpty())
    return true;
  rect = h.rect;
  return !rect.IsEmpty();
}

//! Kinder einer Gruppe lesen (m_children ist geschuetzt)
struct GroupChildren : public CGUIControlGroup
{
  static const std::vector<CGUIControl*>& Of(const CGUIControlGroup& group)
  {
    return static_cast<const GroupChildren&>(group).m_children;
  }
};

inline void FindShape(const CGUIControl& control, const CPoint& p, float maxArea, CRect& best, float& bestArea)
{
  if (!control.IsVisible())
    return;
  if (control.IsGroup())
  {
    for (const CGUIControl* child : GroupChildren::Of(static_cast<const CGUIControlGroup&>(control)))
      FindShape(*child, p, maxArea, best, bestArea);
    return;
  }
  const CGUIControl::GUICONTROLTYPES type = control.GetControlType();
  if (type != CGUIControl::GUICONTROL_IMAGE && type != CGUIControl::GUICONTROL_BORDEREDIMAGE)
    return;
  const CRect& r = control.GetRenderRegion();
  const float area = r.Area();
  if (area <= maxArea && area > bestArea && r.PtInRect(p))
  {
    const std::string& file = static_cast<const CGUIImage&>(control).GetFileName();
    if (file.find("shadow") != std::string::npos || file.find("glow") != std::string::npos)
      return;
    best = r;
    bestArea = area;
  }
}

//! Groesstes sichtbares Bild (oder Knopf mit solcher Ruhe-Textur) mit "hover" im Texturnamen unter dem Zeiger
inline void FindHover(const CGUIControl& control, const CPoint& p, CRect& best, float& bestArea)
{
  if (!control.IsVisible())
    return;
  if (control.IsGroup())
  {
    for (const CGUIControl* child : GroupChildren::Of(static_cast<const CGUIControlGroup&>(control)))
      FindHover(*child, p, best, bestArea);
    return;
  }
  // Bilder und Knoepfe (Titelkapseln: deaktivierte Knoepfe mit hover-pill als Flaeche)
  const CGUIControl::GUICONTROLTYPES type = control.GetControlType();
  const std::string* file = nullptr;
  if (type == CGUIControl::GUICONTROL_IMAGE || type == CGUIControl::GUICONTROL_BORDEREDIMAGE)
    file = &static_cast<const CGUIImage&>(control).GetFileName();
  else if (type == CGUIControl::GUICONTROL_BUTTON)
    file = &static_cast<const CGUIButtonControl&>(control).GlasNoFocusFile();
  if (!file)
    return;
  const CRect& r = control.GetRenderRegion();
  const float area = r.Area();
  if (area > bestArea && r.PtInRect(p) && file->find("hover") != std::string::npos)
  {
    best = r;
    bestArea = area;
  }
}

inline bool FindHoverImage(int windowId, const CPoint& p, CRect& rect)
{
  const CGUIWindow* window = CServiceBroker::GetGUI()->GetWindowManager().GetWindow(windowId);
  if (!window)
    return false;
  CRect best;
  float bestArea = 0.0f;
  for (const CGUIControl* child : GroupChildren::Of(*window))
    FindHover(*child, p, best, bestArea);
  if (best.IsEmpty())
    return false;
  rect = best;
  return true;
}

//! Aus CGUIBaseContainer::ProcessItem nach dem fokussierten Eintrag: nur fuer die Liste unter dem Zeiger
inline void NoteShape(const void* container, const CGUIControlGroup& layout)
{
  Hover& h = LastHover();
  if (!h.valid || h.key != container)
    return;
  CRect cell;
  if (!GLAS_FOCUSITEM::Get(container, cell) || cell.IsEmpty())
    return;
  const MousePosition pos = CServiceBroker::GetInputManager().GetMousePosition();
  CRect best;
  float bestArea = 0.0f;
  FindShape(layout, CPoint(static_cast<float>(pos.x), static_cast<float>(pos.y)), cell.Area() * 1.6f, best,
            bestArea);
  h.haveShape = !best.IsEmpty();
  if (h.haveShape)
    h.shape = best;
}

struct State
{
  enum Phase
  {
    IDLE,
    GROW,
    SHRINK
  } phase = IDLE;
  int mouseState = 0;
  unsigned int start = 0;
  unsigned int duration = 0;
  unsigned int lastTime = 0;
  bool seen = false;
  CRect from; // Bildschirm-Pixel
  float fromAlpha = 1.0f;
  CRect cur;
  float curAlpha = 1.0f;
  CRect last; // letzte Zielflaeche (fuer den Rueckweg)
  bool haveLast = false;
};

inline State& Get()
{
  static State state;
  return state;
}

inline unsigned int Slow()
{
  unsigned int factor = 1;
  if (FILE* f = fopen("/tmp/glas-pointer-slow", "r"))
  {
    unsigned int v = 0;
    if (fscanf(f, "%u", &v) == 1 && v >= 1 && v <= 100)
      factor = v;
    fclose(f);
  }
  return factor;
}

inline CRect Lerp(const CRect& a, const CRect& b, float t)
{
  return CRect(a.x1 + (b.x1 - a.x1) * t, a.y1 + (b.y1 - a.y1) * t, a.x2 + (b.x2 - a.x2) * t,
               a.y2 + (b.y2 - a.y2) * t);
}

//! Aus CGUIWindowPointer::Process nach SetPointer: window = Zeigerfenster, (mx, my) = Mausposition
template<class Window>
void Process(Window& window, float mx, float my, int mouseState, unsigned int now)
{
  CGUIControl* morph = window.GetControl(ID_MORPH);
  if (!morph)
    return;
  CGUIControl* circle = window.GetControl(ID_CIRCLE);
  State& s = Get();

  // Skin-Einstellung "Mauszeiger: Klassisch": Bild 5 per Bedingung aus - Kodis Verhalten (Bild je Zustand)
  if (!morph->IsVisibleFromSkin())
  {
    s.phase = State::IDLE;
    s.seen = false;
    morph->SetVisible(false);
    if (circle)
      circle->SetVisible(mouseState != MOUSE_STATE_FOCUS);
    return;
  }

  // Kreis an der Mausposition, so wie Pointer.xml ihn legt
  const float cx = circle ? circle->GetXPosition() : -15.0f;
  const float cy = circle ? circle->GetYPosition() : -15.0f;
  const float cw = circle ? circle->GetWidth() : 30.0f;
  const float ch = circle ? circle->GetHeight() : 30.0f;
  const CRect dot(mx + cx, my + cy, mx + cx + cw, my + cy + ch);

  if (mouseState != MOUSE_STATE_FOCUS)
    LastHover().valid = false;
  // Knopf ohne Fokusbild: Hover-Bild unter dem Zeiger suchen; ohne eines wie ohne Fokus (Kreis bleibt)
  Hover& hover = LastHover();
  if (mouseState == MOUSE_STATE_FOCUS && hover.valid && hover.unseen)
  {
    if (hover.lastSearch == 0 || now - hover.lastSearch >= SEARCH_MS)
    {
      hover.lastSearch = now ? now : 1;
      hover.haveHoverImage = FindHoverImage(hover.window, CPoint(mx, my), hover.hoverImage);
    }
    if (!hover.haveHoverImage)
      mouseState = MOUSE_STATE_NORMAL;
  }
  CRect target;
  const bool haveTarget = mouseState == MOUSE_STATE_FOCUS && Target(target);
  if (haveTarget)
  {
    s.last = target;
    s.haveLast = true;
  }

  const bool fresh = !s.seen || now - s.lastTime > GAP_MS;
  s.seen = true;
  s.lastTime = now;
  if (fresh)
  {
    s.phase = State::IDLE;
    s.mouseState = mouseState;
  }
  else if (mouseState != s.mouseState)
  {
    const bool running = s.phase != State::IDLE;
    if (mouseState == MOUSE_STATE_FOCUS)
    {
      s.phase = State::GROW;
      s.from = running ? s.cur : dot;
      s.fromAlpha = running ? s.curAlpha : 1.0f;
      s.duration = IN_MS * Slow();
    }
    else
    {
      s.phase = State::SHRINK;
      s.from = running ? s.cur : (s.haveLast ? s.last : dot);
      s.fromAlpha = running ? s.curAlpha : 0.0f;
      s.duration = OUT_MS * Slow();
    }
    s.start = now;
    s.mouseState = mouseState;
    if (s.duration > IN_MS) // nur mit Messschalter /tmp/glas-pointer-slow
      CLog::Log(LOGINFO,
                "GLAS_POINTER {} Ziel {} ({} {}) [{:.0f} {:.0f} {:.0f} {:.0f}] Punkt [{:.0f} {:.0f}] von [{:.0f} {:.0f} "
                "{:.0f} {:.0f}] a {:.2f}",
                s.phase == State::GROW ? "rein" : "raus", haveTarget, LastHover().container,
                LastHover().valid, target.x1, target.y1, target.x2, target.y2, mx, my, s.from.x1, s.from.y1,
                s.from.x2, s.from.y2, s.fromAlpha);
  }

  if (s.phase != State::IDLE)
  {
    const float t = (std::min)(1.0f, static_cast<float>(now - s.start) / (std::max)(1u, s.duration));
    const float u = 1.0f - t;
    const float e = 1.0f - u * u * u; // ease-out, wie die Fokusbewegung
    if (s.phase == State::GROW)
    {
      // die Form folgt der Flaeche, das Bild blendet schneller aus, als es waechst (kein grauer Schleier auf der
      // Kachel)
      s.cur = Lerp(s.from, haveTarget ? target : dot, e);
      s.curAlpha = s.fromAlpha * u * u * u;
    }
    else
    {
      s.cur = Lerp(s.from, dot, e);
      s.curAlpha = s.fromAlpha + (1.0f - s.fromAlpha) * e;
    }
    if (s.duration > IN_MS) // nur mit Messschalter /tmp/glas-pointer-slow
      CLog::Log(LOGINFO, "GLAS_POINTER t {:.2f} now {} [{:.0f} {:.0f} {:.0f} {:.0f}] a {:.2f} Ziel {}", t, now, s.cur.x1,
                s.cur.y1, s.cur.x2, s.cur.y2, s.curAlpha, haveTarget);
    if (t >= 1.0f)
      s.phase = State::IDLE;
  }

  const bool showMorph = s.phase != State::IDLE;
  if (circle)
    circle->SetVisible(mouseState != MOUSE_STATE_FOCUS && s.phase != State::SHRINK);
  morph->SetVisible(showMorph);
  if (showMorph)
  {
    morph->SetPosition(s.cur.x1 - mx, s.cur.y1 - my);
    morph->SetWidth((std::max)(1.0f, s.cur.Width()));
    morph->SetHeight((std::max)(1.0f, s.cur.Height()));
    const unsigned int a = static_cast<unsigned int>((std::clamp)(s.curAlpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    if (morph->SetColorDiffuse(KODI::GUILIB::GUIINFO::CGUIInfoColor((a << 24) | 0x00FFFFFF)))
      morph->MarkDirtyRegion();
  }
}
} // namespace GLAS_POINTER
