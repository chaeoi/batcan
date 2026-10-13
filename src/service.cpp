#include "batcan/service.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "batcan/config.hpp"

namespace batcan {
namespace {

constexpr const char *kInstallDirectory = "/opt/batcan";
constexpr const char *kInstalledBinary = "/opt/batcan/batcan";
constexpr const char *kInstalledConfig = "/opt/batcan/config.yml";
constexpr const char *kUnitPath =
    "/etc/systemd/system/batcan.service";
constexpr const char *kLogDirectory = "/var/log/batcan";
constexpr const char *kPrivateLogDirectory = "/var/log/private/batcan";
constexpr const char *kROSSetup = "/opt/ros/humble/setup.bash";
constexpr auto kInitialUpdateDelay = std::chrono::seconds(30);
constexpr auto kUpdateInterval = std::chrono::hours(24);

void requireRoot() {
  if (::geteuid() != 0) {
    throw std::runtime_error(
        "service changes require root; run with sudo");
  }
}

void writeFileAtomic(const std::string &path, const std::string &content,
                     mode_t mode) {
  const auto temporary = path + ".tmp." + std::to_string(::getpid());
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
      throw std::runtime_error("cannot write " + temporary);
    }
    stream << content;
    stream.flush();
    if (!stream) {
      throw std::runtime_error("cannot flush " + temporary);
    }
  }
  if (::chmod(temporary.c_str(), mode) != 0) {
    std::filesystem::remove(temporary);
    throw std::runtime_error("cannot set permissions on " + temporary);
  }
  std::filesystem::rename(temporary, path);
}

int runCommand(const std::vector<std::string> &arguments,
               bool allow_failure = false) {
  const auto child = ::fork();
  if (child < 0) {
    throw std::runtime_error("fork failed");
  }
  if (child == 0) {
    std::vector<char *> values;
    values.reserve(arguments.size() + 1);
    for (const auto &argument : arguments) {
      values.push_back(const_cast<char *>(argument.c_str()));
    }
    values.push_back(nullptr);
    ::execvp(values[0], values.data());
    _exit(127);
  }
  int status = 0;
  if (::waitpid(child, &status, 0) < 0) {
    throw std::runtime_error("waitpid failed");
  }
  const auto code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
  if (code != 0 && !allow_failure) {
    throw std::runtime_error(arguments.front() + " exited with status " +
                             std::to_string(code));
  }
  return code;
}

std::string runCommandOutput(const std::vector<std::string> &arguments) {
  int pipefd[2];
  if (::pipe(pipefd) < 0) {
    throw std::runtime_error("pipe failed");
  }
  const auto child = ::fork();
  if (child < 0) {
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    throw std::runtime_error("fork failed");
  }
  if (child == 0) {
    ::close(pipefd[0]);
    ::dup2(pipefd[1], STDOUT_FILENO);
    ::close(pipefd[1]);
    std::vector<char *> values;
    values.reserve(arguments.size() + 1);
    for (const auto &argument : arguments) {
      values.push_back(const_cast<char *>(argument.c_str()));
    }
    values.push_back(nullptr);
    ::execvp(values[0], values.data());
    _exit(127);
  }
  ::close(pipefd[1]);
  std::string output;
  char buffer[4096];
  ssize_t bytes = 0;
  while ((bytes = ::read(pipefd[0], buffer, sizeof(buffer))) > 0) {
    output.append(buffer, bytes);
  }
  ::close(pipefd[0]);
  int status = 0;
  ::waitpid(child, &status, 0);
  const auto code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
  if (code != 0) {
    throw std::runtime_error(arguments.front() + " exited with status " +
                             std::to_string(code));
  }
  return output;
}

std::string serviceUnit() {
  std::string unit =
      "[Unit]\n"
      "Description=CAN battery to ROS2 bridge\n"
      "After=network-online.target\n"
      "Wants=network-online.target\n\n"
      "[Service]\n"
      "Type=simple\n";
  unit += "ExecStart=/bin/bash -lc 'source ";
  unit += kROSSetup;
  unit += " && exec /opt/batcan/batcan run --config ";
  unit += kInstalledConfig;
  unit +=
      "'\nRestart=always\nRestartSec=3\n"
      "Environment=ROS_LOCALHOST_ONLY=1\n"
      "Environment=ROS_LOG_DIR=/var/log/batcan/ros\n"
      "LogsDirectory=batcan\nLogsDirectoryMode=0750\n"
      "RestrictAddressFamilies=AF_UNIX AF_NETLINK AF_CAN AF_INET AF_INET6\n\n"
      "[Install]\nWantedBy=multi-user.target\n";
  return unit;
}

