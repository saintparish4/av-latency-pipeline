#!/usr/bin/env bash
# SENDER machine: replays the pinned capture onto the wire at its recorded
# rate, fragmented to the link MTU exactly as an Ouster sensor's 24.8 KB
# datagrams are, addressed to the receiver.
#   usage: sudo scripts/external_tx.sh <iface> <rx-ip> <rx-mac> [loops]
# Needs tcpreplay (tcprewrite with fragroute support). One loop is one 10 Hz
# frame, so 36000 loops is one hour of decisions.
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
IFACE=${1:?iface}
RXIP=${2:?receiver ip}
RXMAC=${3:?receiver mac}
LOOPS=${4:-36000}
CAP=data/OS-2-128-U1_v2.3.0_1024x10
TXIP=$(ip -4 -o addr show "$IFACE" | awk '{print $4}' | cut -d/ -f1)
TXMAC=$(cat "/sys/class/net/$IFACE/address")
MTU=$(cat "/sys/class/net/$IFACE/mtu")
mkdir -p build/tx
echo "ip_frag $((MTU - 20))" > build/tx/frag.conf
tcprewrite --infile="$CAP.pcap" --outfile=build/tx/rewritten.pcap \
  --enet-smac="$TXMAC" --enet-dmac="$RXMAC" \
  --srcipmap=0.0.0.0/0:"$TXIP"/32 --dstipmap=0.0.0.0/0:"$RXIP"/32 \
  --fixcsum --fragroute=build/tx/frag.conf
echo "sending $LOOPS loops of $(tcpcapinfo build/tx/rewritten.pcap 2> /dev/null | grep -c Packet || echo '?') packets"
tcpreplay --intf1="$IFACE" --preload-pcap --loop="$LOOPS" --stats=60 build/tx/rewritten.pcap
