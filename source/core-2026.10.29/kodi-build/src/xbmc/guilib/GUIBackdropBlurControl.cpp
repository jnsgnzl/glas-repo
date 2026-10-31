/*
 *  Copyright (C) 2026 Team Kodi / Glas
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GUIBackdropBlurControl.h"

#include "ServiceBroker.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "cores/VideoPlayer/Interface/StreamInfo.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindow.h"
#include "guilib/GUIWindowManager.h"
#include "rendering/RenderSystem.h"
#include "utils/StringUtils.h"
#include "utils/TimeUtils.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include <algorithm>
#include <array>
#include <map>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#include <atomic>
#include <fcntl.h>
#include <mutex>
#include <thread>
#include <unistd.h>

#include <sys/ioctl.h>

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

//! Video-Glas auch im Stereo-Betrieb (am 17.10.2026 ueber einem 3D-Film geprueft: das Videogeraet liefert ein normales
//! Bild, das Glas sah richtig aus). Ob es zu sehen ist, entscheidet die Skin (glas_coreblur, Skin.HasSetting
//! (glas_stereo_video)); Messschalter /tmp/glas-stereo-video-off schaltet es im Kern ab
inline bool VideoAllowed()
{
  static auto lastCheck = std::chrono::steady_clock::time_point();
  static bool allowed = true;
  const auto now = std::chrono::steady_clock::now();
  if (now - lastCheck > std::chrono::seconds(1))
  {
    lastCheck = now;
    allowed = access("/tmp/glas-stereo-video-off", F_OK) != 0;
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
    off = access("/tmp/glas-liquid-inner-off", F_OK) == 0;
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
    share = access("/tmp/glas-stereo-share-off", F_OK) != 0;
  }
  return share;
}

//! Glas mit eigener Quelle (<source>): Messschalter /tmp/glas-source-off (wieder greifen), /tmp/glas-source-always
//! (Quelle in jedem Bild neu zeichnen statt nur nach einer Aenderung)
inline bool SourceOff()
{
  static auto lastCheck = std::chrono::steady_clock::time_point();
  static bool off = false;
  const auto now = std::chrono::steady_clock::now();
  if (now - lastCheck > std::chrono::seconds(1))
  {
    lastCheck = now;
    off = access("/tmp/glas-source-off", F_OK) == 0;
  }
  return off;
}

//! Messschalter /tmp/glas-source-timing: CPU-Zeit am Anfang des Bildes ins Log (ohne glFinish wie /tmp/glas-timing)
inline bool SourceTiming()
{
  static auto lastCheck = std::chrono::steady_clock::time_point();
  static bool timing = false;
  const auto now = std::chrono::steady_clock::now();
  if (now - lastCheck > std::chrono::seconds(1))
  {
    lastCheck = now;
    timing = access("/tmp/glas-source-timing", F_OK) == 0;
  }
  return timing;
}

inline bool SourceAlways()
{
  static auto lastCheck = std::chrono::steady_clock::time_point();
  static bool always = false;
  const auto now = std::chrono::steady_clock::now();
  if (now - lastCheck > std::chrono::seconds(1))
  {
    lastCheck = now;
    always = access("/tmp/glas-source-always", F_OK) == 0;
  }
  return always;
}

//! Stereo-3D: Video-Glas am Anfang des Bildes nur aus dem Videobild (Vorgabe an; /tmp/glas-stereo-preframe-off)
inline bool PreFrameAllowed()
{
  static auto lastCheck = std::chrono::steady_clock::time_point();
  static bool allowed = true;
  const auto now = std::chrono::steady_clock::now();
  if (now - lastCheck > std::chrono::seconds(1))
  {
    lastCheck = now;
    allowed = access("/tmp/glas-stereo-preframe-off", F_OK) != 0;
  }
  return allowed;
}

//! Stereo-3D: Abstand zwischen zwei Video-Glas-Bildern in ms (Vorgabe 0 = jedes Videobild, live: gemessen kamen mit
//! 80/125 ms gleich viele Bilder zu spaet wie ohne Drosselung, das Glas wirkte nur traege); Messschalter: Zahl in
//! /tmp/glas-stereo-video-ms (je Sekunde gelesen)
inline unsigned int VideoIntervalMs()
{
  static auto lastCheck = std::chrono::steady_clock::time_point();
  static unsigned int ms = 0;
  const auto now = std::chrono::steady_clock::now();
  if (now - lastCheck > std::chrono::seconds(1))
  {
    lastCheck = now;
    ms = 0;
    if (FILE* f = fopen("/tmp/glas-stereo-video-ms", "r"))
    {
      unsigned int v = 0;
      if (fscanf(f, "%u", &v) == 1 && v <= 1000)
        ms = v;
      fclose(f);
    }
  }
  return ms;
}
} // namespace GLAS_STEREO

#if defined(HAS_GLES)
#include "system_gl.h"

#include <EGL/egl.h>

//! Video-Glas: Geraet /dev/amvideocap0 nur von einem Leser zur Zeit. Kodis Bildschirmfoto (ScreenshotAML) liest
//! es ebenfalls; gleichzeitig lieferte eines von beiden kein Bild (schwarzes Video bzw. fehlende Oberflaeche im Foto)
namespace GLAS_VIDEODEVICE
{
std::mutex& Mutex()
{
  static std::mutex mutex;
  return mutex;
}

//! Ein einziger, dauerhaft offener Zugriff (nur unter Mutex() benutzen). Der amvideocap-Treiber der Vero reserviert
//! bei jedem open() 32 MB im Videospeicher (codec_mm) und gibt sie beim close() NICHT zurueck: nach elf Oeffnungen
//! (Video-Glas, Bildschirmfotos) waren 352 von 432 MB belegt und 3D-Filme (MVC) bekamen keinen Speicher mehr
//! (17.10.2026, /sys/class/codec_mm/codec_mm_dump). Daher hoechstens einmal oeffnen, nie schliessen: 32 MB fest.
int Fd()
{
  static int fd = -1;
  if (fd < 0)
    fd = open("/dev/amvideocap0", O_RDWR);
  return fd;
}
} // namespace GLAS_VIDEODEVICE

#if HAS_GLES >= 3
namespace GLAS_GUITARGET
{
//! Ist fbo das Zwischenbild der Oberflaeche, dessen Farbtextur (Griff per Shader statt glBlitFramebuffer)
bool SourceTexture(GLint fbo, GLuint& tex, int& w, int& h);
}
#endif

namespace
{
const char* VERTEX_SHADER = R"(
attribute vec2 a_pos;
attribute vec2 a_uv;
attribute vec2 a_local;
varying vec2 v_uv;
varying vec2 v_local;
void main()
{
  v_uv = a_uv;
  v_local = a_local;
  gl_Position = vec4(a_pos, 0.0, 1.0);
}
)";

// Dual Kawase, Verkleinern: Mitte x4 plus vier Diagonalen
const char* DOWN_SHADER = R"(
precision mediump float;
uniform sampler2D u_tex;
uniform vec2 u_off;
varying vec2 v_uv;
void main()
{
  vec4 sum = texture2D(u_tex, v_uv) * 4.0;
  sum += texture2D(u_tex, v_uv - u_off);
  sum += texture2D(u_tex, v_uv + u_off);
  sum += texture2D(u_tex, v_uv + vec2(u_off.x, -u_off.y));
  sum += texture2D(u_tex, v_uv - vec2(u_off.x, -u_off.y));
  gl_FragColor = sum / 8.0;
}
)";

// Griff aus dem Zwischenbild (Textur): auf 1/4 verkleinern, vier bilineare Abtastungen je Pixel = Mittel aus 4 x 4
// Quellpixeln (u_texel = ein Quellpixel). Ersetzt glBlitFramebuffer (eine Abtastung = 2 x 2 Pixel, flimmerte eher).
// Gemessen gleich schnell wie der Blit (29.10.2026): ein Griff kostet auf der Mali unabhaengig davon, wie viel gelesen
// wird - der Treiber laesst die CPU warten, sobald in einem Bild ein weiterer Zeichendurchgang folgt (s. PreFrameSource).
const char* GRAB_SHADER = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D u_tex;
uniform vec2 u_texel;
varying vec2 v_uv;
void main()
{
  vec4 sum = texture2D(u_tex, v_uv + vec2(-u_texel.x, -u_texel.y));
  sum += texture2D(u_tex, v_uv + vec2(u_texel.x, -u_texel.y));
  sum += texture2D(u_tex, v_uv + vec2(-u_texel.x, u_texel.y));
  sum += texture2D(u_tex, v_uv + vec2(u_texel.x, u_texel.y));
  gl_FragColor = sum * 0.25;
}
)";

// Dual Kawase, Vergroessern: acht Abtastungen
const char* UP_SHADER = R"(
precision mediump float;
uniform sampler2D u_tex;
uniform vec2 u_off;
varying vec2 v_uv;
void main()
{
  vec4 sum = texture2D(u_tex, v_uv + vec2(-u_off.x * 2.0, 0.0));
  sum += texture2D(u_tex, v_uv + vec2(-u_off.x, u_off.y)) * 2.0;
  sum += texture2D(u_tex, v_uv + vec2(0.0, u_off.y * 2.0));
  sum += texture2D(u_tex, v_uv + vec2(u_off.x, u_off.y)) * 2.0;
  sum += texture2D(u_tex, v_uv + vec2(u_off.x * 2.0, 0.0));
  sum += texture2D(u_tex, v_uv + vec2(u_off.x, -u_off.y)) * 2.0;
  sum += texture2D(u_tex, v_uv + vec2(0.0, -u_off.y * 2.0));
  sum += texture2D(u_tex, v_uv + vec2(-u_off.x, -u_off.y)) * 2.0;
  gl_FragColor = sum / 12.0;
}
)";

// Glas mit eigener Quelle: die ganze Pyramide (zweimal Kawase runter, einmal hoch) in einem Pass, damit das Ergebnis
// schon im Bild nach der Quelle fertig ist (je Bild nur ein Durchgang, s. PreFrameSource). Eingabe ist doppelt so gross
// wie die Ausgabe; ein Gauss aus 9 x 9 bilinearen Abtastungen im Abstand von zwei Eingabepixeln, jede an einer
// Pixelkante (Mittel aus 2 x 2). Die Gewichte geben dieselbe Breite wie die Kawase-Kette (Impulsantwort simuliert:
// Varianz 12,1 Eingabepixel^2 je Achse, Gauss sigma 3,59 auf neun Stuetzstellen, 29.10.2026)
const char* GAUSS_SHADER = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D u_tex;
uniform vec2 u_texel;
varying vec2 v_uv;
void main()
{
  float w[9];
  w[0] = 0.01885; w[1] = 0.05572; w[2] = 0.12085; w[3] = 0.19232; w[4] = 0.22453;
  w[5] = 0.19232; w[6] = 0.12085; w[7] = 0.05572; w[8] = 0.01885;
  vec4 sum = vec4(0.0);
  for (int j = 0; j < 9; j++)
  {
    vec4 row = vec4(0.0);
    for (int i = 0; i < 9; i++)
      row += texture2D(u_tex, v_uv + vec2(float(i - 4), float(j - 4)) * 2.0 * u_texel) * w[i];
    sum += row * w[j];
  }
  gl_FragColor = sum;
}
)";

// Zusammensetzen: Saettigung, Toenung, runde Maske (SDF in Bildschirm-Pixeln), Alpha
const char* COMPOSITE_SHADER = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D u_tex;
uniform vec2 u_size;
uniform float u_radius;
uniform float u_saturation;
uniform vec4 u_tint;
uniform float u_alpha;
varying vec2 v_uv;
varying vec2 v_local;
void main()
{
  vec3 c = texture2D(u_tex, v_uv).rgb;
  float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
  c = mix(vec3(lum), c, u_saturation);
  c = mix(c, u_tint.rgb, u_tint.a);
  vec2 half_size = u_size * 0.5;
  vec2 q = abs(v_local - half_size) - (half_size - vec2(u_radius));
  float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - u_radius;
  float mask = clamp(0.5 - d, 0.0, 1.0);
  gl_FragColor = vec4(c, u_alpha * mask);
}
)";

//! Liquid Glass (style liquid): wie tvOS 26 - am Rand Lichtbrechung (Inhalt von weiter aussen, teils aus dem
//! schaerferen Griff), Material hell wie Apples Glas (9 % Weiss, im Apple-Video gemessen; erst ueber sehr hellem
//! Hintergrund dunkler, damit weisser Text lesbar bleibt). Lichtkante wie im Apple-Handbuch (tvOS 27, Dock und
//! Uhr-Kapsel ueber Schwarz gemessen, 29.10.2026): Licht von oben, oben Weiss ~23 %, Seiten ~4 %, unten ~8 %, kaum
//! Schein nach innen. Koordinaten: Framebuffer-Pixel, y nach oben.
const char* LIQUID_SHADER = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D u_tex;
uniform sampler2D u_sharp;
uniform vec2 u_size;
uniform vec2 u_screen;
uniform float u_radius;
uniform float u_px;
uniform float u_saturation;
uniform vec4 u_tint;
uniform float u_alpha;
uniform float u_lensOld;
varying vec2 v_uv;
varying vec2 v_local;
void main()
{
  vec2 half_size = u_size * 0.5;
  vec2 p = v_local - half_size;
  vec2 q = abs(p) - (half_size - vec2(u_radius));
  float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - u_radius;
  float inside = -d;
  float rimW = max(6.0, min(u_radius * 0.8, 22.0 * u_px));
  const vec3 luma = vec3(0.2126, 0.7152, 0.0722);
  vec2 g = (q.x > 0.0 && q.y > 0.0) ? normalize(q) : (q.x > q.y ? vec2(1.0, 0.0) : vec2(0.0, 1.0));
  vec2 n = g * sign(p);
  float rim = 1.0 - smoothstep(0.0, rimW, inside);
  float rim2 = rim * rim;
  // Linse wie Apples Liquid Glass: gewoelbter Rand ueber lensW (Eckradius, hoechstens 48 px), der den Hintergrund
  // von aussen hereinholt - an der Kante um 0,8 lensW, nach innen quadratisch auslaufend (Inhalt am Rand gestaucht)
  float lensW = max(6.0, min(u_radius, 48.0 * u_px));
  float lens = 1.0 - clamp(inside / lensW, 0.0, 1.0);
  lens *= lens;
  vec2 off = n * lens * (0.8 * lensW) / u_screen;
  if (u_lensOld > 0.5)
  {
    lens = rim2;
    off = n * rim2 * (14.0 * u_px) / u_screen;
  }
  vec3 blurred = texture2D(u_tex, v_uv + off).rgb;
  vec3 sharp = texture2D(u_sharp, v_uv + off * 1.3).rgb;
  vec3 c = mix(blurred, sharp, lens * 0.55);
  c = mix(vec3(dot(c, luma)), c, u_saturation);
  float lum = dot(blurred, luma);
  c = mix(c, vec3(1.0), 0.09 * (1.0 - smoothstep(0.55, 0.85, lum)));
  c = mix(c, vec3(0.118), 0.28 * smoothstep(0.62, 0.92, lum));
  c = mix(c, u_tint.rgb, u_tint.a);
  vec2 L = normalize(vec2(-0.20, 0.98));
  float facing = dot(n, L);
  float line = 1.0 - smoothstep(0.6 * u_px, 2.2 * u_px, inside);
  float spec = 0.03 + 0.21 * pow(max(facing, 0.0), 2.0) + 0.05 * pow(max(-facing, 0.0), 3.0);
  float glow = rim2 * (0.00 + 0.03 * max(facing, 0.0));
  c += vec3(line * spec + glow);
  float mask = clamp(0.5 - d, 0.0, 1.0);
  gl_FragColor = vec4(min(c, vec3(1.0)), u_alpha * mask);
}
)";

//! Liquid Glass innen (jenseits des Rands): dort sind Lichtbrechung, scharfer Griff, Lichtkante und Schein genau null.
//! Composite zeichnet das Innere einer Flaeche mit diesem schlanken Shader (ein Texturzugriff) und nur die Randstreifen
//! mit LIQUID_SHADER - gleiches Ergebnis. Grosse Flaechen (Popover) kosteten in 3D sonst zu viel: zwei Augen, zweiter,
//! versetzter Texturzugriff je Pixel (17.10.2026: Untertitelmenue 6-10 statt 0-2 verspaetete Bilder/s; eine
//! Verzweigung im Shader half nur halb, Mali reserviert Register fuer den langen Weg)
const char* LIQUID_INNER_SHADER = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D u_tex;
uniform float u_saturation;
uniform vec4 u_tint;
uniform float u_alpha;
varying vec2 v_uv;
varying vec2 v_local;
void main()
{
  const vec3 luma = vec3(0.2126, 0.7152, 0.0722);
  vec3 b = texture2D(u_tex, v_uv).rgb;
  float l = dot(b, luma);
  vec3 c = mix(vec3(l), b, u_saturation);
  c = mix(c, vec3(1.0), 0.09 * (1.0 - smoothstep(0.55, 0.85, l)));
  c = mix(c, vec3(0.118), 0.28 * smoothstep(0.62, 0.92, l));
  c = mix(c, u_tint.rgb, u_tint.a);
  gl_FragColor = vec4(min(c, vec3(1.0)), u_alpha);
}
)";

/*!
 \brief Video-Glas (Amlogic, Vero): das Video liegt auf einer eigenen Hardware-Ebene unter der Oberflaeche, das Glas
        sieht es im Framebuffer nicht. Ein eigener Faden holt deshalb das aktuelle Videobild klein ueber
        /dev/amvideocap0 (Hardware-Skalierer, gemessen ~9 ms je Bild, unabhaengig von der Groesse) und meldet neue
        Bilder; der Zeichenfaden laedt sie in eine Textur und mischt sie beim Griff unter die Oberflaeche - so wie sie
        auf dem Bildschirm zusammengesetzt wird (Lage des Videos aus /sys/class/video/axis). Echtzeit: jedes neue
        Videobild (24 bis 60 je Sekunde) zeichnet das Glas neu.
*/
namespace GLAS_VIDEOGLASS
{
constexpr int CAP_W = 480; // Vielfaches von 32 (Vorgabe des Treibers)
constexpr int CAP_H = 288;
constexpr unsigned long CAP_SET_WIDTH = 0x40045602; // _IOW('V', 0x02, int)
constexpr unsigned long CAP_SET_HEIGHT = 0x40045603; // _IOW('V', 0x03, int)

class CCapture
{
public:
  static CCapture& Get()
  {
    static CCapture capture;
    return capture;
  }

