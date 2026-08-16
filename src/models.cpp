#include "batcan/models.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "batcan/embedded_models.hpp"

namespace batcan {
namespace {

enum class YamlType { kScalar, kMap, kSequence };

struct YamlNode {
  YamlType type = YamlType::kMap;
  std::string scalar;
  std::map<std::string, YamlNode> mapping;
  std::vector<YamlNode> sequence;
};

struct YamlLine {
  std::size_t number = 0;
  std::size_t indent = 0;
  std::string text;
};

struct Property {
  std::string key;
  std::string value;
};

const std::regex kModelName("^[A-Za-z0-9._-]{1,64}$");
const std::regex kName("^[A-Za-z0-9_-]{1,64}$");
const std::regex kYamlKey("^[A-Za-z][A-Za-z0-9_]*$");
const std::set<std::string> kMetrics = {
    "voltage",           "current",       "temperature",
    "percentage",        "charge",        "capacity",
    "design_capacity",   "power_supply_status",
    "power_supply_health", "power_supply_technology"};

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return "";
  }
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

[[noreturn]] void invalid(const std::string &model,
                          const std::string &detail) {
  throw std::runtime_error("invalid model configuration " + model + ": " +
                           detail);
}

void requireModelName(const std::string &model) {
  if (!std::regex_match(model, kModelName)) {
    throw std::runtime_error("model is invalid");
  }
}

void requireName(const std::string &value, const std::string &model,
                 const std::string &kind) {
  if (!std::regex_match(value, kName)) {
    invalid(model, kind + " name is invalid");
  }
}

std::string removeYamlComment(const std::string &line) {
  bool single_quoted = false;
  bool double_quoted = false;
  bool escaped = false;
  for (std::size_t index = 0; index < line.size(); ++index) {
    const char value = line[index];
    if (double_quoted && value == '\\' && !escaped) {
      escaped = true;
      continue;
    }
    if (value == '\'' && !double_quoted && !escaped) {
      single_quoted = !single_quoted;
    } else if (value == '"' && !single_quoted && !escaped) {
      double_quoted = !double_quoted;
    } else if (value == '#' && !single_quoted && !double_quoted &&
               (index == 0 || line[index - 1] == ' ')) {
      return line.substr(0, index);
    }
    escaped = false;
  }
  return line;
}

std::vector<YamlLine> tokenizeYaml(const std::string &content,
                                   const std::string &model) {
  std::vector<YamlLine> lines;
  std::istringstream input(content);
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    if (line.find('\t') != std::string::npos) {
      invalid(model, "tabs are not allowed at line " +
                         std::to_string(line_number));
    }
    line = removeYamlComment(line);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const auto first = line.find_first_not_of(' ');
    if (first == std::string::npos) {
      continue;
    }
    lines.push_back(YamlLine{line_number, first, trim(line.substr(first))});
  }
  if (lines.empty()) {
    invalid(model, "file is empty");
  }
  return lines;
}

Property splitProperty(const std::string &text, const std::string &model,
                       std::size_t line_number) {
  const auto separator = text.find(':');
  if (separator == std::string::npos) {
    invalid(model, "expected key: value at line " +
                       std::to_string(line_number));
  }
  Property property{trim(text.substr(0, separator)),
                    trim(text.substr(separator + 1))};
  if (!std::regex_match(property.key, kYamlKey)) {
    invalid(model, "invalid key at line " + std::to_string(line_number));
  }
  return property;
}

YamlNode scalarNode(std::string value, const std::string &model,
                    std::size_t line_number) {
  if (value.empty()) {
    invalid(model, "empty scalar at line " + std::to_string(line_number));
  }
  if (value.front() == '\'' || value.front() == '"') {
    if (value.size() < 2 || value.back() != value.front()) {
      invalid(model, "unterminated quoted scalar at line " +
                         std::to_string(line_number));
    }
    value = value.substr(1, value.size() - 2);
  } else if (value.back() == '\'' || value.back() == '"') {
    invalid(model, "unterminated quoted scalar at line " +
                       std::to_string(line_number));
  }
  YamlNode node;
  node.type = YamlType::kScalar;
  node.scalar = std::move(value);
  return node;
}

bool sequenceLine(const std::string &text) {
  return text == "-" || text.rfind("- ", 0) == 0;
}

void insertProperty(std::map<std::string, YamlNode> &mapping,
                    const std::string &key, YamlNode value,
                    const std::string &model, std::size_t line_number) {
  if (!mapping.emplace(key, std::move(value)).second) {
    invalid(model, "duplicate key " + key + " at line " +
                       std::to_string(line_number));
  }
}

