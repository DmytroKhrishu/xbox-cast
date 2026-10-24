"""Shared settings for the dev tools in tools/: environment variables, optionally seeded from the repo-root `.env`
(template: .env.example; real environment variables win).

  XBOX_HOST              Xbox Dev Mode IP/hostname, e.g. <xbox-lan-ip>. Device Portal: https://XBOX_HOST:11443,
                         app dev server: http://XBOX_HOST:7099
  XBOX_PORTAL_URL        full Device Portal URL instead of XBOX_HOST (default https://$XBOX_HOST:11443)
  XBOX_PORTAL_CRED_FILE  file holding "user:pass" for the Device Portal (default ~/.config/xbox-devportal.cred)
  XBOX_DEVSERVER_URL     app dev server URL instead of XBOX_HOST (default http://$XBOX_HOST:7099)
  GITHUB_REPOSITORY      owner/repo for CI artifacts (default: `gh repo view` of this checkout)
  XBOXCAST_ENV_FILE      a different .env file to read
"""
import os
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def _load_env(path):
    try:
        lines = Path(path).read_text().splitlines()
    except OSError:
        return
    for raw in lines:
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        if line.startswith('export '):
            line = line[7:].lstrip()
        k, sep, v = line.partition('=')
        k, v = k.strip(), v.strip()
        if not sep or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', k):
            continue
        if v[:1] in ('"', "'") and v[0] in v[1:]:  # quoted value, then an optional ' # comment'
            v = v[1:v.index(v[0], 1)]
        elif ' #' in v:
            v = v.split(' #', 1)[0].rstrip()
        os.environ.setdefault(k, v)


_load_env(os.environ.get('XBOXCAST_ENV_FILE') or REPO_ROOT / '.env')


def env(name, default=''):
    v = os.environ.get(name)
    if v is None or v.strip() == '' or (v.strip().startswith('<') and v.strip().endswith('>')):  # template placeholder
        return default
    return v.strip()


def need(name, hint=''):
    v = env(name)
    if not v:
        sys.exit(f'{name} is not set{": " + hint if hint else ""} (env var or repo-root .env, template .env.example)')
    return v


def xbox_host():
    return need('XBOX_HOST', 'the Xbox Dev Mode IP, e.g. <xbox-lan-ip>')


def portal_url():
    if env('XBOX_PORTAL_URL'):
        return env('XBOX_PORTAL_URL').rstrip('/')
    return f'https://{xbox_host()}:11443'


def devserver_url():
    if env('XBOX_DEVSERVER_URL'):
        return env('XBOX_DEVSERVER_URL').rstrip('/')
    return f'http://{xbox_host()}:7099'


def portal_cred():
    p = Path(env('XBOX_PORTAL_CRED_FILE', '~/.config/xbox-devportal.cred')).expanduser()
    try:
        return p.read_text().strip()
    except OSError:
        sys.exit(f'Device Portal credentials not found: put "user:pass" in {p} or set XBOX_PORTAL_CRED_FILE')


def github_repo():
    if env('GITHUB_REPOSITORY'):
        return env('GITHUB_REPOSITORY')
    try:
        out = subprocess.run(['gh', 'repo', 'view', '--json', 'nameWithOwner', '-q', '.nameWithOwner'], cwd=REPO_ROOT,
                             capture_output=True, text=True, check=True, timeout=30)
        if out.stdout.strip():
            return out.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        pass
    sys.exit('GITHUB_REPOSITORY is not set and `gh repo view` failed (set it to owner/repo)')
