#include "armonik/sdk/common/Configuration.h"
#include "armonik/sdk/common/Utils.h"
#include <armonik/common/logger/level.h>
#include <armonik/common/options/ComputePlane.h>
#include <armonik/common/options/ControlPlane.h>
#include <armonik/common/utils/Configuration.h>

#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

/**
 * @brief Proxy function definitions
 */

namespace ArmoniK {
namespace Sdk {
namespace Common {

// Compute Plane
ComputePlane::ComputePlane(const Configuration &configuration)
    : impl(std::unique_ptr<armonik::api::common::options::ComputePlane>(
          new armonik::api::common::options::ComputePlane(*configuration.impl))) {}
ComputePlane::ComputePlane(const ComputePlane &computeplane)
    : impl(std::unique_ptr<armonik::api::common::options::ComputePlane>(
          new armonik::api::common::options::ComputePlane(*computeplane.impl))) {}
ComputePlane::ComputePlane(ComputePlane &&) noexcept = default;

ComputePlane &ComputePlane::operator=(const ComputePlane &computeplane) {
  impl = std::unique_ptr<armonik::api::common::options::ComputePlane>(
      new armonik::api::common::options::ComputePlane(*computeplane.impl));
  return *this;
}
ComputePlane &ComputePlane::operator=(ComputePlane &&) noexcept = default;
ComputePlane::~ComputePlane() = default;

armonik::api::string_view ComputePlane::get_server_address() const { return impl->get_server_address(); }
void ComputePlane::set_worker_address(std::string socket_address) {
  impl->set_worker_address(std::move(socket_address));
}

void ComputePlane::set_agent_address(std::string agent_address) { impl->set_agent_address(std::move(agent_address)); }
armonik::api::string_view ComputePlane::get_agent_address() const { return impl->get_agent_address(); }

const armonik::api::common::options::ComputePlane &ComputePlane::get_impl() const {
  const static armonik::api::common::options::ComputePlane default_config =
      armonik::api::common::options::ComputePlane(armonik::api::common::utils::Configuration());
  if (impl) {
    return *impl;
  }
  return default_config;
}
armonik::api::common::options::ComputePlane &ComputePlane::set_impl() {
  if (!impl) {
    impl = std::unique_ptr<armonik::api::common::options::ComputePlane>(
        new armonik::api::common::options::ComputePlane(armonik::api::common::utils::Configuration()));
  }
  return *impl;
}

// Control Plane

namespace {
/**
 * @brief Warn on stderr that the value of a key is ignored, once per key and value in the process
 * @details The control plane options are read before any logger exists, and several times per session.
 */
void warnIgnoredValue(const std::string &key, const std::string &value_str, std::int64_t default_value,
                      const char *reason) {
  static std::mutex mutex;
  static std::set<std::pair<std::string, std::string>> warned;
  std::lock_guard<std::mutex> lock(mutex);
  if (warned.emplace(key, value_str).second) {
    std::cerr << "ArmoniK SDK: ignoring " << key << "='" << value_str << "' (" << reason << "), using default "
              << default_value << std::endl;
  }
}

/**
 * @brief Read a positive integer option, up to max_value
 * @details An unset key gives default_value. 0 is accepted when it is the default, since it then means "not set".
 * Any other value that is not a positive integer up to max_value gives default_value with a warning.
 */
std::int64_t getPositiveFromConfig(const Configuration &config, const std::string &key, std::int64_t default_value,
                                   std::int64_t max_value) {
  const auto value_str = config.get(key);
  const auto first = value_str.find_first_not_of(" \t");
  if (first == std::string::npos) {
    return default_value;
  }
  const auto last = value_str.find_last_not_of(" \t");
  const auto trimmed = value_str.substr(first, last - first + 1);

  std::int64_t value = 0;
  try {
    std::size_t parsed = 0;
    value = std::stoll(trimmed, &parsed);
    if (parsed != trimmed.size()) {
      warnIgnoredValue(key, value_str, default_value, "not an integer");
      return default_value;
    }
  } catch (const std::out_of_range &) {
    warnIgnoredValue(key, value_str, default_value, "out of range");
    return default_value;
  } catch (const std::invalid_argument &) {
    warnIgnoredValue(key, value_str, default_value, "not an integer");
    return default_value;
  }

  if (value == 0 && default_value == 0) {
    return 0;
  }
  if (value <= 0) {
    warnIgnoredValue(key, value_str, default_value, "must be positive");
    return default_value;
  }
  if (value > max_value) {
    warnIgnoredValue(key, value_str, default_value, "out of range");
    return default_value;
  }
  return value;
}

int getIntFromConfig(const Configuration &config, const std::string &key, int default_value) {
  return static_cast<int>(getPositiveFromConfig(config, key, default_value, std::numeric_limits<int>::max()));
}

std::int64_t getInt64FromConfig(const Configuration &config, const std::string &key, std::int64_t default_value) {
  return getPositiveFromConfig(config, key, default_value, std::numeric_limits<std::int64_t>::max());
}
} // namespace

ControlPlane::ControlPlane(const Configuration &config)
    : impl(std::unique_ptr<armonik::api::common::options::ControlPlane>(
          new armonik::api::common::options::ControlPlane(*config.impl))),
      wait_batch_size_(getIntFromConfig(config, "GrpcClient__WaitBatchSize", 200)),
      submit_batch_size_(getIntFromConfig(config, "GrpcClient__SubmitBatchSize", 200)),
      thread_pool_size_(getIntFromConfig(config, "GrpcClient__ThreadPoolSize", 0)),
      override_message_size_(getIntFromConfig(config, "GrpcClient__OverrideMessageSize", 0)),
      download_max_retry_(getIntFromConfig(config, "GrpcClient__DownloadMaxRetry", 3)),
      download_byte_budget_(getInt64FromConfig(config, "GrpcClient__DownloadByteBudget", 0)),
      upload_byte_budget_(getInt64FromConfig(config, "GrpcClient__UploadByteBudget", 0)) {}

ControlPlane::ControlPlane(const ControlPlane &controlplane)
    : impl(std::unique_ptr<armonik::api::common::options::ControlPlane>(
          new armonik::api::common::options::ControlPlane(*controlplane.impl))),
      wait_batch_size_(controlplane.wait_batch_size_), submit_batch_size_(controlplane.submit_batch_size_),
      thread_pool_size_(controlplane.thread_pool_size_), override_message_size_(controlplane.override_message_size_),
      download_max_retry_(controlplane.download_max_retry_), download_byte_budget_(controlplane.download_byte_budget_),
      upload_byte_budget_(controlplane.upload_byte_budget_) {}
ControlPlane::ControlPlane(ControlPlane &&) noexcept = default;

ControlPlane &ControlPlane::operator=(const ControlPlane &controlplane) {
  impl = std::unique_ptr<armonik::api::common::options::ControlPlane>(
      new armonik::api::common::options::ControlPlane(*controlplane.impl));
  wait_batch_size_ = controlplane.wait_batch_size_;
  submit_batch_size_ = controlplane.submit_batch_size_;
  thread_pool_size_ = controlplane.thread_pool_size_;
  override_message_size_ = controlplane.override_message_size_;
  download_max_retry_ = controlplane.download_max_retry_;
  download_byte_budget_ = controlplane.download_byte_budget_;
  upload_byte_budget_ = controlplane.upload_byte_budget_;
  return *this;
}
ControlPlane &ControlPlane::operator=(ControlPlane &&) noexcept = default;
ControlPlane::~ControlPlane() = default;

armonik::api::string_view ControlPlane::getEndpoint() const { return impl->getEndpoint(); }
armonik::api::string_view ControlPlane::getUserCertPemPath() const { return impl->getUserCertPemPath(); }
armonik::api::string_view ControlPlane::getUserKeyPemPath() const { return impl->getUserKeyPemPath(); }
armonik::api::string_view ControlPlane::getUserP12Path() const { return impl->getUserP12Path(); }
armonik::api::string_view ControlPlane::getCaCertPemPath() const { return impl->getCaCertPemPath(); }
bool ControlPlane::isSslValidation() const { return impl->isSslValidation(); }

int ControlPlane::getWaitBatchSize() const { return wait_batch_size_; }
int ControlPlane::getSubmitBatchSize() const { return submit_batch_size_; }
int ControlPlane::getThreadPoolSize() const { return thread_pool_size_; }
int ControlPlane::getOverrideMessageSize() const { return override_message_size_; }
int ControlPlane::getDownloadMaxRetry() const { return download_max_retry_; }
std::int64_t ControlPlane::getDownloadByteBudget() const { return download_byte_budget_; }
std::int64_t ControlPlane::getUploadByteBudget() const { return upload_byte_budget_; }

const armonik::api::common::options::ControlPlane &ControlPlane::get_impl() const {
  const static armonik::api::common::options::ControlPlane default_config =
      armonik::api::common::options::ControlPlane(armonik::api::common::utils::Configuration());
  if (impl) {
    return *impl;
  }
  return default_config;
}

armonik::api::common::options::ControlPlane &ControlPlane::set_impl() {
  if (!impl) {
    impl = std::unique_ptr<armonik::api::common::options::ControlPlane>(
        new armonik::api::common::options::ControlPlane(armonik::api::common::utils::Configuration()));
  }
  return *impl;
}

// Configuration

Configuration::Configuration()
    : impl(std::unique_ptr<armonik::api::common::utils::Configuration>(
          new armonik::api::common::utils::Configuration())) {}
Configuration::Configuration(const Configuration &config)
    : impl(std::unique_ptr<armonik::api::common::utils::Configuration>(
          new armonik::api::common::utils::Configuration(*config.impl))) {}
Configuration::Configuration(Configuration &&) noexcept = default;

Configuration::~Configuration() = default;

Configuration &Configuration::operator=(Configuration &&) noexcept = default;
Configuration &Configuration::operator=(const Configuration &config) {
  impl = std::unique_ptr<armonik::api::common::utils::Configuration>(
      new armonik::api::common::utils::Configuration(*config.impl));
  return *this;
}

std::string Configuration::get(const std::string &key) const { return get_impl().get(key); }
void Configuration::set(const std::string &key, const std::string &value) { set_impl().set(key, value); }

const std::map<std::string, std::string> &Configuration::list() const { return get_impl().list(); }
Configuration &Configuration::add_json_configuration(armonik::api::string_view file_path) {
  set_impl().add_json_configuration(file_path);
  return *this;
}

Configuration &Configuration::add_env_configuration() {
  set_impl().add_env_configuration();
  return *this;
}

ComputePlane Configuration::get_compute_plane() const { return *this; }
ControlPlane Configuration::get_control_plane() const { return *this; }

Configuration::operator armonik::api::common::utils::Configuration() { return get_impl(); }

armonik::api::common::utils::Configuration &Configuration::set_impl() {
  if (!impl) {
    impl =
        std::unique_ptr<armonik::api::common::utils::Configuration>(new armonik::api::common::utils::Configuration());
  }
  return *impl;
}

const armonik::api::common::utils::Configuration &Configuration::get_impl() const {
  const static armonik::api::common::utils::Configuration default_config;
  if (impl) {
    return *impl;
  }
  return default_config;
}

armonik::api::common::logger::Level Configuration::get_log_level() const {
  auto rawLevel = ArmoniK::Sdk::Common::to_capitalized(get("Serilog__MinimumLevel"));

  if (level_name(armonik::api::common::logger::Level::Verbose) == rawLevel) {
    return armonik::api::common::logger::Level::Verbose;
  } else if (level_name(armonik::api::common::logger::Level::Debug) == rawLevel) {
    return armonik::api::common::logger::Level::Debug;
  } else if (level_name(armonik::api::common::logger::Level::Info) == rawLevel) {
    return armonik::api::common::logger::Level::Info;
  } else if (level_name(armonik::api::common::logger::Level::Warning) == rawLevel) {
    return armonik::api::common::logger::Level::Warning;
  } else if (level_name(armonik::api::common::logger::Level::Error) == rawLevel) {
    return armonik::api::common::logger::Level::Error;
  } else if (level_name(armonik::api::common::logger::Level::Fatal) == rawLevel) {
    return armonik::api::common::logger::Level::Fatal;
  }
  return armonik::api::common::logger::Level::Info;
}

} // namespace Common
} // namespace Sdk
} // namespace ArmoniK
