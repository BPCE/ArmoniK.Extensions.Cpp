#include <armonik/sdk/client/SessionService.h>
#include <armonik/sdk/common/Configuration.h>
#include <armonik/sdk/common/Version.h>

#include <iostream>

using ArmoniK::Sdk::Client::SessionService;

// Volatile so the reference survives optimization and the linker must resolve a Client symbol
static void (SessionService::*volatile close_session)() = &SessionService::CloseSession;

int main() {
  // Resolves a Common symbol, no connection is made
  ArmoniK::Sdk::Common::Configuration config;
  (void)config;
  std::cout << ArmoniK::Sdk::Common::getVersion() << std::endl;
  return close_session != nullptr ? 0 : 1;
}
