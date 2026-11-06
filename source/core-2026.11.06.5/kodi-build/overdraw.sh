#!/bin/bash
# Laeuft im Container: Mess-Werkzeug "Overdraw" in /build/kodi (Zweig glas) einbauen.
# Zaehlt je Bild die Bildschirmpixel, die jede Grafik (CGUITexture) zeichnet, und schreibt alle 60 Bilder die
# groessten Verursacher ins Log. Aktiv nur, solange die Datei /tmp/glas-overdraw existiert (Pruefung je Sekunde).
# Bewusst ohne Aenderung an Headern (sonst uebersetzt der halbe Build neu).
set -e
cd /build/kodi
git checkout -q glas
G=xbmc/guilib

if ! grep -q GLAS_OVERDRAW $G/GUITexture.cpp; then
perl -0pi -e 's/(#include <stdexcept>\n)/$1#include <algorithm>
#include <chrono>
#include <map>
#include <unistd.h>
#include <vector>

#include "utils\/log.h"

\/\/ Glas: Mess-Werkzeug Overdraw (siehe tools\/kodi-build\/overdraw.sh)
namespace GLAS_OVERDRAW
{
bool g_enabled = false;
unsigned int g_frames = 0;
double g_total = 0;
std::map<std::string, double> g_area;
std::chrono::steady_clock::time_point g_lastCheck;

void FrameBegin()
{
  const auto now = std::chrono::steady_clock::now();
  if (now - g_lastCheck > std::chrono::seconds(1))
  {
    g_lastCheck = now;
    const bool enabled = access("\/tmp\/glas-overdraw", F_OK) == 0;
    if (enabled != g_enabled)
    {
      g_enabled = enabled;
      g_frames = 0;
      g_total = 0;
      g_area.clear();
    }
  }
  if (!g_enabled)
    return;
  if (++g_frames < 61)
    return;
  const double screen = 1920.0 * 1080.0;
  const double frames = g_frames - 1;
  std::vector<std::pair<double, std::string>> top;
  for (const auto& entry : g_area)
    top.emplace_back(entry.second, entry.first);
  std::sort(top.rbegin(), top.rend());
  CLog::Log(LOGINFO, "GLAS_OVERDRAW: {:.2f} Bildschirmflaechen je Bild ({:.1f} MPix), {} Bilder", g_total \/ frames \/ screen,
            g_total \/ frames \/ 1e6, frames);
  for (size_t i = 0; i < top.size() && i < 15; i++)
  {
    std::string name = top[i].second;
    if (name.size() > 70)
      name = "..." + name.substr(name.size() - 67);
    CLog::Log(LOGINFO, "GLAS_OVERDRAW:   {:5.2f}  {}", top[i].first \/ frames \/ screen, name);
  }
  g_frames = 1;
  g_total = 0;
  g_area.clear();
}

void Add(const std::string& name, double area)
{
  if (!g_enabled)
    return;
  g_total += area;
  g_area[name] += area;
}
} \/\/ namespace GLAS_OVERDRAW
/' $G/GUITexture.cpp
perl -0pi -e 's/(  Draw\(x, y, z, texture, diffuse, orientation\);\n)/  GLAS_OVERDRAW::Add(m_info.filename, std::abs((x[2] - x[0]) * (y[2] - y[0])));\n$1/' $G/GUITexture.cpp
fi

if ! grep -q GLAS_OVERDRAW $G/GUIWindowManager.cpp; then
perl -0pi -e 's/(void CGUIWindowManager::RenderPass\(\) const\n\{\n)/namespace GLAS_OVERDRAW\n{\nvoid FrameBegin();\n}\n\n$1  GLAS_OVERDRAW::FrameBegin();\n/' $G/GUIWindowManager.cpp
fi

grep -c GLAS_OVERDRAW $G/GUITexture.cpp $G/GUIWindowManager.cpp
# GLAS_NO_COMMIT=1: aus apply-glas.sh aufgerufen, das selbst committet
if [ "${GLAS_NO_COMMIT:-0}" != "1" ]; then
    git add -A xbmc
    git commit -qm "Glas: Mess-Werkzeug Overdraw (/tmp/glas-overdraw)" || echo "keine Aenderung"
    git log --oneline -1
fi
