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
config_preexisting=false
if [ -e "$install_directory/config.yml" ]; then
  config_preexisting=true
fi
cleanup() {
  rm -rf "$temporary_directory"
}
trap cleanup EXIT

release_bases=(
  "${BATCAN_RELEASE_BASE_URL:-https://gitwarp.canghai.org/github.com/chaeoi/batcan/releases/latest/download}"
  "https://github.com/chaeoi/batcan/releases/latest/download"
)
cache_buster="$(date +%s)-$$"
download_release() {
  local name="$1"
  local destination="$2"
  local base
  for base in "${release_bases[@]}"; do
    if curl --connect-timeout 8 --max-time 20 --fail --location \
      --silent --show-error --retry 1 \
      "$base/$name?batcan_cache=$cache_buster" -o "$destination"; then
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

config_created=false
if [ ! -e "$install_directory/config.yml" ]; then
  # 优先采用传入的 BATCAN_INTERFACE，否则使用默认值 can5
  selected_interface="${BATCAN_INTERFACE:-can5}"
  case "$selected_interface" in
    *[!a-zA-Z0-9_-]*)
      echo "BATCAN_INTERFACE 只能包含字母、数字、下划线或短横线。" >&2
      exit 1
      ;;
  esac
  # 运行二进制输出默认全开的 auto 配置
  "$binary" service install --force-config >/dev/null 2>&1 || true
  if [ -n "${BATCAN_INTERFACE:-}" ] && [ -f "$install_directory/config.yml" ]; then
    sed -i "s/^interface:.*/interface: $selected_interface/" "$install_directory/config.yml"
  fi
  config_created=true
fi

"$binary" service install
echo "batcan 安装完成：$binary"
if [ "$config_created" = true ]; then
  echo "已生成默认配置（开启 auto 自动探测全部协议，默认接口：${selected_interface}）：$install_directory/config.yml"
  echo "提示：请根据实际设备检查并修改 interface 接口名称，若不需要探测部分 BMS，直接在 profile 列表中删除对应 UUID 行即可。"
elif [ "$config_preexisting" = true ]; then
  echo "已保留现有配置：$install_directory/config.yml"
fi
