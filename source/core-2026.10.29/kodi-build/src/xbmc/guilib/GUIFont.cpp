/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GUIFont.h"

#include "GUIFontTTF.h"
#include "ServiceBroker.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindowManager.h"
#include "utils/CharsetConverter.h"
#include "utils/MathUtils.h"
#include "utils/TimeUtils.h"
#include "utils/TransformMatrix.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"

#include <algorithm>
#include <cmath>
#include <mutex>

#include <unistd.h>

#define ROUND(x) (float)(MathUtils::round_int(x))

CScrollInfo::CScrollInfo(unsigned int wait /* = 50 */,
                         float pos /* = 0 */,
                         int speed /* = defaultSpeed */,
                         const std::string& scrollSuffix /* = " | " */)
  : m_initialWait(wait), m_initialPos(pos)

{
  SetSpeed(speed ? speed : defaultSpeed);
  std::wstring wsuffix;
  g_charsetConverter.utf8ToW(scrollSuffix, wsuffix);
  m_suffix.clear();
  m_suffix.reserve(wsuffix.size());
  m_suffix = vecText(wsuffix.begin(), wsuffix.end());
  Reset();
}

float CScrollInfo::GetPixelsPerFrame()
{
  static const float alphaEMA = 0.05f;

  if (0 == m_pixelSpeed)
    return 0; // not scrolling

  unsigned int currentTime = CTimeUtils::GetFrameTime();
  float delta =
      m_lastFrameTime ? static_cast<float>(currentTime - m_lastFrameTime) : m_averageFrameTime;
  if (delta > 100)
    delta = 100; // assume a minimum of 10 fps
  m_lastFrameTime = currentTime;
  // do an exponential moving average of the frame time
  if (delta)
    m_averageFrameTime = m_averageFrameTime + (delta - m_averageFrameTime) * alphaEMA;
  // and multiply by pixel speed (per ms) to get number of pixels to move this frame
  return m_pixelSpeed * m_averageFrameTime;
}

CGUIFont::CGUIFont(const std::string& strFontName,
                   uint32_t style,
                   UTILS::COLOR::Color textColor,
                   UTILS::COLOR::Color shadowColor,
                   float lineSpacing,
                   float origHeight,
                   CGUIFontTTF* font)
  : m_strFontName(strFontName)
{
  m_style = style & FONT_STYLE_MASK;
  m_textColor = textColor;
  m_shadowColor = shadowColor;
  m_lineSpacing = lineSpacing;
  m_origHeight = origHeight;
  m_font = font;

  if (m_font)
    m_font->AddReference();
}

CGUIFont::~CGUIFont()
{
  GlasClearScaled();
  if (m_font)
    m_font->RemoveReference();
}

void CGUIFont::GlasClearScaled()
{
  for (auto& it : m_glasScaled)
  {
    GlasEndOne(it.second);
    delete it.second;
  }
  m_glasScaled.clear();
  m_glasSeen.clear();
  m_glasWidth.clear();
}

