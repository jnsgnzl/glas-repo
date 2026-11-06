/*
 *  Glas (skin.glas): Blu-ray-Abbild (ISO) direkt mit dem Hauptfilm starten, den die NFO des Films nennt.
 *  Nur in GUIDialogSimpleMenu.cpp eingebunden (apply-glas.sh / apply-glas.ps1).
 *
 *  Kodi zeigt vor einer ISO die Titelliste ("vereinfachtes Menue", disc.playback 0) oder nimmt die laengste Playlist
 *  ("Hauptfilm abspielen", 2). Bei vielen Discs ist die laengste Playlist aber nicht der Film (Schleifen, "Alles
 *  abspielen" der Extras, Attrappen; gemessen: 127 von 298 ISOs). Steht in der NFO neben der ISO
 *      <mainplaylist>BDMV/PLAYLIST/00800.mpls</mainplaylist>
 *  (schreibt z. B. Kodex, geht auch von Hand), waehlt Glas diese Playlist so, wie es die Auswahl in der Titelliste
 *  taete (Pfad bluray://.../xxxxx.mpls; Mediathek, Fortsetzen und Gesehen bleiben am Eintrag).
 *  NFO wie bei Kodi: <Name>.nfo neben der Datei, sonst movie.nfo im selben Ordner.
 *  Wie bisher (Titelliste bzw. Kodis Einstellung) ohne Tag, bei "Disc-Menue anzeigen" (disc.playback 1) und wenn die
 *  Playlist auf dem Abbild fehlt. Andere Titel: Kontextmenue "Abspielen von ..." (laeuft nicht hierueber).
 */
#pragma once

#include "FileItem.h"
#include "ServiceBroker.h"
#include "URL.h"
#include "filesystem/File.h"
#include "settings/DiscSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/FileUtils.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/log.h"

#include <string>
#include <vector>

namespace GLAS_MAINPLAYLIST
{
constexpr const char* TAG_OPEN = "<mainplaylist>";
constexpr const char* TAG_CLOSE = "</mainplaylist>";

//! Inhalt von <mainplaylist> in der Datei, nur in der Form BDMV/PLAYLIST/nnnnn.mpls; leer sonst.
inline std::string FromNfo(const std::string& nfo)
{
  std::vector<uint8_t> buffer;
  XFILE::CFile file;
  if (!CFileUtils::Exists(nfo) || file.LoadFile(nfo, buffer) <= 0)
    return "";
  const std::string text(buffer.begin(), buffer.end());
  const size_t open = text.find(TAG_OPEN);
  if (open == std::string::npos)
    return "";
  const size_t start = open + std::char_traits<char>::length(TAG_OPEN);
  const size_t close = text.find(TAG_CLOSE, start);
  if (close == std::string::npos)
    return "";
  std::string playlist = text.substr(start, close - start);
  StringUtils::Trim(playlist);
  StringUtils::Replace(playlist, '\\', '/');
  if (playlist.size() != 24 || !StringUtils::StartsWithNoCase(playlist, "BDMV/PLAYLIST/") ||
      !StringUtils::EndsWithNoCase(playlist, ".mpls") || !StringUtils::IsNaturalNumber(playlist.substr(14, 5)))
    return "";
  return "BDMV/PLAYLIST/" + playlist.substr(14, 5) + ".mpls";
}

//! Hauptfilm aus der NFO der Datei (<Name>.nfo, sonst movie.nfo); leer ohne Angabe.
inline std::string Lookup(const std::string& path)
{
  std::string playlist = FromNfo(URIUtils::ReplaceExtension(path, ".nfo"));
  if (playlist.empty())
    playlist = FromNfo(URIUtils::AddFileToFolder(URIUtils::GetDirectory(path), "movie.nfo"));
  return playlist;
}

//! true = Hauptfilm gewaehlt (Titelliste nicht zeigen), false = Kodi wie bisher.
inline bool Apply(CFileItem& item)
{
  if (!item.IsDiscImage())
    return false;
  const auto settings = CServiceBroker::GetSettingsComponent();
  if (!settings || !settings->GetSettings() ||
      settings->GetSettings()->GetInt(CSettings::SETTING_DISC_PLAYBACK) == BD_PLAYBACK_DISC_MENU)
    return false;
  const std::string original = item.GetDynPath();
  const std::string playlist = Lookup(original);
  if (playlist.empty())
    return false;
  CURL udf("udf://");
  udf.SetHostName(original);
  udf.SetFileName(playlist);
  if (!CFileUtils::Exists(udf.Get()))
  {
    CLog::Log(LOGWARNING, "Glas: Hauptfilm {} aus der NFO fehlt auf {}", playlist, CURL::GetRedacted(original));
    return false;
  }
  udf.SetFileName("");
  CURL bluray("bluray://");
  bluray.SetHostName(udf.Get());
  bluray.SetFileName(playlist);
  // wie die Auswahl in der Titelliste (CGUIDialogSimpleMenu::ShowPlaySelection)
  item.SetDynPath(bluray.Get());
  item.SetProperty("get_stream_details_from_player", true);
  item.SetProperty("original_listitem_url", original);
  CLog::Log(LOGINFO, "Glas: Hauptfilm {} aus der NFO ({})", playlist, CURL::GetRedacted(original));
  return true;
}
} // namespace GLAS_MAINPLAYLIST
