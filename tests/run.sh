#!/usr/bin/env bash
# Host tests (no console needed).
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

# BearSSL for the TLS tests, built once from third_party/bearssl.
ssl_lib="$root/build/host/libbearssl.a"
if [[ ! -f $ssl_lib ]]; then
    mkdir -p "$root/build/host/bearssl-obj"
    while IFS= read -r src; do
        obj="$root/build/host/bearssl-obj/$(echo "${src#third_party/bearssl/src/}" | tr / _).o"
        cc -O2 -w -I third_party/bearssl/inc -I third_party/bearssl/src -c "$src" -o "$obj"
    done < <(find third_party/bearssl/src -name '*.c' | sort)
    ar rcs "$ssl_lib" "$root"/build/host/bearssl-obj/*.o
fi
export WARP_BEARSSL_LIB="$ssl_lib"

python3 -m unittest discover -s tests -p 'test_*.py'

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for t in dm_pages_test dm_index_test file_lock_test zero_test cookies_test vfile_test; do
    cc -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
        -o "$tmp/$t" "radv/android_probe/$t.c"
    "$tmp/$t" > /dev/null
    echo "$t: ok"
done
