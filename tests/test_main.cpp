#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

#include "batcan/config.hpp"
#include "batcan/models.hpp"
#include "batcan/protocol.hpp"

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

batcan::FieldConfig field(const std::string &metric, std::size_t offset,
                          std::size_t length, const std::string &encoding,
                          const std::string &endian, double scale,
                          double bias) {
  batcan::FieldConfig result;
  result.metric = metric;
  result.offset = offset;
  result.length = length;
  result.encoding = encoding;
  result.endian = endian;
  result.scale = scale;
  result.bias = bias;
  return result;
}

std::filesystem::path writeConfig(const std::string &contents,
                                   const char *name) {
  const auto path = std::filesystem::temp_directory_path() / name;
  std::ofstream stream(path);
  stream << contents;
  return path;
}

void testDefaultConfig() {
  const auto generated = batcan::defaultConfig();
  require(generated.find(
              "# profile: 98b8d1c1-6a34-45a4-9687-e9a09ef20204 # kvms:") !=
              std::string::npos,
          "default config must list KVMS profile");
  require(generated.find(
              "# profile: fc3da911-07a0-42b3-8cb4-1aa8dd26b558 # htbms:") !=
              std::string::npos,
          "default config must list HTBMS profile");
  require(generated.find(
              "# profile: d7a1d64a-6671-4ee2-8fbd-859043083a68 # jbd:") !=
              std::string::npos,
          "default config must list JBD profile");
  require(generated.find("# interface: can5 #") != std::string::npos,
          "default config must annotate interface");
  require(generated.find("# bitrate: auto #") != std::string::npos,
          "default config must annotate bitrate");
  const auto path = writeConfig(generated, "batcan-default-test.yml");
  bool selection_required = false;
  try {
    (void)batcan::loadConfig(path.string());
  } catch (const std::exception &) {
    selection_required = true;
  }
  require(selection_required, "commented default config must require selection");
  std::filesystem::remove(path);

  const auto kvms_path = writeConfig(
      "profile: 98b8d1c1-6a34-45a4-9687-e9a09ef20204\ninterface: can1\n",
      "batcan-kvms-test.yml");
  const auto config = batcan::loadConfig(kvms_path.string());
  std::filesystem::remove(kvms_path);
  require(config.can.interface == "can1", "interface override mismatch");
  require(config.can.bitrate == 250000, "KVMS bitrate mismatch");
  require(config.can.queries.size() == 1, "KVMS query count mismatch");
  require(config.can.queries[0].responses.size() >= 14,
          "KVMS must expose all supported response pages");
  require(config.can.queries[0].responses[0].id == 0x04008000U,
          "KVMS cell response ID mismatch");
  require(config.can.queries[0].responses[0].collect,
          "KVMS cell response must collect repeated pages");
  require(config.model == "kvms", "profile mismatch");
  require(config.model_id == "98b8d1c1-6a34-45a4-9687-e9a09ef20204",
          "KVMS unique model ID mismatch");
  require(config.bms_model == "KVMS", "BMS model mismatch");
  require(config.ros.topic == "/batcan/data", "single topic mismatch");

  const auto commented_path = writeConfig(
      "profile: 98b8d1c1-6a34-45a4-9687-e9a09ef20204 # select the protocol\n"
      "interface: can2 # machine-specific interface\n"
      "bitrate: 250000 # explicit physical rate\n",
      "batcan-inline-comments-test.yml");
  const auto commented = batcan::loadConfig(commented_path.string());
  std::filesystem::remove(commented_path);
  require(commented.model == "kvms" && commented.can.interface == "can2" &&
              commented.can.bitrate == 250000,
          "inline config comments must be ignored");

  const auto auto_path = writeConfig(
      "profile: auto\n"
      "profiles: 98b8d1c1-6a34-45a4-9687-e9a09ef20204, "
      "fc3da911-07a0-42b3-8cb4-1aa8dd26b558\n"
      "interface: can7\n"
      "bitrate: auto\n",
      "batcan-auto-test.yml");
  const auto automatic = batcan::loadConfig(auto_path.string());
  std::filesystem::remove(auto_path);
  require(automatic.auto_detect, "auto profile mode must be enabled");
  require(automatic.auto_profiles.size() == 2,
          "auto profile candidates mismatch");
  require(automatic.model == "auto" && automatic.model_id == "auto",
          "auto profile identity mismatch");
  require(automatic.can.interface == "can7" &&
              automatic.can.bitrate == 250000,
          "auto profile must retain first candidate defaults");
  require(automatic.interface_override && !automatic.bitrate_override,
          "auto runtime overrides mismatch");

  const auto invalid_auto_path = writeConfig(
      "profile: auto\nprofiles: kvms\n", "batcan-invalid-auto-test.yml");
  bool invalid_auto_rejected = false;
  try {
    (void)batcan::loadConfig(invalid_auto_path.string());
  } catch (const std::exception &) {
    invalid_auto_rejected = true;
  }
  std::filesystem::remove(invalid_auto_path);
  require(invalid_auto_rejected,
          "automatic candidates must require valid UUIDs");
}