YamlNode parseYamlBlock(const std::vector<YamlLine> &lines, std::size_t &index,
                        std::size_t indent, const std::string &model) {
  if (index >= lines.size() || lines[index].indent != indent) {
    invalid(model, "invalid indentation");
  }
  const bool is_sequence = sequenceLine(lines[index].text);
  YamlNode result;
  result.type = is_sequence ? YamlType::kSequence : YamlType::kMap;

  while (index < lines.size() && lines[index].indent >= indent) {
    const auto line = lines[index];
    if (line.indent != indent) {
      invalid(model, "unexpected indentation at line " +
                         std::to_string(line.number));
    }
    if (sequenceLine(line.text) != is_sequence) {
      invalid(model, "cannot mix mappings and sequences at line " +
                         std::to_string(line.number));
    }

    if (!is_sequence) {
      const auto property = splitProperty(line.text, model, line.number);
      ++index;
      if (property.value.empty()) {
        if (index >= lines.size() || lines[index].indent <= indent) {
          invalid(model, "missing value for " + property.key + " at line " +
                             std::to_string(line.number));
        }
        auto child = parseYamlBlock(lines, index, lines[index].indent, model);
        insertProperty(result.mapping, property.key, std::move(child), model,
                       line.number);
      } else {
        insertProperty(result.mapping, property.key,
                       scalarNode(property.value, model, line.number), model,
                       line.number);
        if (index < lines.size() && lines[index].indent > indent) {
          invalid(model, "scalar cannot have children at line " +
                             std::to_string(lines[index].number));
        }
      }
      continue;
    }

    const auto item_text = trim(line.text.substr(1));
    ++index;
    if (item_text.empty()) {
      if (index >= lines.size() || lines[index].indent <= indent) {
        invalid(model, "empty sequence item at line " +
                           std::to_string(line.number));
      }
      result.sequence.push_back(
          parseYamlBlock(lines, index, lines[index].indent, model));
      continue;
    }

    const auto property = splitProperty(item_text, model, line.number);
    if (property.value.empty()) {
      invalid(model, "inline sequence key requires a scalar at line " +
                         std::to_string(line.number));
    }
    YamlNode item;
    item.type = YamlType::kMap;
    insertProperty(item.mapping, property.key,
                   scalarNode(property.value, model, line.number), model,
                   line.number);
    if (index < lines.size() && lines[index].indent > indent) {
      auto continuation =
          parseYamlBlock(lines, index, lines[index].indent, model);
      if (continuation.type != YamlType::kMap) {
        invalid(model, "sequence item properties must be a mapping at line " +
                           std::to_string(line.number));
      }
      for (auto &[key, value] : continuation.mapping) {
        insertProperty(item.mapping, key, std::move(value), model,
                       line.number);
      }
    }
    result.sequence.push_back(std::move(item));
  }
  return result;
}

YamlNode parseYaml(const std::string &content, const std::string &model) {
  const auto lines = tokenizeYaml(content, model);
  if (lines.front().indent != 0) {
    invalid(model, "top-level keys must not be indented");
  }
  std::size_t index = 0;
  auto root = parseYamlBlock(lines, index, 0, model);
  if (index != lines.size() || root.type != YamlType::kMap) {
    invalid(model, "top level must be a mapping");
  }
  return root;
}

const std::map<std::string, YamlNode> &asMap(const YamlNode &node,
                                             const std::string &model,
                                             const std::string &context) {
  if (node.type != YamlType::kMap) {
    invalid(model, context + " must be a mapping");
  }
  return node.mapping;
}

const std::vector<YamlNode> &asSequence(const YamlNode &node,
                                        const std::string &model,
                                        const std::string &context) {
  if (node.type != YamlType::kSequence) {
    invalid(model, context + " must be a sequence");
  }
  return node.sequence;
}

const YamlNode &requiredNode(const std::map<std::string, YamlNode> &mapping,
                             const std::string &key,
                             const std::string &model,
                             const std::string &context) {
  const auto found = mapping.find(key);
  if (found == mapping.end()) {
    invalid(model, "missing " + context + "." + key);
  }
  return found->second;
}

std::string asScalar(const YamlNode &node, const std::string &model,
                     const std::string &context) {
  if (node.type != YamlType::kScalar) {
    invalid(model, context + " must be a scalar");
  }
  return node.scalar;
}

std::string requiredScalar(const std::map<std::string, YamlNode> &mapping,
                           const std::string &key, const std::string &model,
                           const std::string &context) {
  return asScalar(requiredNode(mapping, key, model, context), model,
                  context + "." + key);
}