namespace sha256 {

inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
inline uint32_t sig0(uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
inline uint32_t sig1(uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
inline uint32_t theta0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
inline uint32_t theta1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

const uint32_t K[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

std::string fileSha256(const std::string &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return "";
  }
  uint32_t h[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
  };
  std::vector<uint8_t> buffer(64);
  uint64_t total_bytes = 0;
  while (stream) {
    stream.read(reinterpret_cast<char*>(buffer.data()), 64);
    const auto count = stream.gcount();
    if (count == 64) {
      total_bytes += 64;
      uint32_t w[64];
      for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(buffer[i * 4]) << 24) |
               (static_cast<uint32_t>(buffer[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(buffer[i * 4 + 2]) << 8) |
               (static_cast<uint32_t>(buffer[i * 4 + 3]));
      }
      for (int i = 16; i < 64; ++i) {
        w[i] = theta1(w[i - 2]) + w[i - 7] + theta0(w[i - 15]) + w[i - 16];
      }
      uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
      uint32_t e = h[4], f = h[5], g = h[6], h_val = h[7];
      for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h_val + sig1(e) + ch(e, f, g) + K[i] + w[i];
        uint32_t t2 = sig0(a) + maj(a, b, c);
        h_val = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
      }
      h[0] += a; h[1] += b; h[2] += c; h[3] += d;
      h[4] += e; h[5] += f; h[6] += g; h[7] += h_val;
    } else {
      total_bytes += static_cast<uint64_t>(count);
      buffer[count] = 0x80;
      size_t pad_len = count + 1;
      if (pad_len > 56) {
        std::fill(buffer.begin() + pad_len, buffer.end(), 0);
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
          w[i] = (static_cast<uint32_t>(buffer[i * 4]) << 24) |
                 (static_cast<uint32_t>(buffer[i * 4 + 1]) << 16) |
                 (static_cast<uint32_t>(buffer[i * 4 + 2]) << 8) |
                 (static_cast<uint32_t>(buffer[i * 4 + 3]));
        }
        for (int i = 16; i < 64; ++i) {
          w[i] = theta1(w[i - 2]) + w[i - 7] + theta0(w[i - 15]) + w[i - 16];
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], h_val = h[7];
        for (int i = 0; i < 64; ++i) {
          uint32_t t1 = h_val + sig1(e) + ch(e, f, g) + K[i] + w[i];
          uint32_t t2 = sig0(a) + maj(a, b, c);
          h_val = g; g = f; f = e; e = d + t1;
          d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += h_val;
        pad_len = 0;
      }
      std::fill(buffer.begin() + pad_len, buffer.end(), 0);
      uint64_t bit_len = total_bytes * 8;
      for (int i = 0; i < 8; ++i) {
        buffer[63 - i] = static_cast<uint8_t>((bit_len >> (i * 8)) & 0xff);
      }
      uint32_t w[64];
      for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(buffer[i * 4]) << 24) |
               (static_cast<uint32_t>(buffer[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(buffer[i * 4 + 2]) << 8) |
               (static_cast<uint32_t>(buffer[i * 4 + 3]));
      }
      for (int i = 16; i < 64; ++i) {
        w[i] = theta1(w[i - 2]) + w[i - 7] + theta0(w[i - 15]) + w[i - 16];
      }
      uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
      uint32_t e = h[4], f = h[5], g = h[6], h_val = h[7];
      for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h_val + sig1(e) + ch(e, f, g) + K[i] + w[i];
        uint32_t t2 = sig0(a) + maj(a, b, c);
        h_val = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
      }
      h[0] += a; h[1] += b; h[2] += c; h[3] += d;
      h[4] += e; h[5] += f; h[6] += g; h[7] += h_val;
      break;
    }
  }
  std::ostringstream oss;
  for (int i = 0; i < 8; ++i) {
    oss << std::hex << std::setw(8) << std::setfill('0') << h[i];
  }
  return oss.str();
}

}  // namespace sha256

std::string getArchitectureAsset() {
  struct utsname uts;
  if (::uname(&uts) < 0) {
    throw std::runtime_error("uname failed");
  }
  const std::string machine = uts.machine;
  if (machine == "aarch64" || machine == "arm64") {
    return "batcan-linux-arm64";
  }
  if (machine == "x86_64" || machine == "amd64") {
    return "batcan-linux-amd64";
  }
  throw std::runtime_error("unsupported architecture: " + machine);
}

std::string extractExpectedHash(const std::string &sha256sums_content,
                                const std::string &asset_name) {
  std::istringstream stream(sha256sums_content);
  std::string line;
  while (std::getline(stream, line)) {
    std::istringstream line_stream(line);
    std::string hash;
    std::string name;
    if (line_stream >> hash >> name) {
      if (!name.empty() && name.front() == '*') {
        name = name.substr(1);
      }
      if (name == asset_name) {
        return hash;
      }
    }
  }
  return "";
}

