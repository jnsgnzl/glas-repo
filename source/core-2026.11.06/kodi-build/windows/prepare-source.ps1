<#
.SYNOPSIS
    ENTWURF, UNGETESTET (01.11.2026). Windows-Gegenstueck zu tools/kodi-build/prepare-source.sh:
    Kodi-Quelltext 21.3-Omega holen und als Git-Repo mit den Zweigen vanilla (und spaeter glas) anlegen.

.DESCRIPTION
    Unter Windows gibt es keine OSMC-Patches: Grundlage ist das unveraenderte Kodi 21.3 (Tag 21.3-Omega, Commit
    a3a448d - derselbe Commit, auf dem OSMCs Vero-Paket aufsetzt).

    Das Skript
      - klont den Tag flach (--depth 1) nach <WorkDir>\kodi, mit core.autocrlf=false (Dateien bleiben LF wie im
        Kodi-Repo, apply-glas.ps1 schreibt ebenfalls LF) und core.longpaths=true,
      - prueft den Commit,
      - legt den Zweig vanilla an (apply-glas.ps1 legt glas darauf an),
      - setzt einen lokalen Git-Benutzer fuer die Build-Commits (nur in diesem Repo),
      - nimmt den Build-Ordner kodi-build in .git/info/exclude auf.

    Es laedt NICHTS ausser dem Kodi-Quelltext. Abhaengigkeiten (download-dependencies.bat usw.) startet man danach
    selbst, siehe README.md in diesem Ordner.

.PARAMETER WorkDir
    Arbeitsordner (Quelltext, Build, Patches). Auf einem Laufwerk mit mindestens 30 GB frei.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\kodi-build\windows\prepare-source.ps1 -WorkDir D:\glas-win
#>
[CmdletBinding()]
param(
    [string]$WorkDir = (Join-Path $env:USERPROFILE 'glas-win'),
    [string]$Tag = '21.3-Omega',
    [string]$Commit = 'a3a448d26b8d560a65655dab2cd122994dc4e146',
    [string]$Remote = 'https://github.com/xbmc/xbmc.git'
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

function Invoke-Git {
    param([string[]]$GitArgs)
    & git @GitArgs
    if ($LASTEXITCODE -ne 0) { throw ('git ' + ($GitArgs -join ' ') + ' fehlgeschlagen (' + $LASTEXITCODE + ')') }
}

if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'git nicht gefunden (Git fuer Windows installieren)' }

$Kodi = Join-Path $WorkDir 'kodi'
if (-not (Test-Path -LiteralPath $WorkDir)) { New-Item -ItemType Directory -Path $WorkDir | Out-Null }

# freier Platz auf dem Ziellaufwerk (Quelltext ~1 GB, Abhaengigkeiten + MSYS2 + Build ~20 GB)
$drive = (Get-Item -LiteralPath $WorkDir).PSDrive
if ($drive -and $drive.Free) {
    $freeGb = [math]::Round($drive.Free / 1GB, 1)
    Write-Host "Frei auf $($drive.Name): $freeGb GB"
    if ($freeGb -lt 30) { Write-Warning 'Weniger als 30 GB frei - Build kann knapp werden' }
}

if (Test-Path -LiteralPath (Join-Path $Kodi '.git')) {
    Write-Host "Quelltext vorhanden: $Kodi (nichts geladen)"
} else {
    Write-Host "Klone $Remote ($Tag) nach $Kodi"
    Invoke-Git @('-c', 'core.autocrlf=false', '-c', 'core.longpaths=true', 'clone', '--depth', '1', '--branch', $Tag,
        $Remote, $Kodi)
    Invoke-Git @('-C', $Kodi, 'config', 'core.autocrlf', 'false')
    Invoke-Git @('-C', $Kodi, 'config', 'core.longpaths', 'true')
    Invoke-Git @('-C', $Kodi, 'config', 'user.email', 'build@glas')
    Invoke-Git @('-C', $Kodi, 'config', 'user.name', 'Glas Build')
    Invoke-Git @('-C', $Kodi, 'checkout', '-q', '-b', 'vanilla')
}

$head = (& git -C $Kodi rev-parse vanilla).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Zweig vanilla fehlt' }
if ($head -ne $Commit) {
    Write-Warning "vanilla steht auf $head, erwartet $Commit ($Tag). Passt der Tag noch zur Vero (OSMC)?"
} else {
    Write-Host "vanilla = $Tag ($head)"
}

# Build-Ordner im Quellbaum (BuildSetup.bat legt kodi-build an) nicht als Aenderung zaehlen
$exclude = Join-Path $Kodi '.git\info\exclude'
$lines = @()
if (Test-Path -LiteralPath $exclude) { $lines = @(Get-Content -LiteralPath $exclude) }
foreach ($entry in @('kodi-build/', 'project/BuildDependencies/')) {
    if ($lines -notcontains $entry) { Add-Content -LiteralPath $exclude -Value $entry -Encoding Ascii }
}

& git -C $Kodi log --oneline -1
Write-Host ''
Write-Host 'Naechste Schritte (README.md):'
Write-Host "  1. cd $Kodi\tools\buildsteps\windows\x64"
Write-Host '     download-dependencies.bat, download-msys2.bat, make-mingwlibs.bat'
Write-Host '  2. tools\kodi-build\windows\apply-glas.ps1 -KodiDir ' $Kodi
Write-Host '  3. cmake -G "Visual Studio 17 2022" -A x64 -T host=x64 ... ; cmake --build . --config Release'
