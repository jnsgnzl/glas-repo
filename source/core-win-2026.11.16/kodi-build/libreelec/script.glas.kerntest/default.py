# Glas Kerntest: Live-Glas-Kern auf LibreELEC ohne SSH bis zum naechsten Neustart einbinden (16.11.2026).
# Kodi laeuft unter LibreELEC als root. Stoppen, Einhaengen und Starten laufen in einer eigenen systemd-Einheit
# (systemd-run), sonst wuerde "systemctl stop kodi" dieses Skript gleich mit beenden.
# Logs: Im Live-Modus liegt /storage im Arbeitsspeicher, nach dem Ausschalten sind die Logs weg. Das Add-on
# sichert sie deshalb auf die Startpartition des Sticks (/flash, FAT, unter Windows lesbar) - auf Wunsch ueber
# das Menue und von selbst 45 Sekunden nach dem Einbinden (falls der Kern gar nicht erst hochkommt).
import os
import shutil
import subprocess

import xbmc
import xbmcaddon
import xbmcgui
import xbmcvfs

TITLE = 'Glas Kerntest'
TARGET = '/usr/lib/kodi/kodi.bin'
STORE = '/storage/glas/kodi.bin'
SAVER = '/storage/glas/sichern.sh'
LOG = '/storage/.kodi/temp/kodi.log'
# Weg ueber den Glas Helper vor einer Veroeffentlichung pruefen: Manifest und Kern liegen auf dem Stick
# (le-testpaket.py), der Helper liest die Adresse des Manifests aus seiner Datei core/manifest_url.
TEST_MANIFEST = '/flash/glas-test/manifest.json'
HELPER_CORE = 'special://masterprofile/addon_data/script.glas.helper/core/'

# Als Text im Skript statt als eigene Datei: so bleiben die Zeilenenden sicher LF, egal wie das ZIP gebaut wurde.
# GLAS_FLASH/GLAS_STORAGE gibt es nur fuer die Pruefung mit Attrappen.
SICHERN = r'''#!/bin/sh
# Glas Kerntest: Logs auf die Startpartition des Sticks sichern. Aufruf: sichern.sh [vorsilbe]
FLASH=${GLAS_FLASH:-/flash}
STORAGE=${GLAS_STORAGE:-/storage}
ZIEL=$FLASH/glas-logs/${1:-log}-$(date +%Y%m%d-%H%M%S)
mount -o remount,rw "$FLASH" || { echo "Startpartition $FLASH laesst sich nicht schreibbar einbinden"; exit 2; }
ok=1
mkdir -p "$ZIEL" || ok=0
if [ $ok = 1 ]; then
  for f in "$STORAGE"/.kodi/temp/kodi.log "$STORAGE"/.kodi/temp/kodi.old.log \
           "$STORAGE"/.kodi/temp/kodi_crashlog*.log "$STORAGE"/screenshots/*.png; do
    if [ -f "$f" ]; then cp "$f" "$ZIEL/" || ok=0; fi
  done
  journalctl -b --no-pager > "$ZIEL/journal.txt" 2>&1
  dmesg > "$ZIEL/dmesg.txt" 2>&1
  cat /proc/mounts > "$ZIEL/mounts.txt" 2>&1
  cat /etc/os-release > "$ZIEL/os-release.txt" 2>&1
  sha256sum /usr/lib/kodi/kodi.bin > "$ZIEL/kern.txt" 2>&1
  # Weg ueber den Glas Helper (glas-core-le.sh): liest LibreELEC das Drop-in, was steht im Zustand?
  systemctl cat kodi > "$ZIEL/kodi-unit.txt" 2>&1
  {
    ls -la "$STORAGE"/.config/glas-core "$STORAGE"/.config/system.d/kodi.service.d
    for f in build base starts off; do echo "== $f"; cat "$STORAGE/.config/glas-core/$f"; done
  } > "$ZIEL/helper-kern.txt" 2>&1
fi
sync
mount -o remount,ro "$FLASH"
if [ $ok != 1 ]; then echo "Kopieren nach $ZIEL fehlgeschlagen"; exit 3; fi
echo "$ZIEL"
'''


def is_libreelec():
    try:
        with open('/etc/os-release') as f:
            return 'libreelec' in f.read().lower()
    except OSError:
        return False


def mounted():
    try:
        with open('/proc/mounts') as f:
            return any(line.split()[1] == TARGET for line in f if len(line.split()) > 1)
    except OSError:
        return False


def startline():
    try:
        with open(LOG, errors='replace') as f:
            for line in f:
                if 'Glas: eigener Kodi-Build' in line:
                    return line.strip()
    except OSError:
        pass
    return ''