CGUIFontTTF* CGUIFont::GlasScaledFont(CGraphicContext& context, float x, float y, float& scale)
{
  scale = 1.0f;
  // Messschalter /tmp/glas-font-off: wie bisher per GPU verkleinern (Vergleich)
  static unsigned int lastCheck = 0;
  static bool off = false;
  const unsigned int frameTime = CTimeUtils::GetFrameTime();
  if (frameTime - lastCheck > 1000)
  {
    lastCheck = frameTime;
    off = access("/tmp/glas-font-off", F_OK) == 0;
  }
  if (off || !m_font || m_font->m_glasPath.empty())
    return nullptr;
  // Massstab der Transformation in Schriftpixeln (Skin-Koordinaten -> Bildschirm, ohne GUI-Skalierung)
  const float x0 = context.ScaleFinalXCoord(x, y);
  const float y0 = context.ScaleFinalYCoord(x, y);
  const float sx = (context.ScaleFinalXCoord(x + 100.0f, y) - x0) * context.GetGUIScaleX() / 100.0f;
  const float sy = (context.ScaleFinalYCoord(x, y + 100.0f) - y0) * context.GetGUIScaleY() / 100.0f;
  const float kx = (context.ScaleFinalYCoord(x + 100.0f, y) - y0) / 100.0f;
  const float ky = (context.ScaleFinalXCoord(x, y + 100.0f) - x0) / 100.0f;
  // nur gleichmaessige, deutliche Verkleinerung ohne Drehung (die Zoom-Stufen des Scope-Modus liegen bei 68-82 %)
  if (std::fabs(kx) > 0.001f || std::fabs(ky) > 0.001f || std::fabs(sx - sy) > 0.01f || sx > 0.92f || sx < 0.4f)
    return nullptr;
  const int pct = MathUtils::round_int(static_cast<double>(sx) * 100.0);
  // Nur der ruhende Massstab eines Fensters (Scope-Verkleinerung): im vorigen gezeichneten Bild von mindestens vier
  // Texten (alle Schriften) gemeinsam benutzt. Ein einzelner fokussierter, gezoomter Eintrag und laufende
  // Zoom-Animationen bleiben bei der GPU-Verkleinerung - sonst wechselte ihr Text beim Scrollen zwischen beiden
  // Darstellungen und wackelte (21:9, Auswahldialoge, 29.10.2026). Wird ein Massstab in diesem Bild ruhend, ein Bild
  // nachzeichnen lassen, damit ein danach stillstehender Bildschirm scharf wird.
  // Mit Traegheit: einmal ruhend, bleibt ein Massstab es, solange er weiter benutzt wird (erst nach 1 s ohne Benutzung
  // nicht mehr). Ohne sie fiel z. B. im Aktionsblatt "Mehr" (fuenf Texte) beim Fokuswechsel die Zahl unter vier, und
  // die ruhende Ueberschrift sprang bildweise zwischen beiden Darstellungen
  static unsigned int s_countFrame = 0;
  static std::map<int, int> s_counts, s_prevCounts;
  static std::map<int, unsigned int> s_stable; //!< ruhender Massstab -> zuletzt benutzt (Bildzeit)
  if (s_countFrame != frameTime)
  {
    s_prevCounts.swap(s_counts);
    s_counts.clear();
    s_countFrame = frameTime;
    for (auto it = s_stable.begin(); it != s_stable.end();)
      it = frameTime - it->second >= 1000 ? s_stable.erase(it) : std::next(it);
  }
  const int count = ++s_counts[pct];
  const auto prev = s_prevCounts.find(pct);
  const auto stable = s_stable.find(pct);
  const bool wasStable = stable != s_stable.end() && frameTime - stable->second < 1000;
  if (!wasStable && (prev == s_prevCounts.end() || prev->second < 4))
  {
    if (count == 4 && CServiceBroker::GetGUI())
      CServiceBroker::GetGUI()->GetWindowManager().MarkDirty();
    return nullptr;
  }
  s_stable[pct] = frameTime;
  // zuletzt verwendet (Bildzeit, zum Verdraengen)
  const unsigned int now = frameTime;
  m_glasSeen[pct].last = now;
  auto it = m_glasScaled.find(pct);
  if (it == m_glasScaled.end())
  {
    // hoechstens eine neue Fassung je Bild (Laden kostet ein paar ms); bis dahin wie bisher gezeichnet
    static unsigned int lastCreate = 0;
    if (lastCreate == now)
      return nullptr;
    lastCreate = now;
    // hoechstens sechs Fassungen je Schrift: die am laengsten nicht verwendete verwerfen
    if (m_glasScaled.size() >= 6)
    {
      auto oldest = m_glasScaled.begin();
      for (auto i = m_glasScaled.begin(); i != m_glasScaled.end(); ++i)
        if (m_glasSeen[i->first].last < m_glasSeen[oldest->first].last)
          oldest = i;
      GlasEndOne(oldest->second);
      delete oldest->second;
      m_glasScaled.erase(oldest);
    }
    CGUIFontTTF* font = CGUIFontTTF::CreateGUIFontTTF(m_font->GetFontIdent() + "_glas" + std::to_string(pct));
    if (font && !font->Load(m_font->m_glasPath, m_font->m_height * pct / 100.0f, m_font->m_glasAspect, 1.0f,
                            m_font->m_glasBorder))
    {
      delete font;
      font = nullptr;
    }
    it = m_glasScaled.emplace(pct, font).first;
    CLog::Log(LOGDEBUG, "GLAS_FONT: {} in {} % ({} Fassungen)", m_strFontName, pct, m_glasScaled.size());
  }
  if (!it->second)
    return nullptr;
  scale = pct / 100.0f;
  return it->second;
}

