#!/usr/bin/env bash
# TLBanalyser self-test: proves end-to-end that the tool measures and
# attributes TLB shootdowns correctly on THIS host, and diagnoses the
# environment when it cannot.  Run as root:  sudo ./selftest.sh
set -u
cd "$(dirname "$0")"

PASS=0; FAIL=0; WARN=0
ok()   { printf '  [PASS] %s\n' "$1"; PASS=$((PASS+1)); }
bad()  { printf '  [FAIL] %s\n' "$1"; FAIL=$((FAIL+1)); }
warn() { printf '  [WARN] %s\n' "$1"; WARN=$((WARN+1)); }
info() { printf '  [info] %s\n' "$1"; }

echo "== TLBanalyser self-test =="
[ "$(id -u)" = 0 ] || { echo "must run as root"; exit 1; }

echo "-- environment"
info "kernel $(uname -r), $(nproc) CPUs, $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
virt=$(systemd-detect-virt 2>/dev/null); [ -n "${virt:-}" ] && [ "$virt" != none ] && info "virtualization: $virt"
cont=$(systemd-detect-virt -c 2>/dev/null); [ -n "${cont:-}" ] && [ "$cont" != none ] && \
    warn "running inside a container ($cont) - tracefs/PMU may be blocked by the runtime"
[ -r /sys/kernel/security/lockdown ] && info "lockdown: $(cat /sys/kernel/security/lockdown)"

if [ -d /sys/kernel/tracing/events ] || mount -t tracefs tracefs /sys/kernel/tracing 2>/dev/null; then
    ok "tracefs mounted at /sys/kernel/tracing"
else
    bad "tracefs cannot be mounted (container without CAP_SYS_ADMIN, or kernel lacks tracefs)"
fi
if [ -f /sys/kernel/tracing/events/tlb/tlb_flush/id ]; then
    ok "tlb:tlb_flush tracepoint present (id $(cat /sys/kernel/tracing/events/tlb/tlb_flush/id))"
else
    bad "tlb:tlb_flush tracepoint missing - attribution impossible on this kernel"
    info "available event groups: $(ls /sys/kernel/tracing/events 2>/dev/null | tr '\n' ' ' | cut -c1-120)"
fi
grep -q '^ *TLB:' /proc/interrupts && ok "TLB shootdown row in /proc/interrupts" \
                                    || bad "no TLB row in /proc/interrupts"

echo "-- build"
make -s tlbanalyser 2>/dev/null || { bad "build failed - run make for details"; exit 1; }
cc -O2 -o test/shootgen test/shootgen.c -lpthread || { bad "shootgen build failed"; exit 1; }
ok "built tlbanalyser and test/shootgen"

echo "-- baseline (4 s idle capture)"
BASE=$(./tlbanalyser -b -n 2 -d 2 2>/dev/null)
base_recv=$(echo "$BASE" | awk '/tlb_recv_ipi/ {gsub(/[^0-9.]/,"",$2); s+=$2; n++} END {printf "%d", n? s/n : 0}')
echo "$BASE" | grep -q 'ipc ' && ok "PMU counters readable (see l1d/l2/dtlb above in output)" \
                              || warn "PMU counters unavailable (vPMU-restricted instance? /proc panels still exact)"
if ! echo "$BASE" | grep -q 'send_ev/s'; then
    bad "tracepoint capture inactive - the attribution test below will fail"
fi
info "baseline shootdown IPIs: ${base_recv}/s"

echo "-- storm: known workload must be caught and attributed"
./test/shootgen 10 > /tmp/shootgen.$$ &
GEN=$!
sleep 1
STORM=$(./tlbanalyser -b -n 3 -d 2 -t 8 2>/dev/null)
wait "$GEN" 2>/dev/null
GENPID=$(awk '/^shootgen pid/ {print $3; exit}' /tmp/shootgen.$$)
rm -f /tmp/shootgen.$$

echo "$STORM" | tail -14 | sed 's/^/    | /'

storm_recv=$(echo "$STORM" | awk '/tlb_recv_ipi/ {gsub(/[^0-9.]/,"",$2); if ($2>m) m=$2} END {printf "%d", m+0}')
lost=$(echo "$STORM" | grep -o 'lost [0-9]*' | awk '{s+=$2} END {print s+0}')
gen_send=$(echo "$STORM" | awk -v p="$GENPID" '$1==p {if ($3>m) m=$3} END {printf "%d", m+0}')
gen_origin=$(echo "$STORM" | awk -v p="$GENPID" '$1==p {print $NF; exit}')

if [ "${storm_recv:-0}" -gt $(( base_recv * 5 + 500 )) ]; then
    ok "shootdown IPIs surged under load: ${base_recv}/s -> ${storm_recv}/s (/proc/interrupts, exact)"
else
    bad "expected an IPI surge, got ${base_recv}/s -> ${storm_recv}/s"
fi
if [ "${gen_send:-0}" -gt 100 ]; then
    ok "shootgen (pid $GENPID) attributed as sender at ${gen_send} sends/s"
else
    bad "shootgen (pid $GENPID) not attributed as a top sender (got '${gen_send:-none}')"
fi
if [ -n "${gen_origin:-}" ] && [ "$gen_origin" != "-" ]; then
    ok "kernel origin resolved for shootgen: $gen_origin"
else
    bad "no kernel origin resolved for shootgen"
fi
if [ "${lost:-0}" -eq 0 ]; then
    ok "zero lost trace events during the storm (capture is complete)"
else
    bad "$lost trace events lost - attribution incomplete at this load"
fi

echo "== result: $PASS passed, $FAIL failed, $WARN warnings =="
[ "$FAIL" -eq 0 ]
