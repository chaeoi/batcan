#!/bin/sh
set -eu

repo=${BATCAN_REPO:-chaeoi/batcan}
version=${BATCAN_VERSION:-latest}
robot_model=

usage() {
	cat >&2 <<'EOF'
Usage: install.sh --robot-model MODEL [--version TAG]
EOF
}

die() {
	echo "batcan installer: $*" >&2
	exit 1
}

while [ "$#" -gt 0 ]; do
	case "$1" in
		--robot-model)
			[ "$#" -ge 2 ] || die "--robot-model requires a value"
			robot_model=$2
			shift 2
			;;
		--version)
			[ "$#" -ge 2 ] || die "--version requires a value"
			version=$2
			shift 2
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			usage
			die "unknown option: $1"
			;;
	esac
done

[ "$(id -u)" -eq 0 ] || die "run as root"
[ -n "$robot_model" ] || die "--robot-model is required"
printf '%s\n' "$robot_model" | grep -Eq '^[A-Za-z0-9._-]{1,64}$' || die "invalid robot model"
command -v curl >/dev/null 2>&1 || die "curl is required"
command -v sha256sum >/dev/null 2>&1 || die "sha256sum is required"

case "$(uname -m)" in
	x86_64|amd64) arch=amd64 ;;
	aarch64|arm64) arch=arm64 ;;
	*) die "unsupported Linux architecture: $(uname -m)" ;;
esac

tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
asset="batcan-linux-$arch"
if [ "$version" = latest ]; then
	base_url="https://github.com/$repo/releases/latest/download"
else
	base_url="https://github.com/$repo/releases/download/$version"
fi
curl --fail --location --silent --show-error "$base_url/$asset" -o "$tmp_dir/$asset"
curl --fail --location --silent --show-error "$base_url/SHA256SUMS" -o "$tmp_dir/SHA256SUMS"
(cd "$tmp_dir" && grep "  $asset\$" SHA256SUMS | sha256sum -c -) || die "release checksum verification failed"
chmod 0755 "$tmp_dir/$asset"
"$tmp_dir/$asset" service install --robot-model "$robot_model"
