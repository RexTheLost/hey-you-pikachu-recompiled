# Follows the live log the Android app streams while "Live log" is on (Words list in the game):
# prints new game-log lines and saves each Talk recording to reports/live/<session>/.
#
#   python tools/live_log.py            print everything since the last run, then exit
#   python tools/live_log.py --follow   keep following (Ctrl+C to stop)
#   python tools/live_log.py --all      start from the beginning of what the server keeps (7 days)
#
# The admin key is HYP_REPORT_KEY in D:\AI\.env (never printed). The last seen id is kept in
# reports/live/.last so each run shows only what's new.
import datetime, json, os, sys, time, urllib.request

ENV = r'D:\AI\.env'
SERVER = 'https://hyp-reports.pinball-leaderboard.workers.dev'
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'reports', 'live')


def key():
    for line in open(ENV, encoding='utf-8-sig'):
        if line.startswith('HYP_REPORT_KEY='):
            return line.split('=', 1)[1].strip().strip('"')
    sys.exit('HYP_REPORT_KEY is missing from ' + ENV)


def get(path):
    request = urllib.request.Request(SERVER + path, headers={'User-Agent': 'hyp-tools'})
    with urllib.request.urlopen(request, timeout=30) as response:
        return response.read()


def main():
    os.makedirs(OUT, exist_ok=True)
    last_file = os.path.join(OUT, '.last')
    after = 0
    if '--all' not in sys.argv and os.path.exists(last_file):
        after = int(open(last_file).read().strip() or 0)
    k = key()
    while True:
        data = json.loads(get('/live?key=%s&after=%d' % (k, after)))
        items = [('log', x) for x in data['lines']] + [('clip', x) for x in data['clips']]
        items.sort(key=lambda t: t[1]['id'])
        for kind, x in items:
            when = datetime.datetime.fromtimestamp(x['at'] / 1000).strftime('%H:%M:%S')
            if kind == 'log':
                for line in x['text'].rstrip('\n').split('\n'):
                    print('%s [%s] %s' % (when, x['session'][:6], line))
            else:
                folder = os.path.join(OUT, x['session'] or 'unknown')
                os.makedirs(folder, exist_ok=True)
                path = os.path.join(folder, '%05d_%s' % (x['id'], x['name'] or 'clip.wav'))
                with open(path, 'wb') as f:
                    f.write(get('/clip/%d?key=%s' % (x['id'], k)))
                print('%s [%s] == clip saved: %s (%d bytes)' % (when, x['session'][:6], os.path.relpath(path), x['size'] or 0))
            after = max(after, x['id'])
        with open(last_file, 'w') as f:
            f.write(str(after))
        sys.stdout.flush()
        if '--follow' not in sys.argv:
            break
        time.sleep(3)


if __name__ == '__main__':
    main()
