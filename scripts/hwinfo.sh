#!/usr/bin/env bash
# Everything a stranger needs to know about the machine a result came from.
#   usage: scripts/hwinfo.sh [iface] > results/hwinfo.txt
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1
IFACE=${1:-}
section() { printf '\n## %s\n' "$1"; }
section "when / what";   date -u +%FT%TZ; git rev-parse HEAD 2>/dev/null; git status --porcelain 2>/dev/null | head -5
section "cpu";           lscpu | grep -E "Model name|Architecture|^CPU\(s\)|Thread|Core|Socket|MHz|L1d|L1i|L2|L3|NUMA node\(s\)|Flags" | sed 's/Flags:.*\(avx2\|neon\).*/Flags: (has \1)/'
grep -m1 microcode /proc/cpuinfo
section "kernel";        uname -srvmo; cat /proc/cmdline   # no hostname: results get published
section "power"
for f in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor; do
  [ -r "$f" ] && echo "$f: $(cat "$f")"
done | head -4
cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null | sed 's/^/boost: /'
cat /sys/devices/system/cpu/smt/control 2>/dev/null | sed 's/^/smt: /'
section "memory";        free -g | head -2
command -v dmidecode >/dev/null && [ "$(id -u)" = 0 ] && dmidecode -t memory | grep -E "^\s+(Speed|Type|Size):" | sort | uniq -c
section "virtualization"; systemd-detect-virt 2>/dev/null || echo unknown
if [ -n "$IFACE" ]; then
  section "nic $IFACE";  ethtool -i "$IFACE"; ethtool -T "$IFACE"; ethtool -c "$IFACE" 2>/dev/null | grep -E "rx-usecs|adaptive"
fi
section "toolchain";     clang++-18 --version | head -1; cmake --version | head -1; ninja --version
python3 --version; [ -x .venv/bin/python ] && .venv/bin/python -m pip freeze 2>/dev/null | grep -iE "numpy|scipy|matplotlib|ouster|open3d"