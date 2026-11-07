#!/usr/bin/env bash
# RECEIVER machine: enables NIC hardware receive timestamps, disciplines the
# NIC clock to CLOCK_REALTIME, moves NIC interrupts off the measured CPU, and
# runs udp_rx on it. Start this first, then scripts/external_tx.sh on the
# sender.
#   usage: sudo IMPL=simd scripts/external_rx.sh <iface> <cpu> [decisions]
# Needs linuxptp (hwstamp_ctl, phc2sys) and ethtool, and a NIC whose
# `ethtool -T` lists hardware-receive and HWTSTAMP_FILTER_ALL.
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
IFACE=${1:?iface}
CPU=${2:?cpu}
N=${3:-36000}
IMPL=${IMPL:-simd}
CAP=data/OS-2-128-U1_v2.3.0_1024x10
mkdir -p results # gitignored, so absent in a fresh clone
ethtool -T "$IFACE" | grep -q "hardware-receive" || {
  echo "$IFACE has no hardware RX timestamps"
  exit 1
}
hwstamp_ctl -i "$IFACE" -r 1                        # HWTSTAMP_FILTER_ALL
ethtool -C "$IFACE" rx-usecs 0 2> /dev/null || true # no interrupt coalescing delay
# NIC IRQs on CPU 0, away from $CPU. Some drivers (mlx5, ...) name their IRQs
# after themselves, not the interface; then nothing matches and it is manual.
irqs=$(grep "$IFACE" /proc/interrupts | cut -d: -f1 || true)
[ -n "$irqs" ] || echo "warning: no IRQ in /proc/interrupts names $IFACE; move the NIC's IRQs off CPU $CPU by hand" >&2
for irq in $irqs; do
  echo 0 > "/proc/irq/$irq/smp_affinity_list"
done
phc2sys -s CLOCK_REALTIME -c "$IFACE" -O 0 -m > results/phc2sys.log 2>&1 &
PHC=$!
trap 'kill $PHC 2>/dev/null' EXIT
sleep 10 # let the servo converge
tail -3 results/phc2sys.log
kill -0 "$PHC" 2> /dev/null || {
  echo "phc2sys exited; the NIC clock is not disciplined (log above)"
  exit 1
}
build/rel/tools/udp_rx --meta "$CAP.json" --impl "$IMPL" --cpu "$CPU" --fifo 80 --spin \
  --helper-cpu "$((CPU + 1))" --decisions "$N" --csv results/external.csv --label "ext-$IMPL" \
  --log "results/external_${IMPL}_decisions.csv"
echo "phc2sys offset over the run (ns):"
grep -o 'offset *-\?[0-9]*' results/phc2sys.log | awk '{print $2}' | sort -n | awk '{a[NR]=$1} END {print "min", a[1], "p50", a[int((NR + 1) / 2)], "max", a[NR]}'
