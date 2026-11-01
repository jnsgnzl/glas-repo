/*
 *  Copyright (C) 2026 Team Kodi / Glas
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

/*!
 \file GUIBackdropBlurControl.cpp (Windows, DirectX 11)
 \brief DX11-Fassung von <control type="backdropblur"> fuer den Windows-Build (Weg B in
        docs/plan-liveglas-windows.md): derselbe Header wie die GLES-Fassung der Vero
        (tools/kodi-build/src/xbmc/guilib/GUIBackdropBlurControl.cpp), dieselben Tags, dieselbe Logik fuer
        Griff-Bedarf, Drosselung und Buchfuehrung; nur Griff, Pyramide und Zusammensetzen laufen ueber Direct3D 11.

 Ablauf je Glasflaeche (Render):
  1. Rechteck in Bildschirm-Pixeln (y nach unten, Ursprung oben links wie DX).
  2. Ziel bestimmen: aktuell gebundenes RTV des verzoegerten Kontexts (DX::DeviceResources::GetD3DContext), so wie
     unter GL GL_FRAMEBUFFER_BINDING. Normalerweise der Backbuffer, bei Hardware-Stereo ein Slice des Arrays.
  3. Griff nur bei Bedarf (NeedsGrab wie GL): CopySubresourceRegion des Viewports in eine Textur gleichen Formats
     (der Backbuffer der Flip-Swapchain laesst sich nicht abtasten), dann GRAB-Pass auf 1/grabscale.
  4. Pyramide Dual Kawase (DOWN/UP) in CD3DTexture-Stufen.
  5. Zusammensetzen frosted bzw. liquid (Rand und Innen getrennt) mit Kodis Blend SRC_ALPHA / INV_SRC_ALPHA.
  6. Zustand zurueck: RTV/DSV, Viewport, Scissor, dann CRenderSystemDX::ApplyStateBlock (Rasterizer, Tiefe, Blend,
     GUI-Shader mit Konstantenpuffern, Sampler, Vertexpuffer, Topologie).

 Farbraum (Plan 3.5): Griff und Pyramide haben Format und Kodierung der Quelle (8 oder 10 Bit, voller oder begrenzter
 Bereich, SDR oder PQ/BT.2020). Das Zusammensetzen rechnet erst nach SDR-Gamma im vollen Bereich zurueck
 (FromOutput), wendet das Material an und kodiert wie Kodis GUI-Shader wieder (ToOutput = tonemapHDR(
 adjustColorRange(c))). Sonst wuerde die Toenung im begrenzten Bereich doppelt umgerechnet und waere ueber PQ viel
 zu hell bzw. falsch getoent.

 Alle Befehle laufen auf dem verzoegerten Kontext (sonst stimmte die Reihenfolge mit der Oberflaeche nicht).
 Shader: HLSL in dieser Datei, zur Laufzeit mit D3DCompile (vs_4_0/ps_4_0, also ab Feature-Level 10_0).

 Nicht (noch nicht) unter Windows: eigenes Zwischenbild (Mali-Umweg), Glas mit eigener Quelle (<source>: greift hier
 wie sonst), Video-Glas ueber /dev/amvideocap0 (unter DX liegt das Video schon im Backbuffer), Bild-Mitschnitt und
 Schwarz-Pruefung der Diagnose.
*/

#include "GUIBackdropBlurControl.h"

#include "ServiceBroker.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "guilib/GUIControlGroupList.h"
#include "guilib/D3DResource.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindowManager.h"
#include "rendering/RenderSystem.h"
#include "rendering/dx/DeviceResources.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <d3dcompiler.h>
#include <io.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

//! Messschalter wie auf der Vero: Datei "/tmp/glas-..." vorhanden (unter Windows <Laufwerk>:\tmp\glas-...)
#define GLAS_SWITCH(path) (_access(path, 0) == 0)

//! Stereo-3D: Kodi zeichnet die Oberflaeche je Auge einmal (Application.cpp: SetStereoView LEFT, RIGHT). MONO
//! (3D-Film, Ausgabe 2D) ist ein Durchgang mit Ansicht LEFT und zaehlt hier nicht als Stereo.
namespace GLAS_STEREO
{
inline bool Active(CGraphicContext& context)
{
  const RENDER_STEREO_MODE mode = context.GetStereoMode();
  return mode != RENDER_STEREO_MODE_OFF && mode != RENDER_STEREO_MODE_MONO;
}

//! 0 = 2D, MONO oder linkes Auge; 1 = rechtes Auge (eigener Zwischenspeicher)
inline int Eye(CGraphicContext& context)
{
  return Active(context) && context.GetStereoView() == RENDER_STEREO_VIEW_RIGHT ? 1 : 0;
}

//! Video-Glas auch im Stereo-Betrieb; Messschalter /tmp/glas-stereo-video-off schaltet es im Kern ab
inline bool VideoAllowed()
{
  static auto lastCheck = std::chrono::steady_clock::time_point();
  static bool allowed = true;
  const auto now = std::chrono::steady_clock::now();
  if (now - lastCheck > std::chrono::seconds(1))
  {
    lastCheck = now;
    allowed = !GLAS_SWITCH("/tmp/glas-stereo-video-off");
  }
  return allowed;
}

//! Messschalter /tmp/glas-liquid-inner-off: Liquid-Flaechen ganz mit dem vollen Shader (Vergleich)
inline bool LiquidInnerOff()
{
  static auto lastCheck = std::chrono::steady_clock::time_point();
  static bool off = false;
  const auto now = std::chrono::steady_clock::now();
  if (now - lastCheck > std::chrono::seconds(1))
  {
    lastCheck = now;
    off = GLAS_SWITCH("/tmp/glas-liquid-inner-off");
  }
  return off;
}

//! Stereo-3D: ein Griff fuer beide Augen, auch ohne Video (Vorgabe an; Messschalter /tmp/glas-stereo-share-off)
inline bool ShareEyes()
{
  static auto lastCheck = std::chrono::steady_clock::time_point();
  static bool share = true;
  const auto now = std::chrono::steady_clock::now();
  if (now - lastCheck > std::chrono::seconds(1))
  {
    lastCheck = now;
    share = !GLAS_SWITCH("/tmp/glas-stereo-share-off");
  }
  return share;
}
} // namespace GLAS_STEREO

/*!
 \brief Video-Glas unter DX11: das Videobild liegt schon im Backbuffer, bevor CGUIWindowFullScreen die Oberflaeche
        darueber zeichnet. Der Griff sieht es mit; es braucht keinen eigenen Aufnahmeweg. Solange ein Video laeuft
        (nicht pausiert), gilt jedes Bild als neuer Hintergrund (Process).
*/
namespace GLAS_DXVIDEO
{
struct State
{
  unsigned int frame = 0;
  bool video = false; //!< Video laeuft (auch pausiert): gemeinsamer Griff fuer alle Fenster, keine Drosselung
  bool moving = false; //!< Video laeuft und ist nicht pausiert: jedes Bild neu greifen
};
inline State g_state;

inline const State& Query(unsigned int frame)
{
  if (frame != g_state.frame)
  {
    g_state.frame = frame;
    const auto appPlayer = CServiceBroker::GetAppComponents().GetComponent<CApplicationPlayer>();
    g_state.video = appPlayer && appPlayer->IsPlayingVideo();
    if (g_state.video && GLAS_STEREO::Active(CServiceBroker::GetWinSystem()->GetGfxContext()) &&
        !GLAS_STEREO::VideoAllowed())
      g_state.video = false;
    g_state.moving = g_state.video && !appPlayer->IsPausedPlayback();
  }
  return g_state;
}
} // namespace GLAS_DXVIDEO

/*!
 \brief Farbraum der Ausgabe fuers Zusammensetzen (Plan 3.5), je Bild einmal gelesen, mit denselben Quellen wie
        CGUIShaderDX::ApplyChanges: begrenzter Farbbereich (UseLimitedColor), PQ (IsTransferPQ) und Weiss der
        Oberflaeche (GetGuiSdrPeakLuminance). 10 Bit ohne PQ braucht nichts: Griff und Pyramide haben das Format der
        Quelle. Messschalter /tmp/glas-force-pq: PQ-Zweig auch auf dem SDR-Backbuffer (nur Lauftest, Bild falsch).
*/
namespace GLAS_DXCOLOR
{
struct State
{
  unsigned int frame = ~0u;
  float black = 0.0f;
  float range = 1.0f;
  float sdrPeak = 10000.0f / 203.0f;
  bool pq = false;
  bool forced = false;
};
inline State g_state;

inline const State& Query(unsigned int frame)
{
  if (frame == g_state.frame)
    return g_state;
  g_state.frame = frame;
  CWinSystemBase* winSystem = CServiceBroker::GetWinSystem();
  const bool limited = winSystem && winSystem->UseLimitedColor();
  g_state.black = limited ? 16.0f / 255.0f : 0.0f;
  g_state.range = limited ? (235.0f - 16.0f) / 255.0f : 1.0f;
  static auto lastCheck = std::chrono::steady_clock::time_point();
  const auto now = std::chrono::steady_clock::now();
  if (now - lastCheck > std::chrono::seconds(1))
  {
    lastCheck = now;
    g_state.forced = GLAS_SWITCH("/tmp/glas-force-pq");
  }
  const bool pq = DX::DeviceResources::Get()->IsTransferPQ() || g_state.forced;
  if (pq)
  {
    const float peak = winSystem ? winSystem->GetGuiSdrPeakLuminance() : 0.0f;
    g_state.sdrPeak = 10000.0f / (peak > 1.0f ? peak : 203.0f);
  }
  g_state.pq = pq;
  // ins Log bei jedem Wechsel (Start, HDR-Umschaltung, Einstellung, Messschalter)
  static int s_lastKey = -1;
  const int key = (pq ? 1 : 0) | (limited ? 2 : 0) | (g_state.forced ? 4 : 0);
  if (key != s_lastKey)
  {
    s_lastKey = key;
    CLog::Log(LOGINFO, "CGUIBackdropBlurControl: Ausgabe {}{}, Farbbereich {}, Weiss {:.0f} nits", pq ? "PQ" : "SDR",
              g_state.forced ? " (erzwungen: /tmp/glas-force-pq)" : "", limited ? "begrenzt" : "voll",
              10000.0f / g_state.sdrPeak);
  }
  return g_state;
}
} // namespace GLAS_DXCOLOR

namespace
{
// HLSL. Koordinaten: uv und local mit y nach unten (DX), Ursprung oben links.
const char* VERTEX_SHADER = R"(
struct VS_IN
{
  float2 pos : POSITION;
  float2 uv : TEXCOORD0;
  float2 local : TEXCOORD1;
};
struct PS_IN
{
  float4 pos : SV_POSITION;
  float2 uv : TEXCOORD0;
  float2 local : TEXCOORD1;
};
PS_IN main(VS_IN i)
{
  PS_IN o;
  o.pos = float4(i.pos, 0.0, 1.0);
  o.uv = i.uv;
  o.local = i.local;
  return o;
}
)";

