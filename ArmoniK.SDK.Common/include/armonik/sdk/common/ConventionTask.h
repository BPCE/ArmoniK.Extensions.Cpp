#pragma once

#include <map>
#include <string>

namespace ArmoniK {
namespace Sdk {
namespace Common {

struct TaskOptions;

/**
 * @brief Decoded convention task: what a worker needs to run it, independently of how the service code is loaded.
 *
 * The convention is the cross-SDK encoding (C++, Java, C#): ConventionVersion and Symbol in the task options,
 * and a JSON payload {"inputs":{name:value},"outputs":{name:blob id}}. The library keys (LibraryPath,
 * LibraryBlobId) are not read here; only workers that load the service dynamically need them.
 */
struct ConventionTask {
  /**
   * @brief Name of the method to call (Symbol task option)
   */
  std::string method_name;
  /**
   * @brief Named inputs. Values that match a data dependency id are replaced by the dependency's content.
   */
  std::map<std::string, std::string> inputs;
  /**
   * @brief Named outputs (name → blob id)
   */
  std::map<std::string, std::string> outputs;

  /**
   * @brief Checks if the task options mark a convention task
   * @param options Task options
   * @return true if the ConventionVersion key is present
   */
  static bool IsConventionTask(const TaskOptions &options);

  /**
   * @brief Decodes a convention task
   * @param options Task options
   * @param payload Raw task payload
   * @param data_dependencies Downloaded data dependencies (blob id → content)
   * @return Decoded task
   * @throws ArmoniKSdkException if the convention version is unsupported, Symbol is missing or the payload is invalid
   */
  static ConventionTask Decode(const TaskOptions &options, const std::string &payload,
                               const std::map<std::string, std::string> &data_dependencies);
};

} // namespace Common
} // namespace Sdk
} // namespace ArmoniK