std::string optionalScalar(const std::map<std::string, YamlNode> &mapping,
                           const std::string &key, const std::string &model,
                           const std::string &context) {
  const auto found = mapping.find(key);
  return found == mapping.end()
             ? ""
             : asScalar(found->second, model, context + "." + key);
}

void validateKeys(const std::map<std::string, YamlNode> &mapping,
                  const std::set<std::string> &allowed,
                  const std::string &model, const std::string &context) {
  for (const auto &[key, value] : mapping) {
    (void)value;
    if (allowed.find(key) == allowed.end()) {
      invalid(model, "unknown property " + context + "." + key);
    }
  }
}

std::uint32_t parseUnsigned(const std::string &value, std::uint32_t maximum,
                            const std::string &model,
                            const std::string &context) {
  if (value.empty() || value.front() == '-') {
    invalid(model, context + " must be an unsigned integer");
  }
  std::size_t length = 0;
  unsigned long long parsed = 0;
  try {
    const auto base = value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0
                          ? 16
                          : 10;
    parsed = std::stoull(value, &length, base);
  } catch (const std::exception &) {
    invalid(model, context + " must be an unsigned integer");
  }
  if (length != value.size() || parsed > maximum) {
    invalid(model, context + " is out of range");
  }
  return static_cast<std::uint32_t>(parsed);
}

double parseDouble(const std::string &value, const std::string &model,
                   const std::string &context) {
  std::size_t length = 0;
  double parsed = 0.0;
  try {
    parsed = std::stod(value, &length);
  } catch (const std::exception &) {
    invalid(model, context + " must be a finite number");
  }
  if (length != value.size() || !std::isfinite(parsed)) {
    invalid(model, context + " must be a finite number");
  }
  return parsed;
}

bool parseBoolean(const std::string &value, const std::string &model,
                  const std::string &context) {
  if (value == "true") {
    return true;
  }
  if (value == "false") {
    return false;
  }
  invalid(model, context + " must be true or false");
}

std::vector<std::uint8_t> parseBytes(const std::string &value,
                                     const std::string &model,
                                     const std::string &context) {
  std::istringstream input(value);
  std::vector<std::uint8_t> bytes;
  std::string token;
  while (input >> token) {
    if (token.rfind("0x", 0) != 0 && token.rfind("0X", 0) != 0) {
      token = "0x" + token;
    }
    const auto byte = parseUnsigned(token, 0xFFU, model, context);
    bytes.push_back(static_cast<std::uint8_t>(byte));
  }
  if (bytes.size() > 8) {
    invalid(model, context + " may contain at most 8 bytes");
  }
  return bytes;
}

std::vector<std::string> parseList(const std::string &value,
                                   const std::string &model,
                                   const std::string &context) {
  std::vector<std::string> values;
  std::set<std::string> unique;
  std::size_t start = 0;
  while (start <= value.size()) {
    const auto separator = value.find(',', start);
    const auto item = trim(value.substr(start, separator - start));
    if (item.empty() || !unique.insert(item).second) {
      invalid(model, context + " contains an empty or duplicate value");
    }
    values.push_back(item);
    if (separator == std::string::npos) {
      break;
    }
    start = separator + 1;
  }
  return values;
}

std::map<std::uint8_t, std::uint8_t> parseValueMap(
    const std::string &value, const std::string &model,
    const std::string &context) {
  std::map<std::uint8_t, std::uint8_t> result;
  for (const auto &item : parseList(value, model, context)) {
    const auto separator = item.find(':');
    if (separator == std::string::npos) {
      invalid(model, context + " entries must use input:output");
    }
    const auto input = parseUnsigned(trim(item.substr(0, separator)), 0xFFU,
                                     model, context);
    const auto output = parseUnsigned(trim(item.substr(separator + 1)), 0xFFU,
                                      model, context);
    if (!result.emplace(static_cast<std::uint8_t>(input),
                        static_cast<std::uint8_t>(output))
             .second) {
      invalid(model, context + " has a duplicate input value");
    }
  }
  return result;
}

