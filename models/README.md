# BMS profiles

`bms.yml` is the only BMS configuration file. It contains one YAML document per
protocol profile, separated by `---`. Each document has a readable `profile`
name and an independent immutable UUID in `model.id`; UUIDs and profile names
must both be unique. The file exists only in the repository: CMake embeds it in
the `batcan` executable at build time. Target machines switch profiles only
through `/opt/batcan/config.yml`; they do not need this file beside the binary.

Profiles contain BMS protocol defaults, including bitrate, query timing, CAN
IDs, byte order, CRC validation and field decoding. The target-machine config
contains only `profile`, `interface`, and an optional `bitrate` override for a
BMS whose installed bitrate is configurable.

| Node | Fields | Purpose |
| --- | --- | --- |
| `profile` | short name | Readable profile name; use `model.id` for selection |
| `model` | `id`, `bms_model` | Unique immutable UUID and specification name |
| `can` | `bitrate`, `query_interval_ms`, `response_timeout_ms` | Protocol defaults |
| `ros` | `topic`, `frame_id`, `localhost_only`, `domain_id`, `qos_depth` | Publish settings |
| `queries[]` | `name`, `send_request`, `request`, `responses` | Active query or passive broadcast collection |
| `request` | `id`, `extended`, `remote`, `data` | CAN data frame or remote request frame |
| `responses[]` | `name`, `id`, `id_mask`, `extended`, `collect`, `crc16`, `sequence`, `fields` | Response matching and validation |
| `fields[]` | `metric`, `offset`, `length`, `index`, `encoding`, `endian`, `scale`, `bias`, `value_map`, `invalid_values` | Field decoding |

`collect: true` keeps receiving a matching response until the query timeout.
`sequence` maps a response page number to indexed metrics, for example
`cell_voltage.1`. `crc16: modbus` validates a Modbus CRC-16 carried in the last
two response bytes, high byte first.

Profiles may be shared only when BMS models use the same query behavior, CAN
identifier format, byte order, field layout and checksum rule. Similar battery
capacity or enclosure style alone is not enough.

The UUID is the canonical runtime profile selector. The short names `kvms`,
`htbms`, and `jbd` remain compatibility selectors, as do the previous names
`2m_v0.1.2`, `htbms_v1.1.0`, and `canbus_500k`.
The reported DALY K-series BMS (13S, 100A) inside the 心向阳 battery is
documented in the `kvms` header. J24K3 is only a candidate if it is the hardware
variant; KVMS remains the profile name for the pack's externally observed CAN
protocol.
