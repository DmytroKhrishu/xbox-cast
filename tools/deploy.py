#!/usr/bin/env python3
"""Download a CI build (artifact xboxcast-x64) and install it on the Xbox (Dev Mode) through the Device Portal REST API.

Usage: deploy.py [--run-id N] [--skip-download] [--skip-install] [--reinstall] [--launch] [--repo owner/repo]
  --skip-install   download (and list) the package only; never contacts the console
  --reinstall      uninstall the existing xbox-cast package first (signing cert changed, e.g. a fork's per-run cert)
Config (env or repo-root .env, see tools/toolenv.py): XBOX_HOST (or XBOX_PORTAL_URL), XBOX_PORTAL_CRED_FILE
(default ~/.config/xbox-devportal.cred, "user:pass", never printed), GITHUB_REPOSITORY (default: `gh repo view`).
gh uses GH_TOKEN if set; --gh-user NAME takes the token of that gh login (`gh auth token --user NAME`).
"""
import argparse
import base64
import json
import os
import ssl
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolenv  # noqa: E402

WORK = toolenv.REPO_ROOT / '.deploy'
ARTIFACT = 'xboxcast-x64'
CTX = ssl.create_default_context()
CTX.check_hostname = False
CTX.verify_mode = ssl.CERT_NONE  # Device Portal uses a self-signed cert


class Portal:
    def __init__(self):
        self.csrf = None
        self.cookies = {}
        self.base = toolenv.portal_url()
        self.auth = 'Basic ' + base64.b64encode(toolenv.portal_cred().encode()).decode()

    def call(self, method, path, body=None, ctype=None, timeout=60):
        h = {'Authorization': self.auth}
        if self.cookies:
            h['Cookie'] = '; '.join(f'{k}={v}' for k, v in self.cookies.items())
        if self.csrf and method != 'GET':
            h['X-CSRF-Token'] = self.csrf
        if ctype:
            h['Content-Type'] = ctype
        req = urllib.request.Request(self.base + path, body, h, method=method)
        try:
            resp = urllib.request.urlopen(req, context=CTX, timeout=timeout)
        except urllib.error.HTTPError as e:
            resp = e
        for c in resp.headers.get_all('Set-Cookie') or []:
            k, v = c.split(';', 1)[0].split('=', 1)
            self.cookies[k] = v
            if k == 'CSRF-Token':
                self.csrf = v
        return resp.status, resp.read()

    def packages(self, match):
        st, out = self.call('GET', '/api/app/packagemanager/packages')
        if st != 200:
            sys.exit(f'listing packages failed: HTTP {st}')
        return [p for p in json.loads(out).get('InstalledPackages', []) if match in p.get('PackageFullName', '')]


def download(run_id, artifact, repo):
    WORK.mkdir(exist_ok=True)
    for p in sorted(WORK.glob('**/*'), reverse=True):
        if p.is_file():
            p.unlink()
        elif p.is_dir():
            p.rmdir()
    args = ['gh', 'run', 'download', '-R', repo or toolenv.github_repo(), '-n', artifact, '-D', str(WORK)]
    if run_id:
        args.insert(3, str(run_id))
    print('download:', ' '.join(args))
    subprocess.run(args, check=True)


def multipart(files):
    boundary = uuid.uuid4().hex
    parts = []
    for p in files:
        parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="{p.name}"; filename="{p.name}"\r\n'
                     f'Content-Type: application/octet-stream\r\n\r\n'.encode() + p.read_bytes() + b'\r\n')
    return b''.join(parts) + f'--{boundary}--\r\n'.encode(), f'multipart/form-data; boundary={boundary}'


def collect():
    pkgs = [p for p in WORK.rglob('*') if p.suffix in ('.msix', '.appx')]
    main_pkg = [p for p in pkgs if 'Dependencies' not in p.parts]
    # Only x64 dependencies, never the *.Desktop.* VCLibs (they don't install on Xbox).
    deps = [p for p in pkgs if 'Dependencies' in p.parts and 'x64' in p.parts and '.Desktop.' not in p.name]
    if len(main_pkg) != 1:
        sys.exit(f'expected 1 main package in {WORK}, found {[str(p) for p in main_pkg]}')
    return main_pkg[0], deps


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--run-id', help='CI run id (default: the latest run that has the artifact)')
    ap.add_argument('--repo', help='owner/repo with the CI artifacts (default: GITHUB_REPOSITORY or `gh repo view`)')
    ap.add_argument('--artifact', default=ARTIFACT)
    ap.add_argument('--match', default='XboxCast', help='package name substring to report/launch/uninstall')
    ap.add_argument('--gh-user', help='use the token of this gh login for the download (gh auth token --user NAME)')
    ap.add_argument('--skip-download', action='store_true', help=f'reuse what is in {WORK.name}/')
    ap.add_argument('--skip-install', action='store_true', help='download and list only; do not contact the console')
    ap.add_argument('--launch', action='store_true', help='launch after install (needs a signed-in profile)')
    ap.add_argument('--reinstall', action='store_true', help='uninstall the existing package first (signing cert changed)')
    a = ap.parse_args()

    if a.gh_user:
        tok = subprocess.run(['gh', 'auth', 'token', '--user', a.gh_user], capture_output=True, text=True, check=True)
        os.environ['GH_TOKEN'] = tok.stdout.strip()
    if not a.skip_download:
        download(a.run_id, a.artifact, a.repo)

    main_pkg, deps = collect()
    files = [main_pkg] + deps
    print('package:', *[f'  {p.relative_to(WORK)} ({p.stat().st_size // 1024} KB)' for p in files], sep='\n')
    if a.skip_install:
        print('--skip-install: not contacting the console')
        return

    portal = Portal()
    portal.call('GET', '/api/os/info')  # obtains the CSRF cookie
    if a.reinstall:
        for p in portal.packages(a.match):
            q = urllib.parse.urlencode({'package': p['PackageFullName']})
            print('uninstall:', p['PackageFullName'], portal.call('DELETE', f'/api/app/packagemanager/package?{q}', timeout=120)[0])
    body, ctype = multipart(files)
    q = urllib.parse.urlencode({'package': main_pkg.name})
    st, out = portal.call('POST', f'/api/app/packagemanager/package?{q}', body, ctype, timeout=600)
    print('upload:', st, out[:300].decode(errors='replace'))
    if st >= 300:
        sys.exit(1)

    for _ in range(120):
        time.sleep(2)
        st, out = portal.call('GET', '/api/app/packagemanager/state')
        if st == 204 or not out:
            continue  # still installing
        txt = out.decode(errors='replace')
        print('state:', st, txt[:500])
        if st == 200:
            try:
                j = json.loads(txt)
            except ValueError:
                j = {}
            if j.get('Success') is False or j.get('Code', 0) not in (0, None):
                print('install failed; if the signing certificate changed, retry with --reinstall', file=sys.stderr)
                sys.exit(1)
            break
        if st >= 400:
            sys.exit(1)
    else:
        sys.exit('install did not finish within 4 minutes')

    for p in portal.packages(a.match):
        print('installed:', p['Name'], p['PackageFullName'], p.get('PackageRelativeId'))
        if a.launch:
            b64 = lambda s: urllib.parse.quote(base64.b64encode(s.encode()).decode(), safe='')  # noqa: E731
            st, out = portal.call('POST', f"/api/taskmanager/app?appid={b64(p['PackageRelativeId'])}"
                                          f"&package={b64(p['PackageFullName'])}")
            print('launch:', st, out[:200].decode(errors='replace') or 'ok')


if __name__ == '__main__':
    main()
