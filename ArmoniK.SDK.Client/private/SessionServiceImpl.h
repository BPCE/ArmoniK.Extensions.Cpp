#pragma once

#include "ByteBudget.h"
#include "ChannelPool.h"
#include "ThreadPool.h"
#include "armonik/sdk/client/WaitBehavior.h"
#include <armonik/client/results/ResultsClient.h>
#include <armonik/sdk/common/TaskOptions.h>
#include <istream>
#include <mutex>
#include <results_service.grpc.pb.h>

namespace ArmoniK {
namespace Sdk {
namespace Common {
struct Properties;
struct TaskPayload;
struct TaskDefinition;
} // namespace Common
} // namespace Sdk
} // namespace ArmoniK

namespace ArmoniK {
namespace Sdk {
namespace Client {
class IServiceInvocationHandler;
}
} // namespace Sdk
} // namespace ArmoniK

namespace ArmoniK {
namespace Sdk {
namespace Client {
namespace Internal {
/**
 * @brief Private implementation of the Session Service
 */
class SessionServiceImpl {
private:
  /**
   * @brief Session
   */
  std::string session;
  /**
   * @brief Session task options
   */
  ArmoniK::Sdk::Common::TaskOptions taskOptions;

  /**
   * @brief Map between taskId and resultId
   */
  std::map<std::string, std::string> taskId_resultId;

  /**
   * @brief Map between result id and task id
   */
  std::map<std::string, std::string> resultId_taskId;

  /**
   * @brief Map between the task id to its result ids
   */
  std::map<std::string, std::vector<std::string>> tid_rids;

  /**
   * @brief Map between a result_id and it's handler
   */
  std::map<std::string, std::shared_ptr<IServiceInvocationHandler>> result_handlers;

  /**
   * @brief Maps mutex
   */
  std::mutex maps_mutex;

  /**
   * @brief Channel pool
   */
  ChannelPool channel_pool;

  /**
   * @brief Thread pool
   */
  ThreadPool thread_pool_;

  /**
   * @brief Local logger
   *
   */
  armonik::api::common::logger::LocalLogger logger_;

  /**
   * @brief Batch size for waiting results
   */
  int wait_batch_size_;

  /**
   * @brief Max number of attempts for a full result-data download RPC after a mid-stream
   *        failure (see Configuration::getDownloadMaxRetry)
   */
  int download_max_retry_;

  /**
   * @brief Batch size for task submission
   */
  int submit_batch_size_;

  /**
   * @brief Override message size for result upload and creation
   */
  int override_message_size_;

  /**
   * @brief Maximum size of a data chunk, from override_message_size_ or the server configuration
   */
  std::size_t data_chunk_max_size_ = 0;

  /**
   * @brief Guards the single initialization of data_chunk_max_size_
   */
  std::once_flag data_chunk_max_size_once_;

  /**
   * @brief Maximum size of a data chunk, fetched from the server on the first call only
   */
  std::size_t DataChunkMaxSize();

  /**
   * @brief Bounds the total result-payload bytes reserved at once during WaitResults()
   */
  ByteBudget download_byte_budget_;

  /**
   * @brief Bounds the total task-payload/raw-input bytes reserved at once during
   * Submit()/SubmitRaw()
   */
  ByteBudget upload_byte_budget_;

public:
  SessionServiceImpl() = delete;
  SessionServiceImpl(const SessionServiceImpl &) = delete;

  /**
   * @brief Creates a SessionService from the given Properties
   * @param properties Session properties
   */
  explicit SessionServiceImpl(const ArmoniK::Sdk::Common::Properties &properties,
                              armonik::api::common::logger::Logger &logger, const std::string &session_id = "");

  /**
   * @brief Submits the given list of task requests using the session's task options
   * @param task_requests List of task requests
   * @param handler Result handler for this batch of requests
   * @return List of task ids
   */
  std::vector<std::string> Submit(const std::vector<Common::TaskPayload> &task_requests,
                                  std::shared_ptr<IServiceInvocationHandler> handler);

