# batcan

`batcan` 是一个运行在 ROS 2 Humble 上的 BMS 电池数据桥接节点。它通过 Linux
SocketCAN 接口收发 CAN 帧，按照内置的 BMS 协议解析电压、电流、温度、SOC、容量、
单体电压、故障和充放电状态，然后发布为 ROS 2 的诊断消息。

它适合把不同型号的 BMS 接入同一个 ROS 2 系统。安装后只需要修改一个运行时配置文件，
即可切换协议和 CAN 接口。

## 功能与特性

- 支持 KVMS、HTBMS CAN V1.1.0 和 JBD-compatible CANBUS 三种协议。
- 支持 29 位扩展帧、11 位标准帧、主动查询、广播接收以及 JBD Modbus CRC-16 校验。
- 自动配置 SocketCAN 接口和协议所需的 CAN 波特率。
- 支持 `profile: auto` 自动探测 BMS；更换电池时可以不改程序，只调整候选列表。
- 自动模式检测到有效响应后会选择对应协议；连续一段时间没有有效帧时会重新探测。
- 一个 ROS 2 话题包含摘要和各类响应的完整解码结果，同时保留每个响应的原始 CAN 字节。
- 单体电压、单体温度、故障页等分页数据会保留页码，便于订阅者按需读取。
- 提供 ARM64 和 AMD64 Linux 发布包，可直接部署到目标机。
- 通过 systemd 管理，支持开机启动、自动重启和日志查看。
- 安装后每天自动检查 GitHub Releases；校验通过且内容发生变化时自动更新并重启服务。

### 支持的协议

| 配置名称 | BMS 协议 | CAN 特征 | 默认波特率 |
| --- | --- | --- | ---: |
| `kvms` | KVMS | 29 位扩展帧，主动发送查询 | 250000 |
| `htbms` | HTBMS CAN V1.1.0 | 29 位扩展广播帧 | 500000 |
| `jbd` | JBD-compatible CANBUS | 11 位标准帧，远程帧查询 | 500000 |

每个协议都有一个固定 UUID。配置自动模式时使用 UUID 最可靠：

| UUID | 协议 |
| --- | --- |
| `98b8d1c1-6a34-45a4-9687-e9a09ef20204` | KVMS |
| `fc3da911-07a0-42b3-8cb4-1aa8dd26b558` | HTBMS |
| `d7a1d64a-6671-4ee2-8fbd-859043083a68` | JBD |

## ROS 2 数据如何使用

节点发布单一话题：

```text
/batcan/data
```

消息类型为 `diagnostic_msgs/msg/DiagnosticArray`。`status[]` 中包含以下两类条目：

- `batcan/<profile>/summary`：电压、电流、温度、SOC、容量和电源状态等常用摘要。
- `batcan/<profile>/<response>`：某个 CAN 响应的全部解码字段，以及
  `raw.<response>` 形式的原始字节。

分页数据会把页码写入字段名，例如 `cell_voltage.1`、`cell_temperature.8`、
`fault_page_byte.15` 和 `raw.cell_voltages.1`。这样订阅者可以只读取需要的响应组。

查看话题和一条样例消息：

```bash
source /opt/ros/humble/setup.bash
ros2 topic info /batcan/data -v
ros2 topic echo /batcan/data --once
```

摘要中的 `profile_mode` 会显示 `manual` 或 `auto`，`profile` 和 `profile_id` 会
显示当前实际使用的协议，`version` 会显示当前程序版本，例如 `batcan 20260911`。

## 部署

目标机需要安装 ROS 2 Humble，并且已经有要使用的 SocketCAN 接口，例如 `can0` 或
`can5`。项目提供 ARM64 和 AMD64 的 Linux 发布包。

### 一键安装

Ubuntu 主机可以直接运行下面的命令安装最新发布包：

```bash
curl -fsSL https://gitwarp.canghai.org/raw.githubusercontent.com/chaeoi/batcan/main/script/install.sh | sudo bash
```

脚本会自动识别 CPU 架构、下载并校验对应的发布包，然后安装 `batcan.service` 和每日
更新检查。已有的 `/opt/batcan/config.yml` 会保留。首次安装时如果同时设置了
`BATCAN_INTERFACE`，脚本会直接生成自动识别配置，例如：

```bash
curl -fsSL https://gitwarp.canghai.org/raw.githubusercontent.com/chaeoi/batcan/main/script/install.sh \
  | sudo env BATCAN_INTERFACE=can5 bash
```

