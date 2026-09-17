# Local mirror of the shared Debian 13 PREEMPT_RT lab machine.
#
# The container shares the host kernel, so it is NOT a PREEMPT_RT system and
# latency numbers measured here mean little. Use it to build the labs, run the
# smoke tests and check that the programs behave; measure on the lab machine.

FROM debian:trixie

ENV DEBIAN_FRONTEND=noninteractive

# Same package set as the lab VM (infrastructure-ut, guest template 4.2/4.2b).
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential g++ make cmake gdb valgrind strace pkg-config \
        linux-perf rt-tests stress-ng trace-cmd tuna util-linux libcap2-bin \
        tmux htop procps vim-tiny less git tree jq ca-certificates \
        python3 python3-matplotlib python3-numpy \
    && rm -rf /var/lib/apt/lists/*

# Work as a normal user, like a student account on the VM. Real-time limits
# (rtprio 80, memlock 256 MiB) come from compose.yaml, not from sudo.
RUN useradd --create-home --shell /bin/bash --uid 1000 student
USER student
WORKDIR /workspace

CMD ["/bin/bash"]