void testOtherProfiles() {
  const auto htbms_path = writeConfig(
      "profile: fc3da911-07a0-42b3-8cb4-1aa8dd26b558\n",
      "batcan-htbms-test.yml");
  const auto htbms = batcan::loadConfig(htbms_path.string());
  std::filesystem::remove(htbms_path);
  require(htbms.can.bitrate == 500000, "HTBMS default bitrate mismatch");
  require(htbms.can.queries.size() == 1 && !htbms.can.queries[0].send_request,
          "HTBMS must use passive broadcast collection");
  require(htbms.can.queries[0].responses[0].id_mask == 0x1FFF0000U,
          "HTBMS ID mask mismatch");

  const auto htbms_alias_path =
      writeConfig("profile: htbms_v1.1.0\n", "batcan-htbms-alias-test.yml");
  const auto htbms_alias = batcan::loadConfig(htbms_alias_path.string());
  std::filesystem::remove(htbms_alias_path);
  require(htbms_alias.model == "htbms", "HTBMS compatibility alias mismatch");

  const auto canbus_path = writeConfig(
      "profile: d7a1d64a-6671-4ee2-8fbd-859043083a68\n",
      "batcan-jbd-test.yml");
  const auto canbus = batcan::loadConfig(canbus_path.string());
  std::filesystem::remove(canbus_path);
  require(canbus.can.queries.size() == 17,
          "JBD must query IDs 0x100 through 0x110");
  require(canbus.can.queries.front().request_remote,
          "JBD requests must be remote frames");
  require(!canbus.can.queries.front().extended,
          "JBD requests must use standard IDs");
  require(canbus.can.queries.front().responses.front().crc16,
          "JBD responses must use CRC-16");

  const auto canbus_alias_path =
      writeConfig("profile: canbus_500k\n", "batcan-canbus-alias-test.yml");
  const auto canbus_alias = batcan::loadConfig(canbus_alias_path.string());
  std::filesystem::remove(canbus_alias_path);
  require(canbus_alias.model == "jbd", "CANBUS compatibility alias mismatch");

  const auto short_name_path =
      writeConfig("profile: kvms\n", "batcan-short-name-test.yml");
  const auto short_name = batcan::loadConfig(short_name_path.string());
  std::filesystem::remove(short_name_path);
  require(short_name.model_id == "98b8d1c1-6a34-45a4-9687-e9a09ef20204",
          "short profile compatibility selector mismatch");
}

void testUniqueModelIds() {
  const auto models = batcan::supportedModels();
  require(models.size() == 3, "expected three embedded profiles");
  std::set<std::string> ids;
  for (const auto &model : models) {
    require(model.id.size() == 36, "model ID must be a UUID");
    require(ids.insert(model.id).second, "embedded model IDs must be unique");
  }
  require(models[0].profile == "kvms" && models[1].profile == "htbms" &&
              models[2].profile == "jbd",
          "embedded profile names must remain stable");
}

