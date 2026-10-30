#!/usr/bin/env bash
# Reports whether this machine is configured for reportable latency runs and
# prints the fix for each item that is not. It changes nothing itself.
#   usage: scripts/rt_check.sh <measured-cpu>      e.g. scripts/rt_check.sh 3
# Exit status is the number of failed checks, so reproduce.sh can refuse to
# label a run "reported" on an unprepared machine.
set -uo pipefail
CPU=${1:-3}
fail=0
ok()  { printf '  ok    %s\n' "$1"; }
bad() { printf '  FAIL  %s\n        fix: %s\n' "$1" "$2"; fail=$((fail + 1)); }

cmdline=$(cat /proc/cmdline)
echo "kernel: $(uname -r)"
echo "cmdline: $cmdline"

if grep -qi microsoft /proc/version; then
  bad "running under WSL2/Hyper-V" "boot native Linux; a hypervisor owns the timer, the PMU and the NIC"
fi
# True if CPU is in a kernel cpu-list such as "domain,managed_irq,2-5,7".
in_list() {
  local IFS=, part
  for part in $1; do
    if [[ "$part" =~ ^([0-9]+)-([0-9]+)$ ]]; then
      (( $2 >= BASH_REMATCH[1] && $2 <= BASH_REMATCH[2] )) && return 0
    elif [[ "$part" == "$2" ]]; then
      return 0
    fi
  done
  return 1
}
for opt in isolcpus nohz_full rcu_nocbs; do
  if [[ "$cmdline" =~ (^| )$opt=([^ ]*) ]] && in_list "${BASH_REMATCH[2]}" "$CPU"; then
    ok "$opt covers CPU $CPU"
  else
    bad "$opt does not list CPU $CPU" "add '$opt=$CPU' to GRUB_CMDLINE_LINUX in /etc/default/grub, sudo update-grub, reboot"
  fi
done

gov=/sys/devices/system/cpu/cpu$CPU/cpufreq/scaling_governor
if [ -r "$gov" ]; then
  [ "$(cat "$gov")" = performance ] && ok "governor performance" ||
    bad "governor is $(cat "$gov")" "sudo cpupower -c $CPU frequency-set -g performance"
else
  bad "no cpufreq governor visible" "check BIOS/firmware power settings; record the fixed frequency in hwinfo"
fi

smt=/sys/devices/system/cpu/smt/control
if [ -r "$smt" ]; then
  [ "$(cat "$smt")" = off ] || [ "$(cat "$smt")" = notsupported ] && ok "SMT $(cat "$smt")" ||
    bad "SMT is $(cat "$smt"): a sibling hyperthread shares the measured core" "echo off | sudo tee $smt"
fi

boost=/sys/devices/system/cpu/cpufreq/boost
if [ -r "$boost" ]; then
  [ "$(cat "$boost")" = 0 ] && ok "turbo/boost disabled" ||
    bad "boost enabled: frequency depends on temperature" "echo 0 | sudo tee $boost"
fi

if systemctl is-active --quiet irqbalance 2>/dev/null; then
  bad "irqbalance running: it may move IRQs onto CPU $CPU" "sudo systemctl stop irqbalance"
else
  ok "irqbalance not running"
fi

rt=$(cat /proc/sys/kernel/sched_rt_runtime_us 2>/dev/null || echo unknown)
[ "$rt" = -1 ] && ok "RT throttling disabled" ||
  bad "sched_rt_runtime_us=$rt: SCHED_FIFO is throttled to 95%" "echo -1 | sudo tee /proc/sys/kernel/sched_rt_runtime_us"

lim=$(ulimit -l)
[ "$lim" = unlimited ] && ok "memlock unlimited" ||
  bad "memlock limit $lim KiB: mlockall will fail" "run under sudo, or set memlock unlimited in /etc/security/limits.conf"

echo "$fail check(s) failed"
exit "$fail"