def write_saver():
    os.makedirs(os.path.dirname(SAVER), exist_ok=True)
    with open(SAVER, 'w', newline='\n') as f:
        f.write(SICHERN)


def save_logs(dialog):
    xbmc.log('Glas Kerntest: sichere Logs auf die Startpartition', xbmc.LOGINFO)
    try:
        write_saver()
        result = subprocess.run(['sh', SAVER, 'log'], capture_output=True, text=True, timeout=180)
    except (OSError, subprocess.SubprocessError) as err:
        dialog.ok(TITLE, 'Sichern fehlgeschlagen: %s' % err)
        return
    lines = [s for s in (result.stdout + '\n' + result.stderr).splitlines() if s.strip()]
    if result.returncode == 0 and lines:
        dialog.ok(TITLE, 'Logs gesichert auf der Startpartition des Sticks:[CR]%s[CR]'
                         'Unter Windows: Laufwerk LIBREELEC, Ordner glas-logs.' % lines[0])
    else:
        dialog.ok(TITLE, 'Sichern fehlgeschlagen (%d).[CR]%s' % (result.returncode, ' '.join(lines)[:300]))


def helper_test(dialog):
    if not os.path.exists(TEST_MANIFEST):
        dialog.ok(TITLE, 'Kein Test-Manifest auf dem Stick (glas-test/manifest.json).')
        return
    folder = xbmcvfs.translatePath(HELPER_CORE)
    try:
        os.makedirs(folder, exist_ok=True)
        with open(os.path.join(folder, 'manifest_url'), 'w') as f:
            f.write('file://' + TEST_MANIFEST)
    except OSError as err:
        dialog.ok(TITLE, 'Umstellen fehlgeschlagen: %s' % err)
        return
    xbmc.log('Glas Kerntest: Glas Helper liest das Manifest von %s' % TEST_MANIFEST, xbmc.LOGINFO)
    dialog.ok(TITLE, 'Der Glas Helper nimmt den Kern jetzt vom Stick.[CR]Weiter in den Glas-Einstellungen: '
                     'Live-Glas-Kern installieren (ohne ihn vorher hier einzubinden).[CR]'
                     'Danach hier die Logs sichern.')


def bind_core(dialog):
    src = os.path.join(xbmcvfs.translatePath(xbmcaddon.Addon().getAddonInfo('path')), 'kodi.bin')
    if not os.path.exists(src):
        dialog.ok(TITLE, 'kodi.bin fehlt im Add-on.')
        return
    if not dialog.yesno(TITLE, 'Live-Glas-Kern bis zum naechsten Neustart einbinden?[CR]Kodi startet dabei neu. '
                        'Danach dieses Add-on noch einmal oeffnen, es zeigt dann die Startzeile des Kerns.[CR]'
                        '45 Sekunden nach dem Neustart landen die Logs von selbst auf dem Stick.'):
        return
    runner = shutil.which('systemd-run')
    if not runner:
        dialog.ok(TITLE, 'systemd-run fehlt - Einbinden nicht moeglich.')
        return
    os.makedirs(os.path.dirname(STORE), exist_ok=True)
    shutil.copyfile(src, STORE)
    os.chmod(STORE, 0o755)
    write_saver()
    cmd = ('sleep 2; systemctl stop kodi; umount %s 2>/dev/null; mount --bind %s %s; systemctl start kodi; '
           'sleep 45; sh %s start' % (TARGET, STORE, TARGET, SAVER))
    xbmc.log('Glas Kerntest: binde %s ueber %s ein' % (STORE, TARGET), xbmc.LOGINFO)
    subprocess.Popen([runner, '--no-block', 'sh', '-c', cmd])


def main():
    dialog = xbmcgui.Dialog()
    if not is_libreelec():
        dialog.ok(TITLE, 'Nur fuer LibreELEC.')
        return
    line = startline()
    running = mounted() and bool(line)
    choice = dialog.select(TITLE, ['Startzeile des Kerns anzeigen' if running else 'Live-Glas-Kern einbinden',
                                   'Logs auf den Stick sichern',
                                   'Glas Helper testen: Kern vom Stick anbieten'])
    if choice == 1:
        save_logs(dialog)
    elif choice == 2:
        helper_test(dialog)
    elif choice == 0 and running:
        dialog.textviewer(TITLE, 'Der Live-Glas-Kern laeuft.\n\n' + line +
                          '\n\nZurueck zu LibreELECs Kodi: Rechner neu starten.')
    elif choice == 0:
        bind_core(dialog)


main()