bool downloadReleaseFile(const std::string &name,
                         const std::string &destination,
                         const std::string &cache_buster) {
  const std::vector<std::string> base_urls = {
      "https://gitwarp.canghai.org/github.com/chaeoi/batcan/releases/latest/download",
      "https://github.com/chaeoi/batcan/releases/latest/download"};

  for (const auto &base : base_urls) {
    const std::string url = base + "/" + name + "?batcan_cache=" + cache_buster;
    const std::vector<std::string> cmd = {
        "curl", "--connect-timeout", "8", "--max-time", "20", "--fail",
        "--location", "--silent", "--show-error", "--retry", "1",
        url, "-o", destination};
    if (runCommand(cmd, true) == 0) {
      return true;
    }
    std::filesystem::remove(destination);
  }
  return false;
}

int performUpdate() {
  int lock_fd = ::open("/run/lock/batcan-update.lock", O_CREAT | O_RDWR, 0600);
  if (lock_fd >= 0) {
    if (::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
      ::close(lock_fd);
      return 0;
    }
  }

  struct LockGuard {
    int fd;
    ~LockGuard() {
      if (fd >= 0) {
        ::flock(fd, LOCK_UN);
        ::close(fd);
      }
    }
  } lock_guard{lock_fd};

  std::string temp_dir_template =
      (std::filesystem::temp_directory_path() / "batcan-update.XXXXXX").string();
  char *temp_dir_c = ::mkdtemp(temp_dir_template.data());
  if (!temp_dir_c) {
    throw std::runtime_error("cannot create temporary directory for update");
  }
  const std::filesystem::path temp_dir(temp_dir_c);

  struct TempDirGuard {
    std::filesystem::path path;
    ~TempDirGuard() {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  } temp_dir_guard{temp_dir};

  const auto asset = getArchitectureAsset();
  const auto now = std::chrono::system_clock::now().time_since_epoch().count();
  const std::string cache_buster =
      std::to_string(now) + "-" + std::to_string(::getpid());

  const auto downloaded_binary = (temp_dir / "batcan").string();
  const auto downloaded_sums = (temp_dir / "SHA256SUMS").string();

  if (!downloadReleaseFile(asset, downloaded_binary, cache_buster)) {
    throw std::runtime_error("cannot download release asset: " + asset);
  }
  if (!downloadReleaseFile("SHA256SUMS", downloaded_sums, cache_buster)) {
    throw std::runtime_error("cannot download release checksums (SHA256SUMS)");
  }

  std::ifstream sums_file(downloaded_sums);
  if (!sums_file) {
    throw std::runtime_error("cannot open downloaded SHA256SUMS");
  }
  std::stringstream sums_buffer;
  sums_buffer << sums_file.rdbuf();
  const auto expected_hash =
      extractExpectedHash(sums_buffer.str(), asset);
  if (expected_hash.empty()) {
    throw std::runtime_error("release checksum is missing for " + asset);
  }

  const auto actual_hash = sha256::fileSha256(downloaded_binary);
  if (actual_hash != expected_hash) {
    throw std::runtime_error("release checksum mismatch");
  }

  ::chmod(downloaded_binary.c_str(), 0755);
  std::string new_version;
  try {
    new_version = runCommandOutput({downloaded_binary, "--version"});
    while (!new_version.empty() &&
           (new_version.back() == '\n' || new_version.back() == '\r')) {
      new_version.pop_back();
    }
  } catch (const std::exception &e) {
    new_version = "unknown";
  }

  std::string current_hash;
  if (std::filesystem::exists(kInstalledBinary)) {
    current_hash = sha256::fileSha256(kInstalledBinary);
  }

  if (!current_hash.empty() && current_hash == expected_hash) {
    std::cout << "batcan is up to date: " << new_version << '\n';
    return 0;
  }

  const std::string binary_new = std::string(kInstalledBinary) + ".new";
  std::filesystem::copy_file(downloaded_binary, binary_new,
                             std::filesystem::copy_options::overwrite_existing);
  ::chmod(binary_new.c_str(), 0755);
  std::filesystem::rename(binary_new, kInstalledBinary);

  std::cout << "updated batcan to " << new_version << '\n';

  if (runCommand({"systemctl", "is-active", "--quiet", "batcan.service"}, true) == 0) {
    runCommand({"systemctl", "restart", "batcan.service"}, true);
  }
  return 0;
}

void installService(const std::vector<std::string> &arguments,
                    const std::string &executable_path) {
  requireRoot();
  bool force_config = false;
  for (const auto &argument : arguments) {
    if (argument == "--force-config") {
      force_config = true;
    } else {
      throw std::runtime_error("unknown service install option: " + argument);
    }
  }

  std::filesystem::create_directories(kInstallDirectory);
  bool valid_config = false;
  if (!force_config && std::filesystem::exists(kInstalledConfig)) {
    try {
      (void)loadConfig(kInstalledConfig);
      valid_config = true;
    } catch (const std::exception &) {
      writeFileAtomic(kInstalledConfig, defaultConfig(), 0644);
    }
  } else {
    writeFileAtomic(kInstalledConfig, defaultConfig(), 0644);
  }

  const auto temporary_binary =
      std::string(kInstalledBinary) + ".tmp." + std::to_string(::getpid());
  if (std::filesystem::weakly_canonical(executable_path) !=
      std::filesystem::weakly_canonical(kInstalledBinary)) {
    std::filesystem::copy_file(
        executable_path, temporary_binary,
        std::filesystem::copy_options::overwrite_existing);
    ::chmod(temporary_binary.c_str(), 0755);
    std::filesystem::rename(temporary_binary, kInstalledBinary);
  }
  writeFileAtomic(kUnitPath, serviceUnit(), 0644);
  runCommand({"systemctl", "daemon-reload"});
  if (!valid_config) {
    runCommand({"systemctl", "disable", "--now", "batcan.service"}, true);
    std::cout << "installed batcan.service; select one profile in "
              << kInstalledConfig << " and start it with: systemctl enable "
              << "--now batcan\n";
    return;
  }
  runCommand({"systemctl", "enable", "batcan.service"});
  runCommand({"systemctl", "restart", "batcan.service"});
  std::cout << "installed and started batcan.service with config "
            << kInstalledConfig << '\n';
  std::cout << "\n提示：\n"
            << "1. 配置文件已生成于: " << kInstalledConfig << "（默认开启 auto 自动探测全部协议）。\n"
            << "2. 默认 CAN 接口为 can5，如实际接口不同请修改 interface。\n"
            << "3. profile 列表中已包含所有支持的 BMS UUID，可直接删除不需要探测的 UUID。\n\n";
}

void uninstallService(const std::vector<std::string> &arguments) {
  requireRoot();
  if (!arguments.empty()) {
    throw std::runtime_error("service uninstall takes no options");
  }
  runCommand({"systemctl", "disable", "--now", "batcan.service"},
             true);
  std::filesystem::remove(kUnitPath);
  runCommand({"systemctl", "daemon-reload"});
  runCommand({"systemctl", "reset-failed", "batcan.service"}, true);
  std::filesystem::remove("/run/lock/batcan-update.lock");
  std::filesystem::remove_all(kLogDirectory);
  std::filesystem::remove_all(kPrivateLogDirectory);
  std::filesystem::remove_all(kInstallDirectory);
  std::cout << "已彻底卸载 batcan 服务并清理全部程序文件、配置与运行日志。\n";
}

}  // namespace

