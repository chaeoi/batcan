#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "batcan/config.hpp"

namespace batcan {

struct BatterySample {
  bool present = false;
  std::optional<double> voltage;
  std::optional<double> current;
  std::optional<double> temperature;
  std::optional<double> percentage;
  std::optional<double> charge;
  std::optional<double> capacity;
  std::optional<double> design_capacity;
  std::optional<std::uint8_t> power_supply_status;
  std::optional<std::uint8_t> power_supply_health;
  std::optional<std::uint8_t> power_supply_technology;
};

double decodeField(const FieldConfig &field,
                   const std::array<std::uint8_t, 8> &data,
                   std::size_t data_length);
void applyResponse(const ResponseConfig &response,
                   const std::array<std::uint8_t, 8> &data,
                   std::size_t data_length, BatterySample &sample);

}  // namespace batcan