  ~CCapture()
  {
    m_stop = true;
    if (m_thread.joinable())
      m_thread.join();
  }

  //! Glas ueber Video sichtbar: Aufnahme (weiter) laufen lassen
  void Want()
  {
    m_lastWant = Now();
    if (!m_started)
    {
      m_started = true;
      m_thread = std::thread(&CCapture::Run, this);
    }
  }

  unsigned int Seq() const { return m_seq; }
  bool Running() const { return m_running; }

  //! neuestes Bild (wenn neuer als seq) nach out kopieren; Lage des Videos normiert (x0, y0, x1, y1, oben links)
  bool Take(unsigned int& seq, std::vector<uint8_t>& out, float axis[4])
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    for (int i = 0; i < 4; i++)
      axis[i] = m_axis[i];
    if (m_seq == seq || m_frame.empty())
      return false;
    out = m_frame;
    seq = m_seq;
    return true;
  }

private:
  static int64_t Now()
  {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  void ReadAxis()
  {
    // Bildschirmmodus z. B. "1080p60hz", "2160p23hz": Hoehe vorn, Breite 16:9
    float dispH = 1080.0f;
    if (FILE* f = fopen("/sys/class/display/mode", "r"))
    {
      int h = 0;
      if (fscanf(f, "%d", &h) == 1 && h > 0)
        dispH = static_cast<float>(h);
      fclose(f);
    }
    const float dispW = dispH * 16.0f / 9.0f;
    int a[4] = {0, 0, 0, 0};
    bool ok = false;
    if (FILE* f = fopen("/sys/class/video/axis", "r"))
    {
      ok = fscanf(f, "%d %d %d %d", &a[0], &a[1], &a[2], &a[3]) == 4 && a[2] > a[0] && a[3] > a[1];
      fclose(f);
    }
    // 3D (Frame Packing "1080fp24hz", Top-Bottom, Side-by-Side): die Lage steht in Koordinaten des doppelt hohen bzw.
    // breiten Bildes (z. B. 0 0 1919 2204), das Videogeraet liefert aber das Bild eines Auges. Mit 1080 geteilt lag
    // das Video doppelt hoch im Glas - oben ein schwarzer Balken aus dem gestreckten Kinobalken (17.10.2026)
    float dispWEye = dispW, dispHEye = dispH;
    if (ok && a[3] + 1 > dispH * 1.5f)
      dispHEye = static_cast<float>(a[3] + 1);
    if (ok && a[2] + 1 > dispW * 1.5f)
      dispWEye = static_cast<float>(a[2] + 1);
    std::unique_lock<std::mutex> lock(m_mutex);
    if (ok)
    {
      m_axis[0] = a[0] / dispWEye;
      m_axis[1] = a[1] / dispHEye;
      m_axis[2] = (a[2] + 1) / dispWEye;
      m_axis[3] = (a[3] + 1) / dispHEye;
    }
    else
    {
      m_axis[0] = m_axis[1] = 0.0f;
      m_axis[2] = m_axis[3] = 1.0f;
    }
  }

  void Run()
  {
    std::vector<uint8_t> buf(CAP_W * CAP_H * 3);
    uint32_t lastSum = 0;
    int64_t lastAxis = 0;
    bool logged = false;
    while (!m_stop)
    {
      const int64_t start = Now();
      if (start - m_lastWant > 1500)
      {
        m_running = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
      ssize_t n;
      const int64_t readStart = Now();
      {
        // Geraet bleibt offen (GLAS_VIDEODEVICE::Fd, Treiber gibt Speicher beim Schliessen nicht frei)
        std::unique_lock<std::mutex> device(::GLAS_VIDEODEVICE::Mutex());
        const int fd = ::GLAS_VIDEODEVICE::Fd();
        if (fd < 0)
        {
          device.unlock();
          if (!logged)
            CLog::Log(LOGWARNING, "GLAS_VIDEOGLASS: /dev/amvideocap0 nicht verfuegbar");
          logged = true;
          std::this_thread::sleep_for(std::chrono::seconds(1));
          continue;
        }
        ioctl(fd, CAP_SET_WIDTH, CAP_W);
        ioctl(fd, CAP_SET_HEIGHT, CAP_H);
        n = pread(fd, buf.data(), buf.size(), 0);
      }
      // Messung (/tmp/glas-timing): Dauer eines Lesens je Sekunde, Mittel und Hoechstwert
      {
        const int64_t readMs = Now() - readStart;
        m_readSum += readMs;
        m_readMax = std::max<int64_t>(m_readMax, readMs);
        m_reads++;
        if (start - m_readReport >= 1000)
        {
          if (access("/tmp/glas-timing", F_OK) == 0 && m_reads)
            CLog::Log(LOGINFO, "GLAS_VIDEOGLASS: {} Lesungen, je avg {:.1f} / max {} ms", m_reads,
                      static_cast<double>(m_readSum) / m_reads, m_readMax);
          m_readReport = start;
          m_reads = 0;
          m_readSum = m_readMax = 0;
        }
      }
      if (n == static_cast<ssize_t>(buf.size()))
      {
        // neues Videobild? (Stichprobe; bei 24p liefern mehrere Aufnahmen dasselbe Bild)
        uint32_t sum = 0;
        for (size_t i = 0; i < buf.size(); i += 61)
          sum = sum * 31 + buf[i];
        if (sum != lastSum)
        {
          std::unique_lock<std::mutex> lock(m_mutex);
          m_frame.swap(buf);
          if (buf.size() != m_frame.size())
            buf.resize(m_frame.size());
          m_seq++;
        }
        lastSum = sum;
        m_running = true;
      }
      else
        m_running = false;
      if (start - lastAxis > 500)
      {
        lastAxis = start;
        ReadAxis();
      }
      // Takt nach der Bildwiederholrate der Anzeige (mehr Videobilder gibt es nicht): bei 60 Hz alle 15 ms, bei 24 Hz
      // alle 37 ms. Vorher immer 15 ms (66 je Sekunde): in 3D kostete jedes Lesen Kernelzeit, der Faden lag bei 32 %
      // und liess das Zeichnen der Oberflaeche auf bis zu 54 ms je Bild springen (17.10.2026)
      int64_t period = 15;
      if (CWinSystemBase* ws = CServiceBroker::GetWinSystem())
      {
        const float fps = ws->GetGfxContext().GetFPS();
        if (fps > 1.0f)
          period = std::max<int64_t>(15, static_cast<int64_t>(1000.0f / fps) - 5);
      }
      // Messschalter: fester Abstand in ms aus /tmp/glas-capture-ms (je Sekunde gelesen)
      if (start - m_overrideCheck >= 1000)
      {
        m_overrideCheck = start;
        m_overrideMs = 0;
        if (FILE* f = fopen("/tmp/glas-capture-ms", "r"))
        {
          int v = 0;
          if (fscanf(f, "%d", &v) == 1 && v > 0 && v <= 1000)
            m_overrideMs = v;
          fclose(f);
        }
      }
      if (m_overrideMs > 0)
        period = m_overrideMs;
      const int64_t spent = Now() - start;
      if (spent < period)
        std::this_thread::sleep_for(std::chrono::milliseconds(period - spent));
    }
  }

  std::thread m_thread;
  int64_t m_readSum = 0, m_readMax = 0, m_readReport = 0, m_overrideCheck = 0;
  int m_overrideMs = 0;
  int m_reads = 0;
  std::atomic<bool> m_stop{false};
  std::atomic<bool> m_started{false};
  std::atomic<bool> m_running{false};
  std::atomic<int64_t> m_lastWant{0};
  std::atomic<unsigned int> m_seq{0};
  std::mutex m_mutex;
  std::vector<uint8_t> m_frame;
  float m_axis[4] = {0.0f, 0.0f, 1.0f, 1.0f};
};

//! Video laeuft (einmal je Bild abgefragt), HDR-Video (PQ/HLG)
struct State
{
  unsigned int frame = 0;
  bool video = false;
  bool hdr = false;
};
inline State g_state;

inline const State& Query(unsigned int frame)
{
  if (frame != g_state.frame)
  {
    g_state.frame = frame;
    const auto& components = CServiceBroker::GetAppComponents();
    const auto appPlayer = components.GetComponent<CApplicationPlayer>();
    g_state.video = appPlayer && appPlayer->IsPlayingVideo();
    // Stereo-3D: Video-Glas nur, wenn nicht per Messschalter /tmp/glas-stereo-video-off abgeschaltet
    if (g_state.video && GLAS_STEREO::Active(CServiceBroker::GetWinSystem()->GetGfxContext()) &&
        !GLAS_STEREO::VideoAllowed())
      g_state.video = false;
    g_state.hdr = false;
    if (g_state.video)
    {
      VideoStreamInfo info;
      appPlayer->GetVideoStreamInfo(CURRENT_STREAM, info);
      g_state.hdr = info.hdrType != StreamHdrType::HDR_TYPE_NONE;
      // Decoder zuerst: amvideocap reserviert beim (einzigen) Oeffnen 32 MB im Videospeicher, ein 3D-Film (MVC) braucht
      // dort viel; erst wenn der Videostrom bekannt ist, also der Decoder seinen Speicher hat (17.10.2026). 3D-Filme
      // in 2D (MONO) bekommen Video-Glas wie andere Filme; echte 3D-Ausgabe schliesst GLAS_STEREO::Active oben aus.
      const bool known = info.valid && info.width > 0;
      if (!known)
        g_state.video = false;
    }
  }
  return g_state;
}
} // namespace GLAS_VIDEOGLASS

//! Mischen beim Griff: Oberflaeche (vormultipliziert, Alpha = Deckung) ueber dem Videobild an seiner Stelle,
//! schwarz ausserhalb; HDR-Bilder (PQ) grob nach SDR (Reinhard, Weiss 203 nits)
const char* VIDEOMIX_SHADER = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D u_tex;
uniform sampler2D u_vid;
uniform vec4 u_axis;
uniform float u_hdr;
varying vec2 v_uv;
vec3 pq2sdr(vec3 c)
{
  vec3 p = pow(max(c, vec3(0.0)), vec3(1.0 / 78.84375));
  vec3 l = pow(max(p - 0.8359375, vec3(0.0)) / (18.8515625 - 18.6875 * p), vec3(1.0 / 0.1593017578125));
  vec3 x = l * (10000.0 / 203.0);
  x = x / (1.0 + x);
  return pow(x, vec3(1.0 / 2.2));
}
void main()
{
  vec4 g = texture2D(u_tex, v_uv);
  vec2 s = vec2(v_uv.x, 1.0 - v_uv.y);
  vec2 t = (s - u_axis.xy) / (u_axis.zw - u_axis.xy);
  vec3 v = vec3(0.0);
  if (t.x >= 0.0 && t.y >= 0.0 && t.x <= 1.0 && t.y <= 1.0)
  {
    v = texture2D(u_vid, t).bgr;
    if (u_hdr > 0.5)
      v = pq2sdr(v);
  }
  gl_FragColor = vec4(g.rgb + v * (1.0 - g.a), 1.0);
}
)";

