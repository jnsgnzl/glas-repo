<#
.SYNOPSIS
    Windows-Gegenstueck zu tools/kodi-build/apply-glas.sh: Glas-Aenderungen in den Kodi-Quellbaum (Zweig glas)
    uebernehmen. Beide Stufen am 01.11.2026 gegen Kodi 21.3-Omega (a3a448d) gelaufen und gebaut (VS 2022, x64).

.DESCRIPTION
    Voraussetzung: prepare-source.ps1 (Kodi 21.3-Omega als Git-Repo, Zweig vanilla).

    Arbeitsweise (wie apply-glas.sh, aber mit Textankern statt perl-Regex):
      - Jede beruehrte Datei wird zuerst auf den Stand von "vanilla" zurueckgesetzt, dann gepatcht. Dadurch laesst
        sich das Skript beliebig oft wiederholen.
      - Jeder Eingriff bricht mit einer Meldung ab, wenn sein Anker fehlt (Kodi-Version passt nicht).
      - Dateien, deren Inhalt gleich geblieben ist, bekommen ihren alten Zeitstempel zurueck (sonst uebersetzt
        MSBuild alles neu, was sie einbindet).
      - Dateien werden mit LF und UTF-8 ohne BOM geschrieben (wie im Kodi-Repo).

    Zwei Stufen, automatisch gewaehlt:
      Live-Glas    wenn windows/src/xbmc/guilib/GUIBackdropBlurControl.cpp (DX11-Fassung) existiert und
                   -NoLiveGlass fehlt: Control backdropblur, Fabrik, CMake, DoProcess-Meldungen, Zwischenbild-Haken
                   in CApplication::Render, GUIWindowManager/GUIWindowFullScreen wie auf der Vero.
      Durchstich   sonst: nur plattformfreie Teile (Schrift in Zielgroesse, Stereo-Texturen, Overdraw). Die
                   Startlog-Zeile enthaelt dann bewusst NICHT "Glas: eigener Kodi-Build", damit script.glas.helper
                   die Live-Glas-Einstellung nicht anbietet.

    Bewusst weggelassen (nur Amlogic/Vero): ScreenshotAML.cpp, AMLUtils.cpp, WinSystemAmlogic.cpp,
    RenderSystemGLES.cpp.

    Plan und Hintergrund: docs/plan-liveglas-windows.md

.PARAMETER KodiDir
    Kodi-Quellbaum (Git) aus prepare-source.ps1.

.PARAMETER ToolsDir
    tools/kodi-build (Vorgabe: Ordner ueber diesem Skript).

.PARAMETER NoLiveGlass
    Auch mit vorhandener DX-Fassung nur den Durchstich einbauen.

.PARAMETER VideoGlass
    Startlog meldet "Video-Glas" (erst setzen, wenn Video-Glas unter DX11 funktioniert; sonst zeigt der Skin
    Live-Glas ueber laufendem Video, bevor es geht).

.PARAMETER NoCommit
    Aenderungen nur in den Arbeitsbaum, ohne Commit im Zweig glas.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\kodi-build\windows\apply-glas.ps1 -KodiDir D:\glas-win\kodi
#>
[CmdletBinding()]
param(
    [string]$KodiDir = (Join-Path $env:USERPROFILE 'glas-win\kodi'),
    [string]$ToolsDir = '',
    [switch]$NoLiveGlass,
    [switch]$VideoGlass,
    [switch]$NoCommit
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

# $PSScriptRoot ist in Windows PowerShell 5.1 in param-Vorgaben leer
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $ToolsDir) { $ToolsDir = Split-Path -Parent $ScriptDir }

# ---------------------------------------------------------------------------------------------------------------
# Hilfen
# ---------------------------------------------------------------------------------------------------------------

# Argumente immer als Array uebergeben: '--' und '-q' wuerden sonst von PowerShell als eigene Zeichen gelesen
function Invoke-Git {
    param([string[]]$GitArgs)
    & git -C $KodiDir @GitArgs
    if ($LASTEXITCODE -ne 0) { throw ('git ' + ($GitArgs -join ' ') + ' fehlgeschlagen (' + $LASTEXITCODE + ')') }
}

$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function ConvertTo-Lf {
    param([string]$Text)
    return $Text.Replace("`r`n", "`n")
}

function Read-Source {
    param([string]$Rel)
    $path = Join-Path $KodiDir $Rel
    if (-not (Test-Path -LiteralPath $path)) { throw "Datei fehlt: $Rel" }
    return ConvertTo-Lf ([System.IO.File]::ReadAllText($path))
}

function Write-Source {
    param([string]$Rel, [string]$Text)
    $path = Join-Path $KodiDir $Rel
    $dir = Split-Path -Parent $path
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
    [System.IO.File]::WriteAllText($path, $Text, $Utf8NoBom)
}