没有设置接口时，脚本会生成注释模板；编辑配置后执行：

```bash
sudo /opt/batcan/batcan --check-config --config /opt/batcan/config.yml
sudo systemctl enable --now batcan
```

从 GitHub Releases 下载与目标机架构对应的文件。下面以 ARM64 为例，直接创建目录、
下载、授权，然后安装服务：

```bash
sudo mkdir -p /opt/batcan
cd /opt/batcan
sudo wget -O batcan https://github.com/chaeoi/batcan/releases/latest/download/batcan-linux-arm64
sudo chmod +x batcan
sudo ./batcan service install
```

AMD64 主机把下载地址中的 `batcan-linux-arm64` 改为 `batcan-linux-amd64`。

`service install` 会完成服务安装，并在配置有效时尝试立即启动。它会自动创建：

- `/opt/batcan/batcan`：可执行文件。
- `/opt/batcan/config.yml`：运行时配置。
- `/etc/systemd/system/batcan.service`：systemd 服务单元。
- `/opt/batcan/update.sh`：自动更新脚本。
- `batcan-update.timer`：每天执行一次更新检查。

第一次安装时，`/opt/batcan/config.yml` 会自动生成一个带注释的模板；因为没有有效
配置，服务不会启动。编辑该文件，填写有效的 `profile`、（自动模式下的）`profiles`
和 `interface`，再执行：

```bash
sudo /opt/batcan/batcan --check-config --config /opt/batcan/config.yml
sudo systemctl enable --now batcan
```

安装服务时会同时启用每日更新检查。也可以手动立即检查：

```bash
sudo /opt/batcan/batcan service update
```

更新器会下载当前架构的发布包，校验 `SHA256SUMS` 后再替换程序；校验失败或下载
失败时会保留当前版本。由于发布版本按提交日期显示，同一天的多个提交由文件校验值
区分，仍然可以自动更新到最新内容。

服务默认以 `ubuntu` 用户运行。该用户需要能够访问 CAN 设备，通常应属于 `dialout`
组；接口名称可以用下面的命令确认：

```bash
ip -brief link
groups ubuntu
```

## 配置文件怎么写

配置文件会在首次执行 `service install` 时自动生成；之后只修改目标机上的
`/opt/batcan/config.yml`。配置文件是简单的 YAML，每行一个字段，行尾可以写注释。
自动生成的是注释模板，必须取消注释并填写配置后才能启动。

### 自动识别模式（推荐）

```yaml
profile: auto
profiles: 98b8d1c1-6a34-45a4-9687-e9a09ef20204,fc3da911-07a0-42b3-8cb4-1aa8dd26b558,d7a1d64a-6671-4ee2-8fbd-859043083a68
interface: can5
```

- `profile: auto`：启用自动探测。
- `profiles`：逗号分隔的候选 UUID。已确定不会使用某种 BMS 时，可以删掉对应 UUID，
  以减少探测时间。
- `interface`：本机的 SocketCAN 接口名，例如 `can0` 或 `can5`。

自动模式会独占配置的 CAN 接口；同一接口不要同时启动第二个 `batcan` 进程。如果
同时有多个候选产生有效响应，程序不会猜测，应缩小 `profiles` 列表或改用手动模式。

### 手动指定模式

确定 BMS 型号后，可以填写对应的 UUID：

```yaml
profile: 98b8d1c1-6a34-45a4-9687-e9a09ef20204
interface: can5
```

手动模式不需要 `profiles` 字段。程序只接受规范 UUID，不接受协议名称或旧别名。

## 修改配置并检查运行状态

修改配置后，先校验文件，再重启服务：

```bash
sudo /opt/batcan/batcan --check-config --config /opt/batcan/config.yml
sudo systemctl restart batcan
systemctl status batcan --no-pager
journalctl -u batcan -f
```

服务启动后会自动把接口设为指定波特率并拉起；没有收到有效 CAN 帧时服务仍会保持运行
并继续探测。查看当前接口状态：

```bash
ip -details link show can5
```

常用服务操作：

```bash
sudo systemctl enable batcan   # 开机启动
sudo systemctl disable batcan  # 取消开机启动
sudo systemctl stop batcan
sudo systemctl start batcan
sudo systemctl status batcan-update.timer
sudo journalctl -u batcan-update.service -n 50 --no-pager
```

卸载 systemd 服务但保留 `/opt/batcan` 文件：

```bash
sudo /opt/batcan/batcan service uninstall
```
