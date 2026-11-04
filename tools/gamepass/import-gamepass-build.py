"""Add the Game Pass column to profiles/motor-builds.json.

Inputs:
  --mapping  JSON list from tools/gamepass/map-gamepass-build.ps1: {gog, steam, gamepass, code, method, votes}
  --image    the Game Pass executable image laid out by virtual address (tools/gamepass/dump-motor-image.ps1)
Every catalogued address gets "gamepass" (null when unmapped). Code addresses also get
"gamepassBytes": 32 bytes at that address in the image, the fingerprint build::matches() checks.
The Game Pass profile itself (identity, package) is written from the command line values.
"""
import argparse, json
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument('--mapping', required=True)
ap.add_argument('--image', required=True)
ap.add_argument('--code-image-sha256', required=True)
ap.add_argument('--timestamp', required=True)
ap.add_argument('--image-size', required=True)
ap.add_argument('--package-version', required=True)
args = ap.parse_args()

root = Path(__file__).resolve().parent.parent.parent
catalog_path = root / 'profiles/motor-builds.json'
catalog = json.loads(catalog_path.read_text(encoding='utf-8'))
image = Path(args.image).read_bytes()
mapping = {int(m['gog'], 16): m for m in json.loads(Path(args.mapping).read_text(encoding='utf-8-sig'))}

profile = {
    'id': 'GamePass170', 'name': 'Game Pass 1.0.17.0 (package %s)' % args.package_version, 'store': 'gamepass',
    'version': '1,0,17,0', 'identity': 'code-image',
    'packageName': 'BethesdaSoftworks.ProjectRelic', 'packageFamilyName': 'BethesdaSoftworks.ProjectRelic_3275kfvn8vcwc',
    'packageVersion': args.package_version, 'appId': 'Game',
    'sha256': args.code_image_sha256.lower(), 'timestamp': args.timestamp, 'imageSize': args.image_size,
}
catalog['profiles'] = [p for p in catalog['profiles'] if p['id'] != 'GamePass170'] + [profile]

image_size = int(args.image_size, 16)
seen, mapped, code_bytes = {}, 0, 0
for key, entry in catalog['addresses'].items():
    m = mapping.get(int(key, 16))
    if m and int(m['steam'], 16) != int(entry['steam'], 16):
        raise SystemExit('mapping disagrees with catalog Steam address for %s' % key)
    entry.pop('gamepass', None); entry.pop('gamepassBytes', None)
    if not m:
        entry['gamepass'] = None
        if entry['code']:
            raise SystemExit('unmapped code address %s: every hook site needs a Game Pass address' % key)
        continue
    at = int(m['gamepass'], 16)
    if not 0x1000 <= at < image_size:
        raise SystemExit('out-of-image Game Pass address %s for %s' % (m['gamepass'], key))
    if at in seen:
        raise SystemExit('Game Pass address %s mapped twice (%s, %s)' % (m['gamepass'], seen[at], key))
    seen[at] = key
    entry['gamepass'] = '0x%x' % at
    mapped += 1
    if entry['code']:
        entry['gamepassBytes'] = image[at:at + 32].hex()
        code_bytes += 1

catalog_path.write_bytes((json.dumps(catalog, indent=2) + '\n').encode('utf-8'))
print('Game Pass column: %d of %d addresses mapped, %d code fingerprints' % (mapped, len(catalog['addresses']), code_bytes))
