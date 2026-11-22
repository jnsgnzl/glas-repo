#!/usr/bin/env python3
"""Testpaket fuer einen LibreELEC-Kern auf die Startpartition des Sticks legen (vor einer Veroeffentlichung).

  python tools/kodi-build/libreelec/le-testpaket.py <kodi.bin> --id 2026.11.16 --le 12.2.1 \
      --base <LibreELECs eigenes kodi.bin> --out D:/ [--addons]

Schreibt nach <out>:
  script.glas.helper-<Version>.1-test.zip, skin.glas-<Version>.1-test.zip   nur mit --addons: Stand des Arbeitsbaums.
      Noetig, solange der veroeffentlichte Glas Helper den Weg noch nicht kennt (16.11.2026: Helper 0.13.0 aus dem
      Repository kannte LibreELEC nicht, die Glas-Einstellungen zeigten deshalb keinen Eintrag "Live-Glas-Kern").
  script.glas.kerntest-<Version>.zip   Add-on Glas Kerntest mit diesem Kern (Einbinden von Hand, Logs sichern)
  glas-test/kodi.bin.xz                derselbe Kern, gepackt wie im Release
  glas-test/manifest.json              Manifest mit genau diesem Build; die Adresse zeigt auf /flash/glas-test

Unter LibreELEC stellt der Menuepunkt "Glas Helper testen" des Add-ons den Glas Helper auf dieses Manifest um; danach
laeuft die Installation ueber die Glas-Einstellungen wie spaeter bei Testern, nur ohne Netz und ohne Release.
Der Eintrag im Manifest hat dieselben Felder wie publish_core_libreelec in tools/publish-repo.py.
"""
import argparse
import hashlib
import json
import lzma
import re
import shutil
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
ADDON = HERE / 'script.glas.kerntest'
LE_DEVICE = 'le-generic.x86_64'  # wie tools/publish-repo.py
FLASH = '/flash/glas-test'       # Startpartition des Sticks unter LibreELEC


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def pack_addon(addon_id, out):
    """Add-on aus dem Arbeitsbaum als ZIP, Version mit angehaengter .1: so ersetzt es beim Installieren aus der ZIP
    den veroeffentlichten Stand, und das Repository spielt den nicht wieder darueber; das naechste Release ist hoeher."""
    source = ROOT / addon_id
    xml = (source / 'addon.xml').read_text(encoding='utf-8')
    match = re.search(r'(<addon\b[^>]*?\bversion=")([^"]+)(")', xml)
    version = match.group(2) + '.1'
    xml = xml[:match.start(2)] + version + xml[match.end(2):]
    target = out / ('%s-%s-test.zip' % (addon_id, version))
    with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('%s/addon.xml' % addon_id, xml.replace('\r\n', '\n').encode('utf-8'))
        for path in sorted(source.rglob('*')):
            rel = path.relative_to(source).as_posix()
            if (not path.is_file() or rel == 'addon.xml' or '__pycache__' in path.parts
                    or path.name in ('Thumbs.db', 'desktop.ini', '.DS_Store')):
                continue
            z.write(path, '%s/%s' % (addon_id, rel))
    print('%s (%.1f MB)' % (target.name, target.stat().st_size / 1e6))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--addons', action='store_true',
                        help='auch Glas Helper und Glas aus dem Arbeitsbaum als Test-ZIPs dazulegen (der '
                             'veroeffentlichte Helper kennt einen neuen Weg evtl. noch nicht)')
    parser.add_argument('binary', type=Path)
    parser.add_argument('--id', required=True)
    parser.add_argument('--le', required=True, help='LibreELEC-Version (VERSION in /etc/os-release), z. B. 12.2.1')
    parser.add_argument('--base', type=Path, help='LibreELECs eigenes kodi.bin dieser Version (Pruefsumme)')
    parser.add_argument('--out', type=Path, required=True, help='Startpartition des Sticks, z. B. D:/')
    args = parser.parse_args()

    test = args.out / 'glas-test'
    test.mkdir(parents=True, exist_ok=True)
    packed = test / 'kodi.bin.xz'
    with open(args.binary, 'rb') as src, lzma.open(packed, 'wb', preset=9) as dst:
        shutil.copyfileobj(src, dst, 1 << 20)
    entry = {
        'id': args.id, 'device': LE_DEVICE, 'libreelec': args.le, 'kodi': '21.3',
        'url': 'file://%s/kodi.bin.xz' % FLASH,
        'sha256': sha256(args.binary), 'size': args.binary.stat().st_size, 'xz_size': packed.stat().st_size,
    }
    if args.base:
        entry['kodi_sha256'] = sha256(args.base)
    (test / 'manifest.json').write_text(json.dumps({'builds': [entry]}, indent=2) + '\n', encoding='utf-8',
                                        newline='\n')

    version = re.search(r'<addon[^>]*\bversion="([^"]+)"', (ADDON / 'addon.xml').read_text(encoding='utf-8')).group(1)
    for old in args.out.glob('script.glas.kerntest-*.zip'):
        if old.name != 'script.glas.kerntest-%s.zip' % version:
            print('Aelteres Add-on liegt noch dort: %s' % old)
    target = args.out / ('script.glas.kerntest-%s.zip' % version)
    with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as z:
        for name in ('addon.xml', 'default.py'):
            # Zeilenenden LF, egal wie der Arbeitsbaum ausgecheckt ist
            z.writestr('script.glas.kerntest/' + name, (ADDON / name).read_bytes().replace(b'\r\n', b'\n'))
        z.write(args.binary, 'script.glas.kerntest/kodi.bin')
    print('%s: Kern %s (%.1f MB, gepackt %.1f MB), LibreELEC %s%s' % (
        target.name, entry['sha256'], entry['size'] / 1e6, entry['xz_size'] / 1e6, args.le,
        ', passt zu kodi.bin %s' % entry['kodi_sha256'] if args.base else ''))
    print('Manifest: %s' % (test / 'manifest.json'))
    if args.addons:
        for addon_id in ('script.glas.helper', 'skin.glas'):
            pack_addon(addon_id, args.out)


if __name__ == '__main__':
    main()
