#include "batcan/protocol.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace batcan {

double decodeField(const FieldConfig &field,
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

void applyResponse(const ResponseConfig &response,
                   const std::array<std::uint8_t, 8> &data,
                   std::size_t data_length, BatterySample &sample) {
  sample.present = true;
  for (const auto &field : response.fields) {
    const auto value = decodeField(field, data, data_length);
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
      sample.power_supply_status = static_cast<std::uint8_t>(value);
    } else if (field.metric == "power_supply_health") {
      sample.power_supply_health = static_cast<std::uint8_t>(value);
    } else if (field.metric == "power_supply_technology") {
      sample.power_supply_technology = static_cast<std::uint8_t>(value);
    }
  }
}

}  // namespace batcan
