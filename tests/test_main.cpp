#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include "batcan/config.hpp"
#include "batcan/protocol.hpp"

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void testDefaultConfig() {
  const auto path = std::filesystem::temp_directory_path() /
                    "batcan-default-test.yml";
  {
    std::ofstream stream(path);
    stream << batcan::defaultConfig("2m_v0.1.2");
  }
  const auto config = batcan::loadConfig(path.string());
  std::filesystem::remove(path);
  require(config.can.interface == "can5", "default CAN interface mismatch");
  require(config.can.queries.size() == 1, "default query count mismatch");
  require(config.can.queries[0].responses.size() == 3,
          "default response count mismatch");
  require(config.robot_model == "2m_v0.1.2", "robot model mismatch");
  require(config.ros.topic == "/bms_can/battery_data",
          "default ROS topic mismatch");
}

void testRejectsCANConfiguration() {
  const auto path = std::filesystem::temp_directory_path() /
                    "batcan-invalid-test.yml";
  {
    std::ofstream stream(path);
    stream << "robot_model: 2m_v0.1.2\ncan:\n  interface: can0\n";
  }
  bool rejected = false;
  try {
    (void)batcan::loadConfig(path.string());
  } catch (const std::exception &) {
    rejected = true;
  }
  std::filesystem::remove(path);
  require(rejected, "external CAN configuration must be rejected");
}

void testXinxiangyangDecode() {
  batcan::BatterySample sample;
  batcan::ResponseConfig pack;
  pack.fields = {
      {"voltage", 0, 2, "uint", "big", 0.1, 0.0},
      {"current", 2, 2, "uint", "big", 0.1, -3000.0},
      {"percentage", 4, 2, "uint", "big", 0.001, 0.0},
  };
  const std::array<std::uint8_t, 8> data = {
      0x02, 0x0C, 0x75, 0x99, 0x03, 0x84, 0x00, 0x00};
  batcan::applyResponse(pack, data, data.size(), sample);
  require(sample.present, "sample should be present");
  require(std::abs(sample.voltage.value() - 52.4) < 0.0001,
          "voltage decode mismatch");
  require(std::abs(sample.current.value() - 10.5) < 0.0001,
          "current decode mismatch");
  require(std::abs(sample.percentage.value() - 0.9) < 0.0001,
          "percentage decode mismatch");
}

void testSignedLittleEndianDecode() {
  const batcan::FieldConfig field{
      "current", 0, 2, "int", "little", 0.01, 0.0};
  const std::array<std::uint8_t, 8> data = {
      0x9C, 0xFF, 0, 0, 0, 0, 0, 0};
  const auto value = batcan::decodeField(field, data, data.size());
  require(std::abs(value - (-1.0)) < 0.0001,
          "signed little-endian decode mismatch");
}

}  // namespace

int main() {
  try {
    testDefaultConfig();
    testRejectsCANConfiguration();
    testXinxiangyangDecode();
    testSignedLittleEndianDecode();
    std::cout << "all tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "test failure: " << error.what() << '\n';
    return 1;
  }
}
