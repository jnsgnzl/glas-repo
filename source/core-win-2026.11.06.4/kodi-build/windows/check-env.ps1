<#
.SYNOPSIS
    Prueft ohne Installation und ohne Registry, ob die Werkzeuge fuer den Windows-Build von Kodi 21 da sind.
    Aendert nichts, laedt nichts.

.DESCRIPTION
    Anforderungen laut Kodi docs/README.Windows.md (Tag 21.3-Omega): Visual Studio 2022 mit Workload
    "Desktopentwicklung mit C++" (MSVC, Windows SDK, fxc), CMake >= 3.21, Git, Java-Laufzeit im PATH, NSIS nur fuer
    den Installer. CMake 4.x ist fuer Kodi 21 nicht erprobt (entfernte Kompatibilitaet < 3.5).

.PARAMETER WorkDir
    geplanter Arbeitsordner (fuer die Pruefung des freien Platzes).
#>
[CmdletBinding()]
param(
    [string]$WorkDir = (Join-Path $env:USERPROFILE 'glas-win')
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Continue'

$results = New-Object System.Collections.Generic.List[object]
function Add-Result {
    param([string]$Name, [string]$State, [string]$Detail)
    $results.Add([pscustomobject]@{ Werkzeug = $Name; Stand = $State; Detail = $Detail })
}

function Get-ToolPath {
    param([string]$Name)
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cmd) { return $cmd.Source }
    return $null
}

# Visual Studio ueber vswhere (Standardort des Installers)
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath = $null
if (Test-Path -LiteralPath $vswhere) {
    $all = & $vswhere -products '*' -format value -property installationPath
    $cpp = & $vswhere -products '*' -version '[17.0,18.0)' -requires 'Microsoft.VisualStudio.Workload.NativeDesktop' -format value -property installationPath
    $ver = & $vswhere -products '*' -version '[17.0,18.0)' -format value -property catalog_productDisplayVersion
    if ($cpp) {
        $vsPath = @($cpp)[0]
        Add-Result 'Visual Studio 2022' 'ok' "$vsPath ($(@($ver)[0]))"
    } elseif ($all) {
        Add-Result 'Visual Studio 2022' 'FEHLT TEIL' "$(@($all)[0]) ($(@($ver)[0])) ohne Workload 'Desktopentwicklung mit C++'"
    } else {
        Add-Result 'Visual Studio 2022' 'FEHLT' 'keine Installation gefunden'
    }
} else {
    Add-Result 'Visual Studio 2022' 'FEHLT' 'vswhere.exe nicht am Standardort'
}

if ($vsPath) {
    $msvc = Join-Path $vsPath 'VC\Tools\MSVC'
    if (Test-Path -LiteralPath $msvc) {
        $v = (Get-ChildItem -LiteralPath $msvc -Directory | Sort-Object Name | Select-Object -Last 1).Name
        Add-Result 'MSVC (v143)' 'ok' $v
    } else {
        Add-Result 'MSVC (v143)' 'FEHLT' $msvc
    }
    $vsCmake = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (Test-Path -LiteralPath $vsCmake) { Add-Result 'CMake aus VS' 'ok' $vsCmake }
}

# Windows SDK mit fxc.exe (HLSL-Compiler fuer Kodis GUI-Shader)
$kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
$fxc = $null
if (Test-Path -LiteralPath $kits) {
    $fxc = Get-ChildItem -LiteralPath $kits -Recurse -Filter fxc.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -like '*\x64\*' } | Sort-Object FullName | Select-Object -Last 1
}
if ($fxc) { Add-Result 'Windows SDK (fxc)' 'ok' $fxc.FullName } else { Add-Result 'Windows SDK (fxc)' 'FEHLT' 'kommt mit dem C++-Workload' }

# CMake im PATH
$cmake = Get-ToolPath 'cmake'
if ($cmake) {
    $line = (& $cmake --version | Select-Object -First 1)
    $state = 'ok'
    $detail = "$line ($cmake)"
    if ($line -match '(\d+)\.(\d+)') {
        $major = [int]$Matches[1]; $minor = [int]$Matches[2]
        if ($major -ge 4) { $state = 'WARNUNG'; $detail += ' - CMake 4 fuer Kodi 21 nicht erprobt, besser 3.31.x' }
        elseif ($major -lt 3 -or ($major -eq 3 -and $minor -lt 21)) { $state = 'ZU ALT'; $detail += ' - mindestens 3.21' }
    }
    Add-Result 'CMake (PATH)' $state $detail
} else {
    Add-Result 'CMake (PATH)' 'FEHLT' 'mindestens 3.21'
}

$git = Get-ToolPath 'git'
if ($git) { Add-Result 'Git' 'ok' ((& $git --version) + " ($git)") } else { Add-Result 'Git' 'FEHLT' '' }

$java = Get-ToolPath 'java'
if ($java) {
    # java -version schreibt nach stderr; ueber cmd umleiten, damit PowerShell 5.1 keinen Fehler meldet
    $jv = (cmd /c "`"$java`" -version 2>&1" | Select-Object -First 1)
    Add-Result 'Java (Laufzeit)' 'ok' "$jv ($java)"
} else {
    Add-Result 'Java (Laufzeit)' 'FEHLT' 'JRE/JDK, bin-Ordner im PATH'
}

$nsis = Join-Path ${env:ProgramFiles(x86)} 'NSIS\makensis.exe'
if (Test-Path -LiteralPath $nsis) { Add-Result 'NSIS' 'ok' $nsis } else { Add-Result 'NSIS' 'optional' 'nur fuer den Installer' }

foreach ($tool in @('python', 'nasm', 'xz', 'gh')) {
    $p = Get-ToolPath $tool
    if ($p) { Add-Result $tool 'vorhanden' $p } else { Add-Result $tool 'optional' 'fuer Kodi selbst nicht noetig' }
}

# MinGW-Werkzeuge im PATH koennen CMake verwirren (gcc, ninja, cmake aus WinLibs)
$gcc = Get-ToolPath 'gcc'
if ($gcc) { Add-Result 'gcc im PATH' 'WARNUNG' "$gcc - fuer den Kodi-Build aus dem PATH nehmen (Developer PowerShell)" }

# freier Platz
$target = $WorkDir
while ($target -and -not (Test-Path -LiteralPath $target)) { $target = Split-Path -Parent $target }
if ($target) {
    $drive = (Get-Item -LiteralPath $target).PSDrive
    $freeGb = [math]::Round($drive.Free / 1GB, 1)
    $state = 'ok'
    if ($freeGb -lt 30) { $state = 'KNAPP' }
    Add-Result 'Platz' $state "$($drive.Name): $freeGb GB frei (Ziel $WorkDir, Bedarf ~25-35 GB)"
}

$results | Format-Table -AutoSize -Wrap
