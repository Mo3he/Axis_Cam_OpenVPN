#!/usr/bin/env sh
# Unit tests for the OpenVPN netstack helper: camera dial.
# Run by CI (build.yml) and by acap-build.sh before every release.
set -eu
cd "$(dirname "$0")/../app/netstack"
unformatted=$(gofmt -l .)
if [ -n "$unformatted" ]; then
	echo "not gofmt-clean:" >&2
	echo "$unformatted" >&2
	exit 1
fi
go vet ./...
go test -race -count=1 ./...
