#pragma once

#include <utility>

namespace ArmoniK {
namespace Sdk {
namespace DynamicWorker {
/**
 * @brief Manage external dynamic library
 */
class DynamicLib {
private:
  /**
   * @brief Native handle to the dlopen'ed library
   */
  void *handle = nullptr;

public:
  /**
   * @brief Default constructor
   *
   * DynamicLib points to no library
   */
  DynamicLib() noexcept = default;

  /**
   * @brief Load a library constructor
   *
   * @param filename path to the library to load
   */
  explicit DynamicLib(const char *filename);

  /**
   * @brief Deleted copy constructor
   */
  DynamicLib(const DynamicLib &) = delete;

  /**
   * @brief Deleted copy assignment operator
   */
  DynamicLib &operator=(const DynamicLib &) = delete;

  /**
   * @brief Move constructor
   * @param other Other DynamicLib
   */
  DynamicLib(DynamicLib &&other) noexcept : handle(other.handle) { other.handle = nullptr; }

  /**
   * @brief Move assignment operator
   * @param other Other DyncmicLib
   * @return this
   */
  DynamicLib &operator=(DynamicLib &&other) noexcept {
    std::swap(handle, other.handle);
    return *this;
  }

  /**
   * @brief Destructor, unloads the library without throwing
   */
  ~DynamicLib();

  /**
   * @brief Unload the library
   * @throws ArmoniK::Sdk::Common::ArmoniKSdkException if dlclose fails
   */
  void unload();

  /**
   * @brief Retrieve symbol from lib
   * @param symbol_name Name of the symbol
   * @return Function pointer for the requested symbol
   */
  void *get(const char *symbol_name) const;

  /**
   * @brief Retrieve symbol from lib
   * @tparam T Function pointer type
   * @param symbol_name Name of the symbol
   * @return Function pointer for the requested symbol
   */
  template <class T> T get(const char *symbol_name) const { return (T)get(symbol_name); }

  /**
   * @brief Test whether a library is loaded or not
   * @return true if a library is loaded
   */
  explicit operator bool() const noexcept { return handle; }
};
} // namespace DynamicWorker
} // namespace Sdk
} // namespace ArmoniK