void CGUIFont::GlasDrawText(CGraphicContext& context,
                            CGUIFontTTF* scaled,
                            float scale,
                            float x,
                            float y,
                            const std::vector<UTILS::COLOR::Color>& colors,
                            const vecText& text,
                            uint32_t alignment,
                            float maxPixelWidth,
                            bool scrolling)
{
  if (!scaled)
  {
    GlasBegin(m_font);
    m_font->DrawTextInternal(context, x, y, colors, text, alignment, maxPixelWidth, scrolling);
    return;
  }
  // die Verkleinerung um den Textursprung herum aus der Transformation nehmen: die Glyphen der kleineren Fassung
  // liegen dann im selben Massstab, aber pixelgenau
  const TransformMatrix unscale = TransformMatrix::CreateTranslation(x, y) *
                                  TransformMatrix::CreateScaler(1.0f / scale, 1.0f / scale) *
                                  TransformMatrix::CreateTranslation(-x, -y);
  // Breite fuer das Kuerzen in Pixeln der kleineren Fassung. Durch das Hinting laeuft sie etwas breiter als genau
  // scale x Original: passt der Text in der Originalschrift, nie kuerzen (sonst wurde "1920x1080p (2D)" zu "...(2...")
  // Die Breiten zu messen heisst den Text zu setzen (HarfBuzz): je Text, Breite und Fassung nur einmal, sonst kostete es
  // in jedem Bild fuer jedes Label (gemessen: Top Shelf in 21:9 14,5 statt 10,3 verspaetete Bilder/s)
  float width = maxPixelWidth > 0 ? maxPixelWidth * scale : maxPixelWidth;
  if (maxPixelWidth > 0 && !(alignment & XBFONT_JUSTIFIED))
  {
    uint64_t key = 1469598103934665603ULL;
    for (character_t ch : text)
      key = (key ^ ch) * 1099511628211ULL;
    key = (key ^ static_cast<uint64_t>(maxPixelWidth * 16.0f)) * 1099511628211ULL;
    key = (key ^ reinterpret_cast<uintptr_t>(scaled)) * 1099511628211ULL;
    auto it = m_glasWidth.find(key);
    if (it == m_glasWidth.end())
    {
      if (m_glasWidth.size() > 2048)
        m_glasWidth.clear();
      float fits = width;
      if (m_font->GetTextWidthInternal(text) <= maxPixelWidth)
        fits = std::max(width, scaled->GetTextWidthInternal(text) + 1.0f);
      it = m_glasWidth.emplace(key, fits).first;
    }
    width = it->second;
  }
  GlasBegin(scaled);
  context.AddTransform(unscale);
  scaled->DrawTextInternal(context, x, y, colors, text, alignment, width, scrolling);
  context.RemoveTransform();
}

std::string& CGUIFont::GetFontName()
{
  return m_strFontName;
}

