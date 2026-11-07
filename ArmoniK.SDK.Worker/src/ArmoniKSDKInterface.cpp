#include "armonik/sdk/worker/ArmoniKSDKInterface.h"
#include "armonik/sdk/common/ArmoniKSdkException.h"
#include "armonik/sdk/worker/ServiceBase.h"
#include <cstring>

extern "C" {

void armonik_destroy_service_default(void *p) {
  if (p) {
    delete static_cast<ArmoniK::Sdk::Worker::ServiceBase *>(p);
  }
}

#ifdef __linux__
__attribute__((weak, alias("armonik_destroy_service_default")))
#endif
void armonik_destroy_service(void *p);

void *armonik_enter_session_default(void *service_context, const char *session_id) {
  return static_cast<ArmoniK::Sdk::Worker::ServiceBase *>(service_context)->enter_session(session_id);
}

#ifdef __linux__
__attribute__((weak, alias("armonik_enter_session_default")))
#endif
void *armonik_enter_session(void *service_context, const char *session_id);

void armonik_leave_session_default(void *service_context, void *session_context) {
  static_cast<ArmoniK::Sdk::Worker::ServiceBase *>(service_context)->leave_session(session_context);
}

#ifdef __linux__
__attribute__((weak, alias("armonik_leave_session_default")))
#endif
void armonik_leave_session(void *service_context, void *session_context);

/**
 * \inherit armonik_call
 */
armonik_status_t armonik_call_default(void *armonik_context, void *service_context, void *session_context,
                                      const char *function_name, const char *input, size_t input_size,
                                      armonik_callback_t callback) {
  try {
    auto output = static_cast<ArmoniK::Sdk::Worker::ServiceBase *>(service_context)
                      ->call(session_context, std::string(function_name), std::string(input, input_size));
    callback(armonik_context, ARMONIK_STATUS_OK, output.data(), output.size());
    return ARMONIK_STATUS_OK;
  } catch (const ArmoniK::Sdk::Common::ArmoniKSdkException &e) {
    auto msg = e.what();
    callback(armonik_context, ARMONIK_STATUS_ERROR, msg, std::strlen(msg));
    return ARMONIK_STATUS_ERROR;
  } catch (const std::exception &e) {
    auto msg = e.what();
    callback(armonik_context, ARMONIK_STATUS_RETRY, msg, std::strlen(msg));
    return ARMONIK_STATUS_RETRY;
  }
}

#ifdef __linux__
__attribute__((weak, alias("armonik_call_default")))
#endif
armonik_status_t armonik_call(void *armonik_context, void *service_context, void *session_context,
                              const char *function_name, const char *input, size_t input_size,
                              armonik_callback_t callback);
}
