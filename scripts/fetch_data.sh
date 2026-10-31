#!/usr/bin/env bash
# Fetches the pinned Ouster captures into data/ and verifies their SHA-256.
# Source: ouster-lidar/ouster-sdk tests/pcaps at a fixed commit, BSD-3-Clause
# (the SDK repository's license), so they may be fetched by a script. The
# larger Ouster sample drives are CC BY-NC-SA and are never committed here.
set -euo pipefail
cd "$(dirname "$0")/.."
SHA=46b7bceee7995914e2f3f3facf4d78418335b2d7
BASE=https://raw.githubusercontent.com/ouster-lidar/ouster-sdk/$SHA/tests/pcaps
mkdir -p data
while read -r sum name; do
  if [ ! -f "data/$name" ] || ! echo "$sum  data/$name" | sha256sum -c --quiet - 2> /dev/null; then
    curl -fsSL --retry 3 -o "data/$name" "$BASE/$name"
  fi
  echo "$sum  data/$name" | sha256sum -c -
done << 'EOF'
66ef581f62f7cec8d8067efff38f46687672c6b39cdaa65095e795268d3362b5 OS-2-128-U1_v2.3.0_1024x10.pcap
7e77f644a80e998a9c60ac67309200c2b299fcaf6a140c8bec209af14372334a OS-2-128-U1_v2.3.0_1024x10.json
EOF
