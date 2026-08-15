# batcan

`batcan` is a compact SocketCAN to ROS2 battery bridge. It publishes the
standard `sensor_msgs/msg/BatteryState` message on the fixed topic
`/bms_can/battery_data`. Baize and other ROS consumers only subscribe to that
topic; they do not access CAN.

The release binary contains the complete, reviewed BMS protocol for each robot
model: CAN interface, request frames, response IDs and field decoding. The
local configuration selects only the model. It cannot be used to alter CAN
traffic or run commands.

## Install a GitHub release

The project is built by GitHub Actions for Linux AMD64 and ARM64. Target robots
download a release binary and do not compile source code.

```bash
curl -fsSL https://raw.githubusercontent.com/chaeoi/batcan/main/deploy/install.sh \
  | sudo sh -s -- --robot-model 2m_v0.1.2
```

The installer downloads the architecture-matched `latest` release, verifies its
SHA-256 checksum, installs `/usr/local/bin/batcan`, generates
`/etc/batcan/config.yml`, registers the service and starts it.

To register first and select the model later:

```bash
sudo batcan service install
sudoedit /etc/batcan/config.yml
sudo systemctl start batcan
```

The generated file has one field:

```yaml
robot_model: 2m_v0.1.2
```

## Service management

```bash
sudo batcan service install --robot-model 2m_v0.1.2
batcan service status
sudo batcan service uninstall
sudo batcan service uninstall --purge
```

`uninstall` stops and removes the service but preserves its binary and model
selection. `--purge` removes both. The service runs as `ubuntu` with only
`CAP_NET_RAW`, and does not reconfigure the CAN interface or bitrate.

## Supported models

| Model | Battery protocol | CAN interface |
| --- | --- | --- |
| `2m_v0.1.2` | Xinxiangyang, extended-frame polling | `can5` |

For `2m_v0.1.2`, the bridge sends `0x0400FF80` and consumes only the reviewed
pack, temperature and status responses. SOC is published in ROS2's standard
0–1 range.

## Developer build

Only maintainers need a local build. Runtime requires ROS2 Humble on the target
robot because ROS2 libraries are dynamically linked.

```bash
source /opt/ros/humble/setup.bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```