GLuint CompileShader(GLenum type, const char* source)
{
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok)
  {
    char log[1024] = {};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    CLog::Log(LOGERROR, "CGUIBackdropBlurControl: shader compile failed: {}", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

GLuint LinkProgram(const char* fragment)
{
  GLuint vs = CompileShader(GL_VERTEX_SHADER, VERTEX_SHADER);
  GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragment);
  if (!vs || !fs)
    return 0;
  GLuint program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glBindAttribLocation(program, 0, "a_pos");
  glBindAttribLocation(program, 1, "a_uv");
  glBindAttribLocation(program, 2, "a_local");
  glLinkProgram(program);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok)
  {
    char log[1024] = {};
    glGetProgramInfoLog(program, sizeof(log), nullptr, log);
    CLog::Log(LOGERROR, "CGUIBackdropBlurControl: program link failed: {}", log);
    glDeleteProgram(program);
    return 0;
  }
  return program;
}

GLuint CreateTexture(int w, int h)
{
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  return tex;
}

// GPU-Zeitstempel nur mit GLES 3
#if HAS_GLES >= 3
void BeginTimeQuery(GLuint query) { glBeginQuery(GL_TIME_ELAPSED_EXT, query); }
void EndTimeQuery() { glEndQuery(GL_TIME_ELAPSED_EXT); }
#else
void BeginTimeQuery(GLuint) {}
void EndTimeQuery() {}
#endif

void DrawQuad(const float* pos, const float* uv, const float* local)
{
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  if (local)
  {
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, local);
    glEnableVertexAttribArray(2);
  }
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray(0);
  glDisableVertexAttribArray(1);
  if (local)
    glDisableVertexAttribArray(2);
}

bool Overlaps(const float a[4], const float b[4])
{
  return a[0] < b[2] && b[0] < a[2] && a[1] < b[3] && b[1] < a[3];
}

//! Liegt b wirklich hinter a (mindestens 25 % der kleineren Flaeche)? Nachbarn, die sich beim Fokus-Zoom um ein paar
//! Pixel beruehren, zaehlen nicht: sonst griff das Glas mitten im Dialog neu, mit dessen schon gezeichneten Teilen
//! (Kontrollzentrum: helle Flecken der weissen Fokus-Kapsel in den Kreisen, 29.10.2026)
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
 \brief Gemeinsamer Hintergrund aller Glasflaechen: hoechstens ein Griff je Bild ueber den ganzen Viewport,
        stark verkleinert und weichgezeichnet (Apple-Material ist so unscharf, dass 1/16 als tiefste Stufe reicht).
        Jede Flaeche schneidet sich beim Zusammensetzen ihren Teil heraus.

 Griff mitten im Bild beendet auf Mali den laufenden Render-Pass; jeder weitere Durchgang im selben Bild liess den
 Treiber die CPU 5-7 ms warten (29.10.2026), unabhaengig von der Groesse. Deshalb einmal je Bild statt einmal je
 Flaeche, und wo moeglich gar nicht: Glas mit eigener Quelle (PreFrameSource) rechnet je Bild nur einen Durchgang.
 Flaechen, die vor dem Griff schon zusammengesetzt waren, stecken im Ergebnis ("verschmutzt"): sie greifen beim
 naechsten Bild selbst neu, als erste in der Zeichenreihenfolge, dann ist das Ergebnis wieder sauber.
*/
class CBackdropBlurShared
{
public:
  //! Eigene Pyramide fuer Glas mit eigener Quelle (s. PreFrameSource), unabhaengig von den Griffen
  static constexpr int SOURCE = 2;

  //! eye: 0 = 2D/MONO/linkes Auge, 1 = rechtes Auge (Stereo-3D: eigener Griff, eigene Pyramide, eigenes Ergebnis;
  //! wird erst im Stereo-Betrieb angelegt), SOURCE = Glas mit eigener Quelle.
  //! window: je Fenster ein eigener Griff (0 = gemeinsam, ueber Video). Ein gemeinsames Ergebnis fuer alle Fenster liess
  //! Dialoge das Ergebnis eines Fensters darunter nehmen: das Dock-Glas griff den Homescreen ohne die Abdunklung des
  //! Kontrollzentrums, dessen Kreise zeigten dann helle, unabgedunkelte Kacheln (29.10.2026)
  static CBackdropBlurShared& Get(int eye = 0, int window = 0)
  {
    // bewusst nie freigegeben: beim Beenden von Kodi gibt es keinen GL-Kontext mehr fuer glDelete*
    static std::map<std::pair<int, int>, CBackdropBlurShared*> shared;
    CBackdropBlurShared*& instance = shared[{std::clamp(eye, 0, SOURCE), window}];
    if (!instance)
      instance = new CBackdropBlurShared;
    return *instance;
  }

  bool Prepare()
  {
    if (m_ready)
      return true;
    if (m_failed)
      return false;
    // Programme fuer alle Instanzen (je Auge und Fenster) nur einmal uebersetzen
    static GLuint programs[8] = {};
    if (!programs[0])
    {
      const char* sources[8] = {DOWN_SHADER,     UP_SHADER,           COMPOSITE_SHADER, LIQUID_SHADER,
                                LIQUID_INNER_SHADER, VIDEOMIX_SHADER, GRAB_SHADER,      GAUSS_SHADER};
      for (int i = 0; i < 8; i++)
        programs[i] = LinkProgram(sources[i]);
    }
    m_down = programs[0];
    m_up = programs[1];
    m_composite = programs[2];
    m_liquid = programs[3];
    m_liquidInner = programs[4];
    m_videoMix = programs[5];
    m_grab = programs[6];
    m_gauss = programs[7];
    if (!m_down || !m_up || !m_composite || !m_liquid || !m_liquidInner || !m_videoMix || !m_grab || !m_gauss)
    {
      m_failed = true;
      return false;
    }
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    m_gles3 = version && std::string(version).find("OpenGL ES 3") != std::string::npos;
#if HAS_GLES >= 3
    const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    m_timerQuery = m_gles3 && extensions &&
                   std::string(extensions).find("GL_EXT_disjoint_timer_query") != std::string::npos;
    if (m_timerQuery)
      glGenQueries(QUERY_SLOTS, m_queries);
#endif
    CLog::Log(LOGINFO, "CGUIBackdropBlurControl: bereit ({}, GPU-Zeitstempel {})", version ? version : "?",
              m_timerQuery ? "ja" : "nein");
    m_ready = true;
    return true;
  }

  enum class Need
  {
    NONE, //!< vorhandenes Ergebnis taugt
    BACKGROUND, //!< Hintergrund hat sich geaendert (darf gedrosselt werden)
    FORCED //!< kein brauchbares Ergebnis (neu, andere Einstellung, Flaeche steckt selbst im Ergebnis)
  };

  //! Ist fuer diese Flaeche (Framebuffer-Pixel) ein neuer Griff noetig, oder taugt der vorhandene?
  Need NeedsGrab(const float rect[4], unsigned int frame, bool backgroundDirty, int grabScale, int deepest)
  {
    m_lastReason = 0;
    if (!m_result || grabScale != m_grabScale || deepest != m_deepest)
      return Need::FORCED;
    // Verschmutzt: diese Flaeche steckt selbst im Ergebnis (war vor dem Griff schon gezeichnet)
    m_lastReason = 1;
    for (const auto& r : m_polluted)
      if (OverlapsMuch(rect, r.data()))
        return Need::FORCED;
    // Hinter einem spaeteren Glas aendert sich etwas (Process, WantGrab): schon das erste Glas des Bildes greift,
    // bevor der Dialog gezeichnet ist - sonst griffe erst das spaetere, mit allem, was davor schon im Dialog liegt
    if (!backgroundDirty && !(frame == m_wantFrame && frame != m_grabFrame))
      return Need::NONE;
    m_lastReason = 3;
    if (frame != m_grabFrame)
      return Need::BACKGROUND;
    // in diesem Bild schon gegriffen: wiederverwenden, ausser seither gezeichnetes Glas liegt dahinter
    m_lastReason = 2;
    for (const auto& r : m_composited)
      if (OverlapsMuch(rect, r.data()))
        return Need::FORCED;
    return Need::NONE;
  }

  //! Process: hinter dieser Glasflaeche hat sich in diesem Bild etwas geaendert
  void WantGrab(unsigned int frame) { m_wantFrame = frame; }
  int LastReason() const { return m_lastReason; }
  unsigned int m_wantFrame = 0;

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

  unsigned int LastGrabFrame() const { return m_grabFrame; }

