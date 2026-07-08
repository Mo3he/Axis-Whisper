#!/usr/bin/env sh
# Thin wrapper around aarch64/Dockerfile. Whisper is aarch64-only (the ML
# workload is not practical on 32-bit armv7hf), so there is a single target.
#
#   ./build.sh
#
# Override the container runtime with RUNTIME=docker|podman if autodetect is
# wrong.
set -eu

REPO_ROOT=$(cd -P "$(dirname "$0")" && pwd)

# Auto-detect container runtime: prefer docker when its daemon is reachable,
# otherwise fall back to podman.
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

TAG="whisper-acap-build-$$"

echo '==> Cleaning old .eap files...'
rm -f "${REPO_ROOT}"/*.eap

echo '==> Building aarch64 (aarch64/Dockerfile)...'
"$RUNTIME" build -f "${REPO_ROOT}/aarch64/Dockerfile" -t "$TAG" "$REPO_ROOT"

# Copy the packaged .eap out of the built image via a throwaway container.
CID=$("$RUNTIME" create "$TAG")
TMP=$(mktemp -d)
"$RUNTIME" cp "${CID}:/opt/app/." "$TMP/" >/dev/null 2>&1 ||
	"$RUNTIME" cp "${CID}:/opt/app" "$TMP/"
find "$TMP" -name '*.eap' -exec cp {} "${REPO_ROOT}/" \;
rm -rf "$TMP"
"$RUNTIME" rm -f "$CID" >/dev/null 2>&1 || true
"$RUNTIME" rmi -f "$TAG" >/dev/null 2>&1 || true

echo '==> Done!'
ls -lh "${REPO_ROOT}"/*.eap
