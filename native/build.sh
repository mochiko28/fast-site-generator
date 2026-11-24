#!/bin/sh
set -eu
cd "$(dirname "$0")"
tmp=../site.node.$$
trap 'rm -f "$tmp"' EXIT HUP INT TERM
gcc -std=c11 -O3 -fPIC -shared -nostdlib -ffreestanding -fno-stack-protector \
    -fno-builtin -fno-unwind-tables -fno-asynchronous-unwind-tables \
    -fno-strict-aliasing -fvisibility=hidden -flto -fno-ipa-cp-clone -Wall -Wextra -Werror \
    -Wl,--no-undefined,--build-id=none,-s "$@" -march=x86-64 -mtune=generic \
    core.c text.c template.c markdown.c site.c -o "$tmp"
mv "$tmp" ../site.node
