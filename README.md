# batcan

`batcan` 是一个 SocketCAN 到 ROS 2 的电池数据桥接程序。它从 BMS 的 CAN 总线读取电池信息，并在固定主题 `/bms_can/battery_data` 发布标准消息 `sensor_msgs/msg/BatteryState`。

## 目录架构

```text
.
├── config.example.yml      # 运行配置示例，只选择型号
├── cmake/
│   └── embedded_models.hpp.in # 二进制资源嵌入模板
├── deploy/
│   └── install.sh          # Release 安装脚本
├── doc/
│   ├── KVMS-内网通信CAN协议-客户版.xlsx
│   └── HTBMS-CAN协议-V1.1.0-20260630.docx
├── include/batcan/         # 头文件与核心数据结构
├── models/
│   ├── README.md            # 型号配置格式说明
│   └── 2m_v0.1.2.yml       # 独立机器人型号配置
├── src/
│   ├── main.cpp            # 命令行入口和 ROS 2 运行入口
│   ├── config.cpp          # 运行配置解析
│   ├── models.cpp          # 通用型号配置加载与校验
│   ├── bridge.cpp          # SocketCAN 采集和 ROS 2 消息发布
│   ├── protocol.cpp        # CAN 报文解析
│   └── service.cpp         # systemd 服务安装与管理
├── tests/                  # 配置和协议解析测试
├── CMakeLists.txt          # ROS 2 软件包定义
└── package.xml             # ROS 2 软件包元数据
```

## 功能

- 通过 SocketCAN 连接 BMS，按型号配置发送查询或接收广播。
- 将电压、电流、温度、剩余电量和供电状态转换为 `sensor_msgs/msg/BatteryState`。
- 在固定主题 `/bms_can/battery_data` 发布数据，电量比例采用 0 到 1 的标准范围。
- 仓库中的每个型号使用独立 `models/*.yml` 文件维护 CAN 接口、查询帧、响应 ID 和字段解析规则。
- 查询、广播接收、响应匹配和字段解码均使用通用代码，不包含特定型号分支。
- 打包时会把所有型号配置编入同一个二进制；目标设备的 `config.yml` 只需选择 `model`，不需要部署型号文件。
- 提供 systemd 服务，支持开机启动和异常重启；服务仅授予访问 CAN 所需的 `CAP_NET_RAW` 权限。

## 支持的型号

当前 Release 支持以下配置：

| 机器人型号 | BMS 型号 | CAN 接口 | 采集方式 |
| --- | --- | --- | --- |
| `2m_v0.1.2` | `KVMS` | `can5` | 每 2 秒发送查询帧 |

`2m_v0.1.2` 使用查询帧 `0x0400FF80`，读取总压/电流/SOC、温度和状态响应。响应 ID 会匹配协议中的 BMS 板号通配符；静止状态会发布为 `NOT_CHARGING`。协议字段的字节序、倍率和偏移以 `doc/KVMS-内网通信CAN协议-客户版.xlsx` 为准。

`doc/HTBMS-CAN协议-V1.1.0-20260630.docx` 是另一种 BMS 协议的原始资料。该资料中的 CAN 速率和设备地址需要结合具体机器人确认，因此当前 Release 尚未将其关联到机器人型号。

## 部署

### 前置条件

- 目标设备为 Linux `amd64` 或 `arm64` 架构。
- 已安装 ROS 2 Humble，路径为 `/opt/ros/humble`。
- 系统存在 `ubuntu` 用户，且 BMS 使用的 `can5` 接口已经由系统配置并可用。
- 设备可访问 GitHub，且已安装 `curl` 和 `sha256sum`。

### 安装 Release

安装最新 Release：

```bash
curl -fsSL https://raw.githubusercontent.com/chaeoi/batcan/main/deploy/install.sh \
  | sudo sh
```

指定版本时：

```bash
curl -fsSL https://raw.githubusercontent.com/chaeoi/batcan/main/deploy/install.sh \
  | sudo sh -s -- --version v0.1.0
```

安装脚本会下载包含全部型号配置的单一 Release 二进制、校验 SHA-256、安装可执行文件并注册 systemd 服务。首次安装会生成一份所有支持型号均被注释的配置文件，选择型号后再启动服务。

默认路径如下：

| 文件 | 路径 |
| --- | --- |
| 安装目录 | `/opt/batcan` |
| 可执行文件 | `/opt/batcan/batcan` |
| 配置文件 | `/opt/batcan/config.yml` |
| systemd 服务 | `/etc/systemd/system/batcan.service` |
| ROS 2 日志 | `/var/log/batcan/ros` |

### 配置与启动

安装后编辑配置：

```bash
sudoedit /opt/batcan/config.yml
```

默认内容类似下面这样。每个已编入二进制的型号对应一行，取消目标行前的 `#` 即可；只能保留一个有效的 `model`：

```yaml
# model: 2m_v0.1.2
```

检查配置并启动服务：

```bash
sudo /opt/batcan/batcan --check-config --config /opt/batcan/config.yml
sudo systemctl enable --now batcan
```

如果 `/opt/batcan/config.yml` 已存在且格式有效，安装或升级时会保留现有配置；如果文件不存在或格式无效，会直接重写为新的默认配置。

### 服务管理

```bash
sudo /opt/batcan/batcan service status
sudo systemctl restart batcan
sudo systemctl stop batcan
sudo /opt/batcan/batcan service uninstall
```

卸载会停止并移除 systemd 服务及日志，保留 `/opt/batcan` 目录中的程序和配置，便于后续重新注册服务。
