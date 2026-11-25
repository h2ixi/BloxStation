#!/usr/bin/env python3
"""Build the PPSA99187 title, deploy eboot.bin to the PS5, run the Roblox stage
and print the roblox/bionic lines of the console's results file.

usage: roblox_cycle.py [stage] [wait_seconds]
"""
import json, os, subprocess, sys, time
from ftplib import FTP
from io import BytesIO
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen

H = os.environ.get('PS5_HOST') or sys.exit('set PS5_HOST to your console address')
B = 'http://127.0.0.1:19113'
ROOT = Path(__file__).resolve().parents[2]
stage = sys.argv[1] if len(sys.argv) > 1 else 'ctors'
wait = int(sys.argv[2]) if len(sys.argv) > 2 else 30


def post(p, d, t=40):
    try:
        return json.load(urlopen(Request(B + p, data=json.dumps(d).encode(),
                                         headers={'Content-Type': 'application/json'}, method='POST'), timeout=t))
    except HTTPError as e:
        return {'http': e.code, 'body': e.read().decode()[:200]}


env = dict(os.environ, PATH='/usr/bin:' + os.environ['PATH'],
           RADV_ARCHIVE=str(ROOT / '.deps/native/radv-release/lib/libvulkan_radeon.ps5.a'))
r = subprocess.run(['nice', '-n', '10', 'bash', 'tools/build-radv-title.sh'], cwd=ROOT, env=env,
                   capture_output=True, text=True)
errs = [l for l in (r.stdout + r.stderr).splitlines() if ' error' in l or 'undefined' in l]
if r.returncode != 0:
    print('BUILD FAILED'); print('\n'.join(errs[:40] or (r.stdout + r.stderr).splitlines()[-30:])); sys.exit(1)
print('build ok')

# Close the running title through the system's application manager (as the
# PS menu's Close does) rather than killing its process: repeated process
# kills are suspected of leaving the loader in the ETXTBSY state that only a
# console restart clears. Fall back to a process kill if that is refused.
for p in json.load(urlopen(B + '/api/ps5/process/list?addr=' + H, timeout=10))['processes']:
    if p.get('name') == 'eboot.bin' and 'PPSA99187' in json.dumps(p):
        # First ask the title to exit by itself (it drains the GPU, then
        # exits; see rv_quit_watch): a killed title is the suspect for the
        # ETXTBSY that needs a console restart.
        gone = False
        try:
            q = FTP(); q.connect(H, 2120, timeout=20); q.login()
            q.storbinary('STOR /data/homebrew/PPSA99187/quit-request', BytesIO(b'1')); q.quit()
            t0 = time.time()
            while time.time() - t0 < 15:
                time.sleep(1)
                procs = json.load(urlopen(B + '/api/ps5/process/list?addr=' + H, timeout=10))['processes']
                if not any(x.get('pid') == p['pid'] for x in procs):
                    gone = True
                    break
            print('clean exit:', 'done in %.0f s' % (time.time() - t0) if gone else 'no exit within 15 s', flush=True)
        except Exception as e:
            print('clean exit request failed:', e, flush=True)
        if gone:
            time.sleep(20)
            continue
        r = post('/api/ps5/app/lifecycle', {'addr': H, 'action': 'kill', 'app_id': p.get('app_id')})
        print('close app', p.get('app_id'), '->', r, flush=True)
        for _ in range(10):
            time.sleep(1)
            procs = json.load(urlopen(B + '/api/ps5/process/list?addr=' + H, timeout=10))['processes']
            if not any(q.get('pid') == p['pid'] for q in procs):
                break
        else:
            print('still running: process kill', post('/api/ps5/process/kill', {'addr': H, 'pid': p['pid']}), flush=True)
        # ShadowMount needs time to release the old run's mounts; a launch 2 s
        # after the exit was deferred, then timed out ("Device busy")
        time.sleep(20)
f = FTP(); f.connect(H, 2120, timeout=60); f.login(); f.cwd('/data/homebrew/PPSA99187')
# Never open the executable for writing in place: the title folder stays
# mounted by ShadowMount between runs, and writing into an executable there
# is the prime suspect for the loader's ETXTBSY ("text file busy") state that
# needs a console restart. Upload under another name and rename it over
# eboot.bin (a new file, the old one is just unlinked), and skip the upload
# when the build has not changed since the last one.
local = ROOT / 'dist/PPSA99187/eboot.bin'
stamp = ROOT / 'build/last-uploaded-eboot.sha256'
import hashlib
digest = hashlib.sha256(local.read_bytes()).hexdigest()
if stamp.exists() and stamp.read_text().strip() == digest:
    print('eboot.bin unchanged since the last upload: not re-sent', flush=True)
