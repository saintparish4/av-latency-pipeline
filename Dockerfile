# Pinned toolchain. The base image is pinned by digest and every apt package
# is resolved against the Ubuntu archive snapshot of one day, so a rebuild
# next year installs the same clang-18 bytes. Python packages are pinned in
# requirements.txt, C++ dependencies by SHA-256 in CMakeLists.txt.
#
# Build and test inside it; measure on the host (or with --privileged and
# --cpuset-cpus on an isolated core), because latency belongs to the kernel
# and hardware, which a container shares rather than pins.
#   docker build -t avl .
#   docker run --rm --security-opt seccomp=unconfined -v "$PWD":/src -w /src avl \
#     scripts/reproduce.sh --check
# (seccomp=unconfined: TSan disables ASLR via personality(), which Docker's
# default seccomp profile refuses. libgl1, libegl1, libgomp1 and libusb-1.0-0
# are runtime dependencies the ouster-sdk and open3d wheels load at import.)
FROM ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852
ARG SNAPSHOT=20261029T000000Z
ENV DEBIAN_FRONTEND=noninteractive
# The snapshot service refuses connections under load now and then; retry.
RUN echo 'Acquire::Retries "5";' > /etc/apt/apt.conf.d/80-retries
# The snapshot archive is HTTPS-only, so ca-certificates comes from the live
# archive first; it is the one unpinned package and affects only TLS trust.
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates \
 && apt-get update --snapshot "$SNAPSHOT" \
 && apt-get install -y --no-install-recommends --snapshot "$SNAPSHOT" \
      clang-18 lld-18 llvm-18 libclang-rt-18-dev bolt-18 \
      clang-format-18 clang-tidy-18 shellcheck shfmt \
      cmake ninja-build git curl ca-certificates python3 python3-venv \
      linux-tools-common rt-tests tcpreplay linuxptp ethtool \
      libgl1 libegl1 libgomp1 libusb-1.0-0 \
 && rm -rf /var/lib/apt/lists/*
COPY requirements.txt /tmp/requirements.txt
RUN python3 -m venv /opt/venv && /opt/venv/bin/pip install --no-cache-dir -r /tmp/requirements.txt
ENV PATH=/opt/venv/bin:$PATH