#!/bin/sh
set -eu

src=$(CDPATH='' cd -- "$(dirname -- "$0")/../src" && pwd)
test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
for file in drcom-packet.h drcom-packet.c; do
	if [ ! -f "$src/$file" ]; then
		printf 'Missing standalone DrCOM packet module: %s\n' "$file" >&2
		exit 1
	fi
done

# Use the same libubox MD5 as the ImmortalWrt 25.12.2 package, without an SDK.
revision=7dd127841e82eb1cfb61185da37dde7b9bd9ba6d
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/libubox"
for file in md5.c md5.h utils.h; do
	curl -fsSL --retry 3 --max-time 30 \
		"https://raw.githubusercontent.com/openwrt/libubox/$revision/$file" \
		-o "$work/libubox/$file"
done

cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
	-I"$src" -I"$work" "$test_dir/packet-test.c" "$src/drcom-packet.c" \
	"$work/libubox/md5.c" -o "$work/packet-test"
"$work/packet-test"