# Erste Fundstelle wie perl s/// ohne /g; mehrere Fundstellen nur als Warnung
function Find-Anchor {
    param([string]$Text, [string]$Anchor, [string]$What)
    $a = ConvertTo-Lf $Anchor
    $i = $Text.IndexOf($a, [System.StringComparison]::Ordinal)
    if ($i -lt 0) { throw "Anker fehlt ($What): $($a.Split("`n")[0])" }
    if ($Text.IndexOf($a, $i + 1, [System.StringComparison]::Ordinal) -ge 0) {
        Write-Warning "Anker mehrfach vorhanden ($What), erste Fundstelle genommen"
    }
    return $i
}

function Add-After {
    param([string]$Text, [string]$Anchor, [string]$Insert, [string]$What)
    $i = Find-Anchor $Text $Anchor $What
    $end = $i + (ConvertTo-Lf $Anchor).Length
    return $Text.Substring(0, $end) + (ConvertTo-Lf $Insert) + $Text.Substring($end)
}

function Add-Before {
    param([string]$Text, [string]$Anchor, [string]$Insert, [string]$What)
    $i = Find-Anchor $Text $Anchor $What
    return $Text.Substring(0, $i) + (ConvertTo-Lf $Insert) + $Text.Substring($i)
}

function Set-Replacement {
    param([string]$Text, [string]$Old, [string]$New, [string]$What)
    $i = Find-Anchor $Text $Old $What
    $len = (ConvertTo-Lf $Old).Length
    return $Text.Substring(0, $i) + (ConvertTo-Lf $New) + $Text.Substring($i + $len)
}

function Edit-Source {
    param([string]$Rel, [scriptblock]$Edit)
    $text = Read-Source $Rel
    $new = & $Edit $text
    if ($new -ceq $text) { throw "Keine Aenderung in $Rel" }
    Write-Source $Rel $new
    Write-Host "  gepatcht: $Rel"
}

function Get-Hash {
    param([string]$Rel)
    $path = Join-Path $KodiDir $Rel
    if (-not (Test-Path -LiteralPath $path)) { return '' }
    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
}

# ---------------------------------------------------------------------------------------------------------------
# Vorpruefung
# ---------------------------------------------------------------------------------------------------------------

if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'git nicht gefunden' }
if (-not (Test-Path -LiteralPath (Join-Path $KodiDir '.git'))) { throw "Kein Git-Repo: $KodiDir (erst prepare-source.ps1)" }
$SharedSrc = Join-Path $ToolsDir 'src'
$WinSrc = Join-Path $ScriptDir 'src'
if (-not (Test-Path -LiteralPath $SharedSrc)) { throw "Glas-Quellen fehlen: $SharedSrc" }

& git -C $KodiDir rev-parse -q --verify vanilla | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Zweig vanilla fehlt (prepare-source.ps1)' }

$DxImpl = Join-Path $WinSrc 'xbmc\guilib\GUIBackdropBlurControl.cpp'
$Live = (Test-Path -LiteralPath $DxImpl) -and (-not $NoLiveGlass)
if ($Live) { Write-Host 'Stufe: Live-Glas (DX11-Fassung gefunden)' } else { Write-Host 'Stufe: Durchstich ohne Live-Glas' }

& git -C $KodiDir rev-parse -q --verify glas | Out-Null
if ($LASTEXITCODE -ne 0) { Invoke-Git @('branch', 'glas', 'vanilla') }
Invoke-Git @('checkout', '-q', 'glas')

$G = 'xbmc/guilib'
$Patched = @(
    "$G/GUIControl.h", "$G/GUIControlFactory.cpp", "$G/GUIFontTTF.h", "$G/GUIFontTTF.cpp", "$G/CMakeLists.txt",
    "$G/GUIControl.cpp", "$G/GUITexture.h", "$G/GUITexture.cpp", "$G/GUIWindowManager.cpp",
    'xbmc/video/windows/GUIWindowFullScreen.cpp', 'xbmc/application/Application.cpp',
    "$G/GUIButtonControl.h", "$G/GUIButtonControl.cpp", "$G/GUISpinControlEx.h", "$G/GUISettingsSliderControl.h",
    "$G/GUIControlGroupList.h", "$G/GUIControlGroupList.cpp", "$G/GUIListGroup.cpp",
    'xbmc/video/ContextMenus.cpp'
)
$Copied = @("$G/GUIFont.cpp", "$G/GUIFont.h", "$G/GUIBackdropBlurControl.cpp", "$G/GUIBackdropBlurControl.h",
    "$G/GUIGlasButton.cpp", "$G/GUIGlasButton.h")

# Stand vor dem Lauf merken (Inhalt und Zeitstempel), fuer die Zeitstempel am Ende
$Before = @{}
foreach ($rel in ($Patched + $Copied)) {
    $path = Join-Path $KodiDir $rel
    if (Test-Path -LiteralPath $path) {
        $Before[$rel] = @{ Hash = (Get-Hash $rel); Time = (Get-Item -LiteralPath $path).LastWriteTimeUtc }
    }
}

# ---------------------------------------------------------------------------------------------------------------
# 1. Frischer Stand: gepatchte Dateien aus vanilla, eigene Dateien neu kopieren
# ---------------------------------------------------------------------------------------------------------------

Write-Host 'Dateien auf vanilla zuruecksetzen'
Invoke-Git (@('checkout', '-q', 'vanilla', '--') + $Patched + @("$G/GUIFont.cpp", "$G/GUIFont.h"))
foreach ($rel in @("$G/GUIBackdropBlurControl.cpp", "$G/GUIBackdropBlurControl.h", "$G/GUIGlasButton.cpp", "$G/GUIGlasButton.h")) {
    $path = Join-Path $KodiDir $rel
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
}

# Quellen: gemeinsame src/ (Vero), darueber windows/src/ (DX-Fassungen). GUIBackdropBlurControl.cpp nie aus der
# gemeinsamen src/ (GLES, uebersetzt unter MSVC nicht); ohne Live-Glas auch den Header nicht
$Sources = @{}
foreach ($dir in @($SharedSrc, $WinSrc)) {
    if (-not (Test-Path -LiteralPath $dir)) { continue }
    $root = (Resolve-Path -LiteralPath $dir).Path.TrimEnd('')
    Get-ChildItem -LiteralPath $root -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($root.Length + 1).Replace('\', '/')
        $Sources[$rel] = $_.FullName
    }
}
$Sources.Remove("$G/GUIBackdropBlurControl.cpp")
if ($Live) {
    $Sources["$G/GUIBackdropBlurControl.cpp"] = $DxImpl
} else {
    $Sources.Remove("$G/GUIBackdropBlurControl.h")
    $Sources.Remove("$G/GUIGlasButton.cpp")
    $Sources.Remove("$G/GUIGlasButton.h")
}
foreach ($rel in ($Sources.Keys | Sort-Object)) {
    Write-Source $rel (ConvertTo-Lf ([System.IO.File]::ReadAllText($Sources[$rel])))
    Write-Host "  kopiert:  $rel"
}

# ---------------------------------------------------------------------------------------------------------------
# 2. Gemeinsame Quellen sind seit Phase 1 portabel (unistd.h nur ausserhalb von Windows, access() dort aus io.h).
#    Messschalter "/tmp/glas-..." landen unter Windows in <Laufwerk>:\tmp\glas-... (Laufwerk des Arbeitsordners).
#    Nur pruefen, nichts mehr ersetzen.
# ---------------------------------------------------------------------------------------------------------------

$FontText = Read-Source "$G/GUIFont.cpp"
if ($FontText.Contains("#include <unistd.h>`n") -and -not $FontText.Contains("#if defined(TARGET_WINDOWS)`n#include <io.h>")) {
    throw 'GUIFont.cpp aus src/ bindet unistd.h ohne Windows-Weiche ein (Plan Phase 1)'
}

# ---------------------------------------------------------------------------------------------------------------
# 3. Plattformfrei (beide Stufen)
# ---------------------------------------------------------------------------------------------------------------

Write-Host 'Schrift in Zielgroesse (GUIFontTTF)'
Edit-Source "$G/GUIFontTTF.h" {
    param($t)
    Add-After $t "  float m_height{0.0f};`n" @'
  // Glas: Datei, Seitenverhaeltnis und Rand fuer verkleinerte Fassungen (CGUIFont::GlasScaledFont)
  std::string m_glasPath;
  float m_glasAspect{1.0f};
  bool m_glasBorder{false};

'@ 'GUIFontTTF.h m_height'
}
Edit-Source "$G/GUIFontTTF.cpp" {
    param($t)
    Add-Before $t "  m_face = g_freeTypeLibrary.GetFont(strFilename, height, aspect, m_fontFileInMemory);`n" @'
  m_glasPath = strFilename;
  m_glasAspect = aspect;
  m_glasBorder = border;

'@ 'GUIFontTTF.cpp GetFont'
}

Write-Host 'Stereo-Texturen (stereo="sbs")'
Edit-Source "$G/GUITexture.h" {
    param($t)
    Add-Before $t '  int        orientation;     // orientation' @'
  bool stereoSbs{false}; // Glas: stereo="sbs" - links Bild fuers linke, rechts fuers rechte Auge

'@ 'GUITexture.h orientation'
}

$OverdrawBlock = @'
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <string>
#include <vector>
#if defined(TARGET_WINDOWS)
#include <io.h>
#define GLAS_OVERDRAW_ACCESS(path) _access(path, 0)
#else
#include <unistd.h>
#define GLAS_OVERDRAW_ACCESS(path) access(path, F_OK)
#endif

#include "utils/log.h"

// Glas: Mess-Werkzeug Overdraw (siehe tools/kodi-build/overdraw.sh)
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
    const bool enabled = GLAS_OVERDRAW_ACCESS("/tmp/glas-overdraw") == 0;
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
  CLog::Log(LOGINFO, "GLAS_OVERDRAW: {:.2f} Bildschirmflaechen je Bild ({:.1f} MPix), {} Bilder", g_total / frames / screen,
            g_total / frames / 1e6, frames);
  for (size_t i = 0; i < top.size() && i < 15; i++)
  {
    std::string name = top[i].second;
    if (name.size() > 70)
      name = "..." + name.substr(name.size() - 67);
    CLog::Log(LOGINFO, "GLAS_OVERDRAW:   {:5.2f}  {}", top[i].first / frames / screen, name);
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
} // namespace GLAS_OVERDRAW

'@

Edit-Source "$G/GUITexture.cpp" {
    param($t)
    # Stereo-Textur: Breite halbieren, rechtes Auge liest die rechte Haelfte
    $t = Add-After $t "  m_frameHeight = (float)m_texture.m_height;`n" @'

  // Glas: Stereo-Textur (stereo="sbs"): je Auge eine Haelfte, Groesse und Seitenverhaeltnis wie eine Haelfte
  if (m_info.stereoSbs)
    m_frameWidth *= 0.5f;

'@ 'GUITexture.cpp m_frameHeight'
    $t = Add-After $t "  OrientateTexture(texture, u3, v3, orientation);`n" @'

  // Glas: Stereo-Textur - rechtes Auge liest die rechte Haelfte, sonst (auch 2D/MONO) die linke. Nur die
  // Bildkoordinaten verschieben: die Maske (diffuse) bleibt ueber der ganzen Flaeche
  if (m_info.stereoSbs &&
      CServiceBroker::GetWinSystem()->GetGfxContext().GetStereoView() == RENDER_STEREO_VIEW_RIGHT)
  {
    texture.x1 += u3;
    texture.x2 += u3;
  }

'@ 'GUITexture.cpp OrientateTexture'
    # Overdraw
    $t = Add-After $t "#include <stdexcept>`n" $OverdrawBlock 'GUITexture.cpp stdexcept'
    $t = Add-Before $t "  Draw(x, y, z, texture, diffuse, orientation);`n" @'
  GLAS_OVERDRAW::Add(m_info.filename, std::abs((x[2] - x[0]) * (y[2] - y[0])));

'@ 'GUITexture.cpp Draw'
    return $t
}

Write-Host 'Fabrik: Attribut stereo'
$FactoryRel = "$G/GUIControlFactory.cpp"
Edit-Source $FactoryRel {
    param($t)
    Add-After $t "    image.useLarge = true;`n" @'
  // Glas: stereo="sbs" - Side-by-Side-Bild, je Auge eine Haelfte (2D/MONO: linke)
  const char* stereo = pNode->Attribute("stereo");
  image.stereoSbs = stereo && StringUtils::EqualsNoCase(stereo, "sbs");

'@ 'GUIControlFactory useLarge'
}

$WmRel = "$G/GUIWindowManager.cpp"
Edit-Source $WmRel {
    param($t)
    $t = Add-After $t "void CGUIWindowManager::RenderPass() const`n{`n" "  GLAS_OVERDRAW::FrameBegin();`n" 'RenderPass'
    $t = Add-Before $t "void CGUIWindowManager::RenderPass() const`n{`n" "namespace GLAS_OVERDRAW`n{`nvoid FrameBegin();`n}`n`n" 'RenderPass ns'
    return $t
}

Write-Host 'Mausrad in Grouplists (vom Scroll-Ziel aus, mindestens 100 px je Raste)'
# Kodi rechnet die naechste Kindgrenze von der laufenden Scroll-Position aus: Rasten waehrend der Animation zeigten auf
# dasselbe Ziel und gingen verloren (Seitenleiste, Einstellungsseiten; 01.11.2026 gemessen: 60 Rasten = 6 Zeilen bei
# 300 ms). Jetzt vom Ziel der laufenden Animation aus wie bei Listen, und je Raste mindestens 100 px (Trennlinien und
# Ueberschriften der Einstellungen sind eigene Kinder von 24 bzw. 44 px)
Edit-Source "$G/GUIControlGroupList.h" {
    param($t)
    Add-After $t "  CScroller m_scroller;`n" "  float m_glasScrollTarget{0.0f}; // Glas: Ziel der laufenden Scroll-Animation (Mausrad)`n" 'GroupList Ziel'
}
Edit-Source "$G/GUIControlGroupList.cpp" {
    param($t)
    $t = Add-After $t "#include `"utils/StringUtils.h`"`n" "`n#include <algorithm>`n" 'GroupList include'
    $t = Set-Replacement $t "void CGUIControlGroupList::ScrollTo(float offset)`n{`n  m_scroller.ScrollTo(offset);`n" "void CGUIControlGroupList::ScrollTo(float offset)`n{`n  m_glasScrollTarget = offset;`n  m_scroller.ScrollTo(offset);`n" 'GroupList ScrollTo'
    $t = Set-Replacement $t @'
    // find the current control and move to the next or previous
    float offset = 0;
    for (ciControls it = m_children.begin(); it != m_children.end(); ++it)
    {
      CGUIControl *control = *it;
      if (!control->IsVisible()) continue;
      float nextOffset = offset + Size(control) + m_itemGap;
      if (event.m_id == ACTION_MOUSE_WHEEL_DOWN && nextOffset > m_scroller.GetValue() && m_scroller.GetValue() < m_totalSize - Size()) // past our current offset
      {
        ScrollTo(nextOffset);
        return EVENT_RESULT_HANDLED;
      }
      else if (event.m_id == ACTION_MOUSE_WHEEL_UP && nextOffset >= m_scroller.GetValue() && m_scroller.GetValue() > 0) // at least at our current offset
      {
        ScrollTo(offset);
        return EVENT_RESULT_HANDLED;
      }
      offset = nextOffset;
    }
'@ @'
    // Glas: vom Ziel einer laufenden Animation aus (sonst gingen Rasten verloren), mindestens 100 px je Raste
    const float maxOffset = std::max(0.0f, m_totalSize - Size());
    const float current = m_scroller.IsScrolling() ? std::min(std::max(m_glasScrollTarget, 0.0f), maxOffset) : m_scroller.GetValue();
    const float minStep = 100.0f;
    float offset = 0;
    float lastBefore = 0;
    for (ciControls it = m_children.begin(); it != m_children.end(); ++it)
    {
      CGUIControl *control = *it;
      if (!control->IsVisible()) continue;
      float nextOffset = offset + Size(control) + m_itemGap;
      if (event.m_id == ACTION_MOUSE_WHEEL_DOWN && current < maxOffset && nextOffset > current && nextOffset >= current + minStep)
      {
        ScrollTo(std::min(nextOffset, maxOffset));
        return EVENT_RESULT_HANDLED;
      }
      if (offset <= current - minStep)
        lastBefore = offset;
      offset = nextOffset;
    }
    if (event.m_id == ACTION_MOUSE_WHEEL_DOWN && current < maxOffset)
    {
      ScrollTo(maxOffset);
      return EVENT_RESULT_HANDLED;
    }
    if (event.m_id == ACTION_MOUSE_WHEEL_UP && current > 0)
    {
      ScrollTo(lastBefore);
      return EVENT_RESULT_HANDLED;
    }
'@ 'GroupList Mausrad'
    return $t
}

# Startlog: Faehigkeiten ehrlich melden (script.glas.helper/service.py core_build sucht diese Woerter)
if ($Live) {
    $Marker = 'Glas: eigener Kodi-Build (skin.glas), Backdrop-Blur-Spike'
    if ($VideoGlass) { $Marker += ', Video-Glas' }
    $Marker += ', Stereo-Texturen, Glas-Knoepfe, Glas-Breite, Windows-DX11'
} else {
    $Marker = 'Glas: Windows-Durchstich ohne Live-Glas (skin.glas)'
}
$AppRel = 'xbmc/application/Application.cpp'
Edit-Source $AppRel {
    param($t)
    Add-Before $t '  CLog::Log(LOGINFO, "Starting {} ({}). Platform' ("  CLog::Log(LOGINFO, `"" + $Marker + "`");`n") 'Startlog'
}

# ---------------------------------------------------------------------------------------------------------------
# 4. Nur mit Live-Glas: Control anmelden und Haken setzen
# ---------------------------------------------------------------------------------------------------------------

if ($Live) {
    Write-Host 'Control backdropblur anmelden'
    Edit-Source "$G/GUIControl.h" {
        param($t)
        Add-After $t "    GUICONTROL_VISUALISATION,`n" "    GUICONTROL_BACKDROPBLUR,`n" 'GUIControl.h Enum'
    }

    # Listen-Layouts pruefen die Typen ihrer Controls: ohne Eintrag warnt Kodi bei jedem Glas in einer Liste
    # ("Trying to add unsupported control type 37", fuegt es aber ein)
    Edit-Source "$G/GUIListGroup.cpp" {
        param($t)
        Add-Before $t "    CGUIControl::GUICONTROL_BORDEREDIMAGE,`n" "    CGUIControl::GUICONTROL_BACKDROPBLUR,`n" 'GUIListGroup supportedTypes'
    }
    # Kontextmenue "Inhalt anzeigen" (Browse into) nicht bei Disc-Abbildern: zeigt nur die rohe Disc-Struktur
    # (BDMV, VIDEO_TS, ANY!); abgespielt wird ueber Wiedergabe bzw. "Titel waehlen"
    Edit-Source 'xbmc/video/ContextMenus.cpp' {
        param($t)
        Add-After $t "bool CVideoBrowse::IsVisible(const CFileItem& item) const`n{`n" "  // Glas: Disc-Abbilder nicht oeffnen (rohe Disc-Struktur ergibt in der Oberflaeche keinen Sinn)`n  if (item.IsDiscImage() || URIUtils::IsDiscImage(item.GetDynPath()))`n    return false;`n" 'VideoBrowse IsVisible'
    }

    Edit-Source $FactoryRel {
        param($t)
        $t = Add-Before $t "#include `"GUIImage.h`"`n" "#include `"GUIBackdropBlurControl.h`"`n" 'Fabrik include'
        $t = Add-After $t "static const ControlMapping controls[] = {`n" "    {`"backdropblur`", CGUIControl::GUICONTROL_BACKDROPBLUR},`n" 'Fabrik controls[]'
        $t = Add-Before $t "    case CGUIControl::GUICONTROL_VISUALISATION:`n" @'
    case CGUIControl::GUICONTROL_BACKDROPBLUR:
    {
      CGUIBackdropBlurControl* blur = new CGUIBackdropBlurControl(parentID, id, posX, posY, width, height);
      float value;
      if (XMLUtils::GetFloat(pControlNode, "blurradius", value))
        blur->SetBlurRadius(value);
      if (XMLUtils::GetFloat(pControlNode, "cornerradius", value))
        blur->SetCornerRadius(value);
      if (XMLUtils::GetFloat(pControlNode, "saturation", value))
        blur->SetSaturation(value);
      GUIINFO::CGUIInfoColor tint;
      if (GetInfoColor(pControlNode, "tintcolor", tint, parentID))
        blur->SetTint(tint);
      std::string refresh;
      if (XMLUtils::GetString(pControlNode, "refresh", refresh))
        blur->SetAlwaysRefresh(refresh == "always");
      bool timing = false;
      if (XMLUtils::GetBoolean(pControlNode, "timing", timing))
        blur->SetTiming(timing);
      int grabScale = 4;
      if (XMLUtils::GetInt(pControlNode, "grabscale", grabScale))
        blur->SetGrabScale(grabScale);
      float updateRate = 15.0f;
      if (XMLUtils::GetFloat(pControlNode, "updaterate", updateRate))
        blur->SetUpdateRate(updateRate);
      bool grabProbe = false;
      if (XMLUtils::GetBoolean(pControlNode, "grabprobe", grabProbe))
        blur->SetGrabProbe(grabProbe);
      std::string glasStyle;
      if (XMLUtils::GetString(pControlNode, "style", glasStyle))
        blur->SetLiquid(glasStyle == "liquid");
      int glasSource = 0;
      if (XMLUtils::GetInt(pControlNode, "source", glasSource))
        blur->SetSource(glasSource);
      int glasFit = 0;
      if (XMLUtils::GetInt(pControlNode, "fitgrouplist", glasFit))
        blur->SetFitGroupList(glasFit);
      control = blur;
      break;
    }
'@ 'Fabrik case'
        return $t
    }

    Edit-Source "$G/CMakeLists.txt" {
        param($t)
        $t = Add-After $t "set(SOURCES DDSImage.cpp`n" "            GUIBackdropBlurControl.cpp`n" 'CMake SOURCES'
        $t = Add-After $t "set(HEADERS DDSImage.h`n" "            GUIBackdropBlurControl.h`n" 'CMake HEADERS'
        $t = Add-After $t "            GUIBackdropBlurControl.cpp`n" "            GUIGlasButton.cpp`n" 'CMake SOURCES Knopf'
        $t = Add-After $t "            GUIBackdropBlurControl.h`n" "            GUIGlasButton.h`n" 'CMake HEADERS Knopf'
        return $t
    }

    # <glass> an Knoepfen: Live-Glas unter der Platte (src/xbmc/guilib/GUIGlasButton.*), z. B. Einstellungszeilen,
    # die Kodi selbst aus den Vorgabe-Controls baut
    Write-Host 'Knoepfe mit <glass>'
    Edit-Source $FactoryRel {
        param($t)
        $t = Add-Before $t "#include `"GUIImage.h`"`n" "#include `"GUIGlasButton.h`"`n" 'Fabrik include Knopf'
        $t = Add-Before $t "  // things that apply to all controls`n  if (control)`n" "  CGlasButtonGlass::FromXml(control, pControlNode, parentID); // Glas: <glass> an Knoepfen`n`n" 'Fabrik glass'
        return $t
    }
    Edit-Source "$G/GUIButtonControl.h" {
        param($t)
        $t = Add-After $t "#include `"GUIControl.h`"`n" "#include `"GUIGlasButton.h`"`n" 'Knopf include'
        $t = Add-After $t "  CGUIButtonControl* Clone() const override { return new CGUIButtonControl(*this); }`n" "  CGlasButtonGlass& Glas() { return m_glas; } // Glas: <glass> (GUIGlasButton.h)`n" 'Knopf Glas()'
        $t = Add-After $t "  bool m_bSelected;`n" "  CGlasButtonGlass m_glas; // Glas: Live-Glas unter der Platte`n" 'Knopf m_glas'
        return $t
    }
    Edit-Source "$G/GUIButtonControl.cpp" {
        param($t)
        $t = Set-Replacement $t "    m_bSelected(control.m_bSelected)`n{`n" "    m_bSelected(control.m_bSelected),`n    m_glas(control.m_glas)`n{`n" 'Knopf Kopie'
        $t = Add-After $t "void CGUIButtonControl::Process(unsigned int currentTime, CDirtyRegionList &dirtyregions)`n{`n" @'
  // Glas: <glass> - Glas vor den Texturen verarbeiten (die Liste enthaelt bis hier nur, was dahinter liegt)
  if (m_glas.Process(*this, m_posX, m_posY, GetWidth(), m_height, currentTime, dirtyregions))
    MarkDirtyRegion();
'@ 'Knopf Process'
        $t = Set-Replacement $t "void CGUIButtonControl::Render()`n{`n" "void CGUIButtonControl::Render()`n{`n  m_glas.Render(); // Glas: unter den Texturen`n" 'Knopf Render'
        $t = Add-After $t "  m_imgNoFocus->FreeResources(immediately);`n" "  m_glas.FreeResources(immediately);`n" 'Knopf FreeResources'
        return $t
    }
    Edit-Source "$G/GUISpinControlEx.h" {
        param($t)
        Add-After $t "  CGUISpinControlEx* Clone() const override { return new CGUISpinControlEx(*this); }`n" "  CGUIButtonControl& GlasButton() { return m_buttonControl; } // Glas: <glass> (GUIGlasButton.h)`n" 'Drehfeld GlasButton'
    }
    Edit-Source "$G/GUISettingsSliderControl.h" {
        param($t)
        Add-After $t "  CGUISettingsSliderControl *Clone() const override { return new CGUISettingsSliderControl(*this); }`n" "  CGUIButtonControl& GlasButton() { return m_buttonControl; } // Glas: <glass> (GUIGlasButton.h)`n" 'Regler GlasButton'
    }

    Write-Host 'Aenderungen melden (CGUIControl::DoProcess)'
    Edit-Source "$G/GUIControl.cpp" {
        param($t)
        $head = "void CGUIControl::DoProcess(unsigned int currentTime, CDirtyRegionList &dirtyregions)`n{`n  CRect dirtyRegion = m_renderRegion;`n"
        $t = Add-Before $t $head @'
namespace GLAS_GUITARGET
{
bool Diag();
void NoteChange(const CGUIControl* control, const CRect& region, int why, unsigned int currentTime);
}

'@ 'DoProcess ns'
        $t = Add-After $t $head "  int glasWhy = ((m_controlDirtyState & DIRTY_STATE_CONTROL) ? 1 : 0) | ((m_bInvalidated && IsVisible()) ? 2 : 0);`n" 'DoProcess glasWhy'
        $t = Set-Replacement $t "  if (Animate(currentTime))`n    MarkDirtyRegion();`n" "  if (Animate(currentTime))`n  {`n    MarkDirtyRegion();`n    glasWhy |= 4;`n  }`n" 'DoProcess Animate'
        $t = Add-After $t "      dirtyRegion.Union(m_renderRegion);`n" "      glasWhy |= 8;`n" 'DoProcess Union'
        $t = Add-Before $t "  changed |= (m_controlDirtyState & DIRTY_STATE_CONTROL) != 0;`n" "  if (m_controlDirtyState & DIRTY_STATE_CONTROL)`n    glasWhy |= 16;`n" 'DoProcess changed'
        $t = Set-Replacement $t "  if (changed)`n  {`n    dirtyregions.emplace_back(dirtyRegion);`n" "  if (changed)`n  {`n    GLAS_GUITARGET::NoteChange(this, dirtyRegion, glasWhy, currentTime);`n    dirtyregions.emplace_back(dirtyRegion);`n" 'DoProcess emplace'
        return $t
    }

    Write-Host 'Haken in CApplication::Render'
    Edit-Source $AppRel {
        param($t)
        $t = Add-Before $t "void CApplication::Render()`n{`n" "namespace GLAS_GUITARGET`n{`nvoid Begin();`nvoid End(bool rendered);`n}`n`n" 'Render ns'
        $t = Add-After $t "  if (!CServiceBroker::GetRenderSystem()->BeginRender())`n    return;`n" "`n  GLAS_GUITARGET::Begin(); // Glas: Haken am Bildanfang (Messung; unter GLES eigenes Zwischenbild)`n" 'Render Begin'
        $t = Add-Before $t "`n  CServiceBroker::GetRenderSystem()->EndRender();`n`n  // reset our info cache" "`n  GLAS_GUITARGET::End(hasRendered);" 'Render End'
        return $t
    }

    Write-Host 'GUIWindowManager: Bereiche und Dialog-Markierung'
    Edit-Source $WmRel {
        param($t)
        $t = Add-Before $t "bool CGUIWindowManager::Render()`n{" "namespace GLAS_GUITARGET`n{`nbool Active();`nbool Regions(CDirtyRegionList& regions);`n}`n`n" 'WM Render ns'
        $t = Add-After $t "  CDirtyRegionList dirtyRegions = m_tracker.GetDirtyRegions();`n" @'
  // Glas: im eigenen Zwischenbild nur die markierten Bereiche (vereinigt) neu zeichnen
  CDirtyRegionList glasRegions = m_tracker.GetMarkedRegions();
  const bool glasPartial = GLAS_GUITARGET::Active() && GLAS_GUITARGET::Regions(glasRegions);

'@ 'WM dirtyRegions'
        $t = Set-Replacement $t "== DIRTYREGION_SOLVER_FILL_VIEWPORT_ON_CHANGE)`n" "== DIRTYREGION_SOLVER_FILL_VIEWPORT_ON_CHANGE && !glasPartial)`n" 'WM FILL_VIEWPORT'
        $t = Set-Replacement $t "  else`n  {`n    for (const auto& i : dirtyRegions)" "  else`n  {`n    for (const auto& i : (glasPartial ? glasRegions : dirtyRegions))" 'WM for'
        $t = Add-Before $t "void CGUIWindowManager::AfterRender()`n{" "namespace GLAS_GUITARGET`n{`nbool KeepDialogMark();`n}`n`n" 'WM AfterRender ns'
        $t = Set-Replacement $t "      if (pWindow && window->IsControlDirty())`n        pWindow->MarkDirtyRegion();`n" "      if (pWindow && window->IsControlDirty() && GLAS_GUITARGET::KeepDialogMark())`n        pWindow->MarkDirtyRegion();`n" 'WM AfterRender'
        return $t
    }

    Write-Host 'GUIWindowFullScreen: Vollbild-Markierung (Messschalter)'
    Edit-Source 'xbmc/video/windows/GUIWindowFullScreen.cpp' {
        param($t)
        $t = Add-Before $t "void CGUIWindowFullScreen::Process(unsigned int currentTime, CDirtyRegionList &dirtyregion)`n{" "namespace GLAS_GUITARGET`n{`nbool SkipFullscreenMark();`n}`n`n" 'FS ns'
        $t = Set-Replacement $t "  if (appPlayer->IsRenderingGuiLayer())`n    MarkDirtyRegion();`n" "  if (appPlayer->IsRenderingGuiLayer() && !GLAS_GUITARGET::SkipFullscreenMark())`n    MarkDirtyRegion();`n" 'FS Process'
        return $t
    }
}

# ---------------------------------------------------------------------------------------------------------------
# 5. Zeitstempel, Commit, Patch
# ---------------------------------------------------------------------------------------------------------------

foreach ($rel in $Before.Keys) {
    $path = Join-Path $KodiDir $rel
    if ((Test-Path -LiteralPath $path) -and ((Get-Hash $rel) -eq $Before[$rel].Hash)) {
        (Get-Item -LiteralPath $path).LastWriteTimeUtc = $Before[$rel].Time
    }
}

Write-Host ''
Write-Host 'GUIFont aus src/ gegen vanilla (aus dem OSMC-Baum kopiert - nur Glas-Aenderungen erwartet):'
& git -C $KodiDir diff --stat vanilla '--' "$G/GUIFont.cpp" "$G/GUIFont.h"

Invoke-Git @('add', '-A', 'xbmc')
& git -C $KodiDir diff --cached --stat
if ($NoCommit) {
    Write-Host 'Ohne Commit (-NoCommit).'
} else {
    & git -C $KodiDir diff --cached --quiet
    if ($LASTEXITCODE -eq 0) {
        Write-Host 'keine Aenderung'
    } else {
        $msg = 'Glas: Windows (DX11), ' + $(if ($Live) { 'Live-Glas' } else { 'Durchstich ohne Live-Glas' })
        Invoke-Git @('commit', '-q', '-m', $msg)
    }
    $PatchDir = Join-Path (Split-Path -Parent $KodiDir) 'patches'
    if (Test-Path -LiteralPath $PatchDir) { Remove-Item -LiteralPath $PatchDir -Recurse }
    Invoke-Git @('format-patch', '-q', 'vanilla..glas', '-o', $PatchDir)
    Write-Host "Patches: $PatchDir"
}
Write-Host "Startlog-Zeile: $Marker"
