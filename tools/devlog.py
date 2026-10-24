#!/usr/bin/env python3
"""Talk to the app's dev server on the console (http://$XBOX_HOST:7099, docs/contract.md §4).

  devlog.py status               print /status (pretty JSON)
  devlog.py log [-f] [--since N] print the log ring (-f: follow, polls /log?since=)
  devlog.py cmd '<json>'         POST /cmd, e.g. cmd '{"viewMode":"Fill"}'  cmd '{"setting":{"showStats":true}}'
  devlog.py key A B Down ...     send controller keys one by one (A B X Y LB RB LT RT Up Down Left Right Menu View)
  devlog.py hold B [--ms 1200]   hold a key (hold-B disconnect)
Config: XBOX_HOST or XBOX_DEVSERVER_URL (env or repo-root .env, see tools/toolenv.py).
"""
import argparse
import json
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolenv  # noqa: E402

KEYS = ['A', 'B', 'X', 'Y', 'LB', 'RB', 'LT', 'RT', 'Up', 'Down', 'Left', 'Right', 'Menu', 'View']


def request(path, body=None, timeout=10):
    url = toolenv.devserver_url() + path
    data = None if body is None else body.encode()
    req = urllib.request.Request(url, data, {'Content-Type': 'application/json'} if data else {},
                                 method='POST' if data is not None else 'GET')
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode(errors='replace')
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(errors='replace')
    except (urllib.error.URLError, OSError) as e:
        sys.exit(f'{url}: {e} (is the app running with devServer on, and XBOX_HOST right?)')


def cmd(body):
    try:
        json.loads(body)
    except ValueError as e:
        sys.exit(f'not JSON: {e}')
    st, out = request('/cmd', body)
    print(out)
    return st == 200


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    sub = ap.add_subparsers(dest='what', required=True)
    sub.add_parser('status', help='print /status')
    lp = sub.add_parser('log', help='print the log (-f to follow)')
    lp.add_argument('-f', '--follow', action='store_true')
    lp.add_argument('--since', type=int, default=0, help='start after this seq (default: whole ring)')
    lp.add_argument('--interval', type=float, default=1.0)
    cp = sub.add_parser('cmd', help="POST /cmd '<json>'")
    cp.add_argument('json')
    kp = sub.add_parser('key', help='send keys: ' + ' '.join(KEYS))
    kp.add_argument('keys', nargs='+')
    kp.add_argument('--gap', type=float, default=0.3, help='seconds between keys')
    hp = sub.add_parser('hold', help='hold a key for --ms')
    hp.add_argument('key')
    hp.add_argument('--ms', type=int, default=1200)
    a = ap.parse_args()

    if a.what == 'status':
        st, out = request('/status')
        try:
            print(json.dumps(json.loads(out), indent=2, ensure_ascii=False))
        except ValueError:
            print(out)
        sys.exit(0 if st == 200 else 1)

    if a.what == 'log':
        since = a.since
        while True:
            st, out = request(f'/log?since={since}')
            if st != 200:
                sys.exit(f'/log: HTTP {st} {out[:200]}')
            j = json.loads(out)
            if j.get('lines') and j.get('first', 0) > since + 1 and since:
                print(f'... {j["first"] - since - 1} lines dropped ...')
            for line in j.get('lines', []):
                print(line, flush=True)
            since = int(j.get('seq', since))
            if not a.follow:
                break
            time.sleep(a.interval)
        return

    if a.what == 'cmd':
        sys.exit(0 if cmd(a.json) else 1)

    if a.what == 'key':
        bad = [k for k in a.keys if k not in KEYS]
        if bad:
            sys.exit(f'unknown key(s) {bad}; valid: {" ".join(KEYS)}')
        ok = True
        for i, k in enumerate(a.keys):
            if i:
                time.sleep(a.gap)
            ok = cmd(json.dumps({'key': k})) and ok
        sys.exit(0 if ok else 1)

    if a.what == 'hold':
        if a.key not in KEYS:
            sys.exit(f'unknown key {a.key}; valid: {" ".join(KEYS)}')
        sys.exit(0 if cmd(json.dumps({'hold': a.key, 'ms': max(50, min(5000, a.ms))})) else 1)


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        pass
