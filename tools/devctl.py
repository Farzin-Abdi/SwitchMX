#!/usr/bin/env python3
"""Talks to SwitchMX's developer mode (Settings → Debug) from a computer on the same network.

  export SWITCHMX_DEV="<console-ip>:8080 <token>"
  tools/devctl.py status                 the app's state
  tools/devctl.py log [since] [src]      log lines (src: event, mpv, all); prints the next number last
  tools/devctl.py watch [src]            follows the log until interrupted
  tools/devctl.py shot out.png [half]    screenshot
  tools/devctl.py input "R:1500,wait:500,A"
  tools/devctl.py open sdmc:/Video/x.mkv
  tools/devctl.py back
  tools/devctl.py exit                   closes the app (open it again to run a new build)
  tools/devctl.py mpv get <property> | mpv cmd "<command>" | mpvlog <level>
  tools/devctl.py ls sdmc:/switch/SwitchMX
  tools/devctl.py cat <path> [from]      file contents (from < 0: the last bytes)
  tools/devctl.py config
  tools/devctl.py fetch <https url> [headers] [POST body]   fetched over the console's network
"""
import json, os, sys, time, urllib.parse, urllib.request

def call(path, params=None, method='GET', raw=False):
    host, token = os.environ['SWITCHMX_DEV'].split()
    url = f'http://{host}/dev/{path}'
    if params:
        url += '?' + urllib.parse.urlencode(params)
    req = urllib.request.Request(url, method=method, headers={'X-Dev-Token': token}, data=b'' if method == 'POST' else None)
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            data = r.read()
    except urllib.error.HTTPError as e:
        data = e.read()
        sys.stderr.write(f'HTTP {e.code}: {data.decode(errors="replace")}\n')
        sys.exit(1)
    except (urllib.error.URLError, ConnectionError, TimeoutError) as e:
        sys.stderr.write(f'not reachable: {getattr(e, "reason", e)}\n')
        sys.exit(2)
    return data if raw else data.decode('utf-8', errors='replace')

def main(a):
    if not a:
        print(__doc__)
        return
    c = a[0]
    if c == 'status':
        print(json.dumps(json.loads(call('status')), indent=2, ensure_ascii=False))
    elif c == 'log':
        print(call('log', {'since': a[1] if len(a) > 1 else 0, 'src': a[2] if len(a) > 2 else 'all'}), end='')
    elif c == 'watch':
        since = 0
        while True:
            d = json.loads(call('log', {'since': since, 'src': a[1] if len(a) > 1 else 'all', 'format': 'json'}))
            for l in d['lines']:
                print(f"[{l['src']}] {l['text']}", flush=True)
            since = d['next']
            time.sleep(1)
    elif c == 'shot':
        open(a[1], 'wb').write(call('screenshot', {'half': 1} if len(a) > 2 else None, raw=True))
        print(a[1])
    elif c == 'input':
        print(call('input', {'keys': a[1]}, 'POST'))
    elif c == 'open':
        print(call('open', {'path': a[1]}, 'POST'))
    elif c == 'fetch':
        # fetch <https url> [headers, one per line] [POST body]
        params = {'url': a[1], 'headers': a[2] if len(a) > 2 else ''}
        print(call('fetch', params, 'POST') if len(a) > 3 and False else '', end='')
        if len(a) > 3:
            host, token = os.environ['SWITCHMX_DEV'].split()
            req = urllib.request.Request(f'http://{host}/dev/fetch?' + urllib.parse.urlencode(params), method='POST',
                                         headers={'X-Dev-Token': token}, data=a[3].encode())
            with urllib.request.urlopen(req, timeout=60) as r:
                print(r.read().decode('utf-8', errors='replace'))
        else:
            print(call('fetch', params))
    elif c == 'dns':
        print(call('dns', {'host': a[1]}))
    elif c == 'dump':
        print(call('dump', method='POST'))
    elif c == 'exit':
        print(call('exit', method='POST'))
    elif c == 'back':
        print(call('back', method='POST'))
    elif c == 'mpv':
        print(call('mpv', {'get': a[2]}) if a[1] == 'get' else call('mpv', {'cmd': a[2]}, 'POST'))
    elif c == 'mpvlog':
        print(call('mpvlog', {'level': a[1] if len(a) > 1 else 'v'}, 'POST'))
    elif c == 'ls':
        for e in json.loads(call('ls', {'path': a[1]})):
            print(('d ' if e['dir'] else '  ') + f"{e['size']:>12}  {e['shown']}")
    elif c == 'cat':
        sys.stdout.buffer.write(call('file', {'path': a[1], 'from': a[2] if len(a) > 2 else 0}, raw=True))
    elif c == 'config':
        print(call('config'), end='')
    else:
        print(__doc__)

main(sys.argv[1:])