void CGUIFont::DrawText(float x,
                        float y,
                        const std::vector<UTILS::COLOR::Color>& colors,
                        UTILS::COLOR::Color shadowColor,
                        const vecText& text,
                        uint32_t alignment,
                        float maxPixelWidth)
{
  CWinSystemBase* const winSystem = CServiceBroker::GetWinSystem();
  if (!m_font || !winSystem)
    return;

  CGraphicContext& context = winSystem->GetGfxContext();

  float scale;
  CGUIFontTTF* scaled = GlasScaledFont(context, x, y, scale);
  bool clip = maxPixelWidth > 0;
  // kleinere Fassung: durch das Hinting ein, zwei Pixel breiter als die Originalschrift - den Schnittbereich etwas
  // weiter fassen, sonst fehlte z. B. die halbe schliessende Klammer von "(2D)"
  float clipX = x;
  float clipWidth = maxPixelWidth;
  if (scaled && clip)
  {
    const float pad = maxPixelWidth * 0.03f + 2.0f;
    if (!(alignment & XBFONT_CENTER_X))
      clipX -= pad;
    clipWidth += 2.0f * pad;
  }
  if (clip && ClippedRegionIsEmpty(context, clipX, y, clipWidth, alignment))
    return;

  maxPixelWidth = ROUND(static_cast<double>(maxPixelWidth / context.GetGUIScaleX()));
  std::vector<UTILS::COLOR::Color> renderColors;
  renderColors.reserve(colors.size());
  for (const auto& color : colors)
    renderColors.emplace_back(context.MergeColor(color ? color : m_textColor));
  if (!shadowColor)
    shadowColor = m_shadowColor;
  if (shadowColor)
  {
    shadowColor = context.MergeColor(shadowColor);
    std::vector<UTILS::COLOR::Color> shadowColors;
    shadowColors.reserve(renderColors.size());
    for (const auto& renderColor : renderColors)
      shadowColors.emplace_back((renderColor & 0xff000000) != 0 ? shadowColor : 0);
    GlasDrawText(context, scaled, scale, x + 1, y + 1, shadowColors, text, alignment, maxPixelWidth,
                 false);
  }
  GlasDrawText(context, scaled, scale, x, y, renderColors, text, alignment, maxPixelWidth, false);

  if (clip)
    context.RestoreClipRegion();
}

bool CGUIFont::UpdateScrollInfo(const vecText& text, CScrollInfo& scrollInfo)
{
  CWinSystemBase* const winSystem = CServiceBroker::GetWinSystem();
  if (!winSystem)
    return false;

  // draw at our scroll position
  // we handle the scrolling as follows:
  //   We scroll on a per-pixel basis (eschewing the use of character indices
  //   which were also in use previously). The complete string, including suffix,
  //   is plotted to achieve the desired effect - normally just the one time, but
  //   if there is a wrap point within the viewport then it will be plotted twice.
  //   If the string is smaller than the viewport, then it may be plotted even
  //   more times than that.
  //
  if (scrollInfo.m_waitTime)
  {
    scrollInfo.m_waitTime--;
    return true;
  }

  if (text.empty())
    return false;

  CScrollInfo old(scrollInfo);

  // move along by the appropriate scroll amount
  float scrollAmount =
      fabs(scrollInfo.GetPixelsPerFrame() * winSystem->GetGfxContext().GetGUIScaleX());

  if (!scrollInfo.m_widthValid)
  {
    /* Calculate the pixel width of the complete string */
    scrollInfo.m_textWidth = GetTextWidth(text);
    scrollInfo.m_totalWidth = scrollInfo.m_textWidth + GetTextWidth(scrollInfo.m_suffix);
    scrollInfo.m_widthValid = true;
  }
  scrollInfo.m_pixelPos += scrollAmount;
  assert(scrollInfo.m_totalWidth != 0);
  while (scrollInfo.m_pixelPos >= scrollInfo.m_totalWidth)
    scrollInfo.m_pixelPos -= scrollInfo.m_totalWidth;

  if (scrollInfo.m_pixelPos < old.m_pixelPos)
    ++scrollInfo.m_loopCount;

  if (scrollInfo.m_pixelPos != old.m_pixelPos)
    return true;

  return false;
}

