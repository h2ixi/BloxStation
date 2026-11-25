"""WARP endpoint candidates: many ranges and ports, saved good endpoint first."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class EndpointListTest(unittest.TestCase):
    def test_candidates_saved_and_rotation(self):
        code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "radv/android_probe/warp/endpoint_list.h"
int main(int argc, char **argv) {
    char ip[32]; unsigned short port;
    /* every candidate is a valid Cloudflare WARP IPv4 address and port */
    unsigned n = warp_endpoint_candidate_count();
    assert(n >= 100);
    int ports_seen_2408 = 0, ports_other = 0, ranges[4] = {0};
    for (unsigned i = 0; i < n; i++) {
        assert(warp_endpoint_candidate(i, ip, sizeof(ip), &port) == 0);
        assert(port > 0);
        if (port == 2408) ports_seen_2408++; else ports_other++;
        if (!strncmp(ip, "162.159.192.", 12)) ranges[0]++;
        else if (!strncmp(ip, "162.159.195.", 12)) ranges[1]++;
        else if (!strncmp(ip, "188.114.9", 9)) ranges[2]++;
        else ranges[3]++;
    }
    assert(ports_seen_2408 > 0 && ports_other > ports_seen_2408);
    assert(ranges[0] && ranges[1] && ranges[2]);
    /* the first 8 candidates already vary both address range and port */
    char first[8][32]; unsigned short fp[8];
    for (int i = 0; i < 8; i++) warp_endpoint_candidate(i, first[i], 32, &fp[i]);
    int diff_port = 0, diff_range = 0;
    for (int i = 1; i < 8; i++) {
        if (fp[i] != fp[0]) diff_port = 1;
        if (strncmp(first[i], first[0], 10)) diff_range = 1;
    }
    assert(diff_port && diff_range);
    assert(warp_endpoint_candidate(n, ip, sizeof(ip), &port) == -1);
    /* every range/port pair exactly once */
    static unsigned char seen[7][56];
    for (unsigned i = 0; i < n; i++) {
        warp_endpoint_candidate(i, ip, sizeof(ip), &port);
        int r = -1, pi = -1;
        for (int k = 0; k < 7; k++) if (!strncmp(ip, warp_endpoint_ranges[k], strlen(warp_endpoint_ranges[k]))) r = k;
        for (int k = 0; k < 56; k++) if (warp_endpoint_ports[k] == port) pi = k;
        assert(r >= 0 && pi >= 0 && !seen[r][pi]);
        seen[r][pi] = 1;
    }

    /* saved endpoint round trip; rejects garbage */
    assert(argc == 2);
    assert(warp_endpoint_save(argv[1], "188.114.98.1", 2408) == 0);
    assert(warp_endpoint_load(argv[1], ip, sizeof(ip), &port) == 0);
    assert(!strcmp(ip, "188.114.98.1") && port == 2408);
    FILE *f = fopen(argv[1], "w"); fputs("not an endpoint\n", f); fclose(f);
    assert(warp_endpoint_load(argv[1], ip, sizeof(ip), &port) == -1);
    assert(warp_endpoint_load("/nonexistent/x", ip, sizeof(ip), &port) == -1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            (p / 't.c').write_text(code)
            b = subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT),
                                str(p / 't.c'), '-o', str(p / 't')], capture_output=True, text=True)
            self.assertEqual(b.returncode, 0, b.stderr)
            subprocess.run([str(p / 't'), str(p / 'saved')], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