  //! Video-Glas je Bild einmal: neues Videobild in die Textur laden. true = Glas ueber laufendem Video
  bool PrepareVideo(unsigned int frame)
  {
    const auto& state = GLAS_VIDEOGLASS::Query(frame);
    m_videoOn = state.video;
    m_videoHdr = state.hdr;
    if (!m_videoOn)
      return false;
    auto& capture = GLAS_VIDEOGLASS::CCapture::Get();
    capture.Want();
    if (frame == m_videoFrame)
      return m_videoTex != 0;
    m_videoFrame = frame;
    if (capture.Take(m_videoSeq, m_videoBuf, m_axis))
    {
      GLuint& tex = m_videoTexs[m_videoNext];
      m_videoNext ^= 1;
      if (!tex)
      {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, GLAS_VIDEOGLASS::CAP_W, GLAS_VIDEOGLASS::CAP_H, 0, GL_RGB,
                     GL_UNSIGNED_BYTE, m_videoBuf.data());
        CLog::Log(LOGINFO, "GLAS_VIDEOGLASS: Videobilder {}x{} im Glas", GLAS_VIDEOGLASS::CAP_W,
                  GLAS_VIDEOGLASS::CAP_H);
      }
      else
      {
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GLAS_VIDEOGLASS::CAP_W, GLAS_VIDEOGLASS::CAP_H, GL_RGB,
                        GL_UNSIGNED_BYTE, m_videoBuf.data());
      }
      m_videoTex = tex;
    }
    return m_videoTex != 0;
  }

  bool VideoOn() const { return m_videoOn && m_videoTex != 0; }

  //! Stereo-3D mit Video-Glas: Ergebnis nur aus dem Videobild, einmal am Anfang des Bildes (vor der Oberflaeche).
  //! Ein Griff mitten im Bild teilt bei Mali den Zeichendurchgang (halbfertiges Bild zurueckschreiben und neu laden):
  //! in 3D (1920x2205) ~9 ms je Bild mit Spitzen bis 50 ms, Menues ueber dem Film stockten (17.10.2026). Hinter dem
  //! Glas liegt ueber dem Film ohnehin fast nur das Video. Die Glasflaechen nehmen dann dieses Ergebnis (kein Griff).
  //! m_videoOnlyActive setzt Render im vorigen Bild (Stereo + Video sichtbar), Groessen von dort.
  //! true: dieses Bild braucht keinen Griff aus der Oberflaeche (alle Glasflaechen nehmen das Videobild-Ergebnis) -
  //! dann auch kein Zwischenbild (Kopie 1920x2205 kostete in 3D ~12 ms GPU je Bild)
  bool PreFrameVideo()
  {
    if (!m_videoOnlyActive || !m_ready || !m_gles3 || m_vpW <= 0 || m_grabScale <= 0)
      return false;
    m_videoOnlyActive = false; // Render bestaetigt es in diesem Bild wieder
    const unsigned int frame = CTimeUtils::GetFrameTime();
    if (!PrepareVideo(frame) || !VideoOn())
      return false;
    if (m_videoOnlySeq == m_videoSeq && m_result)
      return true; // kein neues Videobild
    GLint prevFbo = 0, prevProgram = 0, prevTex = 0, prevActive = 0, prevArray = 0, prevVp[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArray);
    glGetIntegerv(GL_VIEWPORT, prevVp);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean blend = glIsEnabled(GL_BLEND);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    const GLint vp[4] = {0, 0, m_vpW, m_vpH};
    Grab(vp, prevFbo, frame, m_grabScale, m_deepest, false, false, true);
    m_videoOnlySeq = m_videoSeq;
    m_videoOnlyFrame = frame;
    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
    if (scissor)
      glEnable(GL_SCISSOR_TEST);
    if (blend)
      glEnable(GL_BLEND);
    glBindBuffer(GL_ARRAY_BUFFER, prevArray);
    glBindTexture(GL_TEXTURE_2D, prevTex);
    glActiveTexture(prevActive);
    glUseProgram(prevProgram);
    return true;
  }

  //! Render: in diesem Bild Stereo-3D mit Video-Glas (Groesse eines Auges merken)
  void NoteVideoOnly(int vpW, int vpH)
  {
    m_videoOnlyActive = true;
    m_vpW = vpW;
    m_vpH = vpH;
  }
  bool VideoOnlyReady(unsigned int frame) const { return m_result && m_videoOnlyFrame == frame; }
  bool HasResult() const { return m_result != 0; }

  bool m_videoOnlyActive = false;
  unsigned int m_videoOnlySeq = ~0u;
  unsigned int m_videoOnlyFrame = 0;
  int m_vpW = 0;
  int m_vpH = 0;

  //! Glas mit eigener Quelle (<source>: Control, dessen Bild hinter dem Glas liegt, z. B. das Top-Shelf-Bild hinter
  //! dem Dock): Render meldet die Quelle je Bild an, Process eine Aenderung dahinter; am Anfang des naechsten Bildes
  //! zeichnet PreFrameSource das Control klein in die erste Stufe und zeichnet weich. Kein Griff mitten im Bild.
  void NoteSource(int window, int control, const GLint vp[4], int grabScale, int deepest, float updateRate,
                  unsigned int frame)
  {
    if (window != m_srcWindow || control != m_srcControl || vp[2] != m_vpW || vp[3] != m_vpH ||
        grabScale != m_srcScale || deepest != m_srcDeepest)
      m_srcDone = false;
    m_srcNoteFrame = frame;
    m_srcFresh = false;
    m_srcWindow = window;
    m_srcControl = control;
    m_vpW = vp[2];
    m_vpH = vp[3];
    m_srcScale = grabScale;
    m_srcDeepest = deepest;
    m_srcRate = updateRate;
  }
  void MarkSourceDirty() { m_srcDirty = true; }
  //! gedrosselt, noch nicht nachgeholt: Process haelt das Glas zum Zeichnen angemeldet
  bool SourcePending() const { return m_srcDirty || m_srcStage != 0 || m_srcFresh; }
  bool SourceReady(int window, int control) const
  {
    return m_srcDone && m_result && window == m_srcWindow && control == m_srcControl;
  }

  //! Am Anfang des Bildes (GLAS_GUITARGET::Begin, nach Process, vor der Oberflaeche): Quelle neu zeichnen, wenn sie
  //! sich geaendert hat, wie der Griff hoechstens updaterate Mal je Sekunde. Begin laeuft auch in Durchlaeufen, in
  //! denen Kodi nichts zeichnet; erst nach 250 ms ohne gezeichnetes Glas gilt das Ergebnis als veraltet.
  //! Je Bild hoechstens ein Zeichendurchgang: erst die Quelle, im naechsten Bild der Gauss-Pass (GAUSS_SHADER); das
  //! Glas zeigt bis dahin das vorige Ergebnis. Ein zweiter Durchgang im selben Bild liess den Mali-Treiber beim
  //! Umschalten des Zielbilds 5-7 ms warten (glBindFramebuffer, 29.10.2026), ein einzelner kostete nichts.
  //! Ohne Drosselung (updaterate): mit Kawase-Kette ueber vier Bilder und 15 Hz lief das Glas sichtbar hinterher.
  //! Messschalter /tmp/glas-source-steps-off: alle Paesse sofort (wie zuerst)
  void PreFrameSource()
  {
    const unsigned int frame = CTimeUtils::GetFrameTime();
    if (frame - m_srcNoteFrame > 250)
    {
      m_srcDone = false;
      m_srcDirty = false;
      m_srcStage = 0;
      return;
    }
    // Quelle nur, solange sie ganz sichtbar ist: unsichtbar (z. B. im Raster) oder beim Ein- und Ausblenden liegt
    // anderes dahinter, dann greift das Glas wie sonst. Ebenso in Fenstern ohne dieses Control (Uhr in allen Fenstern).
    // In jedem Bild pruefen, auch ohne Aenderung: sonst galt eine eben unsichtbar gewordene Quelle weiter
    CGUIWindow* window = CServiceBroker::GetGUI()->GetWindowManager().GetWindow(m_srcWindow);
    CGUIControl* control = window ? window->GetControl(m_srcControl) : nullptr;
    if (!m_ready || !m_gles3 || !control || !control->IsVisible() ||
        control->IsAnimating(ANIM_TYPE_VISIBLE) || control->IsAnimating(ANIM_TYPE_HIDDEN) || m_vpW <= 0 || m_vpH <= 0)
    {
      m_srcDone = false;
      m_srcDirty = false;
      m_srcStage = 0;
      return;
    }
    // ohne Drosselung (updaterate): die Pyramide ueber mehrere Bilder begrenzt die Rate schon, ein Pass je Bild kostet
    // nichts, und jede Wartezeit liess das Glas sichtbar hinterherlaufen. Messschalter /tmp/glas-source-throttle
    if (m_srcStage == 0)
    {
      if (m_srcDone && !m_srcDirty && !GLAS_STEREO::SourceAlways())
        return;
      if (m_srcDone && m_srcRate > 0 && frame - m_srcFrame < static_cast<unsigned int>(1000.0f / m_srcRate) &&
          access("/tmp/glas-source-throttle", F_OK) == 0)
        return;
    }
    GLint prevFbo = 0, prevProgram = 0, prevTex = 0, prevActive = 0, prevArray = 0, prevVp[4];
    GLfloat clear[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArray);
    glGetIntegerv(GL_VIEWPORT, prevVp);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean blend = glIsEnabled(GL_BLEND);
    const auto t0 = std::chrono::steady_clock::now();

    if (m_srcStage == 0)
    {
      Ensure(m_vpW, m_vpH, m_srcScale, m_srcDeepest);
      SwapTop();
      const Level& top = m_levels[0];
      glBindFramebuffer(GL_FRAMEBUFFER, top.fbo);
      glDisable(GL_SCISSOR_TEST);
      // Fensterfarbe hinter der Quelle: schwarz (Glas-Skin, backgroundcolor)
      glClearColor(0, 0, 0, 1);
      glClear(GL_COLOR_BUFFER_BIT);
      // wie CGUIWindow::DoRender: Aufloesung des Fensters (Kamera, Projektion auf den ganzen Bildschirm), dann das
      // Control mit seiner in Process berechneten Transformation (Lage, Animationen, Deckkraft); die Projektion landet
      // ueber den kleinen Viewport verkleinert in der ersten Stufe
      CServiceBroker::GetWinSystem()->GetGfxContext().SetRenderingResolution(window->GetCoordsRes(), true);
      glViewport(0, 0, top.w, top.h);
      control->DoRender();
      glDisable(GL_SCISSOR_TEST);
      glDisable(GL_BLEND);
      glBindBuffer(GL_ARRAY_BUFFER, 0);
      m_srcDirty = false;
      m_srcFrame = frame;
      m_grabs++;
      m_stepInput = top.tex;
      m_stepW = top.w;
      m_stepH = top.h;
      m_stepLevel = 1;
      m_srcStage = 1;
      m_srcKawase = access("/tmp/glas-source-kawase", F_OK) == 0;
      if (access("/tmp/glas-source-steps-off", F_OK) == 0)
        while (m_srcStage)
          SourceStep();
      else
        for (int i = 1; i < StepsPerFrame() && m_srcStage; i++)
          SourceStep();
    }
    else
      for (int i = 0; i < StepsPerFrame() && m_srcStage; i++)
        SourceStep();

    if (GLAS_STEREO::SourceTiming())
    {
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      m_srcStepMs += ms;
      m_srcMaxMs = std::max(m_srcMaxMs, ms);
      if (++m_srcTimed >= 60)
      {
        CLog::Log(LOGINFO, "GLAS_SOURCE: {} Schritte, CPU avg {:.2f} ms, max {:.2f} ms", m_srcTimed,
                  m_srcStepMs / m_srcTimed, m_srcMaxMs);
        m_srcTimed = 0;
        m_srcStepMs = m_srcMaxMs = 0;
      }
    }

    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
    if (scissor)
      glEnable(GL_SCISSOR_TEST);
    if (blend)
      glEnable(GL_BLEND);
    glBindBuffer(GL_ARRAY_BUFFER, prevArray);
    glBindTexture(GL_TEXTURE_2D, prevTex);
    glActiveTexture(prevActive);
    glUseProgram(prevProgram);
  }

  //! Durchgaenge je Bild (Quelle mitgezaehlt), Messschalter /tmp/glas-source-steps (Zahl), Vorgabe 1
  static int StepsPerFrame()
  {
    static auto lastCheck = std::chrono::steady_clock::time_point();
    static int steps = 1;
    if (std::chrono::steady_clock::now() - lastCheck > std::chrono::seconds(1))
    {
      lastCheck = std::chrono::steady_clock::now();
      steps = 1;
      if (FILE* f = fopen("/tmp/glas-source-steps", "r"))
      {
        if (fscanf(f, "%d", &steps) != 1)
          steps = 1;
        fclose(f);
      }
      steps = std::clamp(steps, 1, 8);
    }
    return steps;
  }

  //! Ein Pass der Pyramide (wie Pyramid); nach dem letzten wird das neue Ergebnis gezeigt
  void SourceStep()
  {
    static const float fullPos[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    static const float fullUv[8] = {0, 0, 1, 0, 0, 1, 1, 1};
    // Vorgabe: ein Gauss-Pass von der ersten Stufe direkt ins Ergebnis (Ergebnis ein Bild nach der Quelle);
    // Messschalter /tmp/glas-source-kawase: Kawase-Kette wie beim Griff (je Pass ein Bild)
    if (m_stepLevel == 1 && m_levels.size() >= 2 && !m_srcKawase)
    {
      m_outIndex = (m_outIndex + 1) % std::max(1, std::min(m_ring, RING));
      const Level& out = m_out[m_outIndex];
      glUseProgram(m_gauss);
      glUniform1i(glGetUniformLocation(m_gauss, "u_tex"), 0);
      glUniform2f(glGetUniformLocation(m_gauss, "u_texel"), 1.0f / m_stepW, 1.0f / m_stepH);
      glBindFramebuffer(GL_FRAMEBUFFER, out.fbo);
      glViewport(0, 0, out.w, out.h);
      glBindTexture(GL_TEXTURE_2D, m_stepInput);
      DrawQuad(fullPos, fullUv, nullptr);
      m_stepInput = out.tex;
    }
    else if (m_stepLevel < m_levels.size())
    {
      const Level& out = m_levels[m_stepLevel];
      glUseProgram(m_down);
      glUniform1i(glGetUniformLocation(m_down, "u_tex"), 0);
      glUniform2f(glGetUniformLocation(m_down, "u_off"), 1.0f / m_stepW, 1.0f / m_stepH);
      glBindFramebuffer(GL_FRAMEBUFFER, out.fbo);
      glViewport(0, 0, out.w, out.h);
      glBindTexture(GL_TEXTURE_2D, m_stepInput);
      DrawQuad(fullPos, fullUv, nullptr);
      m_stepInput = out.tex;
      m_stepW = out.w;
      m_stepH = out.h;
      m_stepLevel++;
      if (m_stepLevel < m_levels.size() || m_levels.size() >= 2)
        return;
    }
    else
    {
      m_outIndex = (m_outIndex + 1) % std::max(1, std::min(m_ring, RING));
      const Level& out = m_out[m_outIndex];
      glUseProgram(m_up);
      glUniform1i(glGetUniformLocation(m_up, "u_tex"), 0);
      glUniform2f(glGetUniformLocation(m_up, "u_off"), 0.5f / m_stepW, 0.5f / m_stepH);
      glBindFramebuffer(GL_FRAMEBUFFER, out.fbo);
      glViewport(0, 0, out.w, out.h);
      glBindTexture(GL_TEXTURE_2D, m_stepInput);
      DrawQuad(fullPos, fullUv, nullptr);
      m_stepInput = out.tex;
    }
    // fertig: neues Ergebnis und dazugehoerige erste Stufe zeigen
    m_result = m_stepInput;
    m_sharpTex = m_levels[0].tex;
    m_srcDone = true;
    m_srcStage = 0;
    m_srcFresh = true;
  }

  int m_srcStage = 0; //!< 0 = bereit, sonst laeuft die Pyramide (je Bild ein Pass)
  bool m_srcKawase = false;
  size_t m_stepLevel = 0;
  GLuint m_stepInput = 0;
  int m_stepW = 0;
  int m_stepH = 0;
  int m_srcTimed = 0;
  double m_srcStepMs = 0, m_srcMaxMs = 0;
  unsigned int m_srcNoteFrame = 0;
  unsigned int m_srcFrame = 0;
  float m_srcRate = 0;
  bool m_srcDone = false;
  bool m_srcDirty = true;
  bool m_srcFresh = false; //!< neues Ergebnis, noch nicht gezeichnet
  int m_srcWindow = 0;
  int m_srcControl = 0;
  int m_srcScale = 0;
  int m_srcDeepest = 0;

  void Grab(const GLint vp[4], GLint prevFbo, unsigned int frame, int grabScale, int deepest, bool probe,
            bool timing, bool videoOnly = false)
  {
    const auto cpuStart = std::chrono::steady_clock::now();
    Ensure(vp[2], vp[3], grabScale, deepest);
    SwapTop();
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    static const float fullPos[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    static const float fullUv[8] = {0, 0, 1, 0, 0, 1, 1, 1};

    GLuint input;
    int inW, inH;
    size_t first;
#if HAS_GLES >= 3
    if (m_gles3)
    {
      // Griff und Verkleinerung in einem Schritt (GPU, linear)
      const Level& top = m_levels[0];
      if (videoOnly)
      {
        // nur das Videobild (am Anfang des Bildes, s. PreFrameVideo): Oberflaeche durchsichtig statt gegriffen
        glBindFramebuffer(GL_FRAMEBUFFER, top.fbo);
        glViewport(0, 0, top.w, top.h);
        GLfloat clear[4];
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glClearColor(clear[0], clear[1], clear[2], clear[3]);
      }
      else
      {
        GLuint srcTex = 0;
        int texW = 0, texH = 0;
        if (!probe && GLAS_GUITARGET::SourceTexture(prevFbo, srcTex, texW, texH))
        {
          // Zwischenbild als Textur lesen und per Shader verkleinern (Messschalter /tmp/glas-grab-blit: wie frueher)
          glBindFramebuffer(GL_FRAMEBUFFER, top.fbo);
          glViewport(0, 0, top.w, top.h);
          glUseProgram(m_grab);
          glUniform1i(glGetUniformLocation(m_grab, "u_tex"), 0);
          glUniform2f(glGetUniformLocation(m_grab, "u_texel"), 1.0f / texW, 1.0f / texH);
          glBindTexture(GL_TEXTURE_2D, srcTex);
          const float u0 = static_cast<float>(vp[0]) / texW, v0 = static_cast<float>(vp[1]) / texH;
          const float u1 = static_cast<float>(vp[0] + vp[2]) / texW, v1 = static_cast<float>(vp[1] + vp[3]) / texH;
          const float grabUv[8] = {u0, v0, u1, v0, u0, v1, u1, v1};
          DrawQuad(fullPos, grabUv, nullptr);
        }
        else
        {
          glBindFramebuffer(GL_READ_FRAMEBUFFER, prevFbo);
          glBindFramebuffer(GL_DRAW_FRAMEBUFFER, top.fbo);
          // Probe: nur eine 64x64-Ecke lesen (fester Aufwand eines Lesens mitten im Bild)
          const int srcW = probe ? 64 : vp[2];
          const int srcH = probe ? 64 : vp[3];
          glBlitFramebuffer(vp[0], vp[1], vp[0] + srcW, vp[1] + srcH, 0, 0, top.w, top.h, GL_COLOR_BUFFER_BIT,
                            GL_LINEAR);
          glBindFramebuffer(GL_FRAMEBUFFER, top.fbo);
        }
      }
      input = top.tex;
      inW = top.w;
      inH = top.h;
      first = 1;
      m_sharpTex = top.tex;
      // Video-Glas: Videobild an seiner Stelle unter die Oberflaeche mischen (wie die Anzeige es zusammensetzt)
      if (VideoOn())
      {
        if (!m_mix.tex || m_mix.w != top.w || m_mix.h != top.h)
        {
          for (Level* mix : {&m_mix, &m_mixAlt})
          {
            if (mix->fbo)
              glDeleteFramebuffers(1, &mix->fbo);
            if (mix->tex)
              glDeleteTextures(1, &mix->tex);
            *mix = CreateLevel(top.w, top.h);
          }
        }
        // Mischstufe ebenfalls im Wechsel (sie ist ueber Video die schaerfere Quelle, s. SwapTop)
        if (!m_spare.empty())
          std::swap(m_mix, m_mixAlt);
        glBindFramebuffer(GL_FRAMEBUFFER, m_mix.fbo);
        glViewport(0, 0, m_mix.w, m_mix.h);
        glUseProgram(m_videoMix);
        glUniform1i(glGetUniformLocation(m_videoMix, "u_tex"), 0);
        glUniform1i(glGetUniformLocation(m_videoMix, "u_vid"), 1);
        glUniform4f(glGetUniformLocation(m_videoMix, "u_axis"), m_axis[0], m_axis[1], m_axis[2], m_axis[3]);
        glUniform1f(glGetUniformLocation(m_videoMix, "u_hdr"), m_videoHdr ? 1.0f : 0.0f);
        glActiveTexture(GL_TEXTURE1);
        GLint prevTex1 = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex1);
        glBindTexture(GL_TEXTURE_2D, m_videoTex);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, top.tex);
        DrawQuad(fullPos, fullUv, nullptr);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, prevTex1);
        glActiveTexture(GL_TEXTURE0);
        input = m_mix.tex;
        m_sharpTex = m_mix.tex;
      }
    }
    else