else:
    with local.open('rb') as h:
        f.storbinary('STOR eboot.bin.new', h, blocksize=1 << 20)
    try:
        f.rename('eboot.bin.new', 'eboot.bin')
    except Exception as e:
        print('rename over eboot.bin refused (%s): delete, then rename' % e, flush=True)
        f.delete('eboot.bin')
        f.rename('eboot.bin.new', 'eboot.bin')
    # a file created over FTP is not executable; the launch then fails 0x80aa001a
    print('chmod', f.sendcmd('SITE CHMOD 777 eboot.bin'), flush=True)
    stamp.parent.mkdir(parents=True, exist_ok=True)
    stamp.write_text(digest)
    print('eboot.bin replaced by rename', flush=True)
# ROBLOX_OPENAT_TEST=1 is required for games: without it Roblox disconnects
# ~60 s after joining ("missing or corrupted files", reason 304). Always keep
# it unless the caller explicitly sets ROBLOX_OPENAT_TEST=0.
extra = os.environ.get("ROBLOX_EXTRA_ENV", "")
if 'ROBLOX_OPENAT_TEST=' not in extra:
    extra = 'ROBLOX_OPENAT_TEST=1\n' + extra
f.storbinary('STOR radv-smoke-env.txt', BytesIO(f'RADV_SMOKE_ONLY=demo\nROBLOX_STAGE={stage}\n{extra}'.encode()))
f.quit()
def launch_once():
    t_launch = time.strftime('%H:%M:%S')
    print('launch', post('/api/ps5/app/launch', {'addr': H, 'title_id': 'PPSA99187'}), flush=True)
    time.sleep(12)
    import re
    log = shadowmount_log()
    started = [m.group(1) for l in log if l[1:9] >= t_launch
               for m in [re.search(r'\[GAME\] started: PPSA99187 pid=(\d+)', l)] if m]
    # Judge only the process this launch started: the previous run's exit can
    # be logged just after the launch.
    mine = [l for l in log if started and 'pid=%s' % started[-1] in l and 'PPSA99187' in l]
    errors = [l for l in mine if 'game error:' in l]  # not 'monitoring game errors'
    if not started or errors or any('process exited' in l for l in mine):
        return errors or mine[-6:] or ['no "[GAME] started" line after %s' % t_launch]
    return None


def shadowmount_log():
    try:
        f = FTP(); f.connect(H, 2120, timeout=20); f.login(); b = BytesIO()
        f.retrbinary('RETR /data/shadowmount/debug.log', b.write); f.quit()
        return b.getvalue().decode(errors='replace').splitlines()
    except Exception:
        return []


# PS5Upload answers ok even when the system loader refuses the title. Launched
# too soon after the previous run closed, the loader reports ETXTBSY on a
# system library that run had mapped. Each refused attempt names a different
# library (AvSetting, Http, Http2...) and the fourth or so starts: retry
# often rather than wait long.
for attempt in range(8):
    failure = launch_once()
    if not failure:
        break
    print('launch attempt %d failed:' % (attempt + 1), flush=True)
    print('\n'.join(failure), flush=True)
    if attempt < 7 and any('ETXTBSY' in l for l in failure):
        print('retrying in 20 s', flush=True)
        time.sleep(20)
        continue
    print('LAUNCH FAILED (needs a console restart if ETXTBSY persists)', flush=True)
    sys.exit(2)
print('launch confirmed: title still running after 12 s', flush=True)
time.sleep(max(0, wait - 12))
f = FTP(); f.connect(H, 2120, timeout=10); f.login(); bb = BytesIO()
f.retrbinary('RETR /data/homebrew/PPSA99187/radv-smoke.txt', bb.write); f.quit()
lines = [l for l in bb.getvalue().decode(errors='replace').splitlines() if 'roblox' in l or 'bionic' in l]
out = ROOT / 'radv/android_probe/build/last-console-log.txt'
out.write_text('\n'.join(lines))
print(len(lines), 'roblox/bionic lines (full: %s)' % out)
print('\n'.join(lines[:80]))
if len(lines) > 80:
    print('...'); print('\n'.join(lines[-15:]))
procs = json.load(urlopen(B + '/api/ps5/process/list?addr=' + H, timeout=10))['processes']
print('title still running:', any(p.get('name') == 'eboot.bin' and 'PPSA99187' in json.dumps(p) for p in procs))
