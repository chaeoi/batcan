# batcan

`batcan` reads BMS CAN traffic through SocketCAN and publishes every decoded
response on one ROS 2 topic. It supports protocol profiles embedded into the
binary, so target machines switch BMS only by editing a small runtime config.

## Single topic

`/batcan/data` has type `diagnostic_msgs/msg/DiagnosticArray`.

Every message has an ordered `status[]` list:

| Status name | Content |
| --- | --- |
| `batcan/<profile>/summary` | Common pack values that were received: voltage, current, temperature, SOC, capacities and power-supply state |
| `batcan/<profile>/<response>` | Every field decoded from one supported BMS response, plus `raw.<response>` hexadecimal CAN bytes |

Repeated pages retain their page number in both metric and raw keys, for
example `cell_voltage.1`, `cell_temperature.8`, `fault_page_byte.15`, and
`raw.cell_voltages.1`. This makes a subscriber able to read only the response
groups it needs while the topic still carries all supported data.

## BMS profiles

| Profile | BMS specification | CAN behavior | Default bitrate |
| --- | --- | --- | --- |
| `kvms` | KVMS | 29-bit extended CAN, actively sends `0x0400FF80`, big-endian response fields | 250 kbit/s |
| `htbms` | HTBMS CAN V1.1.0 | 29-bit extended `0x1822xxxx` broadcast, little-endian fields | 500 kbit/s |
| `jbd` | JBD-compatible CANBUS | 11-bit standard CAN, remote-frame queries `0x100` to `0x110`, big-endian Modbus CRC-16 responses | 500 kbit/s |

The three profiles are different protocols. In particular, HTBMS is neither
the KVMS query protocol nor the 11-bit CANBUS remote-query protocol. HTBMS
documents 250 kbit/s and 500 kbit/s selectable operation; its profile defaults
to 500 kbit/s and may be overridden at deployment if the installed BMS is set
to 250 kbit/s.

`kvms` includes all observed and documented reply IDs `0x040080**` through
`0x040E80**`: pack data, individual cell voltages and temperatures, pack and
cell extrema, MOS and I/O state, capacities, charge information, limits, fault
pages, and the documented-but-unassigned `0x040A80**`/`0x040C80**` bytes. The
last two are emitted as individual byte values and raw frames until their BMS
vendor definitions are available.

`jbd` queries all documented non-empty IDs from `0x100` through
`0x110`: pack/capacity data, balance/protection/FET/version/count information,
six NTC temperatures, and up to 30 cell voltages. It does not query `0x111` or
`0x112`, which are blank in the supplied protocol document.

## Configuration

The BMS profile documents live only in `models/bms.yml` in this repository.
CMake embeds that file into the executable. On a target machine, edit only
`/opt/batcan/config.yml`:

```yaml
profile: 98b8d1c1-6a34-45a4-9687-e9a09ef20204
interface: can5
```

The repository contains one embedded `models/bms.yml` file with one YAML
document per protocol. Each document has a unique immutable UUID in `model.id`;
use that UUID in runtime configuration. The shorter names (`kvms`, `htbms`,
`jbd`) remain accepted as compatibility selectors. The generated config template
annotates every selectable field with an inline remark, and the parser accepts
those remarks when the file is edited.

`interface` is the local SocketCAN interface name and therefore depends on the
machine (`can0`, `can5`, and so on). The BMS profile controls bitrate by default.
Use the optional runtime `bitrate` only when the same BMS protocol has been
configured to another supported physical rate, such as HTBMS at 250 kbit/s:

```yaml
profile: fc3da911-07a0-42b3-8cb4-1aa8dd26b558
interface: can0
bitrate: 250000
```

For migration, `model: 2m_v0.1.2`, `profile: htbms_v1.1.0`, and
`profile: canbus_500k` remain accepted as aliases. `profile: canbus` is also
accepted for builds made during the rename. New configs should use the short
profile names above.

Validate and start the installed service:

```bash
sudo /opt/batcan/batcan --check-config --config /opt/batcan/config.yml
sudo systemctl enable --now batcan
```

## Build

Source ROS 2 Humble and build the package from its workspace:

```bash
source /opt/ros/humble/setup.bash
colcon build --packages-select batcan
```

## Repository layout

```text
models/     Embedded BMS protocol profiles
doc/        Supplied BMS protocol documents
src/        Generic SocketCAN, profile parsing and ROS publishing code
tests/      Profile and protocol decoding tests
```
