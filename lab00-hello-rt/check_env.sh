#!/bin/bash
# check_env.sh - is this account ready for real-time work?
#
# Run once after your first login. Every line ends in OK or a hint.
# Nothing here needs sudo and nothing is modified.

ok()   { printf '  [OK]   %s\n' "$1"; }
warn() { printf '  [WARN] %s\n' "$1"; }

echo "Kernel"
echo "  $(uname -r)  $(uname -v)"
if grep -q PREEMPT_RT <<<"$(uname -v)"; then
    ok "PREEMPT_RT kernel"
else
    warn "not a PREEMPT_RT kernel; ask the instructor"
fi
if [ -f /.dockerenv ]; then
    warn "running inside the local Docker image: fine for building and testing, not for latency numbers"
fi
if [ "$(cat /sys/kernel/realtime 2>/dev/null)" = "1" ]; then
    ok "/sys/kernel/realtime = 1"
else
    warn "/sys/kernel/realtime is not 1"
fi

echo
echo "Real-time limits for user $USER"
rtprio=$(ulimit -r)
memlock=$(ulimit -l)
echo "  ulimit -r (max RT priority) = $rtprio"
echo "  ulimit -l (memlock, kB)     = $memlock"
if [ "$rtprio" -ge 80 ] 2>/dev/null; then
    ok "you may use SCHED_FIFO up to priority $rtprio without sudo"
else
    warn "rtprio is $rtprio; SCHED_FIFO will fail with EPERM. Log out and back in, then ask the instructor"
fi
if [ "$memlock" = "unlimited" ] || [ "$memlock" -ge 65536 ] 2>/dev/null; then
    ok "memlock is enough for mlockall() in lab programs"
else
    warn "memlock is small; mlockall() may fail"
fi

echo
echo "RT throttling (kernel safety net, expected 950000 = 95% of a CPU)"
echo "  kernel.sched_rt_runtime_us = $(cat /proc/sys/kernel/sched_rt_runtime_us)"

echo
echo "Tools"
for t in g++ make gdb cyclictest chrt stress-ng tmux htop; do
    if command -v "$t" >/dev/null 2>&1; then
        ok "$t"
    else
        warn "$t not found"
    fi
done

echo
echo "sudo"
echo "  not needed for any lab. On the shared VM only 'sudo trace-cmd' is allowed."

echo
echo "CPUs visible: $(nproc)   load now: $(cut -d' ' -f1-3 /proc/loadavg)"
echo "Done. If everything is OK, continue with: make && ./hello_rt"
