#include "DynamicWorker.h"
#include "ApplicationManager.h"
#include <armonik/sdk/common/ArmoniKSdkException.h>
#include <armonik/sdk/common/ConventionTask.h>
#include <armonik/sdk/common/Diagnostics.h>
#include <armonik/sdk/common/DynamicLibrary.h>
#include <armonik/sdk/common/TaskOptions.h>
#include <armonik/sdk/common/TaskPayload.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <exception>
#include <fstream>
#include <unistd.h>
#include <vector>

namespace ArmoniK {
namespace Sdk {
namespace DynamicWorker {

DynamicWorker::DynamicWorker(std::unique_ptr<armonik::api::grpc::v1::agent::Agent::Stub> agent,
                             const ArmoniK::Sdk::Common::Configuration &config,
                             const armonik::api::common::logger::Logger &logger)
    : ArmoniKWorker(std::move(agent)), logger(logger.local({{"WorkerName", "DynamicWorker"}})), manager(config, logger),
      library_base_dir_(config.get("Worker__LibraryDirectory")) {}

DynamicWorker::~DynamicWorker() {
  if (library_dir_.empty()) {
    return;
  }
  // Best effort: a library still loaded stays mapped after its file is removed
  if (DIR *dir = ::opendir(library_dir_.c_str())) {
    while (const dirent *entry = ::readdir(dir)) {
      const std::string name = entry->d_name;
      if (name != "." && name != "..") {
        ::unlink((library_dir_ + "/" + name).c_str());
      }
    }
    ::closedir(dir);
  }
  ::rmdir(library_dir_.c_str());
}

std::string DynamicWorker::InstallLibrary(const std::string &blob_id, const char *data, std::size_t size) {
  if (blob_id.empty() || blob_id == "." || blob_id == ".." || blob_id.find('/') != std::string::npos) {
    throw ArmoniK::Sdk::Common::ArmoniKSdkException("Invalid library blob id '" + blob_id + "'");
  }

  // Private directory of this worker process. mkdtemp replaces the trailing XXXXXX with random characters and creates
  // the directory with mode 0700, so no other user can guess its name, create it first, or write in it.
  if (library_dir_.empty()) {
    std::string base = library_base_dir_;
    if (base.empty()) {
      const char *tmpdir = std::getenv("TMPDIR");
      base = tmpdir && *tmpdir ? tmpdir : "/tmp";
    }
    const std::string tmpl = base + "/armonik-libs-XXXXXX";
    std::vector<char> name(tmpl.begin(), tmpl.end());
    name.push_back('\0');
    if (::mkdtemp(name.data()) == nullptr) {
      throw ArmoniK::Sdk::Common::ArmoniKSdkException("Failed to create library directory " + tmpl + ": " +
                                                      std::strerror(errno) +
                                                      ". Set Worker__LibraryDirectory to a writable directory");
    }
    library_dir_ = name.data();
  }

  // The data of a result never changes, so a library already written is reused. Rewriting it would also change the
  // file under a library that may still be loaded.
  const std::string path = library_dir_ + "/lib-" + blob_id + ".so";
  if (::access(path.c_str(), F_OK) == 0) {
    return path;
  }

  // Write to a temporary file, then rename it, so the library file is either absent or complete
  const std::string tmp_path = path + ".tmp";
  {
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    out.write(data, static_cast<std::streamsize>(size));
    out.close();
    if (!out) {
      ::unlink(tmp_path.c_str());
      throw ArmoniK::Sdk::Common::ArmoniKSdkException("Failed to write library to " + tmp_path);
    }
  }
  if (::rename(tmp_path.c_str(), path.c_str()) != 0) {
    const std::string error = std::strerror(errno);
    ::unlink(tmp_path.c_str());
    throw ArmoniK::Sdk::Common::ArmoniKSdkException("Failed to install library to " + path + ": " + error);
  }
  return path;
}

armonik::api::worker::ProcessStatus DynamicWorker::Execute(armonik::api::worker::TaskHandler &taskHandler) {
  try {
    const auto &rawOptions = taskHandler.getTaskOptions();

    // Convention path: ConventionVersion key present in task options
    const ArmoniK::Sdk::Common::TaskOptions opts(rawOptions);
    if (ArmoniK::Sdk::Common::ConventionTask::IsConventionTask(opts)) {
      const auto &deps = taskHandler.getDataDependencies();
      const auto task = ArmoniK::Sdk::Common::ConventionTask::Decode(opts, taskHandler.getPayload(), deps);
      auto lib = opts.GetDynamicLibrary();

      // Blob-based loading: fetch the .so content from data dependencies, write to a temp file,
      // then dlopen it. The library is uploaded as a blob by the client and pre-downloaded by
      // ArmoniK before the worker runs.
      if (!lib.library_blob_id.empty()) {
        const auto blob_it = deps.find(lib.library_blob_id);
        if (blob_it == deps.end()) {
          throw ArmoniK::Sdk::Common::ArmoniKSdkException("Library blob '" + lib.library_blob_id +
                                                          "' not found in data dependencies");
        }
        lib.library_path = InstallLibrary(lib.library_blob_id, blob_it->second.data(), blob_it->second.size());
      }

      return manager.UseLibrary(lib, rawOptions.application_namespace(), rawOptions.application_service())
          .UseSession(taskHandler.getSessionId())
          .Execute(taskHandler, task.method_name, task.inputs, task.outputs);
    }

    // Legacy path: use application_name / application_version based loading
    ARMONIK_SDK_SUPPRESS_DEPRECATED_BEGIN
    auto legacyPayload = ArmoniK::Sdk::Common::TaskPayload::Deserialize(taskHandler.getPayload());
    ARMONIK_SDK_SUPPRESS_DEPRECATED_END
    AppId appId{rawOptions.application_name(), rawOptions.application_version()};
    ServiceId serviceId(appId, rawOptions.application_namespace(), rawOptions.application_service());
    return manager.UseApplication(appId)
        .UseService(serviceId)
        .UseSession(taskHandler.getSessionId())
        .Execute(taskHandler, legacyPayload.method_name, legacyPayload.arguments);
  } catch (const ArmoniK::Sdk::Common::ArmoniKSdkException &e) {
    return armonik::api::worker::ProcessStatus(e.what());
  }
}

} // namespace DynamicWorker
} // namespace Sdk
} // namespace ArmoniK