void testRejectsInvalidRuntimeConfig() {
  const auto path = writeConfig("profile: kvms\ncan:\n  interface: can0\n",
                                "batcan-invalid-test.yml");
  bool rejected = false;
  try {
    (void)batcan::loadConfig(path.string());
  } catch (const std::exception &) {
    rejected = true;
  }
  std::filesystem::remove(path);
  require(rejected, "nested runtime CAN configuration must be rejected");

  const auto duplicate = writeConfig("profile: kvms\nprofile: htbms\n",
                                     "batcan-duplicate-test.yml");
  rejected = false;
  try {
    (void)batcan::loadConfig(duplicate.string());
  } catch (const std::exception &) {
    rejected = true;
  }
  std::filesystem::remove(duplicate);
  require(rejected, "multiple profiles must be rejected");
}

void testKvmsDecode() {
  batcan::BatterySample sample;
  batcan::ResponseConfig pack;
  pack.name = "pack";
  pack.fields = {field("voltage", 0, 2, "uint", "big", 0.1, 0.0),
                 field("current", 2, 2, "uint", "big", 0.1, -3000.0),
                 field("percentage", 4, 2, "uint", "big", 0.001, 0.0)};
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
  require(sample.response_metrics["pack"].count("voltage") == 1,
          "response metrics must include standard fields");
  require(sample.response_raw_frames["pack"].count("pack") == 1,
          "response metrics must include raw frame");
}

void testSequenceDecode() {
  batcan::ResponseConfig cells;
  cells.name = "cell_voltages";
  cells.sequence_offset = 0;
  cells.sequence_base = 1;
  cells.sequence_stride = 3;
  auto first = field("cell_voltage", 1, 2, "uint", "big", 0.001, 0.0);
  auto second = field("cell_voltage", 3, 2, "uint", "big", 0.001, 0.0);
  auto third = field("cell_voltage", 5, 2, "uint", "big", 0.001, 0.0);
  first.index = 0;
  second.index = 1;
  third.index = 2;
  cells.fields = {first, second, third};
  const std::array<std::uint8_t, 8> data = {
      0x02, 0x0C, 0x75, 0x0C, 0x76, 0x0C, 0x77, 0x00};
  batcan::BatterySample sample;
  batcan::applyResponse(cells, data, data.size(), sample);
  require(sample.cell_voltage.size() == 6, "sequence index mismatch");
  require(std::abs(sample.cell_voltage[3] - 3.189) < 0.0001,
          "sequence cell value mismatch");
  require(sample.response_raw_frames["cell_voltages"].count(
              "cell_voltages.2") == 1,
          "sequence raw frame name mismatch");
}

void testSignedLittleEndianDecode() {
  const auto field_config = field("current", 0, 2, "int", "little", 0.01,
                                  0.0);
  const std::array<std::uint8_t, 8> data = {
      0x9C, 0xFF, 0, 0, 0, 0, 0, 0};
  const auto value = batcan::decodeField(field_config, data, data.size());
  require(std::abs(value - (-1.0)) < 0.0001,
          "signed little-endian decode mismatch");
}

void testCrc16Modbus() {
  batcan::ResponseConfig response;
  response.crc16 = true;
  const std::array<std::uint8_t, 8> valid = {
      0x13, 0xE8, 0xFF, 0x9C, 0x00, 0x64, 0x7E, 0x93};
  require(batcan::validateResponse(response, valid, valid.size()),
          "valid Modbus CRC must be accepted");
  auto invalid = valid;
  invalid[7] ^= 0x01;
  require(!batcan::validateResponse(response, invalid, invalid.size()),
          "invalid Modbus CRC must be rejected");
}

}  // namespace

int main() {
  try {
    testDefaultConfig();
    testOtherProfiles();
    testUniqueModelIds();
    testRejectsInvalidRuntimeConfig();
    testKvmsDecode();
    testSequenceDecode();
    testSignedLittleEndianDecode();
    testCrc16Modbus();
    std::cout << "all tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "test failure: " << error.what() << '\n';
    return 1;
  }
}
