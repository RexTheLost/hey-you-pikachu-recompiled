# Downloads the crash / exit reports the Android app sent to the report inbox
# (reports-worker/, https://hyp-reports.pinball-leaderboard.workers.dev) into reports/,
# one text file each, and marks them fetched.
#
#   python tools/fetch_reports.py          fetch and save every waiting report
#   python tools/fetch_reports.py --list   only show what is waiting
#
# The admin key is the HYP_REPORT_KEY line of D:\AI\.env (never committed, never printed).
import datetime, json, os, sys, urllib.request

ENV = r'D:\AI\.env'
INBOX = 'https://hyp-reports.pinball-leaderboard.workers.dev'
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'reports')


def key():
    for line in open(ENV, encoding='utf-8-sig'):
        if line.startswith('HYP_REPORT_KEY='):
            return line.split('=', 1)[1].strip().strip('"')
    sys.exit('HYP_REPORT_KEY is missing from ' + ENV)


def call(method, path, body=None):
    request = urllib.request.Request(INBOX + path, method=method, data=json.dumps(body).encode() if body is not None else None,
                                     headers={'Content-Type': 'application/json', 'User-Agent': 'hyp-tools'})
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.loads(response.read() or b'{}')


def main():
    waiting = call('GET', '/reports?key=' + key())['reports']
    print(len(waiting), 'report(s) waiting')
    os.makedirs(OUT, exist_ok=True)
    for r in waiting:
        when = datetime.datetime.fromtimestamp(r['at'] / 1000)
        name = '%s_%04d_%s.txt' % (when.strftime('%Y%m%d-%H%M%S'), r['id'], r['kind'])
        print('#%d %s %s %s (%s)' % (r['id'], when.strftime('%Y-%m-%d %H:%M'), r['kind'], r['version'], r['device']))
        if '--list' in sys.argv:
            continue
        with open(os.path.join(OUT, name), 'w', encoding='utf-8') as f:
            f.write('report #%d, %s, %s, version %s, %s\n\n%s' % (r['id'], when, r['kind'], r['version'], r['device'], r['text']))
        call('POST', '/reports/%d/fetched?key=%s' % (r['id'], key()))
        print('  saved reports/' + name)


if __name__ == '__main__':
    main()