#endif
    {
      glBindTexture(GL_TEXTURE_2D, m_source);
      glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, vp[0], vp[1], vp[2], vp[3]);
      input = m_source;
      inW = vp[2];
      inH = vp[3];
      first = 0;
    }

    m_result = Pyramid(input, inW, inH, first);

    // Flaechen, die in diesem Bild schon gezeichnet waren, stecken jetzt im Ergebnis
    m_polluted = (frame == m_grabFrame || frame == m_compositeFrame) ? m_composited
                                                                      : std::vector<std::array<float, 4>>{};
    m_composited.clear();
    m_grabFrame = frame;
    m_grabs++;
    if (timing)
      m_cpuGrabSum +=
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpuStart).count();
  }

  void Composite(const float rect[4], const GLint vp[4], float cornerPx, float saturation,
                 UTILS::COLOR::Color tint, float alpha, unsigned int frame, bool timing, bool liquid, float px)
  {
    int slot = -1;
    if (timing && m_timerQuery)
    {
      Collect();
      if (!m_pending[m_qIndex])
        slot = m_qIndex;
      m_qIndex = (m_qIndex + 1) % QUERY_SLOTS;
      if (slot >= 0)
        BeginTimeQuery(m_queries[slot]);
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    const GLuint program = liquid ? m_liquid : m_composite;
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "u_tex"), 0);
    const float rw = rect[2] - rect[0];
    const float rh = rect[3] - rect[1];
    glUniform2f(glGetUniformLocation(program, "u_size"), rw, rh);
    glUniform1f(glGetUniformLocation(program, "u_radius"), std::min(cornerPx, std::min(rw, rh) * 0.5f));
    glUniform1f(glGetUniformLocation(program, "u_saturation"), saturation);
    glUniform4f(glGetUniformLocation(program, "u_tint"), ((tint >> 16) & 0xFF) / 255.0f,
                ((tint >> 8) & 0xFF) / 255.0f, (tint & 0xFF) / 255.0f, ((tint >> 24) & 0xFF) / 255.0f);
    glUniform1f(glGetUniformLocation(program, "u_alpha"), alpha);
    // Liquid: schaerferer Griff (erste Stufe) fuer die Lichtbrechung am Rand auf Einheit 1
    GLint prevTex1 = 0;
    if (liquid)
    {
      glUniform1i(glGetUniformLocation(program, "u_sharp"), 1);
      glUniform2f(glGetUniformLocation(program, "u_screen"), static_cast<float>(vp[2]), static_cast<float>(vp[3]));
      glUniform1f(glGetUniformLocation(program, "u_px"), px);
      // Messschalter /tmp/glas-lens-old: Lichtbrechung wie vor dem 29.10.2026 (schmaler Streifen, 14 px)
      static auto lensCheck = std::chrono::steady_clock::time_point();
      static bool lensOld = false;
      if (std::chrono::steady_clock::now() - lensCheck > std::chrono::seconds(1))
      {
        lensCheck = std::chrono::steady_clock::now();
        lensOld = access("/tmp/glas-lens-old", F_OK) == 0;
      }
      glUniform1f(glGetUniformLocation(program, "u_lensOld"), lensOld ? 1.0f : 0.0f);
      glActiveTexture(GL_TEXTURE1);
      glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex1);
      glBindTexture(GL_TEXTURE_2D, m_sharpTex ? m_sharpTex : (m_gles3 && !m_levels.empty() ? m_levels[0].tex : m_source));
      glActiveTexture(GL_TEXTURE0);
    }
    // Teilrechteck der Flaeche zeichnen (local bleibt relativ zur ganzen Flaeche: Maske/Rand im Shader stimmen)
    auto drawPart = [&](float x0, float y0, float x1, float y1) {
      float pos[8], uv[8], local[8];
      const float xs[2] = {x0, x1};
      const float ys[2] = {y0, y1};
      for (int i = 0; i < 4; i++)
      {
        const float px = xs[i & 1];
        const float py = ys[i >> 1];
        pos[i * 2] = (px - vp[0]) / vp[2] * 2.0f - 1.0f;
        pos[i * 2 + 1] = (py - vp[1]) / vp[3] * 2.0f - 1.0f;
        uv[i * 2] = (px - vp[0]) / vp[2];
        uv[i * 2 + 1] = (py - vp[1]) / vp[3];
        local[i * 2] = px - rect[0];
        local[i * 2 + 1] = py - rect[1];
      }
      DrawQuad(pos, uv, local);
    };
    glBindTexture(GL_TEXTURE_2D, m_result);
    // Liquid: Rand (Breite wie im Shader: rimW, mindestens der Eckradius) mit LIQUID_SHADER, das Innere schlank
    const float radiusPx = std::min(cornerPx, std::min(rw, rh) * 0.5f);
    const float rimW = std::max(6.0f, std::min(radiusPx * 0.8f, 22.0f * px));
    const float e = std::ceil(std::max(rimW, radiusPx) + 1.0f);
    if (liquid && !m_innerOff && rw > 2.0f * e + 8.0f && rh > 2.0f * e + 8.0f)
    {
      const float x0 = rect[0], y0 = rect[1], x1 = rect[2], y1 = rect[3];
      drawPart(x0, y0, x1, y0 + e); // unten (GL: y nach oben)
      drawPart(x0, y1 - e, x1, y1); // oben
      drawPart(x0, y0 + e, x0 + e, y1 - e); // links
      drawPart(x1 - e, y0 + e, x1, y1 - e); // rechts
      glUseProgram(m_liquidInner);
      glUniform1i(glGetUniformLocation(m_liquidInner, "u_tex"), 0);
      glUniform1f(glGetUniformLocation(m_liquidInner, "u_saturation"), saturation);
      glUniform4f(glGetUniformLocation(m_liquidInner, "u_tint"), ((tint >> 16) & 0xFF) / 255.0f,
                  ((tint >> 8) & 0xFF) / 255.0f, (tint & 0xFF) / 255.0f, ((tint >> 24) & 0xFF) / 255.0f);
      glUniform1f(glGetUniformLocation(m_liquidInner, "u_alpha"), alpha);
      drawPart(x0 + e, y0 + e, x1 - e, y1 - e);
    }
    else
      drawPart(rect[0], rect[1], rect[2], rect[3]);
    if (liquid)
    {
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, prevTex1);
      glActiveTexture(GL_TEXTURE0);
    }
    if (slot >= 0)
    {
      EndTimeQuery();
      m_pending[slot] = true;
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

  void Report(bool timing)
  {
    if (!timing || m_frames < 60)
      return;
    CLog::Log(LOGINFO,
              "CGUIBackdropBlurControl: {} Bilder, {} Griffe (1/{} -> 1/{} auf {}x{}), Griff CPU avg {:.2f} ms, "
              "Zusammensetzen GPU avg {:.2f} ms",
              m_frames, m_grabs, m_grabScale, m_deepest, m_levels.empty() ? 0 : m_levels[0].w,
              m_levels.empty() ? 0 : m_levels[0].h, m_grabs ? m_cpuGrabSum / m_grabs : 0.0,
              m_gpuN ? m_gpuSum / m_gpuN : 0.0);
    m_frames = m_grabs = m_gpuN = 0;
    m_gpuSum = m_cpuGrabSum = 0;
  }

  bool m_gles3 = false;
  bool m_innerOff = false; //!< Messschalter /tmp/glas-liquid-inner-off: ganze Flaeche mit LIQUID_SHADER

private:
  static constexpr int QUERY_SLOTS = 8;

  struct Level
  {
    GLuint tex = 0;
    GLuint fbo = 0;
    int w = 0;
    int h = 0;
  };

  void Collect()
  {
#if HAS_GLES >= 3
    GLint disjoint = 0;
    glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
    for (int i = 0; i < QUERY_SLOTS; i++)
    {
      if (!m_pending[i])
        continue;
      GLuint available = 0;
      glGetQueryObjectuiv(m_queries[i], GL_QUERY_RESULT_AVAILABLE, &available);
      if (!available)
        continue;
      m_pending[i] = false;
      GLuint ns = 0;
      glGetQueryObjectuiv(m_queries[i], GL_QUERY_RESULT, &ns);
      if (!disjoint && ns != 0xFFFFFFFF)
      {
        m_gpuSum += ns / 1e6;
        m_gpuN++;
      }
    }
#endif
  }

  // Pyramide: Stufe 0 = Griff (1/grabScale), dann halbieren bis 1/deepest; Ergebnis nach einem Schritt hoch
  void Ensure(int w, int h, int grabScale, int deepest)
  {
    if (w == m_w && h == m_h && grabScale == m_grabScale && deepest == m_deepest && !m_levels.empty())
      return;
    for (auto& level : m_levels)
    {
      glDeleteFramebuffers(1, &level.fbo);
      glDeleteTextures(1, &level.tex);
    }
    m_levels.clear();
    for (auto& set : m_spare)
      for (auto& level : set)
      {
        glDeleteFramebuffers(1, &level.fbo);
        glDeleteTextures(1, &level.tex);
      }
    m_spare.clear();
    for (Level& level : m_out)
    {
      if (level.fbo)
        glDeleteFramebuffers(1, &level.fbo);
      if (level.tex)
        glDeleteTextures(1, &level.tex);
      level = Level();
    }
    if (m_source)
      glDeleteTextures(1, &m_source);
    m_source = 0;
    m_result = 0;
    m_sharpTex = 0;
    m_w = w;
    m_h = h;
    m_grabScale = grabScale;
    m_deepest = deepest;
    int div = m_gles3 ? grabScale : 2;
    if (!m_gles3)
      m_source = CreateTexture(w, h);
    for (; div <= deepest; div *= 2)
      m_levels.push_back(CreateLevel(std::max(1, w / div), std::max(1, h / div)));
    if (m_levels.size() >= 2)
      for (auto& out : m_out)
        out = CreateLevel(m_levels[m_levels.size() - 2].w, m_levels[m_levels.size() - 2].h);
    if (m_gles3)
      for (int i = 1; i < RING; i++)
      {
        std::vector<Level> set;
        for (const auto& level : m_levels)
          set.push_back(CreateLevel(level.w, level.h));
        m_spare.push_back(std::move(set));
      }
  }

  //! Pyramide und Ergebnis reihum in RING Saetzen beschreiben (Griff bzw. Quelle, Stufen, Ergebnis): nie in einen Satz
  //! zeichnen, den die GPU fuer ein voriges Bild evtl. noch liest. Die Quelle braucht mindestens zwei (gezeigter Satz
  //! und der, an dem ueber mehrere Bilder gerechnet wird). Gegen das Warten des Treibers beim Umschalten des Zielbilds
  //! half der Ring nicht (29.10.2026: 2 und 4 Saetze gleich; Ursache ist der zweite Durchgang im Bild, s.
  //! PreFrameSource). Messschalter /tmp/glas-ring (Zahl 1-2): so viele Saetze verwenden
  void SwapTop()
  {
    static auto lastCheck = std::chrono::steady_clock::time_point();
    static int ring = RING;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastCheck > std::chrono::seconds(1))
    {
      lastCheck = now;
      ring = RING;
      if (FILE* f = fopen("/tmp/glas-ring", "r"))
      {
        if (fscanf(f, "%d", &ring) != 1)
          ring = RING;
        fclose(f);
      }
      ring = std::clamp(ring, 1, RING);
    }
    m_ring = ring;
    if (ring < 2 || m_spare.size() < static_cast<size_t>(ring - 1))
      return;
    // aktueller Satz ans Ende der benutzten Reserve, der aelteste wird der neue
    m_spare.insert(m_spare.begin() + (ring - 1), std::move(m_levels));
    m_levels = std::move(m_spare.front());
    m_spare.erase(m_spare.begin());
  }

  //! Weichzeichnen ab Stufe first (input = Bild der Stufe davor), Ergebnis eine Stufe ueber der tiefsten
  GLuint Pyramid(GLuint input, int inW, int inH, size_t first)
  {
    // Messschalter /tmp/glas-pyramid-off: keine Paesse, das Glas zeigt die erste Stufe (nur zum Messen)
    static auto lastCheck = std::chrono::steady_clock::time_point();
    static bool off = false;
    if (std::chrono::steady_clock::now() - lastCheck > std::chrono::seconds(1))
    {
      lastCheck = std::chrono::steady_clock::now();
      off = access("/tmp/glas-pyramid-off", F_OK) == 0;
    }
    if (off)
      return input;
    static const float fullPos[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    static const float fullUv[8] = {0, 0, 1, 0, 0, 1, 1, 1};
    glUseProgram(m_down);
    glUniform1i(glGetUniformLocation(m_down, "u_tex"), 0);
    const GLint offDown = glGetUniformLocation(m_down, "u_off");
    for (size_t i = first; i < m_levels.size(); i++)
    {
      const Level& out = m_levels[i];
      glBindFramebuffer(GL_FRAMEBUFFER, out.fbo);
      glViewport(0, 0, out.w, out.h);
      glBindTexture(GL_TEXTURE_2D, input);
      glUniform2f(offDown, 1.0f / inW, 1.0f / inH);
      DrawQuad(fullPos, fullUv, nullptr);
      input = out.tex;
      inW = out.w;
      inH = out.h;
    }
    if (m_levels.size() >= 2)
    {
      // Ergebnis abwechselnd in eins von zwei eigenen Bildern: das bisherige liest die GPU evtl. noch fuer das
      // vorige Bild; es zu ueberschreiben liess den Treiber bis dahin warten (Griff CPU 6-12 ms)
      m_outIndex = (m_outIndex + 1) % std::max(1, std::min(m_ring, RING));
      const Level& out = m_out[m_outIndex];
      glUseProgram(m_up);
      glUniform1i(glGetUniformLocation(m_up, "u_tex"), 0);
      glUniform2f(glGetUniformLocation(m_up, "u_off"), 0.5f / inW, 0.5f / inH);
      glBindFramebuffer(GL_FRAMEBUFFER, out.fbo);
      glViewport(0, 0, out.w, out.h);
      glBindTexture(GL_TEXTURE_2D, input);
      DrawQuad(fullPos, fullUv, nullptr);
      input = out.tex;
    }
    return input;
  }

  static Level CreateLevel(int w, int h)
  {
    Level level;
    level.w = w;
    level.h = h;
    level.tex = CreateTexture(w, h);
    glGenFramebuffers(1, &level.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, level.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, level.tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      CLog::Log(LOGERROR, "CGUIBackdropBlurControl: framebuffer {}x{} incomplete", w, h);
    return level;
  }

  bool m_ready = false;
  bool m_failed = false;
  bool m_timerQuery = false;
  GLuint m_down = 0;
  GLuint m_up = 0;
  GLuint m_composite = 0;
  GLuint m_liquid = 0;
  GLuint m_liquidInner = 0;
  GLuint m_videoMix = 0;
  GLuint m_grab = 0;
  GLuint m_gauss = 0;
  GLuint m_source = 0;
  //! Video-Glas: Videobild (BGR), Mischstufe in Groesse der ersten Stufe, Lage des Videos, HDR
  GLuint m_videoTex = 0; //!< zuletzt geladen (wird gezeichnet)
  GLuint m_videoTexs[2] = {0, 0}; //!< im Wechsel beschrieben: die vorige liest die GPU evtl. noch
  int m_videoNext = 0;
  Level m_mix;
  Level m_mixAlt;
  bool m_videoOn = false;
  bool m_videoHdr = false;
  unsigned int m_videoSeq = 0;
  unsigned int m_videoFrame = 0;
  float m_axis[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  std::vector<uint8_t> m_videoBuf;
  //! schaerfere Quelle fuer die Lichtbrechung am Rand (erste Stufe, mit Video die Mischstufe)
  GLuint m_sharpTex = 0;
  GLuint m_result = 0;
  int m_w = 0;
  int m_h = 0;
  int m_grabScale = 0;
  int m_deepest = 0;
  std::vector<Level> m_levels;
  int m_lastReason = 0;
  unsigned int m_reasons[4] = {};
  unsigned int m_throttledCount = 0;
  unsigned int m_diagFrames = 0;
  static constexpr int RING = 2;
  int m_ring = RING;
  Level m_out[RING];
  std::vector<std::vector<Level>> m_spare; //!< weitere Saetze der Pyramide (SwapTop)
  int m_outIndex = 0;
  unsigned int m_grabFrame = 0;
  unsigned int m_compositeFrame = 0;
  std::vector<std::array<float, 4>> m_composited;
  std::vector<std::array<float, 4>> m_polluted;
  GLuint m_queries[QUERY_SLOTS] = {};
  bool m_pending[QUERY_SLOTS] = {};
  int m_qIndex = 0;
  int m_frames = 0;
  int m_grabs = 0;
  int m_gpuN = 0;
  double m_gpuSum = 0;
  double m_cpuGrabSum = 0;
};
} // namespace
#endif

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
  // Stereo aus: rechtes Auge wird nicht mehr gezeichnet, sein Nachholen entfaellt
  if (!GLAS_STEREO::Active(CServiceBroker::GetWinSystem()->GetGfxContext()))
    m_pendingRecompute[1] = false;
  if (m_pendingRecompute[0] || m_pendingRecompute[1])
  {
    CServiceBroker::GetGUI()->GetWindowManager().MarkDirty(m_screenRect);
    m_backgroundDirty[0] = m_backgroundDirty[1] = true;
  }
  // sichtbar (Process laeuft auch in Bildern, in denen nichts gezeichnet wird): im Zwischenbild bleiben
  if (m_blurRadius > 0)
    GLAS_GUITARGET::NoteBlur();
  // Video-Glas: jedes neue Videobild zeichnet das Glas neu (Echtzeit). Stereo-3D: hoechstens alle
  // GLAS_STEREO::VideoIntervalMs() (Vorgabe 0 = live; Messschalter zum Drosseln)
  if (m_blurRadius > 0 && GLAS_VIDEOGLASS::Query(currentTime).video)
  {
    auto& capture = GLAS_VIDEOGLASS::CCapture::Get();
    capture.Want();
    // Ein gemeinsamer Takt fuer alle Glasflaechen: mit eigenem Takt je Control (Kreise, Kapseln, Popover) lagen die
    // Zeitpunkte versetzt und zusammen wurde doch jedes Bild neu gezeichnet. Alle Controls desselben Bildes
    // (gleiche currentTime) duerfen gemeinsam.
    static unsigned int s_videoTick = 0;
    const bool stereoNow = GLAS_STEREO::Active(CServiceBroker::GetWinSystem()->GetGfxContext());
    bool due = capture.Seq() != m_videoSeq;
    if (due && stereoNow && currentTime != s_videoTick)
    {
      due = currentTime - s_videoTick >= GLAS_STEREO::VideoIntervalMs();
      if (due)
        s_videoTick = currentTime;
    }
    if (due)
    {
      m_videoSeq = capture.Seq();
      MarkDirtyRegion();
      m_backgroundDirty[0] = m_backgroundDirty[1] = true;
    }
  }
  // Griff-Glas: Aenderung dahinter dem gemeinsamen Griff melden, damit schon das erste Glas des Bildes greift
  if (m_sourceId <= 0 && m_backgroundDirty[0] && m_blurRadius > 0)
    CBackdropBlurShared::Get(0, GLAS_VIDEOGLASS::Query(currentTime).video ? 0 : GetParentID()).WantGrab(currentTime);
  // Glas mit eigener Quelle: Aenderung dahinter -> Quelle am Anfang dieses Bildes neu zeichnen (PreFrameSource)
  if (m_sourceId > 0)
  {
    CBackdropBlurShared& source = CBackdropBlurShared::Get(CBackdropBlurShared::SOURCE);
    if (m_backgroundDirty[0])
      source.MarkSourceDirty();
    // gedrosselt: weiter zeichnen, bis der letzte Stand weichgezeichnet ist
    if (source.SourcePending())
      CServiceBroker::GetGUI()->GetWindowManager().MarkDirty(m_screenRect);
  }
  m_lastProcess = currentTime;
  m_frame = currentTime;
  if (m_tint.Update())
    MarkDirtyRegion();
  CGUIControl::Process(currentTime, dirtyregions);
}

