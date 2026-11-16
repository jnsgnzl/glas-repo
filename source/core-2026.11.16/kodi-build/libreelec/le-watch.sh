#!/bin/bash
# Glas: Ausloeser fuer le-build.sh ohne SSH. Laeuft als geplante Aufgabe im DSM-Aufgabenplaner (Benutzer root, alle
# 5 Minuten):  for d in /volume*/glas-build; do [ -f "$d/le-watch.sh" ] && bash "$d/le-watch.sh"; done
# Startet einen Bau, wenn im Ordner start.txt liegt (vom PC aus per SMB anlegen). Erlaubte Zeilen in start.txt:
# LE_TAG=, PROJECT=, DEVICE=, ARCH=, JOBS=, LTO= (alles andere wird ignoriert).
# Rueckmeldung: status.txt (wartet/laeuft/fertig + Exit-Code), bau.log.
ROOT=$(cd "$(dirname "$0")" && pwd)
cd "$ROOT" || exit 0
# Aufgabenplaner startet mit knappem PATH; docker liegt unter /usr/local/bin
export PATH=/usr/local/bin:/usr/local/sbin:/usr/bin:/usr/sbin:/bin:/sbin:$PATH
[ -f start.txt ] || exit 0
mkdir .lauf 2>/dev/null || exit 0   # laeuft schon
trap 'rmdir "$ROOT/.lauf"' EXIT

# nur bekannte Schluessel mit harmlosen Werten uebernehmen (start.txt kommt ueber die Freigabe)
while IFS='=' read -r key value; do
    value=${value%$'\r'}
    case "$key" in
        LE_TAG|PROJECT|DEVICE|ARCH|JOBS|LTO)
            [[ "$value" =~ ^[A-Za-z0-9._-]+$ ]] && export "$key=$value" ;;
    esac
done < start.txt
rm -f start.txt

owner=$(stat -c %u:%g le-build.sh)
echo "laeuft seit $(date '+%F %T')" > status.txt
bash le-build.sh > bau.log 2>&1
rc=$?
echo "fertig $(date '+%F %T'), Exit $rc" > status.txt
chown "$owner" status.txt bau.log 2>/dev/null
[ -d out ] && chown -R "$owner" out 2>/dev/null
exit 0