FieldConfig parseField(const YamlNode &node, const std::string &model,
                       const std::string &context) {
  const auto &field_map = asMap(node, model, context);
  validateKeys(field_map,
               {"metric", "offset", "length", "encoding", "endian",
                "scale", "bias", "value_map"},
               model, context);
  FieldConfig field;
  field.metric = requiredScalar(field_map, "metric", model, context);
  if (kMetrics.find(field.metric) == kMetrics.end()) {
    invalid(model, context + ".metric is unknown");
  }
  field.offset = parseUnsigned(
      requiredScalar(field_map, "offset", model, context), 7U, model,
      context + ".offset");
  field.length = parseUnsigned(
      requiredScalar(field_map, "length", model, context), 8U, model,
      context + ".length");
  if (field.length == 0 || field.offset + field.length > 8) {
    invalid(model, context + " exceeds a CAN frame");
  }
  field.encoding = requiredScalar(field_map, "encoding", model, context);
  if (field.encoding != "uint" && field.encoding != "int") {
    invalid(model, context + ".encoding must be uint or int");
  }
  field.endian = requiredScalar(field_map, "endian", model, context);
  if (field.endian != "big" && field.endian != "little") {
    invalid(model, context + ".endian must be big or little");
  }
  field.scale = parseDouble(
      requiredScalar(field_map, "scale", model, context), model,
      context + ".scale");
  field.bias = parseDouble(requiredScalar(field_map, "bias", model, context),
                           model, context + ".bias");
  const auto value_map = optionalScalar(field_map, "value_map", model, context);
  if (!value_map.empty()) {
    field.value_map =
        parseValueMap(value_map, model, context + ".value_map");
  }
  return field;
}

