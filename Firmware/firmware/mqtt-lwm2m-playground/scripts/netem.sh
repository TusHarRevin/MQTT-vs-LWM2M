#!/usr/bin/env sh
# Real kernel-level network impairment with Linux tc/netem (Linux or WSL2,
# needs root). Only traffic to/from the playground's server ports is affected,
# so the dashboard stays responsive.
#
#   sudo scripts/netem.sh on 100 5      # 100 ms each way, 5 % loss
#   sudo scripts/netem.sh off
#
# Delay is applied in both directions (on loopback every packet crosses `lo`
# once), so a request/response round trip gains 2 x delay - the same model as
# the built-in app-level simulator. When netem is on, set the dashboard's
# network controls to 0 ms / 0 % or you will impair twice.
set -e
DEV=${DEV:-lo}
PORTS=${PORTS:-"1883 8883 5683 5684"}
case "$1" in
on)
    DELAY=${2:-100}; LOSS=${3:-0}
    tc qdisc del dev "$DEV" root 2>/dev/null || true
    tc qdisc add dev "$DEV" root handle 1: prio bands 4
    tc qdisc add dev "$DEV" parent 1:4 handle 40: netem delay "${DELAY}ms" loss "${LOSS}%"
    for p in $PORTS; do
        tc filter add dev "$DEV" protocol ip parent 1:0 prio 1 u32 match ip dport "$p" 0xffff flowid 1:4
        tc filter add dev "$DEV" protocol ip parent 1:0 prio 1 u32 match ip sport "$p" 0xffff flowid 1:4
    done
    echo "netem on $DEV: ${DELAY} ms, ${LOSS} % loss for ports $PORTS"
    ;;
off)
    tc qdisc del dev "$DEV" root 2>/dev/null || true
    echo "netem off"
    ;;
show)
    tc qdisc show dev "$DEV"; tc filter show dev "$DEV"
    ;;
*)
    echo "usage: $0 on <delay-ms> <loss-%> | off | show" >&2; exit 2
    ;;
esac
