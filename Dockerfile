ARG ARCH=aarch64
# Base image from Dockerfile.openvpn3 (OpenVPN3 tree, asio, static deps, toolchain).
# Declared before any FROM so the builder-stage FROM can resolve it.
ARG OVPN3_BASE=openvpn3-base-${ARCH}

# ── Go build stage: the userspace netstack + proxy sidecar ─────────────────
FROM docker.io/golang:1.27.1 AS gobuilder
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
# Copy only the probe source first so this slow compile stays cached. The core
# can't be a standalone lib, so tun_probe is added as a target in the openvpn3
# tree; USE_TUN_BUILDER routes packets to our TunBuilderBase callbacks.
COPY ./app/probe /opt/app/probe
# isoc23_compat.c satisfies __isoc23_* @ GLIBC_2.38 references so tun_probe
# still loads below OS 12.10.
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
# Point pkg-config at the static OpenSSL; an empty PKG_CONFIG_SYSROOT_DIR keeps
# the .pc's absolute /opt/ovpn3-deps paths from being re-prefixed with the sysroot.
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
COPY ./app /opt/app/
COPY --from=gobuilder /netstack_proxy /opt/app/lib/netstack_proxy
WORKDIR /opt/app
RUN chmod 755 lib/netstack_proxy && \
    sed -i "s/\"BUILDARCH\"/\"${ARCH}\"/" manifest.json && \
    . /opt/axis/acapsdk/environment-setup* && acap-build .

FROM scratch
COPY --from=builder /opt/app/*eap /
