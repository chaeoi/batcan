#include "batcan/protocol.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace batcan {
namespace {

std::uint64_t rawFieldValue(const FieldConfig &field,
                            const std::array<std::uint8_t, 8> &data,
                            std::size_t data_length) {
  if (field.offset + field.length > data_length) {
    throw std::runtime_error("CAN response is shorter than a configured field");
  }
  std::uint64_t raw = 0;
  if (field.endian == "little") {
    for (std::size_t index = 0; index < field.length; ++index) {
      raw |= static_cast<std::uint64_t>(data[field.offset + index])
             << (index * 8U);
    }
  } else {
    for (std::size_t index = 0; index < field.length; ++index) {
      raw = (raw << 8U) | data[field.offset + index];
    }
  }
  return raw;
}

double decodedRawValue(const FieldConfig &field, std::uint64_t raw) {
  double numeric = static_cast<double>(raw);
  if (field.encoding == "int") {
    const auto bits = field.length * 8U;
    std::int64_t signed_value = 0;
    if (bits == 64) {
      signed_value = static_cast<std::int64_t>(raw);
    } else if ((raw & (std::uint64_t{1} << (bits - 1U))) != 0) {
      signed_value = static_cast<std::int64_t>(
          raw | (~std::uint64_t{0} << bits));
    } else {
      signed_value = static_cast<std::int64_t>(raw);
    }
    numeric = static_cast<double>(signed_value);
  }
  return numeric * field.scale + field.bias;
}

bool invalidFieldValue(const FieldConfig &field, std::uint64_t raw) {
  return std::any_of(field.invalid_values.begin(), field.invalid_values.end(),
                     [raw](std::uint8_t value) { return raw == value; });
}

std::size_t fieldIndex(const ResponseConfig &response, const FieldConfig &field,
                       const std::array<std::uint8_t, 8> &data) {
  std::size_t index = field.index;
  if (response.sequence_stride != 0) {
    const auto sequence = data[response.sequence_offset];
    if (sequence >= response.sequence_base) {
      index += static_cast<std::size_t>(sequence - response.sequence_base) *
               response.sequence_stride;
    }
  }
  return index;
}

std::uint16_t crc16Modbus(const std::array<std::uint8_t, 8> &data,
                          std::size_t length) {
  std::uint16_t crc = 0xFFFFU;
  for (std::size_t index = 0; index < length; ++index) {
    crc ^= data[index];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U) != 0U
                ? static_cast<std::uint16_t>((crc >> 1U) ^ 0xA001U)
                : static_cast<std::uint16_t>(crc >> 1U);
    }
  }
  return crc;
}

void setIndexed(std::vector<double> &values, std::size_t index, double value) {
  if (values.size() <= index) {
    values.resize(index + 1, std::numeric_limits<double>::quiet_NaN());
  }
  values[index] = value;
}

std::uint8_t mapValue(const FieldConfig &field, double value) {
  const auto raw = static_cast<std::uint8_t>(value);
  const auto mapped = field.value_map.find(raw);
  return mapped == field.value_map.end() ? raw : mapped->second;
}

bool standardMetric(const std::string &metric) {
  return metric == "voltage" || metric == "current" ||
         metric == "temperature" || metric == "percentage" ||
         metric == "charge" || metric == "capacity" ||
         metric == "design_capacity" ||
         metric == "power_supply_status" ||
         metric == "power_supply_health" ||
         metric == "power_supply_technology";
}

}  // namespace

void BatterySample::clear() {
  present = false;
  voltage.reset();
  current.reset();
  temperature.reset();
  percentage.reset();
  charge.reset();
  capacity.reset();
  design_capacity.reset();
  power_supply_status.reset();
  power_supply_health.reset();
  power_supply_technology.reset();
  cell_voltage.clear();
  cell_temperature.clear();
  metrics.clear();
  raw_frames.clear();
  response_metrics.clear();
  response_raw_frames.clear();
}

double decodeField(const FieldConfig &field,
                   const std::array<std::uint8_t, 8> &data,
                   std::size_t data_length) {
  return decodedRawValue(field, rawFieldValue(field, data, data_length));
}

bool validateResponse(const ResponseConfig &response,
                      const std::array<std::uint8_t, 8> &data,
                      std::size_t data_length) {
  if (!response.crc16) {
    return true;
  }
  if (data_length < 2) {
    return false;
  }
  const auto expected =
      static_cast<std::uint16_t>(data[data_length - 2] << 8U) |
      data[data_length - 1];
  return crc16Modbus(data, data_length - 2) == expected;
}

void applyResponse(const ResponseConfig &response,
                   const std::array<std::uint8_t, 8> &data,
                   std::size_t data_length, BatterySample &sample) {
  const auto payload_length =
      response.crc16 && data_length >= 2 ? data_length - 2 : data_length;
  sample.present = true;
  auto raw_name = response.name;
  if (response.sequence_stride != 0 &&
      response.sequence_offset < payload_length) {
    raw_name += "." + std::to_string(data[response.sequence_offset]);
  }
  const auto raw_data =
      std::vector<std::uint8_t>(data.begin(), data.begin() + data_length);
  sample.raw_frames[raw_name] = raw_data;
  sample.response_raw_frames[response.name][raw_name] = raw_data;
  for (const auto &field : response.fields) {
    // Some JBD pages are truncated at the configured cell count. Decode the
    // fields that are present and leave the unavailable trailing fields out.
    if (response.crc16 && field.offset + field.length > payload_length) {
      continue;
    }
    const auto raw = rawFieldValue(
        field, data, response.crc16 ? payload_length : data_length);
    if (invalidFieldValue(field, raw)) {
      continue;
    }
    const auto value = decodedRawValue(field, raw);
    const auto index = fieldIndex(response, field, data);
    auto metric_name = field.metric;
    if (field.metric == "cell_voltage" || field.metric == "cell_temperature" ||
        (response.sequence_stride != 0 && !standardMetric(field.metric))) {
      metric_name += "." + std::to_string(index + 1);
    }
    sample.response_metrics[response.name][metric_name] = value;
    if (field.metric == "voltage") {
      sample.voltage = value;
    } else if (field.metric == "current") {
      sample.current = value;
    } else if (field.metric == "temperature") {
      sample.temperature = value;
    } else if (field.metric == "percentage") {
      sample.percentage = value;
    } else if (field.metric == "charge") {
      sample.charge = value;
    } else if (field.metric == "capacity") {
      sample.capacity = value;
    } else if (field.metric == "design_capacity") {
      sample.design_capacity = value;
    } else if (field.metric == "power_supply_status") {
      sample.power_supply_status = mapValue(field, value);
    } else if (field.metric == "power_supply_health") {
      sample.power_supply_health = mapValue(field, value);
    } else if (field.metric == "power_supply_technology") {
      sample.power_supply_technology = mapValue(field, value);
    } else if (field.metric == "cell_voltage") {
      setIndexed(sample.cell_voltage, index, value);
    } else if (field.metric == "cell_temperature") {
      setIndexed(sample.cell_temperature, index, value);
    }

    if (!standardMetric(field.metric)) {
      sample.metrics[metric_name] = value;
    }
  }
}

}  // namespace batcan