void CGUIFont::DrawScrollingText(float x,
                                 float y,
                                 const std::vector<UTILS::COLOR::Color>& colors,
                                 UTILS::COLOR::Color shadowColor,
                                 const vecText& text,
                                 uint32_t alignment,
                                 float maxWidth,
                                 const CScrollInfo& scrollInfo)
{
  CWinSystemBase* const winSystem = CServiceBroker::GetWinSystem();
  if (!m_font || !winSystem)
    return;

  CGraphicContext& context = winSystem->GetGfxContext();

  if (!shadowColor)
    shadowColor = m_shadowColor;

  if (!text.size() || ClippedRegionIsEmpty(context, x, y, maxWidth, alignment))
    return; // nothing to render

  if (!scrollInfo.m_widthValid)
  {
    /* Calculate the pixel width of the complete string */
    scrollInfo.m_textWidth = GetTextWidth(text);
    scrollInfo.m_totalWidth = scrollInfo.m_textWidth + GetTextWidth(scrollInfo.m_suffix);
    scrollInfo.m_widthValid = true;
  }

  assert(scrollInfo.m_totalWidth != 0);

  float textPixelWidth =
      ROUND(static_cast<double>(scrollInfo.m_textWidth / context.GetGUIScaleX()));
  float suffixPixelWidth = ROUND(static_cast<double>(
      (scrollInfo.m_totalWidth - scrollInfo.m_textWidth) / context.GetGUIScaleX()));

  float offset;
  if (scrollInfo.m_pixelSpeed >= 0)
    offset = scrollInfo.m_pixelPos;
  else
    offset = scrollInfo.m_totalWidth - scrollInfo.m_pixelPos;

  std::vector<UTILS::COLOR::Color> renderColors;
  renderColors.reserve(colors.size());
  for (const auto& color : colors)
    renderColors.emplace_back(context.MergeColor(color ? color : m_textColor));

  bool scroll = !scrollInfo.m_waitTime && scrollInfo.m_pixelSpeed;
  float scale;
  CGUIFontTTF* scaled = GlasScaledFont(context, x, y, scale);
  if (shadowColor)
  {
    shadowColor = context.MergeColor(shadowColor);
    std::vector<UTILS::COLOR::Color> shadowColors;
    shadowColors.reserve(renderColors.size());
    for (const auto& renderColor : renderColors)
      shadowColors.emplace_back((renderColor & 0xff000000) != 0 ? shadowColor : 0);
    for (float dx = -offset; dx < maxWidth; dx += scrollInfo.m_totalWidth)
    {
      GlasDrawText(context, scaled, scale, x + dx + 1, y + 1, shadowColors, text, alignment,
                   textPixelWidth, scroll);
      GlasDrawText(context, scaled, scale, x + dx + scrollInfo.m_textWidth + 1, y + 1, shadowColors,
                   scrollInfo.m_suffix, alignment, suffixPixelWidth, scroll);
    }
  }
  for (float dx = -offset; dx < maxWidth; dx += scrollInfo.m_totalWidth)
  {
    GlasDrawText(context, scaled, scale, x + dx, y, renderColors, text, alignment, textPixelWidth,
                 scroll);
    GlasDrawText(context, scaled, scale, x + dx + scrollInfo.m_textWidth, y, renderColors,
                 scrollInfo.m_suffix, alignment, suffixPixelWidth, scroll);
  }

  context.RestoreClipRegion();
}

