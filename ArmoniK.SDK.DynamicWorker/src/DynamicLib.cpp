#include "DynamicLib.h"
#include <armonik/sdk/common/ArmoniKSdkException.h>
#include <cstdio>
#include <dlfcn.h>
#include <stdexcept>

namespace ArmoniK {
namespace Sdk {
namespace DynamicWorker {

DynamicLib::DynamicLib(const char *filename) {
#ifdef LM_ID_NEWLM
  auto constexpr openmode = LM_ID_BASE;
  handle = dlmopen(openmode, filename, RTLD_NOW | RTLD_LOCAL);
#else
  handle = dlopen(filename, RTLD_NOW | RTLD_LOCAL);
#endif
  if (!handle) {
    throw ArmoniK::Sdk::Common::ArmoniKSdkException(std::string("Could not dlopen ") + filename + " : " + dlerror());
  }
}

/**
 * @brief Destructor
 * @note A destructor must not throw: a failed dlclose is reported on stderr instead
 */
DynamicLib::~DynamicLib() {
  if (handle && dlclose(handle) != 0) {
    const char *error = dlerror();
    std::fprintf(stderr, "DynamicLib: could not dlclose: %s\n", error ? error : "unknown error");
  }
}

/**
 * @brief Unload the library
 */
void DynamicLib::unload() {
  if (!handle) {
    return;
  }
  if (dlclose(handle) == 0) {
    handle = nullptr;
  } else {
    throw ArmoniK::Sdk::Common::ArmoniKSdkException("Could not dlclose");
  }
}

/**
 * @brief Retrieve symbol from lib
 */
void *DynamicLib::get(const char *symbol_name) const {
  if (!handle) {
    throw ArmoniK::Sdk::Common::ArmoniKSdkException("Was not dlopen'ed");
  }
  void *sym = dlsym(handle, symbol_name);

  if (!sym) {
    throw ArmoniK::Sdk::Common::ArmoniKSdkException(std::string("Could not find symbol in lib : ") + symbol_name);
  }

  return sym;
}

} // namespace DynamicWorker
} // namespace Sdk
} // namespace ArmoniK
