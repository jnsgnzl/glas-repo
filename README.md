# Glas – Kodi-Repository

Kodi-21-Skin im Stil von tvOS (Apple TV), mit Glas Helper und – nur auf der Vero V – einem eigenen Kodi-Build für
echtes Live-Glas. Dieses Repository enthält nur die fertigen Pakete und den Quelltext des Kodi-Builds.

## Installieren

Voraussetzung: Kodi 21 „Omega“ (OSMC aktuell, LibreELEC 12, Windows …) mit Internetzugang.

1. **Unbekannte Quellen erlauben:** Einstellungen › System › Add-ons › „Unbekannte Quellen“ einschalten.
2. **Quelle hinzufügen:** Einstellungen › Dateimanager › Quelle hinzufügen › `<Keine>` › Adresse
   `https://jnsgnzl.github.io/glas-repo/` eingeben, Name z. B. `glas`.
3. **Repository installieren:** Einstellungen › Add-ons › Aus ZIP-Datei installieren › `glas` ›
   `repository.glas-….zip`.
4. **Glas installieren:** Add-ons › Aus Repository installieren › Glas Repository › Skins › Glas › Installieren.
   Kodi holt dabei den Glas Helper und dessen Abhängigkeiten (TMDb Helper, Aerial, Up Next, Apple-TV-Klänge) aus dem
   offiziellen Kodi-Repository mit. Am Ende „Skin verwenden?“ und „Änderung beibehalten?“ mit Ja bestätigen.
5. **Kodi einmal neu starten.** Beim ersten Start richtet Glas die Bedienung ein (Home lang drücken =
   Kontrollzentrum) und fragt, ob Kodi Add-on-Updates nur anzeigen soll. Empfohlen: Ja – dann bestätigst du jedes
   Update selbst unter Add-ons › Updates.

Updates kommen danach wie bei jedem anderen Add-on.

**Glas schon per ZIP installiert?** Schritt 1 bis 4 trotzdem ausführen. Kodi fragt bei Schritt 4, ob die manuell
installierte Version ersetzt werden soll → Ja, danach ob auch alle Daten gelöscht werden sollen → **Nein** (sonst sind
die Glas-Einstellungen weg). Erst danach bietet Kodi Updates aus diesem Repository an.

## Live-Glas-Kern (nur Vero V)

Echtes Live-Glas (auch über laufenden Filmen und in 3D) braucht einen eigenen Kodi-Build. Auf einer Vero V erscheint
dafür in den Glas-Einstellungen (Einstellungen › Oberfläche › Skin › Konfigurieren) die Zeile **Live-Glas-Kern**:

- **Installieren** lädt den Build, der genau zu deiner Version von OSMCs Kodi passt, prüft ihn, sichert OSMCs
  Original und sperrt OSMCs Kodi-Paket für Updates (`apt-mark hold vero5-mediacenter-osmc`), damit ein OSMC-Update
  den Build nicht überschreibt. Andere Systemupdates laufen normal weiter. Kodi startet danach neu.
- **Notfall-Rückweg:** Startet der Build dreimal hintereinander nicht (endet jeweils in der ersten Minute), startet
  ab dann von selbst wieder OSMCs Kodi, und Glas meldet das.
- **Entfernen** stellt OSMCs Kodi wieder her und hebt die Sperre auf.
- Gibt es für deine OSMC-Version noch keinen Build, sagt Glas das, statt etwas zu installieren.
- Danach unter Glas-Einstellungen „Live-Glas“ einschalten.

Von Hand per SSH zurück, falls einmal gar nichts mehr geht:

```sh
sudo systemctl stop mediacenter
sudo cp -a /usr/lib/kodi/kodi.bin.osmc /usr/lib/kodi/kodi.bin
sudo apt-mark unhold vero5-mediacenter-osmc
sudo systemctl start mediacenter
```

## Zurück zum alten Aussehen

Einstellungen › Oberfläche › Skin › Estuary.

## Lizenzen und Quelltext

- Skin: Umbau von Estuary (CC BY-SA 4.0 / GPL 2.0), Schrift Inter (OFL), Symbole Phosphor (MIT).
- Glas Helper: MIT.
- Live-Glas-Kern: Kodi (GPL 2.0) mit OSMCs Patches und den Glas-Änderungen. Der Quelltext zu jedem Build liegt in
  [`source/`](source/), die Builds selbst unter [Releases](https://github.com/jnsgnzl/glas-repo/releases).
- Apple-Klänge und Bildschirmschoner kommen aus dem offiziellen Kodi-Repository und sind nicht Teil von Glas.

Glas ist ein privates Hobbyprojekt ohne Gewähr. Fragen und Rückmeldungen gern im Forum-Thread.