Config parseModel(const embedded::Model &resource) {
  const std::string model(resource.name);
  requireModelName(model);
  const auto root = parseYaml(std::string(resource.content), model);
  const auto &root_map = asMap(root, model, "root");
  validateKeys(root_map, {"model", "can", "ros", "queries"}, model,
               "root");

  const auto &model_map = asMap(requiredNode(root_map, "model", model, "root"),
                                model, "model");
  validateKeys(model_map, {"id", "bms_model"}, model, "model");
  Config config;
  config.model = requiredScalar(model_map, "id", model, "model");
  requireModelName(config.model);
  if (config.model != model) {
    invalid(model, "model.id must match the configuration filename");
  }
  config.bms_model =
      requiredScalar(model_map, "bms_model", model, "model");
  if (config.bms_model.empty()) {
    invalid(model, "model.bms_model must not be empty");
  }

  const auto &can_map = asMap(requiredNode(root_map, "can", model, "root"),
                              model, "can");
  validateKeys(can_map,
               {"interface", "bitrate", "query_interval_ms",
                "response_timeout_ms"},
               model, "can");
  config.can.interface = requiredScalar(can_map, "interface", model, "can");
  if (!std::regex_match(config.can.interface, kName)) {
    invalid(model, "can.interface is invalid");
  }
  config.can.bitrate = static_cast<int>(parseUnsigned(
      requiredScalar(can_map, "bitrate", model, "can"), 10000000U, model,
      "can.bitrate"));
  if (config.can.bitrate <= 0) {
    invalid(model, "can.bitrate must be positive");
  }
  config.can.query_interval_ms = static_cast<int>(parseUnsigned(
      requiredScalar(can_map, "query_interval_ms", model, "can"), 60000U,
      model, "can.query_interval_ms"));
  config.can.response_timeout_ms = static_cast<int>(parseUnsigned(
      requiredScalar(can_map, "response_timeout_ms", model, "can"), 60000U,
      model, "can.response_timeout_ms"));
  if (config.can.query_interval_ms <= 0 || config.can.response_timeout_ms <= 0) {
    invalid(model, "CAN intervals must be positive");
  }

  const auto &ros_map = asMap(requiredNode(root_map, "ros", model, "root"),
                              model, "ros");
  validateKeys(ros_map,
               {"topic", "frame_id", "localhost_only", "domain_id",
                "qos_depth"},
               model, "ros");
  config.ros.topic = requiredScalar(ros_map, "topic", model, "ros");
  config.ros.frame_id = requiredScalar(ros_map, "frame_id", model, "ros");
  config.ros.localhost_only = parseBoolean(
      requiredScalar(ros_map, "localhost_only", model, "ros"), model,
      "ros.localhost_only");
  config.ros.domain_id = static_cast<int>(parseUnsigned(
      requiredScalar(ros_map, "domain_id", model, "ros"),
      static_cast<std::uint32_t>(std::numeric_limits<int>::max()), model,
      "ros.domain_id"));
  config.ros.qos_depth = static_cast<int>(parseUnsigned(
      requiredScalar(ros_map, "qos_depth", model, "ros"),
      static_cast<std::uint32_t>(std::numeric_limits<int>::max()), model,
      "ros.qos_depth"));
  if (config.ros.topic.empty() || config.ros.frame_id.empty() ||
      config.ros.qos_depth <= 0) {
    invalid(model, "ROS topic, frame ID and QoS depth must be valid");
  }

  const auto &query_nodes = asSequence(
      requiredNode(root_map, "queries", model, "root"), model, "queries");
  std::set<std::string> query_names;
  for (std::size_t query_index = 0; query_index < query_nodes.size();
       ++query_index) {
    const auto context = "queries[" + std::to_string(query_index) + "]";
    const auto &query_map = asMap(query_nodes[query_index], model, context);
    validateKeys(query_map,
                 {"name", "send_request", "request", "responses"}, model,
                 context);
    QueryConfig query;
    query.name = requiredScalar(query_map, "name", model, context);
    requireName(query.name, model, "query");
    if (!query_names.insert(query.name).second) {
      invalid(model, "duplicate query name " + query.name);
    }
    query.send_request = parseBoolean(
        requiredScalar(query_map, "send_request", model, context), model,
        context + ".send_request");

    const auto request = query_map.find("request");
    if (request == query_map.end() && query.send_request) {
      invalid(model, context + ".request is required when send_request is true");
    }
    if (request != query_map.end()) {
      const auto request_context = context + ".request";
      const auto &request_map = asMap(request->second, model, request_context);
      validateKeys(request_map, {"id", "extended", "data"}, model,
                   request_context);
      query.request_id = parseUnsigned(
          requiredScalar(request_map, "id", model, request_context),
          0x1FFFFFFFU, model, request_context + ".id");
      query.extended = parseBoolean(
          requiredScalar(request_map, "extended", model, request_context),
          model, request_context + ".extended");
      query.request_data = parseBytes(
          requiredScalar(request_map, "data", model, request_context), model,
          request_context + ".data");
    }

    const auto &response_nodes = asSequence(
        requiredNode(query_map, "responses", model, context), model,
        context + ".responses");
    std::set<std::string> response_names;
    for (std::size_t response_index = 0;
         response_index < response_nodes.size(); ++response_index) {
      const auto response_context =
          context + ".responses[" + std::to_string(response_index) + "]";
      const auto &response_map =
          asMap(response_nodes[response_index], model, response_context);
      validateKeys(response_map,
                   {"name", "id", "id_mask", "extended", "fields"}, model,
                   response_context);
      const auto response_name =
          requiredScalar(response_map, "name", model, response_context);
      requireName(response_name, model, "response");
      if (!response_names.insert(response_name).second) {
        invalid(model, "duplicate response name " + response_name +
                           " in query " + query.name);
      }

      ResponseConfig response;
      response.id = parseUnsigned(
          requiredScalar(response_map, "id", model, response_context),
          0x1FFFFFFFU, model, response_context + ".id");
      const auto id_mask =
          optionalScalar(response_map, "id_mask", model, response_context);
      response.id_mask = id_mask.empty()
                             ? 0U
                             : parseUnsigned(id_mask, 0x1FFFFFFFU, model,
                                             response_context + ".id_mask");
      response.extended = parseBoolean(
          requiredScalar(response_map, "extended", model, response_context),
          model, response_context + ".extended");

      const auto &field_nodes = asSequence(
          requiredNode(response_map, "fields", model, response_context), model,
          response_context + ".fields");
      for (std::size_t field_index = 0; field_index < field_nodes.size();
           ++field_index) {
        response.fields.push_back(parseField(
            field_nodes[field_index], model,
            response_context + ".fields[" + std::to_string(field_index) +
                "]"));
      }
      if (response.fields.empty()) {
        invalid(model, response_context + ".fields must not be empty");
      }
      query.responses.push_back(std::move(response));
    }
    if (query.responses.empty()) {
      invalid(model, context + ".responses must not be empty");
    }
    config.can.queries.push_back(std::move(query));
  }
  if (config.can.queries.empty()) {
    invalid(model, "queries must not be empty");
  }
  return config;
}

}  // namespace

Config loadModel(const std::string &model) {
  requireModelName(model);
  for (const auto &resource : embedded::kModels) {
    if (resource.name == model) {
      return parseModel(resource);
    }
  }
  throw std::runtime_error("unsupported model " + model);
}

std::vector<ModelInfo> supportedModels() {
  std::vector<ModelInfo> models;
  models.reserve(embedded::kModels.size());
  for (const auto &resource : embedded::kModels) {
    const auto config = parseModel(resource);
    models.push_back(ModelInfo{config.model, config.bms_model});
  }
  return models;
}

}  // namespace batcan
