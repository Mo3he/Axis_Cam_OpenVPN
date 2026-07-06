#!/usr/bin/env sh
# Build the OpenVPN ACAP for aarch64 and armv7hf.
#
# Two stages:
#   1. openvpn3-base-<arch>  — cross-compiled OpenVPN3 core (slow, cached)
#   2. the ACAP .eap         — C bridge + C++ client + Go netstack, packaged
#
# The base image is only rebuilt when Dockerfile.openvpn3 changes.
set -eu

REPO_ROOT=$(cd -P "$(dirname "$0")" && pwd)

if [ -z "${RUNTIME:-}" ]; then
	if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
		RUNTIME=docker
	elif command -v podman >/dev/null 2>&1; then
		RUNTIME=podman
	elif command -v docker >/dev/null 2>&1; then
		RUNTIME=docker
	else
		echo 'Error: neither docker nor podman found in PATH' >&2
		exit 1
	fi
fi
echo "==> Using container runtime: ${RUNTIME}"

ARCHES="${ARCHES:-aarch64 armv7hf}"

echo '==> Cleaning old .eap files...'
rm -f "${REPO_ROOT}"/*.eap

build_base() {
	ARCH=$1
	echo "==> Building openvpn3-base-${ARCH} (slow, first time only)..."
	"$RUNTIME" build \
		--build-arg ARCH="$ARCH" \
		-t "openvpn3-base-${ARCH}" \
		-f "${REPO_ROOT}/Dockerfile.openvpn3" \
		"$REPO_ROOT"
}

# Resolve the base image reference. Podman on macOS cannot use a bare local
# image name as a Dockerfile FROM (it tries a registry), so use the image ID.
base_ref() {
	ARCH=$1
	if [ "$RUNTIME" = "podman" ]; then
		"$RUNTIME" image inspect --format '{{.Id}}' \
			"localhost/openvpn3-base-${ARCH}" 2>/dev/null
	else
		echo "openvpn3-base-${ARCH}"
	fi
}

build_acap() {
	ARCH=$1
	BASE=$(base_ref "$ARCH")
	echo "==> Building .eap for ${ARCH} (base ${BASE})..."
	if [ "$RUNTIME" = "podman" ] && [ "$(uname -s)" = "Darwin" ]; then
		TAG="ovpn-build-${ARCH}-$$"
		"$RUNTIME" build \
			--build-arg ARCH="$ARCH" \
			--build-arg OVPN3_BASE="$BASE" \
			-t "$TAG" "$REPO_ROOT"
		CID=$("$RUNTIME" create "$TAG")
		"$RUNTIME" cp "${CID}":/ "${REPO_ROOT}/"
		"$RUNTIME" rm -f "$CID" >/dev/null 2>&1 || true
		"$RUNTIME" rmi -f "$TAG" >/dev/null 2>&1 || true
	else
		DOCKER_BUILDKIT=1 "$RUNTIME" build \
			--build-arg ARCH="$ARCH" \
			--build-arg OVPN3_BASE="$BASE" \
			-o type=local,dest="${REPO_ROOT}" \
			"$REPO_ROOT"
	fi
}

for ARCH in $ARCHES; do
	build_base "$ARCH"
	build_acap "$ARCH"
done

echo '==> Done!'
ls -lh "$REPO_ROOT"/*.eap 2>/dev/null || echo 'No .eap produced — check build output above.'
