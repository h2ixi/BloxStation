#!/usr/bin/env python3
"""Put a patched UniversalApp.rbxm + checksum on the PS5 (title must be closed).
The first time, the console's originals are kept as *.orig.
usage: put-ua.py DIR_WITH_PATCHED_FILES | --restore"""
import os, sys, time, json
from ftplib import FTP, error_perm
from urllib.request import urlopen
H = os.environ.get('PS5_HOST') or sys.exit('set PS5_HOST to your console address')
D = '/data/homebrew/PPSA99187/assets/ExtraContent/models/UniversalApp'
procs = json.load(urlopen('http://127.0.0.1:19113/api/ps5/process/list?addr=' + H, timeout=10))['processes']
if any(p.get('name') == 'eboot.bin' and 'PPSA99187' in json.dumps(p) for p in procs):
    sys.exit('the title is running: close it first')
f = FTP(); f.connect(H, 2120, timeout=60); f.login(); f.cwd(D)
names = f.nlst()
for n in ('UniversalApp.rbxm', 'UniversalApp_checksum'):
    if sys.argv[1] == '--restore':
        if n + '.orig' in names:
            try: f.delete(n)
            except error_perm: pass
            f.rename(n + '.orig', n); print('restored', n)
        continue
    if n + '.orig' not in names:
        f.rename(n, n + '.orig'); print('kept the original as', n + '.orig')
    with open(sys.argv[1] + '/' + n, 'rb') as fh:
        f.storbinary('STOR ' + n + '.new', fh)
    try: f.delete(n)
    except error_perm: pass
    f.rename(n + '.new', n)
    print('uploaded', n, f.size(n))
f.quit()