bool CGUIFont::ClippedRegionIsEmpty(
    CGraphicContext& context, float x, float y, float width, uint32_t alignment) const
{
  if (alignment & XBFONT_CENTER_X)
    x -= width * 0.5f;
  if (alignment & XBFONT_CENTER_Y)
    y -= m_font->GetLineHeight(m_lineSpacing);

  return !context.SetClipRegion(x, y, width, m_font->GetTextHeight(1, 2) * context.GetGUIScaleY());
}

float CGUIFont::GetTextWidth(const vecText& text)
{
  CWinSystemBase* const winSystem = CServiceBroker::GetWinSystem();
  if (!m_font || !winSystem)
    return 0;

  CGraphicContext& context = winSystem->GetGfxContext();

  std::unique_lock<CCriticalSection> lock(context);
  return m_font->GetTextWidthInternal(text) * context.GetGUIScaleX();
}

float CGUIFont::GetCharWidth(character_t ch)
{
  CWinSystemBase* const winSystem = CServiceBroker::GetWinSystem();
  if (!m_font || !winSystem)
    return 0;

  CGraphicContext& context = winSystem->GetGfxContext();

  std::unique_lock<CCriticalSection> lock(context);
  return m_font->GetCharWidthInternal(ch) * context.GetGUIScaleX();
}

float CGUIFont::GetTextHeight(int numLines) const
{
  CWinSystemBase* const winSystem = CServiceBroker::GetWinSystem();
  if (!m_font || !winSystem)
    return 0;

  return m_font->GetTextHeight(m_lineSpacing, numLines) * winSystem->GetGfxContext().GetGUIScaleY();
}

float CGUIFont::GetTextBaseLine() const
{
  CWinSystemBase* const winSystem = CServiceBroker::GetWinSystem();
  if (!m_font || !winSystem)
    return 0;

  return m_font->GetTextBaseLine() * winSystem->GetGfxContext().GetGUIScaleY();
}

float CGUIFont::GetLineHeight() const
{
  CWinSystemBase* const winSystem = CServiceBroker::GetWinSystem();
  if (!m_font || !winSystem)
    return 0;

  return m_font->GetLineHeight(m_lineSpacing) * winSystem->GetGfxContext().GetGUIScaleY();
}

float CGUIFont::GetScaleFactor() const
{
  if (!m_font)
    return 1.0f;

  return m_font->GetFontHeight() / m_origHeight;
}

// Glas: Begin/End starten die Schrift erst beim ersten Zeichnen (GlasBegin), und zwar nur die Fassung, die wirklich
// zeichnet. Kodi beginnt und beendet die Schrift je Label (Shader, Textur, Vertex-Zeiger); mit der kleineren Fassung
// geschah das sonst doppelt, einmal leer fuer die Originalschrift (gemessen: Top Shelf in 21:9 15 statt 11 verspaetete
// Bilder/s)
void CGUIFont::Begin()
{
  if (!m_font)
    return;

  m_glasNested++;
}

void CGUIFont::End()
{
  if (!m_font || m_glasNested == 0)
    return;

  if (--m_glasNested > 0)
    return;
  for (CGUIFontTTF* font : m_glasBegun)
    font->End();
  m_glasBegun.clear();
}

void CGUIFont::GlasBegin(CGUIFontTTF* font)
{
  if (m_glasNested == 0 || std::find(m_glasBegun.begin(), m_glasBegun.end(), font) != m_glasBegun.end())
    return;
  font->Begin();
  m_glasBegun.push_back(font);
}

void CGUIFont::GlasEndOne(CGUIFontTTF* font)
{
  auto it = std::find(m_glasBegun.begin(), m_glasBegun.end(), font);
  if (it == m_glasBegun.end())
    return;
  font->End();
  m_glasBegun.erase(it);
}

void CGUIFont::SetFont(CGUIFontTTF* font)
{
  if (m_font == font)
    return; // no need to update the font if we already have it

  if (m_font)
    m_font->RemoveReference();

  GlasClearScaled();
  GlasEndOne(m_font);
  m_font = font;
  if (m_font)
    m_font->AddReference();
}
