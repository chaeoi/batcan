#!/bin/bash
set -eu

if [ "$(id -u)" -ne 0 ]; then
  echo "请使用 sudo 运行此脚本。" >&2
  exit 1
fi

command -v curl >/dev/null 2>&1 || {
  echo "缺少 curl，请先安装 curl。" >&2
  exit 1
}
command -v systemctl >/dev/null 2>&1 || {
  echo "缺少 systemd，请在 Ubuntu 主机上运行此脚本。" >&2
  exit 1
}

case "$(uname -m)" in
  aarch64|arm64) asset=batcan-linux-arm64 ;;
  x86_64|amd64) asset=batcan-linux-amd64 ;;
  *)
    echo "不支持的架构：$(uname -m)" >&2
    exit 1
    ;;
esac

install_directory=/opt/batcan
binary="$install_directory/batcan"
temporary_directory="$(mktemp -d /tmp/batcan-install.XXXXXX)"
cleanup() {
  rm -rf "$temporary_directory"
}
trap cleanup EXIT

release_bases=(
  "${BATCAN_RELEASE_BASE_URL:-https://github.com/chaeoi/batcan/releases/latest/download}"
  "https://gitwarp.canghai.org/github.com/chaeoi/batcan/releases/latest/download"
)
download_release() {
  local name="$1"
  local destination="$2"
  local base
  for base in "${release_bases[@]}"; do
    if curl --connect-timeout 8 --max-time 20 --fail --location \
      --silent --show-error --retry 1 \
      "$base/$name" -o "$destination"; then
      return 0
    fi
    rm -f "$destination"
  done
  echo "无法下载发布文件：$name" >&2
  exit 1
}

download_release "$asset" "$temporary_directory/batcan"
download_release SHA256SUMS "$temporary_directory/SHA256SUMS"
expected_hash="$(awk -v asset="$asset" '$2 == asset {print $1; exit}' \
  "$temporary_directory/SHA256SUMS")"
if [ -z "$expected_hash" ]; then
  echo "发布校验文件中没有 $asset" >&2
  exit 1
fi
actual_hash="$(sha256sum "$temporary_directory/batcan" | awk '{print $1}')"
if [ "$expected_hash" != "$actual_hash" ]; then
  echo "发布文件校验失败" >&2
  exit 1
fi

chmod 0755 "$temporary_directory/batcan"
mkdir -p "$install_directory"
mv -f "$temporary_directory/batcan" "$binary"

if [ ! -e "$install_directory/config.yml" ] && [ -n "${BATCAN_INTERFACE:-}" ]; then
  case "$BATCAN_INTERFACE" in
    *[!a-zA-Z0-9_.:-]*)
      echo "BATCAN_INTERFACE 只能包含字母、数字、下划线、点、冒号或短横线。" >&2
      exit 1
      ;;
  esac
  cat > "$temporary_directory/config.yml" <<EOF
profile: auto
profiles: 98b8d1c1-6a34-45a4-9687-e9a09ef20204,fc3da911-07a0-42b3-8cb4-1aa8dd26b558,d7a1d64a-6671-4ee2-8fbd-859043083a68
interface: $BATCAN_INTERFACE
EOF
  mv -f "$temporary_directory/config.yml" "$install_directory/config.yml"
fi

"$binary" service install
echo "batcan 安装完成：$binary"
if [ -n "${BATCAN_INTERFACE:-}" ]; then
  echo "已使用接口 $BATCAN_INTERFACE 生成自动识别配置。"
else
  echo "请编辑 $install_directory/config.yml，填写 interface 和 profile 后启动服务。"
fi
