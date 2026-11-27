# Quest (VR) builds on the report server (reports-worker/, BUILDS KV namespace).
#
#   python tools/vr_release.py publish "what changed"
#       build/android-vr/HeyYouPikachuVR.apk -> the VR update channel (/vr/latest, /vr/apk/<code>);
#       the VR app offers it on its next start. ROM-free builds only.
#
#   python tools/vr_release.py first-link
#       build/android-vr/HeyYouPikachuVR-personal.apk (built with HYP_VR_ROM=<rom>, a player's own ROM)
#       -> stored privately on the server, and prints a link that works for ONE download (7 days).
#   python tools/vr_release.py new-link       another one-time link for the same personal build
#   python tools/vr_release.py delete-first   remove the personal build from the server
#
# Never put a ROM build on GitHub or the update channel. Needs the Cloudflare login (wrangler).
import hashlib, json, os, re, secrets, subprocess, sys, urllib.error, urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
APK = os.path.join(ROOT, 'build', 'android-vr', 'HeyYouPikachuVR.apk')
PERSONAL = os.path.join(ROOT, 'build', 'android-vr', 'HeyYouPikachuVR-personal.apk')
MANIFEST = os.path.join(ROOT, 'android', 'AndroidManifest-vr.xml')
WORKER = os.path.join(ROOT, 'reports-worker')
SERVER = 'https://hyp-reports.pinball-leaderboard.workers.dev'
BLOB = 'vr-first-blob'
LINK_DAYS = 7


def wrangler(*args):
    npx = 'npx.cmd' if os.name == 'nt' else 'npx'
    done = subprocess.run([npx, 'wrangler', *args], cwd=WORKER, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if done.returncode != 0:
        sys.exit('wrangler %s failed:\n%s' % (args[:3], (done.stdout + done.stderr)[-1500:]))
    return done.stdout


def get(path):
    with urllib.request.urlopen(urllib.request.Request(SERVER + path, headers={'User-Agent': 'hyp-tools'}), timeout=60) as r:
        return r.read()


def worker_hash(text):
    # Same as hash() in reports-worker/src/index.js.
    return hashlib.sha256(('hyp-reports:' + text).encode()).hexdigest()[:24]


def publish(notes):
    manifest = open(MANIFEST, encoding='utf-8').read()
    code = int(re.search(r'android:versionCode="(\d+)"', manifest).group(1))
    name = re.search(r'android:versionName="([^"]+)"', manifest).group(1)
    data = open(APK, 'rb').read()
    latest = {'versionCode': code, 'versionName': name, 'sha256': hashlib.sha256(data).hexdigest(), 'size': len(data), 'notes': notes}
    print('publishing VR %s (code %d), %.1f MB' % (name, code, len(data) / 1e6))
    wrangler('kv', 'key', 'put', 'vr-apk:%d' % code, '--path', APK, '--binding', 'BUILDS', '--remote')
    tmp = os.path.join(ROOT, 'build', 'android-vr', 'vr-latest.json')
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(latest, f)
    wrangler('kv', 'key', 'put', 'vr-latest', '--path', tmp, '--binding', 'BUILDS', '--remote')
    seen = json.loads(get('/vr/latest'))
    ok = seen.get('versionCode') == code and hashlib.sha256(get('/vr/apk/%d' % code)).hexdigest() == latest['sha256']
    print('VR channel now offers %s; download %s' % (seen.get('versionName'), 'matches' if ok else 'DOES NOT MATCH'))
    if not ok:
        sys.exit(1)


def mint_link():
    token = secrets.token_urlsafe(24)
    wrangler('kv', 'key', 'put', 'vr-first-token:' + worker_hash(token), BLOB, '--ttl', str(LINK_DAYS * 86400),
             '--binding', 'BUILDS', '--remote')
    return SERVER + '/vr/first/' + token


def first_link():
    if not os.path.exists(PERSONAL):
        sys.exit('missing %s: HYP_VR=1 HYP_VR_ROM=<rom.z64> sh tools/package_android.sh' % PERSONAL)
    size = os.path.getsize(PERSONAL)
    if size > 25 * 2**20:
        sys.exit('personal APK is %.1f MB: over the 25 MiB KV limit' % (size / 1e6))
    print('storing the personal build privately (%.1f MB)' % (size / 1e6))
    wrangler('kv', 'key', 'put', BLOB, '--path', PERSONAL, '--binding', 'BUILDS', '--remote')
    print('one-time link (works once, for %d days):\n%s' % (LINK_DAYS, mint_link()))


def new_link():
    print('new one-time link (works once, for %d days):\n%s' % (LINK_DAYS, mint_link()))


def delete_first():
    wrangler('kv', 'key', 'delete', BLOB, '--binding', 'BUILDS', '--remote')
    print('personal build removed from the server (old links now say "gone")')


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else ''
    if cmd == 'publish':
        publish(' '.join(sys.argv[2:]).strip())
    elif cmd == 'first-link':
        first_link()
    elif cmd == 'new-link':
        new_link()
    elif cmd == 'delete-first':
        delete_first()
    else:
        sys.exit(__doc__ or 'usage: vr_release.py publish "notes" | first-link | new-link | delete-first')