void CGUIBackdropBlurControl::Render()
{
#if defined(HAS_GLES)
  // Vergleichsmessung: blurradius 0 zeichnet nichts (mit refresh=always misst das die Bildrate ohne Weichzeichnen)
  if (m_blurRadius <= 0)
    return;
  CGraphicContext& context = CServiceBroker::GetWinSystem()->GetGfxContext();
  // Stereo-3D: je Auge eigener Griff und eigenes Ergebnis (das rechte Auge sieht ein anderes Bild als das linke).
  // Ausnahme Glas ueber laufendem Video: jedes Videobild zeichnet das Glas neu, je Auge waeren das zwei Griffe und
  // zwei Pyramiden je Bild - ueber einem 3D-Film zu viel (Bilder verspaetet, Film ruckelte). Dann nimmt das rechte
  // Auge das Ergebnis des linken (zuerst gezeichnet, Koordinaten relativ zum Viewport des Auges); der Unterschied der
  // Augen ist im stark weichgezeichneten Hintergrund nicht zu sehen, die Glasflaeche selbst bleibt in ihrer Tiefe.
  // Seit 29.10.2026 auch ohne Video: je Auge ein Griff mitten im Bild kostete in den 3D-Menues am meisten (Griff ~ ein
  // verspaetetes Bild); der Augenunterschied ist nach dem Weichzeichnen nicht zu sehen. Messschalter
  // /tmp/glas-stereo-share-off: ohne Video wieder je Auge
  const bool stereo = GLAS_STEREO::Active(context);
  int eye = GLAS_STEREO::Eye(context);
  const bool shareEye =
      stereo && eye == 1 && (GLAS_STEREO::ShareEyes() || GLAS_VIDEOGLASS::Query(m_frame).video);
  CBackdropBlurShared& shared = CBackdropBlurShared::Get(
      shareEye ? 0 : eye, GLAS_VIDEOGLASS::Query(m_frame).video ? 0 : GetParentID());
  if (!shared.Prepare())
    return;

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

  // Skin-Bildschirm (Ursprung oben links) -> Framebuffer-Pixel im Viewport (Ursprung unten links)
  GLint vp[4];
  glGetIntegerv(GL_VIEWPORT, vp);
  const float sx = static_cast<float>(vp[2]) / context.GetWidth();
  const float sy = static_cast<float>(vp[3]) / context.GetHeight();
  float rect[4] = {vp[0] + x0 * sx, vp[1] + vp[3] - y1 * sy, vp[0] + x1 * sx, vp[1] + vp[3] - y0 * sy};
  if (stereo)
  {
    // Stereo-3D: Kodi verschiebt Controls je Auge um <depth> x stereostrength (GraphicContext::SetStereoFactor ->
    // Kamera in RenderSystemGLES::SetCameraPosition); ScaleFinal kennt das nicht. Die Ecken deshalb durch dieselbe
    // Projektion schicken wie die Texturen, damit das Glas in derselben Tiefe liegt wie das Control.
    // Project liefert x in Fensterpixeln (mit Viewport-Versatz), y ab der Oberkante des Viewports nach unten.
    CRenderSystemBase* renderSystem = CServiceBroker::GetRenderSystem();
    float px0 = 1e9f, py0 = 1e9f, px1 = -1e9f, py1 = -1e9f;
    for (int i = 0; i < 4; i++)
    {
      float x = context.ScaleFinalXCoord(cx[i], cy[i]);
      float y = context.ScaleFinalYCoord(cx[i], cy[i]);
      float z = context.ScaleFinalZCoord(cx[i], cy[i]);
      renderSystem->Project(x, y, z);
      const float glY = vp[1] + vp[3] - y;
      px0 = std::min(px0, x);
      px1 = std::max(px1, x);
      py0 = std::min(py0, glY);
      py1 = std::max(py1, glY);
    }
    rect[0] = px0;
    rect[1] = py0;
    rect[2] = px1;
    rect[3] = py1;
  }
  if (rect[2] - rect[0] < 2 || rect[3] - rect[1] < 2)
    return;

  // Staerke: tiefste Stufe der Pyramide; blurradius 48 (Apple-Material) -> 1/16 der Bildschirmaufloesung
  int deepest = 4;
  while (deepest < 64 && deepest * 3 < m_blurRadius * sx)
    deepest *= 2;
  int grabScaleWanted = m_grabScale;
  {
    // Messschalter /tmp/glas-grabscale (Zahl): erste Stufe fuer alle Glasflaechen (Zahl der Paesse)
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

  // Zustand sichern
  GLint prevFbo = 0, prevProgram = 0, prevTex = 0, prevActive = 0, prevArray = 0;
  GLint prevScissor[4];
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
  glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArray);
  glGetIntegerv(GL_SCISSOR_BOX, prevScissor);
  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
  const GLboolean blend = glIsEnabled(GL_BLEND);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  const bool video = shared.PrepareVideo(m_frame);
  // Eigene Quelle (<source>): Hintergrund vom Anfang des Bildes (PreFrameSource), kein Griff. Nur 2D und ohne Video;
  // bis die Quelle bereit ist (erstes Bild), wie bisher greifen
  if (m_sourceId > 0 && !stereo && !video && !GLAS_STEREO::SourceOff())
  {
    CBackdropBlurShared& source = CBackdropBlurShared::Get(CBackdropBlurShared::SOURCE);
    if (source.Prepare())
    {
      // erste Stufe doppelt so gross wie das Ergebnis (Gauss-Pass); bei 1080p wie der Griff 1/4, bei 4K 1/8
      source.NoteSource(GetParentID(), m_sourceId, vp, std::max(2, deepest / 4), deepest, m_updateRate, m_frame);
      if (source.SourceReady(GetParentID(), m_sourceId))
      {
        m_fromSource = true;
        m_pendingRecompute[eye] = false;
        m_backgroundDirty[eye] = false;
        if (scissor)
          glEnable(GL_SCISSOR_TEST);
        else
          glDisable(GL_SCISSOR_TEST);
        source.m_innerOff = GLAS_STEREO::LiquidInnerOff();
        source.Composite(rect, vp, m_cornerRadius * sx * scale, m_saturation, m_tint, alpha, m_frame,
                         m_timing || GLAS_GUITARGET::Timing(), m_liquid, sx * scale);
        source.Report(m_timing || GLAS_GUITARGET::Timing());
        if (!blend)
          glDisable(GL_BLEND);
        glScissor(prevScissor[0], prevScissor[1], prevScissor[2], prevScissor[3]);
        glBindTexture(GL_TEXTURE_2D, prevTex);
        glActiveTexture(prevActive);
        glBindBuffer(GL_ARRAY_BUFFER, prevArray);
        glUseProgram(prevProgram);
        return;
      }
    }
  }
  // Stereo-3D mit Video-Glas: Ergebnis kommt vom Anfang des Bildes (PreFrameVideo), kein Griff mitten im Bild.
  // Abschalten zum Vergleich: /tmp/glas-stereo-preframe-off
  // auch in 2D (17.10.2026): ueber dem Film nur das Videobild, ohne die Abdunklung hinter dem Player-Menue (sonst wirkte
  // das Glas dort rauchig und schwaecher als im Kontrollzentrum)
  const bool videoOnly = video && GLAS_STEREO::PreFrameAllowed();
  if (videoOnly)
    shared.NoteVideoOnly(vp[2], vp[3]);
  const bool preframed = videoOnly && shared.HasResult() &&
                         (shared.VideoOnlyReady(m_frame) || shared.m_videoOnlySeq != ~0u);
  auto need = (shareEye || preframed)
                  ? CBackdropBlurShared::Need::NONE
                  : shared.NeedsGrab(rect, m_frame, m_backgroundDirty[eye], grabScale, deepest);
  // eben noch aus der Quelle: der gemeinsame Griff ist evtl. alt (anderes Fenster, anderer Hintergrund) - neu greifen
  if (m_fromSource && !shareEye && !preframed)
    need = CBackdropBlurShared::Need::FORCED;
  m_fromSource = false;
  // Drosselung: bei laufender Bewegung hinter dem Glas hoechstens updaterate Mal je Sekunde neu weichzeichnen,
  // dazwischen der Zwischenspeicher; der letzte Stand wird nachgeholt (m_pendingRecompute)
  const bool throttled = need == CBackdropBlurShared::Need::BACKGROUND && m_updateRate > 0 && !video &&
                         m_frame - shared.LastGrabFrame() < static_cast<unsigned int>(1000.0f / m_updateRate);
  // Messschalter /tmp/glas-grablog: jede Entscheidung mit Griff-Bedarf ins Log (Fenster, Flaeche, Grund, gedrosselt)
  if (need != CBackdropBlurShared::Need::NONE && access("/tmp/glas-grablog", F_OK) == 0)
    CLog::Log(LOGINFO, "GLAS_GRAB: Bild {} Fenster {} Glas {:.0f},{:.0f} {:.0f}x{:.0f} {} Grund {} dahinter {}{}", m_frame,
              GetParentID(), x0, y0, x1 - x0, y1 - y0,
              need == CBackdropBlurShared::Need::FORCED ? "erzwungen" : "Hintergrund", shared.LastReason(),
              m_backgroundDirty[eye] ? "geaendert" : "ruhig", throttled ? ", gedrosselt" : "");
  if (need != CBackdropBlurShared::Need::NONE && !throttled)
  {
    shared.Grab(vp, prevFbo, m_frame, grabScale, deepest, m_grabProbe, m_timing || GLAS_GUITARGET::Timing());
    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
  }
  m_pendingRecompute[eye] = throttled;
  m_backgroundDirty[eye] = false;
  if (GLAS_GUITARGET::Diag())
  {
    shared.Count(need != CBackdropBlurShared::Need::NONE && !throttled, throttled);
    shared.DiagReport();
  }
  if (scissor)
    glEnable(GL_SCISSOR_TEST);
  else
    glDisable(GL_SCISSOR_TEST);
  shared.m_innerOff = GLAS_STEREO::LiquidInnerOff();
  shared.Composite(rect, vp, m_cornerRadius * sx * scale, m_saturation, m_tint, alpha, m_frame,
                   m_timing || GLAS_GUITARGET::Timing(), m_liquid,
                   sx * scale);
  if (GLAS_GUITARGET::Diag())
    GLAS_GUITARGET::NoteGlass(rect);
  shared.Report(m_timing || GLAS_GUITARGET::Timing());

  // Zustand zurueck
  if (!blend)
    glDisable(GL_BLEND);
  glScissor(prevScissor[0], prevScissor[1], prevScissor[2], prevScissor[3]);
  glBindTexture(GL_TEXTURE_2D, prevTex);
  glActiveTexture(prevActive);
  glBindBuffer(GL_ARRAY_BUFFER, prevArray);
  glUseProgram(prevProgram);
#endif
}