//! gemeinsamer Kopf aller Pixel-Shader: Texturen, Sampler, Konstanten (Aufbau wie CBGlas unten)
const char* PIXEL_HEADER = R"(
Texture2D u_tex : register(t0);
Texture2D u_sharp : register(t1);
SamplerState s_lin : register(s0);
cbuffer cbGlas : register(b0)
{
  float2 u_size;
  float2 u_screen;
  float4 u_tint;
  float2 u_off;
  float u_radius;
  float u_px;
  float u_saturation;
  float u_alpha;
  float u_lensOld;
  float u_pad;
  float u_black; // wie blackLevel in Kodis cbWorld (begrenzter Farbbereich 16/255, sonst 0)
  float u_range; // wie colorRange (219/255 bzw. 1)
  float u_sdrPeak; // wie sdrPeakLum: 10000 / Weiss der Oberflaeche in nits (nur PQ)
  float u_pq; // 1 = Ausgabe PQ/BT.2020 (IsTransferPQ)
};
struct PS_IN
{
  float4 pos : SV_POSITION;
  float2 uv : TEXCOORD0;
  float2 local : TEXCOORD1;
};
static const float3 luma = float3(0.2126, 0.7152, 0.0722);

// ---- Farbraum (Plan 3.5): Griff und Pyramide bleiben im Format der Ausgabe (PQ bzw. begrenzter Bereich). Das
// Material (Toenung, Weiss, Abdunklung) gilt fuer SDR-Gamma im vollen Bereich: erst zurueckrechnen (FromOutput),
// Material, dann genau wie Kodis GUI-Shader wieder hinein (ToOutput = tonemapHDR(adjustColorRange(c))).
static const float PQ_M1 = 2610.0 / (4096.0 * 4.0);
static const float PQ_M2 = (2523.0 / 4096.0) * 128.0;
static const float PQ_C1 = 3424.0 / 4096.0;
static const float PQ_C2 = (2413.0 / 4096.0) * 32.0;
static const float PQ_C3 = (2392.0 / 4096.0) * 32.0;
// REC.709 -> BT.2020 wie transferPQ in guishader_common.hlsl, und die Umkehrung davon
static const float3x3 BT709_TO_2020 =
{
  0.627402, 0.329292, 0.043306,
  0.069095, 0.919544, 0.011360,
  0.016394, 0.088028, 0.895578
};
static const float3x3 BT2020_TO_709 =
{
  1.660495596, -0.587656471, -0.072839713,
  -0.124546199, 1.132895115, -0.008347783,
  -0.018154323, -0.100596990, 1.118751212
};
// Lichter ueber dem SDR-Weiss (HDR-Video unter dem Glas): weich begrenzen, hoechstens 1 + PQ_HEAD (linear)
static const float PQ_HEAD = 0.5;

//! Ausgabe (PQ bzw. begrenzter Bereich) -> SDR-Gamma im vollen Bereich; Umkehrung von ToOutput
float3 FromOutput(float3 c)
{
  if (u_pq > 0.5)
  {
    // PQ -> linear (1 = 10000 nits) -> relativ zum Weiss der Oberflaeche -> BT.709
    float3 p = pow(saturate(c), 1.0 / PQ_M2);
    float3 l = pow(max(p - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * p), 1.0 / PQ_M1) * u_sdrPeak;
    l = max(mul(BT2020_TO_709, l), 0.0);
    // ueber 1,0: Reinhard mit stetiger Steigung, darunter unveraendert (SDR-Inhalt kommt exakt zurueck)
    float3 t = max(l - 1.0, 0.0);
    l = min(l, 1.0) + PQ_HEAD * t / (t + PQ_HEAD);
    c = pow(l, 0.45);
  }
  return (c - u_black) / u_range;
}

//! SDR-Gamma im vollen Bereich -> Ausgabe, wie tonemapHDR(adjustColorRange(c)) der GUI-Shader
float3 ToOutput(float3 c)
{
  c = u_black + u_range * max(c, 0.0);
  if (u_pq > 0.5)
  {
    // c ist hier nie negativ (abs nur, damit der Compiler nicht warnt)
    c = mul(BT709_TO_2020, pow(abs(c), 1.0 / 0.45));
    c = pow(abs(c) / u_sdrPeak, PQ_M1);
    c = pow(abs((PQ_C1 + PQ_C2 * c) / (1.0 + PQ_C3 * c)), PQ_M2);
  }
  return c;
}
)";

// Dual Kawase, Verkleinern: Mitte x4 plus vier Diagonalen (u_off = ein Eingabepixel)
const char* DOWN_SHADER = R"(
float4 main(PS_IN i) : SV_TARGET
{
  float4 sum = u_tex.Sample(s_lin, i.uv) * 4.0;
  sum += u_tex.Sample(s_lin, i.uv - u_off);
  sum += u_tex.Sample(s_lin, i.uv + u_off);
  sum += u_tex.Sample(s_lin, i.uv + float2(u_off.x, -u_off.y));
  sum += u_tex.Sample(s_lin, i.uv - float2(u_off.x, -u_off.y));
  return sum / 8.0;
}
)";

// Griff: Kopie des Viewports auf 1/grabscale verkleinern, vier bilineare Abtastungen je Pixel (u_off = ein Quellpixel)
const char* GRAB_SHADER = R"(
float4 main(PS_IN i) : SV_TARGET
{
  float4 sum = u_tex.Sample(s_lin, i.uv + float2(-u_off.x, -u_off.y));
  sum += u_tex.Sample(s_lin, i.uv + float2(u_off.x, -u_off.y));
  sum += u_tex.Sample(s_lin, i.uv + float2(-u_off.x, u_off.y));
  sum += u_tex.Sample(s_lin, i.uv + float2(u_off.x, u_off.y));
  return sum * 0.25;
}
)";

// Dual Kawase, Vergroessern: acht Abtastungen
const char* UP_SHADER = R"(
float4 main(PS_IN i) : SV_TARGET
{
  float4 sum = u_tex.Sample(s_lin, i.uv + float2(-u_off.x * 2.0, 0.0));
  sum += u_tex.Sample(s_lin, i.uv + float2(-u_off.x, u_off.y)) * 2.0;
  sum += u_tex.Sample(s_lin, i.uv + float2(0.0, u_off.y * 2.0));
  sum += u_tex.Sample(s_lin, i.uv + float2(u_off.x, u_off.y)) * 2.0;
  sum += u_tex.Sample(s_lin, i.uv + float2(u_off.x * 2.0, 0.0));
  sum += u_tex.Sample(s_lin, i.uv + float2(u_off.x, -u_off.y)) * 2.0;
  sum += u_tex.Sample(s_lin, i.uv + float2(0.0, -u_off.y * 2.0));
  sum += u_tex.Sample(s_lin, i.uv + float2(-u_off.x, -u_off.y)) * 2.0;
  return sum / 12.0;
}
)";

// Zusammensetzen (frosted): Saettigung, Toenung, runde Maske (SDF in Bildschirm-Pixeln), Alpha
const char* COMPOSITE_SHADER = R"(
float4 main(PS_IN i) : SV_TARGET
{
  float3 c = FromOutput(u_tex.Sample(s_lin, i.uv).rgb);
  float lum = dot(c, luma);
  c = lerp(float3(lum, lum, lum), c, u_saturation);
  c = lerp(c, u_tint.rgb, u_tint.a);
  float2 half_size = u_size * 0.5;
  float2 q = abs(i.local - half_size) - (half_size - float2(u_radius, u_radius));
  float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - u_radius;
  float mask = saturate(0.5 - d);
  return float4(ToOutput(c), u_alpha * mask);
}
)";

//! Liquid Glass (style liquid), sinngemaess aus LIQUID_SHADER der GLES-Fassung. Dort rechnet der Shader in
//! Framebuffer-Pixeln mit y nach oben; hier zeigt y nach unten (local, uv). Die Richtung des Lichts ist deshalb
//! gespiegelt (L = (-0.20, -0.98)): Licht von oben, helle Kante oben wie auf der Vero.
const char* LIQUID_SHADER = R"(
float4 main(PS_IN i) : SV_TARGET
{
  float2 half_size = u_size * 0.5;
  float2 p = i.local - half_size;
  float2 q = abs(p) - (half_size - float2(u_radius, u_radius));
  float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - u_radius;
  float inside = -d;
  float rimW = max(6.0, min(u_radius * 0.8, 22.0 * u_px));
  float2 g;
  if (q.x > 0.0 && q.y > 0.0)
    g = normalize(q);
  else
    g = q.x > q.y ? float2(1.0, 0.0) : float2(0.0, 1.0);
  float2 n = g * float2(sign(p));
  float rim = 1.0 - smoothstep(0.0, rimW, inside);
  float rim2 = rim * rim;
  // Linse: gewoelbter Rand ueber lensW (Eckradius, hoechstens 48 px), holt den Hintergrund von aussen herein
  float lensW = max(6.0, min(u_radius, 48.0 * u_px));
  float lens = 1.0 - saturate(inside / lensW);
  lens *= lens;
  float2 off = n * lens * (0.8 * lensW) / u_screen;
  if (u_lensOld > 0.5)
  {
    lens = rim2;
    off = n * rim2 * (14.0 * u_px) / u_screen;
  }
  float3 blurred = FromOutput(u_tex.Sample(s_lin, i.uv + off).rgb);
  float3 sharp = FromOutput(u_sharp.Sample(s_lin, i.uv + off * 1.3).rgb);
  float3 c = lerp(blurred, sharp, lens * 0.55);
  float cl = dot(c, luma);
  c = lerp(float3(cl, cl, cl), c, u_saturation);
  float lum = dot(blurred, luma);
  c = lerp(c, float3(1.0, 1.0, 1.0), 0.09 * (1.0 - smoothstep(0.55, 0.85, lum)));
  c = lerp(c, float3(0.118, 0.118, 0.118), 0.28 * smoothstep(0.62, 0.92, lum));
  c = lerp(c, u_tint.rgb, u_tint.a);
  float2 L = normalize(float2(-0.20, -0.98));
  float facing = dot(n, L);
  float line_ = 1.0 - smoothstep(0.6 * u_px, 2.2 * u_px, inside);
  float spec = 0.03 + 0.21 * pow(max(facing, 0.0), 2.0) + 0.05 * pow(max(-facing, 0.0), 3.0);
  float glow = rim2 * (0.00 + 0.03 * max(facing, 0.0));
  c += float3(1.0, 1.0, 1.0) * (line_ * spec + glow);
  float mask = saturate(0.5 - d);
  return float4(ToOutput(min(c, float3(1.0, 1.0, 1.0))), u_alpha * mask);
}
)";

//! Liquid Glass innen (jenseits des Rands): Lichtbrechung, scharfer Griff, Lichtkante und Schein sind dort null
const char* LIQUID_INNER_SHADER = R"(
float4 main(PS_IN i) : SV_TARGET
{
  float3 b = FromOutput(u_tex.Sample(s_lin, i.uv).rgb);
  float l = dot(b, luma);
  float3 c = lerp(float3(l, l, l), b, u_saturation);
  c = lerp(c, float3(1.0, 1.0, 1.0), 0.09 * (1.0 - smoothstep(0.55, 0.85, l)));
  c = lerp(c, float3(0.118, 0.118, 0.118), 0.28 * smoothstep(0.62, 0.92, l));
  c = lerp(c, u_tint.rgb, u_tint.a);
  return float4(ToOutput(min(c, float3(1.0, 1.0, 1.0))), u_alpha);
}
)";