  /**
   * @brief Submits the given list of task requests
   * @param task_requests List of task requests
   * @param handler Result handler for this batch of requests
   * @param task_options Task options to use for this batch of requests
   * @return List of task ids
   */
  std::vector<std::string> Submit(const std::vector<Common::TaskPayload> &task_requests,
                                  std::shared_ptr<IServiceInvocationHandler> handler,
                                  const Common::TaskOptions &task_options);

  /**
   * @brief Submits the given list of task definitions using the session's task options.
   * Raw input data is uploaded automatically before task submission.
   * @param task_requests List of task definitions
   * @param handler Result handler for this batch of requests
   * @return List of task ids
   */
  std::vector<std::string> Submit(const std::vector<Common::TaskDefinition> &task_requests,
                                  std::shared_ptr<IServiceInvocationHandler> handler);

  /**
   * @brief Submits the given list of task definitions.
   * Raw input data is uploaded automatically before task submission.
   * @param task_requests List of task definitions
   * @param handler Result handler for this batch of requests
   * @param task_options Task options to use for this batch of requests
   * @return List of task ids
   */
  std::vector<std::string> Submit(const std::vector<Common::TaskDefinition> &task_requests,
                                  std::shared_ptr<IServiceInvocationHandler> handler,
                                  const Common::TaskOptions &task_options);

  /**
   * @brief Uploads library content to ArmoniK blob storage, reading it chunk by chunk.
   * @param content Seekable stream of the .so file, read from its current position
   * @return Blob ID (result ID) of the uploaded library
   */
  std::string UploadLibrary(std::istream &content);

  /**
   * @brief Get the session Id associated with this service
   * @return Session Id
   */
  [[nodiscard]] const std::string &getSession() const;

  /**
   * @brief Waits for the completion of the given tasks
   * @param task_ids Task ids to wait on. If left empty, will wait for submitted tasks in
   * @param waitBehavior Wait for all tasks completion, any task completion and/or stop waiting if a result is aborted
   * @param options Wait options
   * @note If tasks are being submitted concurrently with the wait, this function may return before the concurrent tasks
   * submission is complete
   */
  void WaitResults(std::set<std::string> task_ids = {}, WaitBehavior waitBehavior = All,
                   const WaitOptions &options = WaitOptions());

  /**
   * @brief Discards all results, cancels the tasks and cancels the session. No handler will be called.
   * @warning The session and its data will not be recoverable.
   * @warning Using this on a session with running or pending tasks will result in undefined behavior
   */
  void DropSession();

  /**
   * Closes the session associated to this service
   */
  void CloseSession();

  /**
   * Cancels the session associated to this service
   */
  void CancelSession();

  /**
   * Purges the session associated to this service
   */
  void PurgeSession();

  /**
   * @brief Discards the results data of the given tasks. The associated results must be completed or aborted.
   * @param task_ids Task ids
   * @warning The data of these results will not be recoverable. Tasks which depend on these data will fail.
   * @warning If the given task has not been processed, the behavior is undefined. The tasks will not be processed by
   * the client.
   */
  void CleanupTasks(std::vector<std::string> task_ids);

private:
  /**
   * @brief Core submission logic operating on pre-serialized payloads.
   * @param serialized_payloads Pre-serialized task payload bytes (one per task)
   * @param data_dependencies Per-task list of data dependency result IDs
   * @param handler Result handler for this batch
   * @param task_options Task options
   * @return List of task ids
   */
  std::vector<std::string> SubmitRaw(const std::vector<std::string> &serialized_payloads,
                                     const std::vector<std::vector<std::string>> &data_dependencies,
                                     std::shared_ptr<IServiceInvocationHandler> handler,
                                     const Common::TaskOptions &task_options);
};
} // namespace Internal
} // namespace Client
} // namespace Sdk
} // namespace ArmoniK
