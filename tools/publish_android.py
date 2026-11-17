# Publishes build/android/HeyYouPikachu.apk as the latest Android build: the app's update check
# (LauncherActivity) then offers it on the phone's next start.
#
#   python tools/publish_android.py "what changed, in a sentence or two"
#
# Uploads the APK and a small "latest" record (version from android/AndroidManifest.xml,
# SHA-256, size, notes) to the BUILDS KV namespace of the reports Worker (reports-worker/),
# served at https://hyp-reports.pinball-leaderboard.workers.dev/latest and /apk/<versionCode>.
import hashlib, json, os, re, subprocess, sys, urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
APK = os.path.join(ROOT, 'build', 'android', 'HeyYouPikachu.apk')
WORKER = os.path.join(ROOT, 'reports-worker')
SERVER = 'https://hyp-reports.pinball-leaderboard.workers.dev'


def wrangler(*args):
    npx = 'npx.cmd' if os.name == 'nt' else 'npx'
    done = subprocess.run([npx, 'wrangler', *args], cwd=WORKER, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if done.returncode != 0:
        sys.exit('wrangler %s failed:\n%s' % (args[:3], (done.stdout + done.stderr)[-1500:]))


def main():
    notes = ' '.join(sys.argv[1:]).strip()
    manifest = open(os.path.join(ROOT, 'android', 'AndroidManifest.xml'), encoding='utf-8').read()
    code = int(re.search(r'android:versionCode="(\d+)"', manifest).group(1))
    name = re.search(r'android:versionName="([^"]+)"', manifest).group(1)
    data = open(APK, 'rb').read()
    latest = {'versionCode': code, 'versionName': name, 'sha256': hashlib.sha256(data).hexdigest(),
              'size': len(data), 'notes': notes}
    print('publishing %s (code %d), %.1f MB' % (name, code, len(data) / 1e6))
    wrangler('kv', 'key', 'put', 'apk:%d' % code, '--path', APK, '--binding', 'BUILDS', '--remote')
    tmp = os.path.join(ROOT, 'build', 'android', 'latest.json')
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(latest, f)
    wrangler('kv', 'key', 'put', 'latest', '--path', tmp, '--binding', 'BUILDS', '--remote')
    # check what the phone will see
    with urllib.request.urlopen(urllib.request.Request(SERVER + '/latest', headers={'User-Agent': 'hyp-tools'}), timeout=30) as r:
        seen = json.loads(r.read())
    with urllib.request.urlopen(urllib.request.Request(SERVER + '/apk/%d' % code, headers={'User-Agent': 'hyp-tools'}), timeout=60) as r:
        served = r.read()
    ok = seen.get('versionCode') == code and hashlib.sha256(served).hexdigest() == latest['sha256']
    print('server now offers %s; download %s' % (seen.get('versionName'), 'matches' if ok else 'DOES NOT MATCH'))
    if not ok:
        sys.exit(1)


if __name__ == '__main__':
    main()
