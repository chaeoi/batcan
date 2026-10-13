# batcan

`batcan` 是一个运行在 ROS 2 Humble 上的 BMS 电池数据桥接节点。它通过 Linux SocketCAN 接口收发 CAN 帧，自动解析常见 BMS 的电压、电流、温度、SOC、单体电压和故障状态，并发布为 ROS 2 诊断消息（`/batcan/data`）。

---

## 支持的 BMS 协议与 UUID

程序使用固定 UUID 标识协议。目前支持的协议如下：

| 协议名称 | BMS 协议 / 适用型号 | CAN 特征 | 默认波特率 | Profile UUID |
| :--- | :--- | :--- | :---: | :--- |
| **KVMS** | KVMS（如心向阳 / 达锂 K系列定制协议） | 29 位扩展帧，主动轮询 | 250000 | `98b8d1c1-6a34-45a4-9687-e9a09ef20204` |
| **HTBMS** | HTBMS CAN V1.1.0 | 29 位扩展帧，被动广播 | 500000 | `fc3da911-07a0-42b3-8cb4-1aa8dd26b558` |
| **JBD** | JBD-compatible CANBUS（嘉佰达兼容） | 11 位标准帧，远程帧轮询 | 500000 | `d7a1d64a-6671-4ee2-8fbd-859043083a68` |

---

## 配置文件说明

运行时配置文件固定位于 `/opt/batcan/config.yml`，格式极简，**仅包含以下三项**：

```yaml
# 1. 自动探测模式: enable (开启) 或 disable (关闭)
auto: enable

# 2. BMS 协议 UUID:
#    - auto 开启时: 支持 1 个或多个 UUID，删除不用的行即可
#    - auto 关闭时: 严格只允许有且仅有 1 个 UUID
profile:
  - 98b8d1c1-6a34-45a4-9687-e9a09ef20204 # KVMS
  - fc3da911-07a0-42b3-8cb4-1aa8dd26b558 # HTBMS CAN V1.1.0
  - d7a1d64a-6671-4ee2-8fbd-859043083a68 # JBD CANBUS-compatible

# 3. SocketCAN 接口名称 (必填项)
interface: can5
```

### 两种使用方式

1. **自动探测模式（推荐，安装后默认配置）**：
   - 设置 `auto: enable`。
   - `profile` 列表中默认包含所有支持的 UUID。
   - **如果某些协议确认用不到，直接删掉对应 UUID 即可**，程序会在剩余候选中轮询探测电池。
2. **固定指定模式**：
   - 设置 `auto: disable`。
   - `profile` 必须**有且仅有 1 个 UUID**（写多个将直接报错拒绝启动）。例如：
     ```yaml
     auto: disable
     profile: 98b8d1c1-6a34-45a4-9687-e9a09ef20204
     interface: can0
     ```

---

## 安装与快速上手

### 1. 一键安装

```bash
curl -fsSL https://gitwarp.canghai.org/raw.githubusercontent.com/chaeoi/batcan/main/script/install.sh | sudo bash
```

- 脚本会自动识别机器架构（ARM64 / AMD64）并安装 `batcan` 与 systemd 服务。
- **首次安装后默认全开 auto 探测模式**，自动启动服务。
- 安装后请根据设备实际情况确认 CAN 接口名称（默认 `can5`，若为 `can0` 请在配置中修改）。

### 2. 检查与启动

```bash
# 检查配置文件格式
sudo /opt/batcan/batcan --check-config --config /opt/batcan/config.yml

# 重启或查看服务状态
sudo systemctl restart batcan
systemctl status batcan
```

### 3. 查看 ROS 2 数据

```bash
source /opt/ros/humble/setup.bash
ros2 topic echo /batcan/data --once
```

---

## 运维与更新

程序将更新逻辑完全内置于单一二进制中，无需任何外部脚本：

```bash
# 手动触发版本检查与原地热更
sudo /opt/batcan/batcan service update

# 查看服务状态
batcan service status

# 卸载 systemd 服务（保留 /opt/batcan 目录及配置）
sudo /opt/batcan/batcan service uninstall
```

服务运行期间也会在后台定期（启动 30 秒后及后续每 24 小时）自动检测 GitHub 最新发布，自动验证校验和并原地无缝替换更新。
