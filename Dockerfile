ARG ARCH=aarch64
# The openvpn3-base image (Dockerfile.openvpn3) provides the OpenVPN3 source
# tree at /src/openvpn3, asio at /src/deps/asio, static lz4 at /opt/ovpn3-deps,
# and the cross toolchain at /opt/ovpn3/toolchain.cmake. Declared here (global,
# before any FROM) so the builder-stage FROM can resolve it.
ARG OVPN3_BASE=openvpn3-base-${ARCH}

# ── Go build stage: the userspace netstack + proxy sidecar ─────────────────
FROM docker.io/golang:1.22 AS gobuilder
ARG ARCH
ENV CGO_ENABLED=0
COPY ./app/netstack /src/netstack
WORKDIR /src/netstack
RUN case "$ARCH" in \
        aarch64) export GOARCH=arm64 ;; \
        armv7hf) export GOARCH=arm GOARM=7 ;; \
    esac && \
    go build -ldflags='-s -w' -o /netstack_proxy . && \
    ls -l /netstack_proxy

# hadolint ignore=DL3006
FROM ${OVPN3_BASE} AS builder
ARG ARCH

# ── build the OpenVPN3 client (tun_probe) ──────────────────────────────────
# Copy ONLY the probe source first so this expensive compile is cached across
# changes to the UI / manifest / bridge / sidecar. The core cannot be a
# standalone lib, so we add tun_probe as a target inside the openvpn3 tree
# (mirroring the ovpncli sample: same core deps + xkey PKI helper) and compile
# it together with the core. USE_TUN_BUILDER routes packets to our
# TunBuilderBase callbacks instead of a kernel /dev/net/tun.
COPY ./app/probe /opt/app/probe
# The SDK 12.10 OpenSSL (libcrypto/libssl) is built with the GCC 13 / glibc 2.38
# C23 redirect, so it imports __isoc23_strtol / __isoc23_strtoul / __isoc23_sscanf
# @ GLIBC_2.38. Linking OpenSSL hoists those symbols into tun_probe's dynsym and
# would cap the floor at OS 12.10 (glibc 2.38) even though the binary is otherwise
# OS 13 ready. isoc23_compat.c defines local forwarders that satisfy those
# references with the plain GLIBC_2.17 symbols, restoring the OS 11 floor.
RUN cp /opt/app/probe/tun_probe.cpp /src/openvpn3/test/ovpncli/tun_probe.cpp && \
    cp /opt/app/probe/isoc23_compat.c /src/openvpn3/test/ovpncli/isoc23_compat.c && \
    { \
      echo ''; \
      echo 'add_executable(tun_probe tun_probe.cpp isoc23_compat.c)'; \
      echo 'target_compile_definitions(tun_probe PRIVATE USE_TUN_BUILDER)'; \
      echo 'target_compile_options(tun_probe PRIVATE -O1)'; \
      echo 'add_core_dependencies(tun_probe)'; \
      echo 'target_link_libraries(tun_probe xkey pthread dl)'; \
    } >> /src/openvpn3/test/ovpncli/CMakeLists.txt

ARG MAKE_JOBS=1
# PKG_CONFIG_PATH points OpenVPN3's pkg_check_modules(openssl) at our static
# OpenSSL 3.5; PKG_CONFIG_SYSROOT_DIR is cleared so the .pc's absolute
# /opt/ovpn3-deps paths are used verbatim (not re-prefixed with the sysroot).
# Only libcrypto.a/libssl.a exist there, so the linker statically bundles them.
RUN . /opt/axis/acapsdk/environment-setup* && \
    cd /src/openvpn3 && \
    PKG_CONFIG_PATH=/opt/ovpn3-deps/lib/pkgconfig PKG_CONFIG_SYSROOT_DIR= \
    cmake -B build \
        -DCMAKE_TOOLCHAIN_FILE=/opt/ovpn3/toolchain.cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=OFF \
        -DASIO_INCLUDE_DIR=/src/deps/asio/asio/include \
        -DLZ4_INCLUDE_DIR=/opt/ovpn3-deps/include \
        -DLZ4_LIBRARY=/opt/ovpn3-deps/lib/liblz4.a && \
    cmake --build build -j"${MAKE_JOBS}" --target tun_probe && \
    mkdir -p /opt/app/lib && \
    cp "$(find /src/openvpn3/build -name tun_probe -type f | head -n1)" /opt/app/lib/tun_probe && \
    ${STRIP:-strip} /opt/app/lib/tun_probe || true; \
    ls -l /opt/app/lib/tun_probe

# ── build + package the ACAP ───────────────────────────────────────────────
# The Makefile builds only the small C bridge; lib/tun_probe (C++) and
# lib/netstack_proxy (Go) are prebuilt and copied in here.
COPY ./app /opt/app/
COPY --from=gobuilder /netstack_proxy /opt/app/lib/netstack_proxy
WORKDIR /opt/app
RUN chmod 755 lib/netstack_proxy && \
    sed -i "s/\"BUILDARCH\"/\"${ARCH}\"/" manifest.json && \
    . /opt/axis/acapsdk/environment-setup* && acap-build .

FROM scratch
COPY --from=builder /opt/app/*eap /