int serviceCommand(const std::vector<std::string> &arguments,
                   const std::string &executable_path) {
  if (arguments.empty()) {
    throw std::runtime_error(
        "service requires install, update, uninstall or status");
  }
  const std::vector<std::string> options(arguments.begin() + 1,
                                         arguments.end());
  if (arguments.front() == "install") {
    installService(options, executable_path);
    return 0;
  }
  if (arguments.front() == "uninstall") {
    uninstallService(options);
    return 0;
  }
  if (arguments.front() == "update") {
    requireRoot();
    if (!options.empty()) {
      throw std::runtime_error("service update takes no options");
    }
    return performUpdate();
  }
  if (arguments.front() == "status") {
    if (!options.empty()) {
      throw std::runtime_error("service status takes no options");
    }
    return runCommand({"systemctl", "status", "batcan.service"},
                      true);
  }
  throw std::runtime_error("unknown service command: " + arguments.front());
}

void automaticUpdateLoop(std::atomic_bool &stopping,
                         std::condition_variable &wake,
                         std::mutex &wake_mutex) {
  std::unique_lock lock(wake_mutex);
  if (wake.wait_for(lock, kInitialUpdateDelay,
                    [&stopping]() { return stopping.load(); })) {
    return;
  }
  while (!stopping.load()) {
    lock.unlock();
    try {
      std::cerr << "batcan automatic update check started\n";
      const auto result = performUpdate();
      if (result != 0) {
        std::cerr << "batcan automatic update exited with status "
                  << result << '\n';
      }
    } catch (const std::exception &error) {
      std::cerr << "batcan automatic update failed: " << error.what()
                << '\n';
    }
    lock.lock();
    if (wake.wait_for(lock, kUpdateInterval,
                      [&stopping]() { return stopping.load(); })) {
      return;
    }
  }
}

}  // namespace batcan
