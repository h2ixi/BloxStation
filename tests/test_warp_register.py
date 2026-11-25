"""On-console first-run WARP registration against a local TLS fake server.

No real Cloudflare endpoint is contacted: the test build redirects the
connection with a compile-time test hook (-DWARP_REGISTER_TEST), and the
normal-build check wraps getaddrinfo/connect to reach only 127.0.0.1.
"""
import base64
import json
import os
import shutil
import socket
import ssl
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LIB = Path(os.environ.get('WARP_BEARSSL_LIB') or ROOT / 'build/host/libbearssl.a')  # tests/run.sh builds it
WG = ROOT / 'third_party/wireguard-lwip/src'
SRC = ROOT / 'radv/android_probe/warp/warp_register.c'

PRIV = bytes.fromhex('77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a')
PUB = bytes.fromhex('8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a')
OTHER = bytes(range(1, 33))
PEER = 'bmXOC+F1FxEMF9dyiK2H5/1SUtzH0JuVo51h2wPfgyo='
TOKEN = 'tok-SECRET_8f2c1d'
REG_ID = 't.1234abcd-0000-4000-8000-00000000beef'.replace('t.', 't-')

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bearssl.h>
#include "radv/android_probe/warp/warp_register.h"
static unsigned char dn[4096];
static size_t dn_len;
static void dn_cb(void *ctx, const void *b, size_t n) {
   (void)ctx;
   if (dn_len + n <= sizeof(dn)) { memcpy(dn + dn_len, b, n); dn_len += n; }
}
static br_x509_decoder_context dc;
static br_x509_trust_anchor ta;
int main(int argc, char **argv) {
   if (argc != 5) return 2;
   static unsigned char der[8192];
   FILE *f = fopen(argv[3], "rb");
   if (!f) return 2;
   size_t n = fread(der, 1, sizeof(der), f);
   fclose(f);
   br_x509_decoder_init(&dc, dn_cb, NULL);
   br_x509_decoder_push(&dc, der, n);
   br_x509_pkey *pk = br_x509_decoder_get_pkey(&dc);
   if (!pk) return 3;
   ta.dn.data = dn; ta.dn.len = dn_len; ta.flags = BR_X509_TA_CA; ta.pkey = *pk;
   warp_register_test_hooks.tas = &ta;
   warp_register_test_hooks.tas_num = 1;
   warp_register_test_hooks.connect_ip = "127.0.0.1";
   warp_register_test_hooks.port = (unsigned short)atoi(argv[2]);
   if (*argv[4]) warp_register_test_hooks.entropy_path = argv[4];
   printf("%d\n", warp_first_run_register(argv[1]));
   return 0;
}
'''

# Normal build: real anchors; DNS fails so the fixed fallbacks are tried, and
# every connect is redirected to the local fake server.
NORMAL = r'''
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include "radv/android_probe/warp/warp_register.h"
int __wrap_getaddrinfo(const char *n, const char *s, const struct addrinfo *h, struct addrinfo **r) {
   (void)n; (void)s; (void)h; (void)r; return EAI_FAIL;
}
int __real_connect(int, const struct sockaddr *, socklen_t);
int __wrap_connect(int fd, const struct sockaddr *a, socklen_t n) {
   (void)a; (void)n;
   struct sockaddr_in dst = {0};
   dst.sin_family = AF_INET; dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   dst.sin_port = htons((unsigned short)atoi(getenv("TEST_PORT")));
   return __real_connect(fd, (const struct sockaddr *)&dst, sizeof(dst));
}
int main(int argc, char **argv) {
   if (argc != 2) return 2;
   printf("%d\n", warp_first_run_register(argv[1]));
   return 0;
}
'''


def reg_reply(endpoint='162.159.192.7:2408', **over):
    peer = {'public_key': PEER, 'endpoint': {'v4': endpoint, 'v6': '[2606:4700:d0::a29f:c007]:2408',
                                              'host': 'engage.cloudflareclient.com:2408'}}
    doc = {'id': REG_ID, 'type': 'a', 'token': TOKEN, 'warp_enabled': False,
           'account': {'id': 'acct', 'license': 'lic', 'warp_plus': False},
           'config': {'client_id': 'abc', 'peers': [peer],
                      'interface': {'addresses': {'v4': '172.16.0.2', 'v6': '2606:4700:110::2'}},
                      'services': {'http_proxy': '172.16.0.1:2480'}}}
    doc.update(over)
    return json.dumps(doc).encode()


class FakeAPI:
    """Sequential TLS server; replies[method] = (status, body, framing)."""

    def __init__(self, cert, key):
        self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.ctx.load_cert_chain(cert, key)
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(('127.0.0.1', 0))
        self.sock.listen(4)
        self.sock.settimeout(0.2)
        self.port = self.sock.getsockname()[1]
        self.replies = {}
        self.requests = []
        self.connections = 0
        self.stop = False
        self.thread = threading.Thread(target=self.loop, daemon=True)
        self.thread.start()

    def loop(self):
        while not self.stop:
            try:
                conn, _ = self.sock.accept()
            except socket.timeout:
                continue
            self.connections += 1
            conn.settimeout(10)
            try:
                with self.ctx.wrap_socket(conn, server_side=True) as tls:
                    self.serve(tls)
            except (ssl.SSLError, OSError):
                pass
        self.sock.close()

    def serve(self, tls):
        data = b''
        while b'\r\n\r\n' not in data:
            chunk = tls.recv(4096)
            if not chunk:
                return
            data += chunk
        head, body = data.split(b'\r\n\r\n', 1)
        lines = head.decode().split('\r\n')
        method, path, _ = lines[0].split(' ')
        headers = {}
        for line in lines[1:]:
            k, v = line.split(':', 1)
            headers[k.strip().lower()] = v.strip()
        length = int(headers.get('content-length', '0'))
        while len(body) < length:
            body += tls.recv(4096)
        self.requests.append({'method': method, 'path': path, 'headers': headers, 'body': body})
        status, payload, framing = self.replies[method]
        out = b'HTTP/1.1 %d X\r\nContent-Type: application/json\r\nConnection: close\r\n' % status
        if framing == 'length':
            out += b'Content-Length: %d\r\n\r\n' % len(payload) + payload
        elif framing == 'short':      # announces more than it sends
            out += b'Content-Length: %d\r\n\r\n' % (len(payload) + 50) + payload
        elif framing == 'huge':
            out += b'Content-Length: 20000\r\n\r\n' + payload
        elif framing == 'chunked':
            out += b'Transfer-Encoding: chunked\r\n\r\n'
            for i in range(0, len(payload), 97):
                part = payload[i:i + 97]
                out += b'%x;ext=1\r\n' % len(part) + part + b'\r\n'
            out += b'0\r\nX-Trailer: 1\r\n\r\n'
        elif framing in ('close', 'abrupt'):  # abrupt: no TLS close_notify
            out += b'\r\n' + payload
        tls.sendall(out)
        if framing == 'close':
            try:
                tls.unwrap()
            except (ssl.SSLError, OSError):
                pass

    def close(self):
        self.stop = True
        self.thread.join(5)


@unittest.skipUnless(LIB.exists(), 'host BearSSL archive unavailable')
class WarpRegisterTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='warp-register-'))
        p = cls.tmp
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:prime256v1',
                        '-nodes', '-subj', '/CN=api.cloudflareclient.com',
                        '-addext', 'subjectAltName=DNS:api.cloudflareclient.com', '-days', '2',
                        '-keyout', str(p / 'key'), '-out', str(p / 'cert')],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        subprocess.run(['openssl', 'x509', '-in', str(p / 'cert'), '-outform', 'DER', '-out', str(p / 'cert.der')],
                       check=True)
        (p / 'h.c').write_text(HARNESS)
        (p / 'n.c').write_text(NORMAL)
        (p / 'entropy').write_bytes(PRIV)
        (p / 'entropy2').write_bytes(OTHER)
        common = ['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-stringop-overread',
                  '-I', str(ROOT), '-I', str(ROOT / 'third_party/bearssl/inc'), '-I', str(WG)]
        crypto = [str(WG / 'crypto/refc/x25519.c'), str(WG / 'crypto.c')]
        for args in ([*common, '-DWARP_REGISTER_TEST', str(p / 'h.c'), str(SRC), *crypto, str(LIB), '-o', str(p / 'h')],
                     [*common, str(p / 'n.c'), str(SRC), *crypto, str(LIB),
                      '-Wl,--wrap=getaddrinfo', '-Wl,--wrap=connect', '-o', str(p / 'n')],
                     [*common, '-c', str(SRC), '-o', str(p / 'normal.o')]):
            built = subprocess.run(args, capture_output=True, text=True)
            if built.returncode:
                raise AssertionError(built.stderr)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        self.api = FakeAPI(str(self.tmp / 'cert'), str(self.tmp / 'key'))
        self.api.replies = {'POST': (200, reg_reply(), 'length'), 'PATCH': (200, b'{"warp_enabled":true}', 'length')}
        self.dir = Path(tempfile.mkdtemp(prefix='warp-dir-', dir=self.tmp))
        os.chmod(self.dir, 0o700)

    def tearDown(self):
        self.api.close()

    def run_test_build(self, entropy='entropy'):
        r = subprocess.run([str(self.tmp / 'h'), str(self.dir), str(self.api.port), str(self.tmp / 'cert.der'),
                            str(self.tmp / entropy) if entropy else ''],
                           capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        for secret in (TOKEN, base64.b64encode(PRIV).decode(), base64.b64encode(OTHER).decode()):
            self.assertNotIn(secret, r.stdout + r.stderr)
        return int(r.stdout.strip())

    def files(self):
        return sorted(p.name for p in self.dir.iterdir())

    def pending(self):
        return json.loads((self.dir / 'warp-pending.json').read_text())

    def assert_failed_safely(self, rc, expected):
        self.assertEqual(rc, expected)
        self.assertFalse((self.dir / 'warp.json').exists())
        self.assertFalse((self.dir / 'warp-staged.json').exists())
        self.assertEqual(base64.b64decode(self.pending()['private_key']), PRIV)
        self.assertEqual((self.dir / 'warp-pending.json').stat().st_mode & 0o777, 0o600)

    def test_success_writes_private_gateway_config(self):
        self.assertEqual(self.run_test_build(), 0)
        self.assertEqual(self.files(), ['warp.json'])
        warp = self.dir / 'warp.json'
        self.assertEqual(warp.stat().st_mode & 0o777, 0o600)
        self.assertEqual(json.loads(warp.read_text()), {
            'private_key': base64.b64encode(PRIV).decode(), 'peer_public_key': PEER,
            'endpoint_ip': '162.159.192.7', 'endpoint_port': 2408, 'address_v4': '172.16.0.2',
            'dns': '1.1.1.1', 'id': REG_ID, 'token': TOKEN})
        self.assertLess(warp.stat().st_size, 2047)  # gateway load_conf buffer
        post, patch = self.api.requests
        self.assertEqual((post['method'], post['path']), ('POST', '/v0a2158/reg'))
        h = post['headers']
        self.assertEqual(h['host'], 'api.cloudflareclient.com')
        self.assertEqual(h['user-agent'], 'okhttp/3.12.1')
        self.assertEqual(h['cf-client-version'], 'a-6.11-2223')
        self.assertTrue(h['content-type'].startswith('application/json'))
        self.assertEqual(h['connection'], 'close')
        self.assertNotIn('authorization', h)
        body = json.loads(post['body'])
        self.assertEqual(base64.b64decode(body['key']), PUB)
        self.assertEqual({k: body[k] for k in ('install_id', 'fcm_token', 'model', 'serial_number', 'locale')},
                         {'install_id': '', 'fcm_token': '', 'model': 'PlayStation 5', 'serial_number': '',
                          'locale': 'en_US'})
        self.assertRegex(body['tos'], r'^20\d\d-\d\d-\d\dT\d\d:\d\d:\d\d\.000Z$')
        self.assertEqual((patch['method'], patch['path']), ('PATCH', '/v0a2158/reg/' + REG_ID))
        self.assertEqual(patch['headers']['authorization'], 'Bearer ' + TOKEN)
        self.assertEqual(json.loads(patch['body']), {'warp_enabled': True})

    def test_chunked_and_close_delimited_bodies(self):
        self.api.replies = {'POST': (200, reg_reply(), 'chunked'), 'PATCH': (200, b'{}', 'close')}
        self.assertEqual(self.run_test_build(), 0)
        self.assertEqual(json.loads((self.dir / 'warp.json').read_text())['endpoint_port'], 2408)

    def test_existing_identity_means_no_network(self):
        original = b'{"private_key":"x"}\n'
        (self.dir / 'warp.json').write_bytes(original)
        self.assertEqual(self.run_test_build(), 1)
        self.assertEqual(self.api.connections, 0)
        self.assertEqual((self.dir / 'warp.json').read_bytes(), original)
        self.assertEqual(self.files(), ['warp.json'])

    def test_pending_key_is_reused_not_regenerated(self):
        self.api.replies['POST'] = (500, b'{"error":"busy"}', 'length')
        self.assert_failed_safely(self.run_test_build(), -8)
        first_key = json.loads(self.api.requests[0]['body'])['key']
        self.api.replies['POST'] = (200, reg_reply(), 'length')
        self.assertEqual(self.run_test_build(entropy='entropy2'), 0)  # different entropy now
        self.assertEqual(json.loads(self.api.requests[1]['body'])['key'], first_key)
        self.assertEqual(json.loads((self.dir / 'warp.json').read_text())['private_key'],
                         base64.b64encode(PRIV).decode())

    def test_invalid_pending_is_left_for_recovery(self):
        (self.dir / 'warp-pending.json').write_text(json.dumps({
            'private_key': base64.b64encode(PRIV).decode(), 'public_key': PEER}))
        os.chmod(self.dir / 'warp-pending.json', 0o600)
        self.assertEqual(self.run_test_build(), -3)
        self.assertEqual(self.api.connections, 0)
        self.assertEqual(json.loads((self.dir / 'warp-pending.json').read_text())['public_key'], PEER)

    def test_bad_endpoints_are_rejected(self):
        for endpoint in ('999.1.1.1:2408', '162.159.192.7:0', '162.159.192.7:70000', '162.159.192.7:',
                         '162.159.192.7', 'engage.cloudflareclient.com:2408', '162.159.192.7:24a8',
                         '[2606:4700:d0::a29f:c007]:2408', '162.159.192.7:02408'):
            with self.subTest(endpoint=endpoint):
                self.api.requests.clear()
                self.api.replies['POST'] = (200, reg_reply(endpoint), 'length')
                self.assert_failed_safely(self.run_test_build(), -9)
                self.assertEqual([r['method'] for r in self.api.requests], ['POST'])

    def test_port_zero_uses_listed_port(self):
        doc = json.loads(reg_reply('162.159.192.7:0'))
        doc['config']['peers'][0]['endpoint']['ports'] = [2408, 500]
        self.api.replies['POST'] = (200, json.dumps(doc).encode(), 'length')
        self.assertEqual(self.run_test_build(), 0)
        self.assertEqual(json.loads((self.dir / 'warp.json').read_text())['endpoint_port'], 2408)

    def test_malformed_replies_are_rejected(self):
        good = reg_reply()
        bad_doc = lambda **kw: reg_reply(**kw)
        cases = {
            'truncated': good[:-1],
            'trailing': good + b'x',
            'two docs': good + good,
            'not object': b'[' + good + b']',
            'duplicate id': good[:-1] + b',"id":"other"}',
            'missing token': bad_doc(token=None),
            'token CRLF': bad_doc(token='abc\r\nX-Evil: 1'),
            'token space': bad_doc(token='abc def'),
            'id slash': bad_doc(id='../x'),
            'escaped id': good.replace(REG_ID.encode(), b't\\u002d1'),
            'short peer key': good.replace(PEER.encode(), PEER[:-2].encode()),
            'long peer key': good.replace(PEER.encode(), PEER[:-1].encode() + b'A='),
            'noncanonical key': good.replace(PEER.encode(), PEER[:42].encode() + b'p='),
            'bad base64': good.replace(PEER.encode(), b'!' + PEER[1:].encode()),
            'bad address': good.replace(b'"172.16.0.2"', b'"172.16.0.256"'),
            'raw control char': good.replace(b'"type": "a"', b'"type": "a\x01"'),
            'bare word': good.replace(b'false', b'nope', 1),
            'leading zero': good.replace(b'"type": "a"', b'"type": 01'),
            'empty peers': good.replace(good[good.index(b'"peers": [') + 10:good.index(b'], "interface"')], b''),
        }
        for name, payload in cases.items():
            with self.subTest(name=name):
                self.api.replies['POST'] = (200, payload, 'length')
                self.assert_failed_safely(self.run_test_build(), -9)

    def test_http_errors_keep_pending(self):
        for status in (400, 403, 429, 500):
            with self.subTest(status=status):
                self.api.requests.clear()
                self.api.replies['POST'] = (status, reg_reply(), 'length')
                self.assert_failed_safely(self.run_test_build(), -8)
                self.assertEqual([r['method'] for r in self.api.requests], ['POST'])

    def test_bad_http_framing(self):
        for framing in ('short', 'huge', 'abrupt'):
            with self.subTest(framing=framing):
                self.api.replies['POST'] = (200, reg_reply(), framing)
                self.assert_failed_safely(self.run_test_build(), -7)

    def test_patch_failure_keeps_registration_and_resumes(self):
        self.api.replies['PATCH'] = (500, b'{}', 'length')
        self.assertEqual(self.run_test_build(), -10)
        self.assertEqual(self.files(), ['warp-pending.json', 'warp-staged.json'])
        self.assertEqual((self.dir / 'warp-staged.json').stat().st_mode & 0o777, 0o600)
        self.api.requests.clear()
        self.api.replies['PATCH'] = (200, b'{}', 'length')
        self.assertEqual(self.run_test_build(), 0)
        self.assertEqual([r['method'] for r in self.api.requests], ['PATCH'])  # no second registration
        self.assertEqual(self.files(), ['warp.json'])

    def test_rejected_credentials_drop_staged_registration(self):
        self.api.replies['PATCH'] = (401, b'{}', 'length')
        self.assert_failed_safely(self.run_test_build(), -10)

    def test_leftovers_from_interrupted_run_are_tidied_without_network(self):
        self.api.replies['PATCH'] = (500, b'{}', 'length')
        self.assertEqual(self.run_test_build(), -10)
        shutil.copy(self.dir / 'warp-staged.json', self.dir / 'warp.json')
        connections = self.api.connections
        self.assertEqual(self.run_test_build(), 1)
        self.assertEqual(self.api.connections, connections)
        self.assertEqual(self.files(), ['warp.json'])

    def test_untrusted_certificate_rejected_in_normal_build(self):
        r = subprocess.run([str(self.tmp / 'n'), str(self.dir)], capture_output=True, text=True, timeout=60,
                           env={**os.environ, 'TEST_PORT': str(self.api.port)})
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(int(r.stdout), -6)
        self.assertGreater(self.api.connections, 0)
        self.assertEqual(self.api.requests, [])  # nothing sent before verification
        self.assertEqual(self.files(), ['warp-pending.json'])

    def test_normal_build_has_no_test_hooks(self):
        symbols = subprocess.run(['nm', str(self.tmp / 'normal.o')], capture_output=True, text=True, check=True)
        self.assertNotIn('warp_register_test_hooks', symbols.stdout)
        self.assertIn('warp_first_run_register', symbols.stdout)


if __name__ == '__main__':
    unittest.main()
