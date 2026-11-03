#!/usr/bin/env bash
# Runs each libFuzzer target for a fixed time from a seed corpus.
#   usage: scripts/fuzz.sh [seconds-per-target]     (default 60)
# Seeds: the pinned capture for pcap_fuzz; for packet_fuzz, an empty corpus
# (the 1 KiB tiny-sensor packets are found quickly from nothing). Crashes
# land in build/fuzz/crashes/ and make the script exit non-zero.
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
SECS=${1:-60}
mkdir -p build/fuzz/corpus/pcap build/fuzz/corpus/packet build/fuzz/crashes
cp -n data/*.pcap build/fuzz/corpus/pcap/ 2> /dev/null || true
for t in pcap packet; do
  echo "== ${t}_fuzz for ${SECS}s"
  build/fuzz/fuzz/${t}_fuzz build/fuzz/corpus/$t -max_total_time="$SECS" -max_len=65536 \
    -artifact_prefix=build/fuzz/crashes/${t}- -print_final_stats=1 2>&1 |
    grep -E "^#[0-9]+.*(DONE|INITED)|stat::number_of_executed_units|ERROR|SUMMARY" || true
done
if ls build/fuzz/crashes/* > /dev/null 2>&1; then
  echo "crashes found:"
  ls build/fuzz/crashes
  exit 1
fi
echo "no crashes"
