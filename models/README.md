# 型号配置

每个机器人型号对应一个同名的 YAML 文件，例如 `2m_v0.1.2.yml` 中的 `model.id` 必须是 `2m_v0.1.2`。这些文件只在仓库中维护，打包时会嵌入二进制，不需要复制到目标设备。

型号文件使用以下结构：

| 节点 | 字段 | 说明 |
| --- | --- | --- |
| `model` | `id`、`bms_model` | 机器人型号和 BMS 型号 |
| `can` | `interface`、`query_interval_ms`、`response_timeout_ms` | CAN 接口和采集时序 |
| `ros` | `topic`、`frame_id`、`localhost_only`、`domain_id`、`qos_depth` | ROS 2 发布参数 |
| `queries[]` | `name`、`send_request`、`request`、`responses` | 一组主动查询或被动广播 |
| `request` | `id`、`extended`、`data` | 主动查询帧；被动广播可省略 |
| `responses[]` | `name`、`id`、`id_mask`、`extended`、`fields` | 响应帧匹配规则 |
| `fields[]` | `metric`、`offset`、`length`、`encoding`、`endian`、`scale`、`bias`、`value_map` | 字段解码规则 |

`send_request: true` 时必须提供 `request`。`send_request: false` 时程序不发送请求，直接在响应超时时间内接收广播帧。两种方式使用相同的 `responses` 和 `fields` 解码流程。

`request.data` 使用空格分隔的十六进制字节，最多 8 字节。`response.id_mask` 可省略，省略时完整匹配 CAN ID；需要匹配一组板号时可用掩码忽略对应位。

`fields[].metric` 支持：

- `voltage`
- `current`
- `temperature`
- `percentage`
- `charge`
- `capacity`
- `design_capacity`
- `power_supply_status`
- `power_supply_health`
- `power_supply_technology`

`encoding` 支持 `uint` 和 `int`，`endian` 支持 `big` 和 `little`。解码结果按 `原始值 * scale + bias` 计算；状态类字段可用 `value_map` 将 BMS 枚举映射到 ROS 2 枚举，例如 `0:3,1:1,2:2`。

解析器会拒绝未知字段、重复名称、无效缩进、越界 CAN ID、超过 8 字节的请求和越过 CAN 帧边界的字段。