void CGUIBackdropBlurControl::FreeResources(bool immediately)
{
  CGUIControl::FreeResources(immediately);
}

/*!
 \brief Eigenes Zwischenbild fuer die Oberflaeche (Glas): Solange Live-Glas im Spiel ist, zeichnet Kodi jedes Bild in
        ein eigenes Framebuffer-Objekt und kopiert es am Ende einmal auf den Bildschirm. Das Glas liest dann aus
        diesem Zwischenbild statt aus dem Fenster-Framebuffer; dort laesst der Mali-Treiber die CPU beim Lesen
        mitten im Bild bis zum Bildwechsel warten (gemessen 17-35 ms, auch bei 64x64 Pixeln).
        Aufgerufen aus CApplication::Render (Begin nach BeginRender, End vor EndRender).
*/
namespace GLAS_GUITARGET
{
#if defined(HAS_GLES) && HAS_GLES >= 3
namespace
{
GLuint g_fbo = 0;
GLuint g_color = 0; //!< Farbtextur des Zwischenbilds (seit 29.10.2026 Textur statt Renderbuffer: Griff per Shader)
int g_w = 0;
int g_h = 0;
bool g_bound = false;
//! Messschalter /tmp/glas-grab-blit: Griff wie frueher per glBlitFramebuffer statt Shader aus der Textur
bool g_grabBlit = false;
bool g_ready = false;
bool g_failed = false;
std::chrono::steady_clock::time_point g_lastBlur;
// Messschalter (je Sekunde geprueft): /tmp/glas-guitarget-always = immer Zwischenbild, -off = nie
int g_force = 0;
//! Zwischenbild auch bei Stereo-3D (Vorgabe seit 17.10.2026; Messschalter /tmp/glas-guitarget-stereo-off = Fenster).
//! Gemessen ueber einem 3D-Film (1080p23.98 Frame Packing, Glas mit Videobild): ohne Zwischenbild 12-18 Bilder/s und
//! bis 177 ms je Bild (Film und Oberflaeche stockten), mit Zwischenbild 24-26 Bilder/s
bool g_stereoTarget = true;
std::chrono::steady_clock::time_point g_lastCheck;
// Diagnose (/tmp/glas-diag): Wechsel Fenster/Zwischenbild und ganz schwarze Bilder ins Log
bool g_diag = false;
// Bildzeiten (/tmp/glas-frames, ohne Rueckleseaufwand): je Sekunde gezeichnete Bilder und Bilder ueber 20 ms
bool g_frames = false;
// Zeitmessung (/tmp/glas-timing, bremst leicht: glFinish): je Sekunde Zeichnen der Oberflaeche (CPU bis alles auf der
// GPU fertig ist) und Kopie des Zwischenbilds, Mittel und Hoechstwert
bool g_timing = false;
//! Stereo-3D: am Ende jedes Bildes auf die GPU warten (glFinish vor und nach der Kopie des Zwischenbilds). Gemessen
//! ueber einem 3D-Film mit Live-Video-Glas (17.10.2026): ohne 2-8 Bilder je Sekunde um einen Bildwechsel verspaetet,
//! mit 0 (Zeichnen ~27 ms + Kopie ~12 ms von 41,7 ms) - sonst stauen sich Bilder in der GPU und verpassen den
//! Bildwechsel. Messschalter /tmp/glas-stereo-finish-off schaltet es ab
bool g_stereoFinish = true;
bool g_preframeWindow = false;
bool g_partialOff = true;
bool g_fsmarkSkip = false;
bool g_afterRenderMark = true;
std::chrono::steady_clock::time_point g_tBegin;
double g_renderSum = 0, g_renderMax = 0, g_blitSum = 0, g_blitMax = 0, g_cpuSum = 0, g_cpuMax = 0;
int g_timedFrames = 0;
std::chrono::steady_clock::time_point g_lastTimingReport;
std::chrono::steady_clock::time_point g_lastFrame;
std::chrono::steady_clock::time_point g_lastFrameReport;
unsigned int g_frameCount = 0, g_lateCount = 0, g_late2Count = 0;
double g_worstMs = 0;
bool g_wasBound = false;
unsigned int g_frameNo = 0;
} // namespace

bool Diag()
{
  return g_diag;
}

//! Diagnose: Mitte der groessten Glasflaeche dieses Bildes (Framebuffer-Pixel); End() prueft dort die Helligkeit
int g_glassX = -1, g_glassY = -1;
float g_glassArea = 0;
int g_lastLum = -1;

void NoteGlass(const float rect[4])
{
  const float area = (rect[2] - rect[0]) * (rect[3] - rect[1]);
  if (area <= g_glassArea)
    return;
  g_glassArea = area;
  g_glassX = static_cast<int>((rect[0] + rect[2]) / 2);
  g_glassY = static_cast<int>((rect[1] + rect[3]) / 2);
}

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
 \brief Pauschale Fenstermarkierung: Kodi markiert ein ganzes Fenster als geaendert, sobald sich in einem Dialog
        darueber etwas tut (CGUIWindowManager::AfterRender, "Dialog state can affect visibility states"), ebenso
        CGUIWindowManager::MarkDirty. Tatsaechliche Aenderungen tragen die Controls im Fenster selbst ein; das Glas
        ignoriert deshalb Flaechen, die ein Fenster nur wegen einer solchen Markierung eintraegt (ohne eigene
        Animation, Verschiebung oder Ungueltigkeit). Sonst hielt jede Bewegung im Kontrollzentrum (und das eigene
        Nachholen) den Hintergrund fuer veraendert: 60 Bilder je Sekunde und 15 Neuberechnungen bei stillem Bild.
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

void NoteBlur()
{
  g_lastBlur = std::chrono::steady_clock::now();
}

void Begin()
{
  g_bound = false;
  const auto now = std::chrono::steady_clock::now();
  g_tBegin = now;
  // Stereo-3D mit Video-Glas: Videobild weichzeichnen, bevor die Oberflaeche gezeichnet wird (kein Griff mitten im
  // Bild, s. CBackdropBlurShared::PreFrameVideo). Ohne Zwischenbild direkt ins Fenster zu zeichnen (spart die Kopie)
  // war in 3D viel langsamer (Player-Menue und Spurmenues 6-9 statt 24 Bilder/s, 17.10.2026) - nur noch als
  // Messschalter /tmp/glas-stereo-preframe-window
  bool videoOnlyFrame = false;
  if (CWinSystemBase* ws = CServiceBroker::GetWinSystem())
  {
    const RENDER_STEREO_MODE mode = ws->GetGfxContext().GetStereoMode();
    const bool stereo = mode != RENDER_STEREO_MODE_OFF && mode != RENDER_STEREO_MODE_MONO;
    videoOnlyFrame = CBackdropBlurShared::Get(0).PreFrameVideo() && stereo && g_preframeWindow;
  }
  // Glas mit eigener Quelle: Quelle weichzeichnen, bevor die Oberflaeche gezeichnet wird
  CBackdropBlurShared::Get(CBackdropBlurShared::SOURCE).PreFrameSource();
  if (now - g_lastCheck > std::chrono::seconds(1))
  {
    g_lastCheck = now;
    const int force = access("/tmp/glas-guitarget-always", F_OK) == 0 ? 1
                      : access("/tmp/glas-guitarget-off", F_OK) == 0  ? -1
                                                                      : 0;
    if (force != g_force)
      CLog::Log(LOGINFO, "GLAS_GUITARGET: Messschalter {}", force);
    g_force = force;
    const bool diag = access("/tmp/glas-diag", F_OK) == 0;
    if (diag != g_diag)
      CLog::Log(LOGINFO, "GLAS_GUITARGET: Diagnose {}", diag ? "an" : "aus");
    g_diag = diag;
    g_frames = access("/tmp/glas-frames", F_OK) == 0;
    g_timing = access("/tmp/glas-timing", F_OK) == 0;
    g_stereoFinish = access("/tmp/glas-stereo-finish-off", F_OK) != 0;
    g_preframeWindow = access("/tmp/glas-stereo-preframe-window", F_OK) == 0;
    g_grabBlit = access("/tmp/glas-grab-blit", F_OK) == 0;
    // Mitschnitt des angezeigten Bildes (End): auch ein ruhendes Bild einmal neu zeichnen (Glas aus dem Zwischenspeicher)
    if (access("/tmp/glas-dump", F_OK) == 0 && CServiceBroker::GetGUI())
      CServiceBroker::GetGUI()->GetWindowManager().MarkDirty();
    // Vorgabe aus (17.10.2026: half ueber 3D-Filmen nicht messbar, kleines Risiko fuer Darstellungsfehler)
    g_partialOff = access("/tmp/glas-partial-on", F_OK) != 0;
    g_fsmarkSkip = access("/tmp/glas-fsmark-skip", F_OK) == 0;
    // Vorgabe wie Kodi (17.10.2026: Weglassen half nicht messbar); /tmp/glas-afterrender-skip zum Messen
    g_afterRenderMark = access("/tmp/glas-afterrender-skip", F_OK) != 0;
    const bool stereoTarget = access("/tmp/glas-guitarget-stereo-off", F_OK) != 0;
    if (stereoTarget != g_stereoTarget)
      CLog::Log(LOGINFO, "GLAS_GUITARGET: Zwischenbild bei Stereo-3D {}", stereoTarget ? "an" : "aus");
    g_stereoTarget = stereoTarget;
  }
  if (g_diag)
    ReportChanges(now);
  if (g_failed || g_force < 0 || (g_force == 0 && now - g_lastBlur > std::chrono::seconds(3)))
  {
    // zurueck zum Fenster: dessen Puffer sind veraltet, einmal alles neu zeichnen
    if (g_ready)
      CServiceBroker::GetGUI()->GetWindowManager().MarkDirty();
    g_ready = false;
    return;
  }
  CWinSystemBase* winSystem = CServiceBroker::GetWinSystem();
  if (!winSystem || videoOnlyFrame ||
      (winSystem->GetGfxContext().GetStereoMode() != RENDER_STEREO_MODE_OFF && !g_stereoTarget))
  {
    // Fenster: Zwischenbild beim naechsten Wechsel erst wieder fuellen (sonst ein Bild mit altem Inhalt)
    if (g_ready && CServiceBroker::GetGUI())
      CServiceBroker::GetGUI()->GetWindowManager().MarkDirty();
    g_ready = false;
    return;
  }
  // Groesse des Fenster-Framebuffers selbst (EGL-Flaeche), nicht der Oberflaeche: bei 3D (Frame Packing) ist er
  // 1920 x 2205 (beide Augen und 45 Zeilen Luecke), die Oberflaeche nur 1920 x 1080. Mit Oberflaechengroesse kopierte
  // das Zwischenbild nur das untere Auge - das linke sah das Video ohne Oberflaeche (17.10.2026)
  int w = static_cast<int>(winSystem->GetWidth());
  int h = static_cast<int>(winSystem->GetHeight());
  {
    EGLDisplay display = eglGetCurrentDisplay();
    EGLSurface surface = eglGetCurrentSurface(EGL_DRAW);
    EGLint sw = 0, sh = 0;
    if (display != EGL_NO_DISPLAY && surface != EGL_NO_SURFACE && eglQuerySurface(display, surface, EGL_WIDTH, &sw) &&
        eglQuerySurface(display, surface, EGL_HEIGHT, &sh) && sw > 0 && sh > 0)
    {
      w = sw;
      h = sh;
    }
  }
  GLint current = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &current);
  if (w <= 0 || h <= 0 || current != 0)
    return;
  if (w != g_w || h != g_h)
  {
    if (g_fbo)
      glDeleteFramebuffers(1, &g_fbo);
    if (g_color)
      glDeleteTextures(1, &g_color);
    GLint prevTex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGenTextures(1, &g_color);
    glBindTexture(GL_TEXTURE_2D, g_color);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, prevTex);
    glGenFramebuffers(1, &g_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_color, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
      CLog::Log(LOGERROR, "GLAS_GUITARGET: Zwischenbild {}x{} nicht vollstaendig ({:#x}), bleibe beim Fenster", w, h,
                status);
      g_failed = true;
      return;
    }
    g_w = w;
    g_h = h;
    g_ready = false;
    CLog::Log(LOGINFO, "GLAS_GUITARGET: Zwischenbild {}x{} angelegt", w, h);
  }
  // Wechsel ins Zwischenbild: dessen Inhalt ist noch leer bzw. veraltet; erst ab dem naechsten, ganz neu
  // gezeichneten Bild verwenden (auch bei Dirty-Region-Verfahren, die nur Teile zeichnen)
  if (!g_ready)
  {
    CServiceBroker::GetGUI()->GetWindowManager().MarkDirty();
    g_ready = true;
    return;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
  g_bound = true;
}