//! Konstanten aller Pixel-Shader (Aufbau wie cbGlas im HLSL, 80 Byte)
struct CBGlas
{
  float size[2];
  float screen[2];
  float tint[4];
  float off[2];
  float radius;
  float px;
  float saturation;
  float alpha;
  float lensOld;
  float pad;
  float black; //!< begrenzter Farbbereich: 16/255, sonst 0 (wie blackLevel in Kodis cbWorld)
  float range; //!< 219/255 bzw. 1 (colorRange)
  float sdrPeak; //!< 10000 / Weiss der Oberflaeche in nits (sdrPeakLum, nur PQ)
  float pq; //!< 1 = Ausgabe PQ/BT.2020
};
static_assert(sizeof(CBGlas) == 80, "cbGlas muss 80 Byte haben");

struct GlasVertex
{
  float pos[2];
  float uv[2];
  float local[2];
};

enum PixelShader
{
  PS_DOWN,
  PS_UP,
  PS_GRAB,
  PS_COMPOSITE,
  PS_LIQUID,
  PS_LIQUID_INNER,
  PS_COUNT
};

//! Liegt b wirklich hinter a (mindestens 25 % der kleineren Flaeche)? Nachbarn, die sich beim Fokus-Zoom um ein paar
//! Pixel beruehren, zaehlen nicht (wie GLES-Fassung)
bool OverlapsMuch(const float a[4], const float b[4])
{
  const float w = std::min(a[2], b[2]) - std::max(a[0], b[0]);
  const float h = std::min(a[3], b[3]) - std::max(a[1], b[1]);
  if (w <= 0 || h <= 0)
    return false;
  const float areaA = (a[2] - a[0]) * (a[3] - a[1]);
  const float areaB = (b[2] - b[0]) * (b[3] - b[1]);
  return w * h >= 0.25f * std::min(areaA, areaB);
}

/*!
 \brief Gemeinsame DX-Objekte aller Glasflaechen: Shader, Eingabe-Layout, Puffer, Zustaende, GPU-Zeitmessung.
        Am Geraeteverlust angemeldet (ID3DResource): OnDestroyDevice gibt alles frei, Prepare legt es beim naechsten
        Glas neu an (die uebersetzten Shader bleiben). Die Texturen der Pyramide (CD3DTexture) sichern sich selbst;
        die Flaechen greifen nach einem Verlust trotzdem neu (Generation).
*/
class CGlasDX : public ID3DResource
{
public:
  static CGlasDX& Get()
  {
    // bewusst nie freigegeben (wie die GLES-Fassung): beim Beenden gibt es das Geraet evtl. nicht mehr
    static CGlasDX* instance = new CGlasDX;
    return *instance;
  }

  bool Prepare()
  {
    if (m_ready)
      return true;
    if (m_failed)
      return false;
    auto dr = DX::DeviceResources::Get();
    ID3D11Device1* device = dr->GetD3DDevice();
    if (!device || !dr->GetD3DContext())
      return false;
    if (dr->GetDeviceFeatureLevel() < D3D_FEATURE_LEVEL_10_0)
    {
      CLog::Log(LOGERROR, "CGUIBackdropBlurControl: Live-Glas braucht Direct3D Feature-Level 10_0 (vs_4_0/ps_4_0)");
      m_failed = true;
      return false;
    }
    if (!Compile())
    {
      m_failed = true;
      return false;
    }
    bool ok = SUCCEEDED(device->CreateVertexShader(m_vsBlob->GetBufferPointer(), m_vsBlob->GetBufferSize(),
                                                   nullptr, m_vs.ReleaseAndGetAddressOf()));
    for (int i = 0; ok && i < PS_COUNT; i++)
      ok = SUCCEEDED(device->CreatePixelShader(m_psBlob[i]->GetBufferPointer(), m_psBlob[i]->GetBufferSize(),
                                               nullptr, m_ps[i].ReleaseAndGetAddressOf()));
    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ok = ok && SUCCEEDED(device->CreateInputLayout(layout, 3, m_vsBlob->GetBufferPointer(),
                                                   m_vsBlob->GetBufferSize(), m_layout.ReleaseAndGetAddressOf()));
    CD3D11_BUFFER_DESC vbDesc(sizeof(GlasVertex) * 4, D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DYNAMIC,
                              D3D11_CPU_ACCESS_WRITE);
    ok = ok && SUCCEEDED(device->CreateBuffer(&vbDesc, nullptr, m_vb.ReleaseAndGetAddressOf()));
    CD3D11_BUFFER_DESC cbDesc(sizeof(CBGlas), D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DYNAMIC,
                              D3D11_CPU_ACCESS_WRITE);
    ok = ok && SUCCEEDED(device->CreateBuffer(&cbDesc, nullptr, m_cb.ReleaseAndGetAddressOf()));
    D3D11_SAMPLER_DESC samp = {};
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samp.MaxLOD = D3D11_FLOAT32_MAX;
    ok = ok && SUCCEEDED(device->CreateSamplerState(&samp, m_sampler.ReleaseAndGetAddressOf()));
    D3D11_RASTERIZER_DESC rs = {};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    rs.ScissorEnable = FALSE;
    ok = ok && SUCCEEDED(device->CreateRasterizerState(&rs, m_rsNoScissor.ReleaseAndGetAddressOf()));
    // Zusammensetzen in beschnittenen Bereichen (Grouplists, Listen): Kodi beschneidet Texturen in Software
    // (CGraphicContext::ClipRect), das Glas ueber den Scissor
    rs.ScissorEnable = TRUE;
    ok = ok && SUCCEEDED(device->CreateRasterizerState(&rs, m_rsScissor.ReleaseAndGetAddressOf()));
    D3D11_DEPTH_STENCIL_DESC ds = {};
    ds.DepthEnable = FALSE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ds.StencilEnable = FALSE;
    ok = ok && SUCCEEDED(device->CreateDepthStencilState(&ds, m_depthOff.ReleaseAndGetAddressOf()));
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ok = ok && SUCCEEDED(device->CreateBlendState(&bd, m_blendOff.ReleaseAndGetAddressOf()));
    m_blendOn.clear();
    if (!ok)
    {
      CLog::Log(LOGERROR, "CGUIBackdropBlurControl: DX11-Objekte liessen sich nicht anlegen");
      Release();
      m_failed = true;
      return false;
    }
    if (!m_registered)
    {
      Register();
      m_registered = true;
    }
    m_ready = true;
    m_generation++;
    CLog::Log(LOGINFO, "CGUIBackdropBlurControl: bereit (Direct3D 11, Feature-Level {:x}, GPU-Zeitstempel ja)",
              static_cast<int>(dr->GetDeviceFeatureLevel()));
    return true;
  }

  //! Zaehlt bei jedem Neuanlegen (Geraeteverlust): Flaechen legen ihre Pyramide dann neu an
  unsigned int Generation() const { return m_generation; }

  //! Kodis Blend fuer GUI-Texturen (SRC_ALPHA / INV_SRC_ALPHA, Alpha ONE / INV_SRC_ALPHA) mit der Schreibmaske des
  //! aktuellen Zustands (Anaglyph-Stereo setzt eigene Masken)
  ID3D11BlendState* BlendOn(ID3D11BlendState* current)
  {
    UINT8 mask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (current)
    {
      D3D11_BLEND_DESC cd;
      current->GetDesc(&cd);
      mask = cd.RenderTarget[0].RenderTargetWriteMask;
    }
    auto it = m_blendOn.find(mask);
    if (it != m_blendOn.end())
      return it->second.Get();
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = mask;
    ComPtr<ID3D11BlendState> state;
    DX::DeviceResources::Get()->GetD3DDevice()->CreateBlendState(&bd, state.GetAddressOf());
    m_blendOn[mask] = state;
    return state.Get();
  }

  //! Eingabe, Vertex-Shader, Sampler, Konstanten: fuer alle Paesse gleich
  void BindCommon(ID3D11DeviceContext* ctx)
  {
    const UINT stride = sizeof(GlasVertex);
    const UINT offset = 0;
    ctx->IASetInputLayout(m_layout.Get());
    ctx->IASetVertexBuffers(0, 1, m_vb.GetAddressOf(), &stride, &offset);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    ctx->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    ctx->PSSetConstantBuffers(0, 1, m_cb.GetAddressOf());
  }