//! Diagnose: fuenf Punkte des fertigen Bildes lesen (bremst, nur mit /tmp/glas-diag); alle schwarz -> Log
void CheckBlack(bool bound)
{
  g_frameNo++;
  g_renderedFrames++;
  if (bound != g_wasBound)
    CLog::Log(LOGINFO, "GLAS_DIAG: Bild {} jetzt {}", g_frameNo, bound ? "Zwischenbild" : "Fenster");
  g_wasBound = bound;
  CWinSystemBase* winSystem = CServiceBroker::GetWinSystem();
  const int w = static_cast<int>(winSystem->GetWidth());
  const int h = static_cast<int>(winSystem->GetHeight());
  const int px[5][2] = {{w / 2, h / 2}, {w / 4, h / 4}, {3 * w / 4, h / 4}, {w / 4, 3 * h / 4}, {3 * w / 4, 3 * h / 4}};
  int bright = 0;
  unsigned char rgba[4];
  for (const auto& p : px)
  {
    glReadPixels(p[0], p[1], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    bright = std::max(bright, rgba[0] + rgba[1] + rgba[2]);
  }
  if (bright < 12)
    CLog::Log(LOGWARNING, "GLAS_DIAG: Bild {} schwarz ({}, Fenster {})", g_frameNo,
              bound ? "Zwischenbild" : "Fenster",
              CServiceBroker::GetGUI()->GetWindowManager().GetActiveWindowOrDialog());
  // Glas: Helligkeit in der Mitte der groessten Flaeche; Einbruch gegenueber dem vorigen Bild -> Log
  if (g_glassX >= 0)
  {
    glReadPixels(g_glassX, g_glassY, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    const int lum = rgba[0] + rgba[1] + rgba[2];
    if (g_lastLum > 30 && lum * 10 < g_lastLum * 6)
      CLog::Log(LOGWARNING, "GLAS_DIAG: Bild {} Glas dunkler bei {},{}: {} -> {} ({})", g_frameNo, g_glassX, g_glassY,
                g_lastLum, lum, bound ? "Zwischenbild" : "Fenster");
    g_lastLum = lum;
  }
  else
    g_lastLum = -1;
  g_glassX = g_glassY = -1;
  g_glassArea = 0;
}

void CountFrame()
{
  const auto now = std::chrono::steady_clock::now();
  const double ms = std::chrono::duration<double, std::milli>(now - g_lastFrame).count();
  g_lastFrame = now;
  // Bildwechsel der Anzeige (24 Hz: 41,7 ms, 60 Hz: 16,7 ms); verspaetet = mehr als 1,5 Bildwechsel
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

void ReportTiming(double renderMs, double blitMs, double cpuMs)
{
  g_timedFrames++;
  g_cpuSum += cpuMs;
  g_cpuMax = std::max(g_cpuMax, cpuMs);
  g_renderSum += renderMs;
  g_blitSum += blitMs;
  g_renderMax = std::max(g_renderMax, renderMs);
  g_blitMax = std::max(g_blitMax, blitMs);
  const auto now = std::chrono::steady_clock::now();
  if (now - g_lastTimingReport >= std::chrono::seconds(1))
  {
    CGraphicContext& context = CServiceBroker::GetWinSystem()->GetGfxContext();
    CLog::Log(LOGINFO,
              "GLAS_TIMING: {} Bilder, Zeichnen avg {:.1f} / max {:.1f} ms (davon CPU avg {:.1f} / max {:.1f}), "
              "Kopie avg {:.1f} / max {:.1f} ms (Stereo {}, Zwischenbild {})",
              g_timedFrames, g_renderSum / g_timedFrames, g_renderMax, g_cpuSum / g_timedFrames, g_cpuMax,
              g_blitSum / g_timedFrames, g_blitMax,
              static_cast<int>(context.GetStereoMode()), g_bound ? "ja" : "nein");
    g_lastTimingReport = now;
    g_timedFrames = 0;
    g_renderSum = g_renderMax = g_blitSum = g_blitMax = g_cpuSum = g_cpuMax = 0;
  }
}

bool Timing()
{
  return g_timing;
}

void End(bool rendered)
{
  if (g_frames && rendered)
    CountFrame();
  std::chrono::steady_clock::time_point tRendered;
  const auto tCpu = std::chrono::steady_clock::now(); // CPU fertig (vor dem Warten auf die GPU)
  // Warten nur ueber einem laufenden 3D-Film (dort noetig, s. g_stereoFinish); in den 3D-Menues ohne Film bremste es nur
  // (24.10.2026: Mediathek 74 statt 18 % verspaetet) - bisher schaltete das der Glas Helper (display3d.py) von aussen
  bool finish = rendered && g_timing;
  if (rendered && !finish && g_stereoFinish &&
      CServiceBroker::GetWinSystem()->GetGfxContext().GetStereoMode() != RENDER_STEREO_MODE_OFF)
  {
    const auto appPlayer = CServiceBroker::GetAppComponents().GetComponent<CApplicationPlayer>();
    finish = appPlayer && appPlayer->IsPlayingVideo();
  }
  if (finish)
  {
    glFinish();
    tRendered = std::chrono::steady_clock::now();
  }
  if (g_diag && rendered)
    CheckBlack(g_bound);
  // Messschalter /tmp/glas-dump: das gerade angezeigte Bild (Zwischenbild bzw. Fenster) einmal nach
  // /tmp/glas-live.rgba (Breite x Hoehe x RGBA, Zeilen von unten); Kodis Screenshot zeichnet dagegen eigens neu.
  // Steht eine Zahl N darin: die naechsten N gezeichneten Bilder nach /tmp/glas-live-NN.rgba (Animationen, Scrollen)
  static int s_dumpLeft = 0;
  static int s_dumpIndex = 0;
  if (rendered && s_dumpLeft == 0 && access("/tmp/glas-dump", F_OK) == 0)
  {
    int count = 0;
    if (FILE* f = fopen("/tmp/glas-dump", "r"))
    {
      if (fscanf(f, "%d", &count) != 1)
        count = 0;
      fclose(f);
    }
    unlink("/tmp/glas-dump");
    s_dumpLeft = count > 0 ? std::min(count, 60) : -1;
    s_dumpIndex = 0;
  }
  if (rendered && s_dumpLeft != 0)
  {
    const int w = g_bound ? g_w : static_cast<int>(CServiceBroker::GetWinSystem()->GetWidth());
    const int h = g_bound ? g_h : static_cast<int>(CServiceBroker::GetWinSystem()->GetHeight());
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_bound ? g_fbo : 0);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    const std::string file = s_dumpLeft < 0 ? std::string("/tmp/glas-live.rgba")
                                            : StringUtils::Format("/tmp/glas-live-{:02}.rgba", s_dumpIndex++);
    if (FILE* f = fopen(file.c_str(), "wb"))
    {
      fwrite(pixels.data(), 1, pixels.size(), f);
      fclose(f);
    }
    CLog::Log(LOGINFO, "GLAS_GUITARGET: angezeigtes Bild {}x{} ({}) nach {}", w, h,
              g_bound ? "Zwischenbild" : "Fenster", file);
    s_dumpLeft = s_dumpLeft < 0 ? 0 : s_dumpLeft - 1;
  }
  if (!g_bound)
  {
    if (g_timing && rendered)
      ReportTiming(std::chrono::duration<double, std::milli>(tRendered - g_tBegin).count(), 0.0,
                   std::chrono::duration<double, std::milli>(tCpu - g_tBegin).count());
    return;
  }
  g_bound = false;
  if (rendered)
  {
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    // alter Fensterinhalt wird komplett ueberschrieben: Kachel-GPU (Mali) muss ihn nicht erst laden
    const GLenum windowColor = GL_COLOR;
    glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 1, &windowColor);
    glBlitFramebuffer(0, 0, g_w, g_h, 0, 0, g_w, g_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (scissor)
      glEnable(GL_SCISSOR_TEST);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (finish)
    glFinish();
  if (g_timing && rendered)
  {
    const auto tDone = std::chrono::steady_clock::now();
    ReportTiming(std::chrono::duration<double, std::milli>(tRendered - g_tBegin).count(),
                 std::chrono::duration<double, std::milli>(tDone - tRendered).count(),
                 std::chrono::duration<double, std::milli>(tCpu - g_tBegin).count());
  }
}

bool Active()
{
  return g_bound;
}

bool SourceTexture(GLint fbo, GLuint& tex, int& w, int& h)
{
  if (!g_bound || g_grabBlit || fbo != static_cast<GLint>(g_fbo))
    return false;
  tex = g_color;
  w = g_w;
  h = g_h;
  return true;
}

//! Messschalter /tmp/glas-fsmark-skip: Vollbild-Video markiert bei Overlays nicht das ganze Fenster (nur Messung)
bool SkipFullscreenMark()
{
  return g_fsmarkSkip && g_ready; // Process laeuft vor Begin: g_ready (Zwischenbild in Gebrauch), nicht g_bound
}

//! CGUIWindowManager::AfterRender: ganzes Fenster unter einem geaenderten Dialog markieren? Nicht, solange das
//! Zwischenbild in Gebrauch ist (Vorgabe: wie Kodi; Messschalter /tmp/glas-afterrender-skip laesst sie weg)
bool KeepDialogMark()
{
  return !g_ready || g_afterRenderMark;
}

//! Kodis markierte Bereiche fuer das Zwischenbild zu einem Rechteck vereinigen (in 3D um die Tiefenverschiebung
//! erweitert: Controls liegen je Auge bis zu ~10 px versetzt). false = wie Kodi (ganzer Bildschirm).
//! Vorgabe aus, Messschalter /tmp/glas-partial-on
bool Regions(CDirtyRegionList& regions)
{
  if (g_partialOff)
    return false;
  if (regions.empty())
    return true;
  CRect all;
  for (const auto& r : regions)
    all.Union(r);
  CWinSystemBase* winSystem = CServiceBroker::GetWinSystem();
  CGraphicContext& context = winSystem->GetGfxContext();
  const float margin = context.GetStereoMode() != RENDER_STEREO_MODE_OFF ? 16.0f : 2.0f;
  all.x1 -= margin;
  all.y1 -= margin;
  all.x2 += margin;
  all.y2 += margin;
  all.Intersect(CRect(0, 0, static_cast<float>(context.GetWidth()), static_cast<float>(context.GetHeight())));
  regions.clear();
  regions.emplace_back(all);
  return true;
}
#else
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
bool Timing()
{
  return false;
}
void NoteChange(const CGUIControl*, const CRect&, int, unsigned int) {}
bool IsWindowMark(const CRect&, unsigned int)
{
  return false;
}
void NoteGlass(const float*) {}
void NoteBlur() {}
bool Diag()
{
  return false;
}
void Begin() {}
void End(bool) {}
bool Active()
{
  return false;
}
#endif
} // namespace GLAS_GUITARGET