  void SetConstants(ID3D11DeviceContext* ctx, const CBGlas& cb)
  {
    D3D11_MAPPED_SUBRESOURCE res;
    if (SUCCEEDED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &res)))
    {
      memcpy(res.pData, &cb, sizeof(cb));
      ctx->Unmap(m_cb.Get(), 0);
    }
  }

  //! Viereck als Dreiecksstreifen: Ecken (x0,y0), (x1,y0), (x0,y1), (x1,y1)
  void Draw(ID3D11DeviceContext* ctx, const GlasVertex v[4])
  {
    D3D11_MAPPED_SUBRESOURCE res;
    if (SUCCEEDED(ctx->Map(m_vb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &res)))
    {
      memcpy(res.pData, v, sizeof(GlasVertex) * 4);
      ctx->Unmap(m_vb.Get(), 0);
      ctx->Draw(4, 0);
    }
  }

  ID3D11PixelShader* PS(PixelShader which) const { return m_ps[which].Get(); }
  ID3D11RasterizerState* RasterNoScissor() const { return m_rsNoScissor.Get(); }
  ID3D11RasterizerState* RasterScissor() const { return m_rsScissor.Get(); }
  ID3D11DepthStencilState* DepthOff() const { return m_depthOff.Get(); }
  ID3D11BlendState* BlendOff() const { return m_blendOff.Get(); }

  // ---- GPU-Zeitmessung (/tmp/glas-timing): Zeitstempel auf dem verzoegerten Kontext, Auslesen auf dem sofortigen ----

  //! Am Bildanfang (GLAS_GUITARGET::Begin): Zeitstempel des Bildes, Glas-Zeitstempel je Flaeche (GlassBegin/End)
  void FrameBegin(bool timing)
  {
    m_cur = -1;
    if (!timing || !m_ready)
      return;
    Collect();
    FrameQueries& f = m_frameQ[m_next];
    if (f.pending)
      return; // GPU hinkt hinterher: dieses Bild nicht messen
    ID3D11Device1* device = DX::DeviceResources::Get()->GetD3DDevice();
    if (!f.disjoint)
    {
      D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
      device->CreateQuery(&qd, f.disjoint.GetAddressOf());
      qd.Query = D3D11_QUERY_TIMESTAMP;
      device->CreateQuery(&qd, f.t0.GetAddressOf());
      device->CreateQuery(&qd, f.t1.GetAddressOf());
      if (!f.disjoint || !f.t0 || !f.t1)
        return;
    }
    ID3D11DeviceContext* ctx = DX::DeviceResources::Get()->GetD3DContext();
    ctx->Begin(f.disjoint.Get());
    ctx->End(f.t0.Get());
    f.used = 0;
    m_cur = m_next;
  }

  void GlassBegin()
  {
    if (m_cur < 0)
      return;
    FrameQueries& f = m_frameQ[m_cur];
    if (f.used >= f.glass.size())
    {
      if (f.glass.size() >= 64)
        return;
      D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP, 0};
      ID3D11Device1* device = DX::DeviceResources::Get()->GetD3DDevice();
      std::pair<ComPtr<ID3D11Query>, ComPtr<ID3D11Query>> pair;
      device->CreateQuery(&qd, pair.first.GetAddressOf());
      device->CreateQuery(&qd, pair.second.GetAddressOf());
      if (!pair.first || !pair.second)
        return;
      f.glass.push_back(pair);
    }
    DX::DeviceResources::Get()->GetD3DContext()->End(f.glass[f.used].first.Get());
    m_glassOpen = true;
  }

  void GlassEnd()
  {
    if (m_cur < 0 || !m_glassOpen)
      return;
    m_glassOpen = false;
    FrameQueries& f = m_frameQ[m_cur];
    DX::DeviceResources::Get()->GetD3DContext()->End(f.glass[f.used].second.Get());
    f.used++;
  }

  void FrameEnd(bool rendered)
  {
    if (m_cur < 0)
      return;
    FrameQueries& f = m_frameQ[m_cur];
    ID3D11DeviceContext* ctx = DX::DeviceResources::Get()->GetD3DContext();
    ctx->End(f.t1.Get());
    ctx->End(f.disjoint.Get());
    f.pending = true;
    f.valid = rendered;
    m_next = (m_cur + 1) % FRAME_SLOTS;
    m_cur = -1;
  }

  //! Ergebnisse fertiger Bilder einsammeln (ohne zu warten)
  void Collect()
  {
    ID3D11DeviceContext* imm = DX::DeviceResources::Get()->GetImmediateContext();
    for (int n = 0; n < FRAME_SLOTS; n++)
    {
      // aelteste zuerst
      FrameQueries& f = m_frameQ[(m_next + n) % FRAME_SLOTS];
      if (!f.pending)
        continue;
      D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
      if (imm->GetData(f.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
        continue;
      f.pending = false;
      UINT64 t0 = 0, t1 = 0;
      if (dj.Disjoint || !f.valid || dj.Frequency == 0 ||
          imm->GetData(f.t0.Get(), &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
          imm->GetData(f.t1.Get(), &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
        continue;
      double glass = 0;
      bool ok = true;
      for (size_t i = 0; i < f.used && ok; i++)
      {
        UINT64 a = 0, b = 0;
        ok = imm->GetData(f.glass[i].first.Get(), &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
             imm->GetData(f.glass[i].second.Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        if (ok && b >= a)
          glass += static_cast<double>(b - a) * 1000.0 / dj.Frequency;
      }
      if (!ok || t1 < t0)
        continue;
      const double frame = static_cast<double>(t1 - t0) * 1000.0 / dj.Frequency;
      m_gpuFrameSum += frame;
      m_gpuFrameMax = std::max(m_gpuFrameMax, frame);
      m_gpuGlassSum += glass;
      m_gpuGlassMax = std::max(m_gpuGlassMax, glass);
      m_gpuN++;
    }
  }

  //! Mittelwerte seit dem letzten Aufruf (ms): Bild GPU, davon Glas GPU; false = keine Messung
  bool TakeGpu(double& frameAvg, double& frameMax, double& glassAvg, double& glassMax, int& n)
  {
    n = m_gpuN;
    if (!m_gpuN)
      return false;
    frameAvg = m_gpuFrameSum / m_gpuN;
    glassAvg = m_gpuGlassSum / m_gpuN;
    frameMax = m_gpuFrameMax;
    glassMax = m_gpuGlassMax;
    m_gpuN = 0;
    m_gpuFrameSum = m_gpuFrameMax = m_gpuGlassSum = m_gpuGlassMax = 0;
    return true;
  }

  void OnDestroyDevice(bool fatal) override
  {
    // waehrend DeviceResources die Liste durchlaeuft: nicht abmelden, nur freigeben
    Release();
  }

  void OnCreateDevice() override {}

private:
  //! Bilder, deren Zeitstempel gleichzeitig unterwegs sein duerfen (mit 6 wurde nur jedes dritte Bild gemessen: die
  //! GPU liefert die Ergebnisse des verzoegerten Kontexts erst einige Bilder spaeter)
  static constexpr int FRAME_SLOTS = 16;

  struct FrameQueries
  {
    ComPtr<ID3D11Query> disjoint, t0, t1;
    std::vector<std::pair<ComPtr<ID3D11Query>, ComPtr<ID3D11Query>>> glass;
    size_t used = 0;
    bool pending = false;
    bool valid = false;
  };

  bool Compile()
  {
    if (m_vsBlob)
      return true;
    auto compile = [](const std::string& source, const char* name, const char* profile, ComPtr<ID3DBlob>& out) {
      ComPtr<ID3DBlob> errors;
      const HRESULT hr = D3DCompile(source.c_str(), source.size(), name, nullptr, nullptr, "main", profile,
                                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out.ReleaseAndGetAddressOf(),
                                    errors.GetAddressOf());
      if (FAILED(hr))
      {
        CLog::Log(LOGERROR, "CGUIBackdropBlurControl: Shader {} nicht uebersetzt: {}", name,
                  errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return false;
      }
      return true;
    };
    const char* sources[PS_COUNT] = {DOWN_SHADER,      UP_SHADER,     GRAB_SHADER,
                                     COMPOSITE_SHADER, LIQUID_SHADER, LIQUID_INNER_SHADER};
    const char* names[PS_COUNT] = {"glas_down",      "glas_up",     "glas_grab",
                                   "glas_composite", "glas_liquid", "glas_liquid_inner"};
    if (!compile(VERTEX_SHADER, "glas_vert", "vs_4_0", m_vsBlob))
      return false;
    for (int i = 0; i < PS_COUNT; i++)
      if (!compile(std::string(PIXEL_HEADER) + sources[i], names[i], "ps_4_0", m_psBlob[i]))
      {
        m_vsBlob.Reset();
        return false;
      }
    return true;
  }

  void Release()
  {
    m_ready = false;
    m_vs.Reset();
    for (auto& ps : m_ps)
      ps.Reset();
    m_layout.Reset();
    m_vb.Reset();
    m_cb.Reset();
    m_sampler.Reset();
    m_rsNoScissor.Reset();
    m_rsScissor.Reset();
    m_depthOff.Reset();
    m_blendOff.Reset();
    m_blendOn.clear();
    for (auto& f : m_frameQ)
      f = FrameQueries();
    m_cur = -1;
    m_glassOpen = false;
  }

  bool m_ready = false;
  bool m_failed = false;
  bool m_registered = false;
  unsigned int m_generation = 0;
  ComPtr<ID3DBlob> m_vsBlob;
  ComPtr<ID3DBlob> m_psBlob[PS_COUNT];
  ComPtr<ID3D11VertexShader> m_vs;
  ComPtr<ID3D11PixelShader> m_ps[PS_COUNT];
  ComPtr<ID3D11InputLayout> m_layout;
  ComPtr<ID3D11Buffer> m_vb;
  ComPtr<ID3D11Buffer> m_cb;
  ComPtr<ID3D11SamplerState> m_sampler;
  ComPtr<ID3D11RasterizerState> m_rsNoScissor;
  ComPtr<ID3D11RasterizerState> m_rsScissor;
  ComPtr<ID3D11DepthStencilState> m_depthOff;
  ComPtr<ID3D11BlendState> m_blendOff;
  std::map<UINT8, ComPtr<ID3D11BlendState>> m_blendOn;
  FrameQueries m_frameQ[FRAME_SLOTS];
  int m_cur = -1;
  int m_next = 0;
  bool m_glassOpen = false;
  int m_gpuN = 0;
  double m_gpuFrameSum = 0, m_gpuFrameMax = 0, m_gpuGlassSum = 0, m_gpuGlassMax = 0;
};

/*!
 \brief Zustand des verzoegerten Kontexts, den das Glas veraendert: vorher lesen, danach zurueck. Den Rest
        (Rasterizer, Tiefe, Blend, GUI-Shader, Konstanten, Sampler, Vertexpuffer, Topologie) setzt
        CRenderSystemDX::ApplyStateBlock aus Kodis eigener Buchfuehrung.
*/
struct CSavedState
{
  explicit CSavedState(ID3D11DeviceContext* context) : ctx(context)
  {
    ctx->OMGetRenderTargets(1, rtv.GetAddressOf(), dsv.GetAddressOf());
    ctx->RSGetViewports(&numViewports, &viewport);
    ctx->RSGetScissorRects(&numScissors, nullptr);
    numScissors = std::min<UINT>(numScissors, 1);
    if (numScissors)
      ctx->RSGetScissorRects(&numScissors, &scissor);
    ctx->RSGetState(raster.GetAddressOf());
    ctx->OMGetBlendState(blend.GetAddressOf(), nullptr, nullptr);
  }

  //! Ziel, Viewport und Scissor der Oberflaeche (fuers Zusammensetzen)
  void BindTarget() const
  {
    ID3D11RenderTargetView* target = rtv.Get();
    ctx->OMSetRenderTargets(1, &target, dsv.Get());
    if (numViewports)
      ctx->RSSetViewports(1, &viewport);
    if (clipRaster)
    {
      // Glas teilweise ausserhalb des Beschnitts (Zeile am Rand einer Grouplist): nur den sichtbaren Teil zeichnen
      ctx->RSSetState(clipRaster);
      ctx->RSSetScissorRects(1, &clipRect);
      return;
    }
    ctx->RSSetState(raster.Get());
    if (numScissors)
      ctx->RSSetScissorRects(1, &scissor);
  }

  void Restore() const
  {
    ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
    ctx->PSSetShaderResources(0, 2, none);
    BindTarget();
    if (CRenderSystemBase* renderSystem = CServiceBroker::GetRenderSystem())
      renderSystem->ApplyStateBlock();
  }

  ID3D11DeviceContext* ctx;
  ComPtr<ID3D11RenderTargetView> rtv;
  ComPtr<ID3D11DepthStencilView> dsv;
  UINT numViewports = 1;
  D3D11_VIEWPORT viewport = {};
  UINT numScissors = 0;
  D3D11_RECT scissor = {};
  ComPtr<ID3D11RasterizerState> raster;
  ComPtr<ID3D11BlendState> blend;
  ID3D11RasterizerState* clipRaster = nullptr; //!< gesetzt: Zusammensetzen nur in clipRect (Pixel)
  D3D11_RECT clipRect = {};
};

/*!
 \brief Gemeinsamer Hintergrund aller Glasflaechen (wie GLES-Fassung): hoechstens ein Griff je Bild ueber den ganzen
        Viewport, stark verkleinert und weichgezeichnet. Jede Flaeche schneidet sich beim Zusammensetzen ihren Teil
        heraus. Flaechen, die vor dem Griff schon zusammengesetzt waren, stecken im Ergebnis ("verschmutzt"): sie
        greifen beim naechsten Bild selbst neu.
*/
class CBackdropBlurShared
{
public:
  //! eye: 0 = 2D/MONO/linkes Auge, 1 = rechtes Auge; window: je Fenster ein eigener Griff (0 = gemeinsam, ueber Video)
  static CBackdropBlurShared& Get(int eye = 0, int window = 0)
  {
    static std::map<std::pair<int, int>, CBackdropBlurShared*> shared;
    CBackdropBlurShared*& instance = shared[{std::clamp(eye, 0, 1), window}];
    if (!instance)
      instance = new CBackdropBlurShared;
    return *instance;
  }

  bool Prepare()
  {
    CGlasDX& dx = CGlasDX::Get();
    if (!dx.Prepare())
      return false;
    if (m_generation != dx.Generation())
    {
      // neu angelegtes Geraet: Pyramide neu, kein altes Ergebnis zeigen
      m_generation = dx.Generation();
      m_w = m_h = 0;
      m_result = nullptr;
      m_sharp = nullptr;
    }
    return true;
  }

  enum class Need
  {
    NONE, //!< vorhandenes Ergebnis taugt
    BACKGROUND, //!< Hintergrund hat sich geaendert (darf gedrosselt werden)
    FORCED //!< kein brauchbares Ergebnis (neu, andere Einstellung, Flaeche steckt selbst im Ergebnis)
  };

  //! Ist fuer diese Flaeche (Bildschirm-Pixel) ein neuer Griff noetig, oder taugt der vorhandene?
  Need NeedsGrab(const float rect[4], unsigned int frame, bool backgroundDirty, int grabScale, int deepest)
  {
    m_lastReason = 0;
    if (!m_result || grabScale != m_grabScale || deepest != m_deepest)
      return Need::FORCED;
    m_lastReason = 1;
    for (const auto& r : m_polluted)
      if (OverlapsMuch(rect, r.data()))
        return Need::FORCED;
    if (!backgroundDirty && !(frame == m_wantFrame && frame != m_grabFrame))
      return Need::NONE;
    m_lastReason = 3;
    if (frame != m_grabFrame)
      return Need::BACKGROUND;
    m_lastReason = 2;
    for (const auto& r : m_composited)
      if (OverlapsMuch(rect, r.data()))
        return Need::FORCED;
    return Need::NONE;
  }

  //! Process: hinter dieser Glasflaeche hat sich in diesem Bild etwas geaendert
  void WantGrab(unsigned int frame) { m_wantFrame = frame; }
  int LastReason() const { return m_lastReason; }
  unsigned int LastGrabFrame() const { return m_grabFrame; }

  //! Diagnose (/tmp/glas-diag): Griffe nach Grund, gedrosselte Bilder
  void Count(bool grabbed, bool throttled)
  {
    if (grabbed)
      m_reasons[m_lastReason]++;
    if (throttled)
      m_throttledCount++;
  }

  void DiagReport()
  {
    if (++m_diagFrames < 120)
      return;
    CLog::Log(LOGINFO,
              "GLAS_DIAG: {} Glas-Bilder, Griffe: neu {} verschmutzt {} Glas-dahinter {} Hintergrund {}, gedrosselt {}",
              m_diagFrames, m_reasons[0], m_reasons[1], m_reasons[2], m_reasons[3], m_throttledCount);
    m_diagFrames = 0;
    m_throttledCount = 0;
    for (auto& r : m_reasons)
      r = 0;
  }

  //! Griff aus dem aktuell gebundenen Ziel (state.rtv) ueber den Viewport, Pyramide. false = nicht moeglich
  bool Grab(const CSavedState& state, unsigned int frame, int grabScale, int deepest, bool probe, bool timing)
  {
    const auto cpuStart = std::chrono::steady_clock::now();
    ID3D11DeviceContext* ctx = state.ctx;
    CGlasDX& dx = CGlasDX::Get();

    ComPtr<ID3D11Resource> resource;
    state.rtv->GetResource(resource.GetAddressOf());
    ComPtr<ID3D11Texture2D> source;
    if (FAILED(resource.As(&source)))
      return LogOnce("Ziel ist keine 2D-Textur");
    D3D11_TEXTURE2D_DESC td;
    source->GetDesc(&td);
    if (td.SampleDesc.Count != 1)
      return LogOnce("Ziel mit Multisampling wird nicht unterstuetzt");
    D3D11_RENDER_TARGET_VIEW_DESC rd;
    state.rtv->GetDesc(&rd);
    UINT mip = 0, slice = 0;
    if (rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D)
      mip = rd.Texture2D.MipSlice;
    else if (rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY)
    {
      mip = rd.Texture2DArray.MipSlice;
      slice = rd.Texture2DArray.FirstArraySlice;
    }
    else
      return LogOnce("Ziel hat eine unbekannte Ansicht");

    // Viewport-Ausschnitt in Pixeln der Quelle
    const UINT srcW = std::max<UINT>(1, td.Width >> mip);
    const UINT srcH = std::max<UINT>(1, td.Height >> mip);
    const int vx = std::clamp(static_cast<int>(std::lround(state.viewport.TopLeftX)), 0, static_cast<int>(srcW) - 1);
    const int vy = std::clamp(static_cast<int>(std::lround(state.viewport.TopLeftY)), 0, static_cast<int>(srcH) - 1);
    const int vw = std::clamp(static_cast<int>(std::lround(state.viewport.Width)), 1, static_cast<int>(srcW) - vx);
    const int vh = std::clamp(static_cast<int>(std::lround(state.viewport.Height)), 1, static_cast<int>(srcH) - vy);
    if (!Ensure(vw, vh, grabScale, deepest, td.Format))
      return false;

    // Kopie (Probe: nur eine 64x64-Ecke, fester Aufwand eines Griffs)
    const int cw = probe ? std::min(64, vw) : vw;
    const int ch = probe ? std::min(64, vh) : vh;
    D3D11_BOX box = {static_cast<UINT>(vx), static_cast<UINT>(vy), 0, static_cast<UINT>(vx + cw),
                     static_cast<UINT>(vy + ch), 1};
    ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
    ctx->PSSetShaderResources(0, 2, none);
    ctx->CopySubresourceRegion(m_copy->Get(), 0, 0, 0, 0, source.Get(),
                               D3D11CalcSubresource(mip, slice, td.MipLevels), &box);

    // Paesse: ohne Scissor, ohne Blend, ohne Tiefe
    dx.BindCommon(ctx);
    ctx->RSSetState(dx.RasterNoScissor());
    ctx->OMSetBlendState(dx.BlendOff(), nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(dx.DepthOff(), 0);

    const float u1 = static_cast<float>(cw) / vw, v1 = static_cast<float>(ch) / vh;
    Pass(ctx, PS_GRAB, *m_levels[0], m_copy->GetShaderResource(), 1.0f / vw, 1.0f / vh, u1, v1);
    m_sharp = m_levels[0]->GetShaderResource();
    m_result = Pyramid(ctx);

    // Flaechen, die in diesem Bild schon gezeichnet waren, stecken jetzt im Ergebnis
    m_polluted = (frame == m_grabFrame || frame == m_compositeFrame) ? m_composited
                                                                      : std::vector<std::array<float, 4>>{};
    m_composited.clear();
    m_grabFrame = frame;
    m_grabs++;
    if (timing)
      m_cpuGrabSum +=
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpuStart).count();
    return true;
  }

  void Composite(const CSavedState& state, const float rect[4], float cornerPx, float saturation,
                 UTILS::COLOR::Color tint, float alpha, unsigned int frame, bool liquid, float px)
  {
    if (!m_result)
      return;
    ID3D11DeviceContext* ctx = state.ctx;
    CGlasDX& dx = CGlasDX::Get();
    ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
    ctx->PSSetShaderResources(0, 2, none);
    state.BindTarget();
    dx.BindCommon(ctx);
    ctx->OMSetBlendState(dx.BlendOn(state.blend.Get()), nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(dx.DepthOff(), 0);

    const D3D11_VIEWPORT& vp = state.viewport;
    const float rw = rect[2] - rect[0];
    const float rh = rect[3] - rect[1];
    const float radiusPx = std::min(cornerPx, std::min(rw, rh) * 0.5f);
    CBGlas cb = {};
    cb.size[0] = rw;
    cb.size[1] = rh;
    cb.screen[0] = vp.Width;
    cb.screen[1] = vp.Height;
    cb.tint[0] = ((tint >> 16) & 0xFF) / 255.0f;
    cb.tint[1] = ((tint >> 8) & 0xFF) / 255.0f;
    cb.tint[2] = (tint & 0xFF) / 255.0f;
    cb.tint[3] = ((tint >> 24) & 0xFF) / 255.0f;
    cb.radius = radiusPx;
    cb.px = px;
    cb.saturation = saturation;
    cb.alpha = alpha;
    // Messschalter /tmp/glas-lens-old: Lichtbrechung wie vor dem 29.10.2026 (schmaler Streifen, 14 px)
    static auto lensCheck = std::chrono::steady_clock::time_point();
    static bool lensOld = false;
    if (std::chrono::steady_clock::now() - lensCheck > std::chrono::seconds(1))
    {
      lensCheck = std::chrono::steady_clock::now();
      lensOld = GLAS_SWITCH("/tmp/glas-lens-old");
    }
    cb.lensOld = lensOld ? 1.0f : 0.0f;
    // Farbraum der Ausgabe: Griff zurueckrechnen, Material in SDR, wieder hinein (Plan 3.5)
    const GLAS_DXCOLOR::State& color = GLAS_DXCOLOR::Query(frame);
    cb.black = color.black;
    cb.range = color.range;
    cb.sdrPeak = color.sdrPeak;
    cb.pq = color.pq ? 1.0f : 0.0f;
    dx.SetConstants(ctx, cb);

    ID3D11ShaderResourceView* views[2] = {m_result, m_sharp ? m_sharp : m_result};
    ctx->PSSetShaderResources(0, 2, views);

    // Teilrechteck der Flaeche zeichnen (local bleibt relativ zur ganzen Flaeche: Maske/Rand im Shader stimmen)
    auto drawPart = [&](float x0, float y0, float x1, float y1) {
      GlasVertex v[4];
      const float xs[2] = {x0, x1};
      const float ys[2] = {y0, y1};
      for (int i = 0; i < 4; i++)
      {
        const float x = xs[i & 1];
        const float y = ys[i >> 1];
        v[i].pos[0] = (x - vp.TopLeftX) / vp.Width * 2.0f - 1.0f;
        v[i].pos[1] = 1.0f - (y - vp.TopLeftY) / vp.Height * 2.0f;
        v[i].uv[0] = (x - vp.TopLeftX) / vp.Width;
        v[i].uv[1] = (y - vp.TopLeftY) / vp.Height;
        v[i].local[0] = x - rect[0];
        v[i].local[1] = y - rect[1];
      }
      dx.Draw(ctx, v);
    };
    // Liquid: Rand (Breite wie im Shader: rimW, mindestens der Eckradius) mit LIQUID_SHADER, das Innere schlank
    const float rimW = std::max(6.0f, std::min(radiusPx * 0.8f, 22.0f * px));
    const float e = std::ceil(std::max(rimW, radiusPx) + 1.0f);
    if (liquid && !m_innerOff && rw > 2.0f * e + 8.0f && rh > 2.0f * e + 8.0f)
    {
      const float x0 = rect[0], y0 = rect[1], x1 = rect[2], y1 = rect[3];
      ctx->PSSetShader(dx.PS(PS_LIQUID), nullptr, 0);
      drawPart(x0, y0, x1, y0 + e); // oben (DX: y nach unten)
      drawPart(x0, y1 - e, x1, y1); // unten
      drawPart(x0, y0 + e, x0 + e, y1 - e); // links
      drawPart(x1 - e, y0 + e, x1, y1 - e); // rechts
      ctx->PSSetShader(dx.PS(PS_LIQUID_INNER), nullptr, 0);
      drawPart(x0 + e, y0 + e, x1 - e, y1 - e);
    }
    else
    {
      ctx->PSSetShader(dx.PS(liquid ? PS_LIQUID : PS_COMPOSITE), nullptr, 0);
      drawPart(rect[0], rect[1], rect[2], rect[3]);
    }

    if (frame != m_compositeFrame)
    {
      if (frame != m_grabFrame)
        m_composited.clear();
      m_compositeFrame = frame;
      m_frames++;
    }
    m_composited.push_back({rect[0], rect[1], rect[2], rect[3]});
  }

  //! Messung je Flaeche (Tag timing bzw. /tmp/glas-timing): je 60 Glas-Bilder Griffe und CPU-Zeit eines Griffs
  void Report(bool timing)
  {
    if (!timing || m_frames < 60)
      return;
    CLog::Log(LOGINFO,
              "CGUIBackdropBlurControl: {} Bilder, {} Griffe (1/{} -> 1/{} auf {}x{}), Griff CPU avg {:.2f} ms "
              "(DX11, GPU-Zeiten in GLAS_TIMING)",
              m_frames, m_grabs, m_grabScale, m_deepest, m_levels.empty() ? 0 : m_levels[0]->GetWidth(),
              m_levels.empty() ? 0 : m_levels[0]->GetHeight(), m_grabs ? m_cpuGrabSum / m_grabs : 0.0);
    m_frames = m_grabs = 0;
    m_cpuGrabSum = 0;
  }

  bool m_innerOff = false; //!< Messschalter /tmp/glas-liquid-inner-off: ganze Flaeche mit LIQUID_SHADER

private:
  bool LogOnce(const char* what)
  {
    if (!m_logged)
      CLog::Log(LOGWARNING, "CGUIBackdropBlurControl: kein Griff moeglich: {}", what);
    m_logged = true;
    return false;
  }

  //! Ein Pass in eine Stufe: Eingabe in (Bildkoordinaten 0..u1, 0..v1), off = Abstand der Abtastungen
  void Pass(ID3D11DeviceContext* ctx, PixelShader ps, CD3DTexture& out, ID3D11ShaderResourceView* in, float offX,
            float offY, float u1 = 1.0f, float v1 = 1.0f)
  {
    CGlasDX& dx = CGlasDX::Get();
    ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
    ctx->PSSetShaderResources(0, 2, none);
    ID3D11RenderTargetView* rtv = out.GetRenderTarget();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    const D3D11_VIEWPORT vp = {0.0f, 0.0f, static_cast<float>(out.GetWidth()), static_cast<float>(out.GetHeight()),
                               0.0f, 1.0f};
    ctx->RSSetViewports(1, &vp);
    CBGlas cb = {};
    cb.off[0] = offX;
    cb.off[1] = offY;
    dx.SetConstants(ctx, cb);
    ctx->PSSetShader(dx.PS(ps), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &in);
    const GlasVertex v[4] = {{{-1.0f, 1.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}},
                             {{1.0f, 1.0f}, {u1, 0.0f}, {0.0f, 0.0f}},
                             {{-1.0f, -1.0f}, {0.0f, v1}, {0.0f, 0.0f}},
                             {{1.0f, -1.0f}, {u1, v1}, {0.0f, 0.0f}}};
    dx.Draw(ctx, v);
  }

  //! Weichzeichnen ab Stufe 1 (Eingabe = Stufe 0), Ergebnis eine Stufe ueber der tiefsten
  ID3D11ShaderResourceView* Pyramid(ID3D11DeviceContext* ctx)
  {
    // Messschalter /tmp/glas-pyramid-off: keine Paesse, das Glas zeigt die erste Stufe (nur zum Messen)
    static auto lastCheck = std::chrono::steady_clock::time_point();
    static bool off = false;
    if (std::chrono::steady_clock::now() - lastCheck > std::chrono::seconds(1))
    {
      lastCheck = std::chrono::steady_clock::now();
      off = GLAS_SWITCH("/tmp/glas-pyramid-off");
    }
    if (off || m_levels.size() < 2)
      return m_levels[0]->GetShaderResource();
    for (size_t i = 1; i < m_levels.size(); i++)
    {
      CD3DTexture& in = *m_levels[i - 1];
      Pass(ctx, PS_DOWN, *m_levels[i], in.GetShaderResource(), 1.0f / in.GetWidth(), 1.0f / in.GetHeight());
    }
    CD3DTexture& in = *m_levels.back();
    Pass(ctx, PS_UP, *m_out, in.GetShaderResource(), 0.5f / in.GetWidth(), 0.5f / in.GetHeight());
    return m_out->GetShaderResource();
  }

  // Pyramide: Kopie in voller Groesse, Stufe 0 = Griff (1/grabScale), dann halbieren bis 1/deepest; Ergebnis eine
  // Stufe ueber der tiefsten
  bool Ensure(int w, int h, int grabScale, int deepest, DXGI_FORMAT format)
  {
    if (w == m_w && h == m_h && grabScale == m_grabScale && deepest == m_deepest && format == m_format &&
        !m_levels.empty())
      return true;
    m_levels.clear();
    m_copy.reset();
    m_out.reset();
    m_result = nullptr;
    m_sharp = nullptr;
    m_w = m_h = 0;
    auto create = [format](int tw, int th) {
      auto tex = std::make_unique<CD3DTexture>();
      if (!tex->Create(std::max(1, tw), std::max(1, th), 1, D3D11_USAGE_DEFAULT, format))
        tex.reset();
      return tex;
    };
    m_copy = create(w, h);
    for (int div = grabScale; div <= deepest; div *= 2)
      m_levels.push_back(create(w / div, h / div));
    if (m_levels.size() >= 2)
      m_out = create(m_levels[m_levels.size() - 2]->GetWidth(), m_levels[m_levels.size() - 2]->GetHeight());
    bool ok = m_copy && !m_levels.empty() && m_out;
    for (const auto& level : m_levels)
      ok = ok && level && level->GetRenderTarget() && level->GetShaderResource();
    ok = ok && m_out->GetRenderTarget() && m_out->GetShaderResource();
    if (!ok)
    {
      m_levels.clear();
      m_copy.reset();
      m_out.reset();
      return LogOnce("Texturen fuer die Pyramide liessen sich nicht anlegen");
    }
    m_w = w;
    m_h = h;
    m_grabScale = grabScale;
    m_deepest = deepest;
    m_format = format;
    CLog::Log(LOGDEBUG, "CGUIBackdropBlurControl: Pyramide {}x{} (Format {}), 1/{} bis 1/{}", w, h,
              static_cast<int>(format), grabScale, deepest);
    return true;
  }

  unsigned int m_generation = ~0u;
  bool m_logged = false;
  std::unique_ptr<CD3DTexture> m_copy; //!< Kopie des Viewports in voller Groesse (Format der Quelle)
  std::vector<std::unique_ptr<CD3DTexture>> m_levels;
  std::unique_ptr<CD3DTexture> m_out;
  ID3D11ShaderResourceView* m_result = nullptr; //!< gehoert m_out bzw. m_levels[0]
  ID3D11ShaderResourceView* m_sharp = nullptr; //!< schaerfere Quelle fuer die Lichtbrechung am Rand (Stufe 0)
  int m_w = 0;
  int m_h = 0;
  int m_grabScale = 0;
  int m_deepest = 0;
  DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
  int m_lastReason = 0;
  unsigned int m_reasons[4] = {};
  unsigned int m_throttledCount = 0;
  unsigned int m_diagFrames = 0;
  unsigned int m_wantFrame = 0;
  unsigned int m_grabFrame = 0;
  unsigned int m_compositeFrame = 0;
  std::vector<std::array<float, 4>> m_composited;
  std::vector<std::array<float, 4>> m_polluted;
  int m_frames = 0;
  int m_grabs = 0;
  double m_cpuGrabSum = 0;
};
} // namespace

namespace GLAS_GUITARGET
{
void NoteBlur();
bool Diag();
bool Timing();
bool IsWindowMark(const CRect& region, unsigned int currentTime);
void NoteGlass(const float rect[4]);
}

CGUIBackdropBlurControl::CGUIBackdropBlurControl(
    int parentID, int controlID, float posX, float posY, float width, float height)
  : CGUIControl(parentID, controlID, posX, posY, width, height)
{
  ControlType = GUICONTROL_BACKDROPBLUR;
}

CGUIBackdropBlurControl::CGUIBackdropBlurControl(const CGUIBackdropBlurControl& from)
  : CGUIControl(from),
    m_blurRadius(from.m_blurRadius),
    m_cornerRadius(from.m_cornerRadius),
    m_saturation(from.m_saturation),
    m_tint(from.m_tint),
    m_alwaysRefresh(from.m_alwaysRefresh),
    m_timing(from.m_timing),
    m_grabScale(from.m_grabScale),
    m_updateRate(from.m_updateRate),
    m_grabProbe(from.m_grabProbe),
    m_liquid(from.m_liquid),
    m_sourceId(from.m_sourceId)
{
}

CGUIBackdropBlurControl::~CGUIBackdropBlurControl() = default;

void CGUIBackdropBlurControl::Process(unsigned int currentTime, CDirtyRegionList& dirtyregions)
{
  // <fitgrouplist>: Breite und Lage der mittig ausgerichteten Grouplist im selben Elternteil (Logo-Pille: so breit wie
  // ihre sichtbaren Stuecke, die der Skin nicht addieren kann)
  if (m_fitId > 0 && m_parentControl)
  {
    auto* parent = dynamic_cast<CGUIControlGroup*>(m_parentControl);
    auto* list = parent ? dynamic_cast<CGUIControlGroupList*>(parent->GetControl(m_fitId)) : nullptr;
    if (list)
    {
      const float total = list->GetTotalSize();
      // eigene Breite der Liste: CGUIControlGroupList::GetWidth meldet waagrecht die Breite des Inhalts
      const float x = list->GetXPosition() + (list->CGUIControl::GetWidth() - total) / 2.0f;
      if (total > 0.0f && (std::fabs(total - m_width) > 0.5f || std::fabs(x - m_posX) > 0.5f))
      {
        SetPosition(x, m_posY);
        SetWidth(total);
        m_backgroundDirty[0] = m_backgroundDirty[1] = true;
      }
    }
  }
  // Process laeuft in Zeichenreihenfolge: die Liste enthaelt bisher nur Flaechen, die hinter diesem Control
  // liegen (auch aus dem Fenster unter einem Dialog). Schneidet eine den eigenen Bereich, neu weichzeichnen.
  if (m_alwaysRefresh)
  {
    // Messmodus: jedes Bild neu zeichnen und neu weichzeichnen, damit Bildrate und Log die Kosten zeigen
    MarkDirtyRegion();
    m_backgroundDirty[0] = m_backgroundDirty[1] = true;
  }
  else if (currentTime - m_lastProcess > 250 || m_screenRect.IsEmpty())
    m_backgroundDirty[0] = m_backgroundDirty[1] = true; // war unsichtbar: Hintergrund unbekannt
  else
  {
    for (const auto& region : dirtyregions)
    {
      CRect overlap(region);
      if (GLAS_GUITARGET::IsWindowMark(region, currentTime))
        continue;
      if (!overlap.Intersect(m_screenRect).IsEmpty())
      {
        m_backgroundDirty[0] = m_backgroundDirty[1] = true;
        // Diagnose: welche Aenderung hinter dem Glas (hoechstens 4 Zeilen je Sekunde)
        static unsigned int s_second = 0, s_lines = 0;
        if (GLAS_GUITARGET::Diag() && (currentTime / 1000 != s_second || s_lines < 4))
        {
          if (currentTime / 1000 != s_second)
            s_lines = 0;
          s_second = currentTime / 1000;
          s_lines++;
          CLog::Log(LOGINFO, "GLAS_DIAG: Glas {:.0f},{:.0f} {:.0f}x{:.0f} (Fenster {}) sieht Aenderung {:.0f},{:.0f} {:.0f}x{:.0f}",
                    m_screenRect.x1, m_screenRect.y1, m_screenRect.Width(), m_screenRect.Height(), GetParentID(),
                    region.x1, region.y1, region.Width(), region.Height());
        }
        break;
      }
    }
  }
  // gedrosselt und noch nicht nachgeholt: weiter zeichnen, bis der letzte Stand weichgezeichnet ist. Nur beim
  // Fenstermanager anmelden, nicht als eigene Aenderung: sonst hielte jedes darueberliegende Glas seinen
  // Hintergrund fuer veraendert und griffe ungedrosselt neu
  if (!GLAS_STEREO::Active(CServiceBroker::GetWinSystem()->GetGfxContext()))
    m_pendingRecompute[1] = false;
  if (m_pendingRecompute[0] || m_pendingRecompute[1])
  {
    CServiceBroker::GetGUI()->GetWindowManager().MarkDirty(m_screenRect);
    m_backgroundDirty[0] = m_backgroundDirty[1] = true;
  }
  if (m_blurRadius > 0)
    GLAS_GUITARGET::NoteBlur();
  // Video-Glas: das Video liegt im Backbuffer unter der Oberflaeche; solange es laeuft, ist jedes Bild ein neuer
  // Hintergrund (Pause: kein Dauer-Neugriff)
  const auto& video = GLAS_DXVIDEO::Query(currentTime);
  if (m_blurRadius > 0 && video.moving)
  {
    MarkDirtyRegion();
    m_backgroundDirty[0] = m_backgroundDirty[1] = true;
  }
  // Aenderung dahinter dem gemeinsamen Griff melden, damit schon das erste Glas des Bildes greift. Glas mit eigener
  // Quelle (<source>) greift unter Windows wie jedes andere
  if (m_backgroundDirty[0] && m_blurRadius > 0)
    CBackdropBlurShared::Get(0, video.video ? 0 : GetParentID()).WantGrab(currentTime);
  m_lastProcess = currentTime;
  m_frame = currentTime;
  if (m_tint.Update())
    MarkDirtyRegion();
  CGUIControl::Process(currentTime, dirtyregions);
}

void CGUIBackdropBlurControl::Render()
{
  // Vergleichsmessung: blurradius 0 zeichnet nichts (mit refresh=always misst das die Bildrate ohne Weichzeichnen)
  if (m_blurRadius <= 0)
    return;
  CGraphicContext& context = CServiceBroker::GetWinSystem()->GetGfxContext();
  // Stereo-3D: je Auge eigener Griff, ausser beide Augen teilen ihn (Vorgabe, wie GLES-Fassung) oder Video laeuft
  const bool stereo = GLAS_STEREO::Active(context);
  const int eye = GLAS_STEREO::Eye(context);
  const bool video = GLAS_DXVIDEO::Query(m_frame).video;
  const bool shareEye = stereo && eye == 1 && (GLAS_STEREO::ShareEyes() || video);
  CBackdropBlurShared& shared = CBackdropBlurShared::Get(shareEye ? 0 : eye, video ? 0 : GetParentID());
  if (!shared.Prepare())
    return;

  // Beschnitt (Grouplist, Liste): Kodi beschneidet Texturen in Software. Ganz draussen: nichts zeichnen
  CRect clip = context.GetClipRegion();
  clip.Intersect(CRect(m_posX, m_posY, m_posX + m_width, m_posY + m_height));
  if (clip.IsEmpty())
    return;
  const bool clipped = clip.x1 > m_posX + 0.5f || clip.y1 > m_posY + 0.5f || clip.x2 < m_posX + m_width - 0.5f ||
                       clip.y2 < m_posY + m_height - 0.5f;

  // Ecken mit der aktuellen Transformation (Animationen) in Skin-Bildschirmkoordinaten, achsenparallele Huelle
  const float cx[4] = {m_posX, m_posX + m_width, m_posX, m_posX + m_width};
  const float cy[4] = {m_posY, m_posY, m_posY + m_height, m_posY + m_height};
  float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
  for (int i = 0; i < 4; i++)
  {
    const float x = context.ScaleFinalXCoord(cx[i], cy[i]);
    const float y = context.ScaleFinalYCoord(cx[i], cy[i]);
    x0 = std::min(x0, x);
    x1 = std::max(x1, x);
    y0 = std::min(y0, y);
    y1 = std::max(y1, y);
  }
  m_screenRect = CRect(x0, y0, x1, y1);

  ID3D11DeviceContext* ctx = DX::DeviceResources::Get()->GetD3DContext();
  if (!ctx)
    return;
  CSavedState state(ctx);
  if (!state.rtv || !state.numViewports || state.viewport.Width < 1 || state.viewport.Height < 1)
    return;
  const D3D11_VIEWPORT& vp = state.viewport;

  // Skin-Bildschirm -> Pixel im Viewport (beide mit Ursprung oben links, y nach unten)
  const float sx = vp.Width / context.GetWidth();
  const float sy = vp.Height / context.GetHeight();
  float rect[4] = {vp.TopLeftX + x0 * sx, vp.TopLeftY + y0 * sy, vp.TopLeftX + x1 * sx, vp.TopLeftY + y1 * sy};
  if (stereo)
  {
    // Stereo-3D: Ecken durch dieselbe Projektion wie die Texturen (Tiefenversatz je Auge). CGUIShaderDX::Project
    // liefert Pixel mit Viewport-Versatz, y nach unten
    CRenderSystemBase* renderSystem = CServiceBroker::GetRenderSystem();
    float px0 = 1e9f, py0 = 1e9f, px1 = -1e9f, py1 = -1e9f;
    for (int i = 0; i < 4; i++)
    {
      float x = context.ScaleFinalXCoord(cx[i], cy[i]);
      float y = context.ScaleFinalYCoord(cx[i], cy[i]);
      float z = context.ScaleFinalZCoord(cx[i], cy[i]);
      renderSystem->Project(x, y, z);
      px0 = std::min(px0, x);
      px1 = std::max(px1, x);
      py0 = std::min(py0, y);
      py1 = std::max(py1, y);
    }
    rect[0] = px0;
    rect[1] = py0;
    rect[2] = px1;
    rect[3] = py1;
  }
  if (rect[2] - rect[0] < 2 || rect[3] - rect[1] < 2)
    return;
  D3D11_RECT clipPx = {};
  if (clipped && !stereo)
  {
    // sichtbarer Teil in Viewport-Pixeln (gleiche Transformation wie die Ecken)
    const float kx[4] = {clip.x1, clip.x2, clip.x1, clip.x2};
    const float ky[4] = {clip.y1, clip.y1, clip.y2, clip.y2};
    float qx0 = 1e9f, qy0 = 1e9f, qx1 = -1e9f, qy1 = -1e9f;
    for (int i = 0; i < 4; i++)
    {
      const float x = context.ScaleFinalXCoord(kx[i], ky[i]);
      const float y = context.ScaleFinalYCoord(kx[i], ky[i]);
      qx0 = std::min(qx0, x);
      qx1 = std::max(qx1, x);
      qy0 = std::min(qy0, y);
      qy1 = std::max(qy1, y);
    }
    clipPx = {static_cast<LONG>(std::floor(vp.TopLeftX + qx0 * sx)), static_cast<LONG>(std::floor(vp.TopLeftY + qy0 * sy)),
              static_cast<LONG>(std::ceil(vp.TopLeftX + qx1 * sx)), static_cast<LONG>(std::ceil(vp.TopLeftY + qy1 * sy))};
    if (clipPx.right <= clipPx.left || clipPx.bottom <= clipPx.top)
      return;
  }

  // Staerke: tiefste Stufe der Pyramide; blurradius 48 (Apple-Material) -> 1/16 der Bildschirmaufloesung
  int deepest = 4;
  while (deepest < 64 && deepest * 3 < m_blurRadius * sx)
    deepest *= 2;
  int grabScaleWanted = m_grabScale;
  {
    // Messschalter /tmp/glas-grabscale (Zahl): erste Stufe fuer alle Glasflaechen
    static auto lastCheck = std::chrono::steady_clock::time_point();
    static int forced = 0;
    if (std::chrono::steady_clock::now() - lastCheck > std::chrono::seconds(1))
    {
      lastCheck = std::chrono::steady_clock::now();
      forced = 0;
      if (FILE* f = fopen("/tmp/glas-grabscale", "r"))
      {
        if (fscanf(f, "%d", &forced) != 1)
          forced = 0;
        fclose(f);
      }
    }
    if (forced > 0)
      grabScaleWanted = forced;
  }
  const int grabScale = std::clamp(grabScaleWanted, 2, deepest / 2);
  const float scale = (m_width > 0) ? (x1 - x0) / m_width : 1.0f;
  const float alpha = ((context.MergeAlpha(0xFF000000) >> 24) & 0xFF) / 255.0f;
  const bool timing = m_timing || GLAS_GUITARGET::Timing();

  CGlasDX::Get().GlassBegin();
  auto need = shareEye ? CBackdropBlurShared::Need::NONE
                       : shared.NeedsGrab(rect, m_frame, m_backgroundDirty[eye], grabScale, deepest);
  // Drosselung: bei laufender Bewegung hinter dem Glas hoechstens updaterate Mal je Sekunde neu weichzeichnen,
  // dazwischen der Zwischenspeicher; der letzte Stand wird nachgeholt (m_pendingRecompute). Ueber Video nie
  const bool throttled = need == CBackdropBlurShared::Need::BACKGROUND && m_updateRate > 0 && !video &&
                         m_frame - shared.LastGrabFrame() < static_cast<unsigned int>(1000.0f / m_updateRate);
  // Messschalter /tmp/glas-grablog: jede Entscheidung mit Griff-Bedarf ins Log (Fenster, Flaeche, Grund, gedrosselt)
  if (need != CBackdropBlurShared::Need::NONE && GLAS_SWITCH("/tmp/glas-grablog"))
    CLog::Log(LOGINFO, "GLAS_GRAB: Bild {} Fenster {} Glas {:.0f},{:.0f} {:.0f}x{:.0f} {} Grund {} dahinter {}{}", m_frame,
              GetParentID(), x0, y0, x1 - x0, y1 - y0,
              need == CBackdropBlurShared::Need::FORCED ? "erzwungen" : "Hintergrund", shared.LastReason(),
              m_backgroundDirty[eye] ? "geaendert" : "ruhig", throttled ? ", gedrosselt" : "");
  if (need != CBackdropBlurShared::Need::NONE && !throttled)
    shared.Grab(state, m_frame, grabScale, deepest, m_grabProbe, timing);
  m_pendingRecompute[eye] = throttled;
  m_backgroundDirty[eye] = false;
  if (GLAS_GUITARGET::Diag())
  {
    shared.Count(need != CBackdropBlurShared::Need::NONE && !throttled, throttled);
    shared.DiagReport();
  }
  shared.m_innerOff = GLAS_STEREO::LiquidInnerOff();
  if (clipped && !stereo)
  {
    state.clipRaster = CGlasDX::Get().RasterScissor();
    state.clipRect = clipPx;
  }
  shared.Composite(state, rect, m_cornerRadius * sx * scale, m_saturation, m_tint, alpha, m_frame, m_liquid,
                   sx * scale);
  state.clipRaster = nullptr;
  CGlasDX::Get().GlassEnd();
  if (GLAS_GUITARGET::Diag())
    GLAS_GUITARGET::NoteGlass(rect);
  shared.Report(timing);

  // Zustand zurueck (Ziel, Viewport, Scissor; Rest ueber Kodis ApplyStateBlock)
  state.Restore();
}

void CGUIBackdropBlurControl::FreeResources(bool immediately)
{
  CGUIControl::FreeResources(immediately);
}

/*!
 \brief Haken aus CApplication::Render (Begin nach BeginRender, End vor EndRender) und dem Fenstermanager. Unter GLES
        schalten sie das eigene Zwischenbild der Oberflaeche (Mali-Umweg); unter DX11 braucht es das nicht, die Haken
        dienen nur der Messung (GLAS_FRAMES, GLAS_TIMING), der Diagnose und der Erkennung pauschaler
        Fenstermarkierungen. Vorgaben wie Kodi: ganze Bereiche, Dialog- und Vollbildmarkierung bleiben.
*/
namespace GLAS_GUITARGET
{
namespace
{
std::chrono::steady_clock::time_point g_lastCheck;
// Diagnose (/tmp/glas-diag): Aenderungen je Control ins Log
bool g_diag = false;
// Bildzeiten (/tmp/glas-frames): je Sekunde gezeichnete Bilder und verspaetete Bilder
bool g_frames = false;
// Zeitmessung (/tmp/glas-timing): CPU je Bild, GPU je Bild und davon Glas (Zeitstempel, ohne zu warten)
bool g_timing = false;
std::chrono::steady_clock::time_point g_tBegin;
double g_cpuSum = 0, g_cpuMax = 0;
int g_timedFrames = 0;
std::chrono::steady_clock::time_point g_lastTimingReport;
std::chrono::steady_clock::time_point g_lastFrame;
std::chrono::steady_clock::time_point g_lastFrameReport;
unsigned int g_frameCount = 0, g_lateCount = 0, g_late2Count = 0;
double g_worstMs = 0;
} // namespace

bool Diag()
{
  return g_diag;
}

bool Timing()
{
  return g_timing;
}

void NoteGlass(const float*) {}

//! Diagnose: Aenderungen je Control in der laufenden Sekunde (Anzahl, Gruende, letzte Flaeche)
struct Change
{
  unsigned int count = 0;
  int why = 0;
  CRect region;
};
std::map<std::string, Change> g_changes;
std::chrono::steady_clock::time_point g_lastChangeReport;
unsigned int g_renderedFrames = 0;

/*!
 \brief Pauschale Fenstermarkierung (wie GLES-Fassung): Kodi markiert ein ganzes Fenster als geaendert, sobald sich in
        einem Dialog darueber etwas tut. Das Glas ignoriert Flaechen, die ein Fenster nur wegen einer solchen
        Markierung eintraegt (ohne eigene Animation, Verschiebung oder Ungueltigkeit).
*/
std::vector<CRect> g_windowMarks;
unsigned int g_windowMarkTime = 0;

bool IsWindowMark(const CRect& region, unsigned int currentTime)
{
  if (currentTime != g_windowMarkTime)
    return false;
  return std::find(g_windowMarks.begin(), g_windowMarks.end(), region) != g_windowMarks.end();
}

void NoteChange(const CGUIControl* control, const CRect& region, int why, unsigned int currentTime)
{
  if (control->GetParentID() == 0 && control->GetControlType() == CGUIControl::GUICONTROL_GROUP &&
      (why & (2 | 4 | 8)) == 0)
  {
    if (currentTime != g_windowMarkTime)
    {
      g_windowMarks.clear();
      g_windowMarkTime = currentTime;
    }
    g_windowMarks.push_back(region);
  }
  if (!g_diag)
    return;
  Change& change = g_changes[std::to_string(control->GetParentID()) + "/" + std::to_string(control->GetID()) +
                             " Typ " + std::to_string(static_cast<int>(control->GetControlType())) + " \"" +
                             control->GetDescription() + "\""];
  change.count++;
  change.why |= why;
  change.region = region;
}

//! Gruende: 1 vorher markiert, 2 ungueltig, 4 Animation, 8 Flaeche verschoben, 16 in Process markiert
void ReportChanges(const std::chrono::steady_clock::time_point& now)
{
  if (now - g_lastChangeReport < std::chrono::seconds(1))
    return;
  g_lastChangeReport = now;
  std::vector<std::pair<unsigned int, std::string>> top;
  for (const auto& entry : g_changes)
    top.emplace_back(entry.second.count, entry.first);
  std::sort(top.rbegin(), top.rend());
  CLog::Log(LOGINFO, "GLAS_DIAG: {} gezeichnete Bilder, {} Controls mit Aenderungen", g_renderedFrames, top.size());
  for (size_t i = 0; i < top.size() && i < 8; i++)
  {
    const Change& c = g_changes[top[i].second];
    CLog::Log(LOGINFO, "GLAS_DIAG:   {:3}x Grund {:2} {} {:.0f},{:.0f} {:.0f}x{:.0f}", c.count, c.why, top[i].second,
              c.region.x1, c.region.y1, c.region.Width(), c.region.Height());
  }
  g_changes.clear();
  g_renderedFrames = 0;
}

void NoteBlur() {}

void Begin()
{
  const auto now = std::chrono::steady_clock::now();
  g_tBegin = now;
  if (now - g_lastCheck > std::chrono::seconds(1))
  {
    g_lastCheck = now;
    const bool diag = GLAS_SWITCH("/tmp/glas-diag");
    if (diag != g_diag)
      CLog::Log(LOGINFO, "GLAS_GUITARGET: Diagnose {}", diag ? "an" : "aus");
    g_diag = diag;
    g_frames = GLAS_SWITCH("/tmp/glas-frames");
    g_timing = GLAS_SWITCH("/tmp/glas-timing");
  }
  if (g_diag)
    ReportChanges(now);
  CGlasDX::Get().FrameBegin(g_timing);
}

void CountFrame()
{
  const auto now = std::chrono::steady_clock::now();
  const double ms = std::chrono::duration<double, std::milli>(now - g_lastFrame).count();
  g_lastFrame = now;
  // Bildwechsel der Anzeige (60 Hz: 16,7 ms); verspaetet = mehr als 1,5 Bildwechsel
  const float fps = CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS();
  const double vsync = 1000.0 / (fps > 1.0f ? fps : 60.0f);
  // nur zusammenhaengende Bewegung zaehlen (Pausen ueber 4 Bildwechsel sind Stillstand)
  if (ms < vsync * 4.5)
  {
    g_frameCount++;
    if (ms > vsync * 1.5)
      g_lateCount++;
    if (ms > vsync * 2.5)
      g_late2Count++;
    g_worstMs = std::max(g_worstMs, ms);
  }
  if (now - g_lastFrameReport >= std::chrono::seconds(1))
  {
    if (g_frameCount)
      CLog::Log(LOGINFO,
                "GLAS_FRAMES: {} Bilder in Bewegung bei {:.2f} Hz, {} um einen Bildwechsel verspaetet, {} um mehr, "
                "laengstes {:.1f} ms (Fenster {})",
                g_frameCount, CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS(), g_lateCount - g_late2Count,
                g_late2Count, g_worstMs, CServiceBroker::GetGUI()->GetWindowManager().GetActiveWindowOrDialog());
    g_lastFrameReport = now;
    g_frameCount = g_lateCount = g_late2Count = 0;
    g_worstMs = 0;
  }
}

void End(bool rendered)
{
  CGlasDX::Get().FrameEnd(rendered);
  if (g_frames && rendered)
    CountFrame();
  if (g_diag && rendered)
    g_renderedFrames++;
  if (!g_timing || !rendered)
    return;
  const double cpuMs =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_tBegin).count();
  g_timedFrames++;
  g_cpuSum += cpuMs;
  g_cpuMax = std::max(g_cpuMax, cpuMs);
  const auto now = std::chrono::steady_clock::now();
  if (now - g_lastTimingReport >= std::chrono::seconds(1))
  {
    double frameAvg = 0, frameMax = 0, glassAvg = 0, glassMax = 0;
    int gpuN = 0;
    CGlasDX::Get().TakeGpu(frameAvg, frameMax, glassAvg, glassMax, gpuN);
    CLog::Log(LOGINFO,
              "GLAS_TIMING: {} Bilder, Zeichnen CPU avg {:.2f} / max {:.2f} ms, GPU je Bild avg {:.2f} / max {:.2f} "
              "ms, davon Glas avg {:.2f} / max {:.2f} ms ({} gemessen, DX11)",
              g_timedFrames, g_cpuSum / g_timedFrames, g_cpuMax, frameAvg, frameMax, glassAvg, glassMax, gpuN);
    g_lastTimingReport = now;
    g_timedFrames = 0;
    g_cpuSum = g_cpuMax = 0;
  }
}

bool Active()
{
  return false;
}

bool KeepDialogMark()
{
  return true;
}

bool SkipFullscreenMark()
{
  return false;
}

bool Regions(CDirtyRegionList&)
{
  return false;
}
} // namespace GLAS_GUITARGET
