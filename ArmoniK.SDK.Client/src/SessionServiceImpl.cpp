#include "SessionServiceImpl.h"
#include "Batcher.h"
#include "ParallelFor.h"
#include "armonik/sdk/client/IServiceInvocationHandler.h"
#include <armonik/client/results/ResultsClient.h>
#include <armonik/client/results_common.pb.h>
#include <armonik/client/results_service.grpc.pb.h>
#include <armonik/client/sessions/SessionsClient.h>
#include <armonik/client/sessions_service.grpc.pb.h>
#include <armonik/client/tasks/TasksClient.h>
#include <armonik/client/tasks_service.grpc.pb.h>
#include <armonik/common/exceptions/ArmoniKApiException.h>
#include <armonik/common/exceptions/ArmoniKTaskError.h>
#include <armonik/common/objects.pb.h>
#include <armonik/common/utils/GuuId.h>
#include <armonik/common/utils/string_view.h>
#include <armonik/sdk/common/Diagnostics.h>
#include <armonik/sdk/common/DynamicLibrary.h>
#include <armonik/sdk/common/Properties.h>
#include <armonik/sdk/common/TaskDefinition.h>
#include <armonik/sdk/common/TaskPayload.h>
#include <armonik/sdk/common/Version.h>
#include <armonik/sdk/common/internal/ConventionPayload.h>
#include <chrono>
#include <istream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ArmoniK {
namespace Sdk {
namespace Client {
namespace Internal {

namespace {
/**
 * @brief Source of the data of a result upload, read chunk by chunk
 */
class ChunkReader {
public:
  virtual ~ChunkReader() = default;

  /**
   * @brief Go back to the start of the data, to retry an upload
   */
  virtual void Rewind() = 0;

  /**
   * @brief Read the next chunk of at most max_size bytes into chunk
   * @return false if there is no data left
   */
  virtual bool Read(std::string &chunk, std::size_t max_size) = 0;
};

/**
 * @brief Reads the chunks from data held in memory
 */
class StringViewReader : public ChunkReader {
public:
  explicit StringViewReader(armonik::api::string_view data) : data_(data), remaining_(data) {}
  void Rewind() override { remaining_ = data_; }
  bool Read(std::string &chunk, std::size_t max_size) override {
    if (remaining_.empty()) {
      return false;
    }
    auto next = remaining_.substr(0, max_size);
    chunk.assign(next.data(), next.size());
    remaining_ = remaining_.substr(next.size());
    return true;
  }

private:
  armonik::api::string_view data_;
  armonik::api::string_view remaining_;
};

/**
 * @brief Reads the chunks from a seekable stream, from its position at construction
 */
class StreamReader : public ChunkReader {
public:
  explicit StreamReader(std::istream &stream) : stream_(stream), start_(stream.tellg()) {
    if (start_ == std::istream::pos_type(-1)) {
      throw std::invalid_argument("The upload stream is not seekable");
    }
  }
  void Rewind() override {
    stream_.clear();
    if (!stream_.seekg(start_)) {
      throw std::runtime_error("Unable to rewind the upload stream");
    }
  }
  bool Read(std::string &chunk, std::size_t max_size) override {
    chunk.resize(max_size);
    stream_.read(&chunk[0], static_cast<std::streamsize>(max_size));
    chunk.resize(static_cast<std::size_t>(stream_.gcount()));
    if (stream_.bad()) {
      throw std::runtime_error("Unable to read the upload stream");
    }
    return !chunk.empty();
  }

private:
  std::istream &stream_;
  std::istream::pos_type start_;
};

/**
 * @brief Upload a large result using a stream, retrying the upload in case of error
 * @param pool The channel pool to use to perform the requests
 * @param session Id of the session where the result has been created
 * @param result_id Id of the result to upload
 * @param reader Source of the content of the result, rewound before each attempt
 * @param data_max_chunk_size Size of the chunks to upload the data
 * @param logger Logger
 * @return Number of bytes uploaded
 */
std::size_t upload_large_result(ArmoniK::Sdk::Client::Internal::ChannelPool &pool, const std::string &session,
                                const std::string &result_id, ChunkReader &reader, std::size_t data_chunk_max_size,
                                armonik::api::common::logger::ILogger &logger) {

  std::exception_ptr eptr;

  // Retry the upload at most 3 times
  const int max_retry = 3;
  for (int retry = 0; retry < max_retry; ++retry) {
    eptr = nullptr;
    std::size_t sent = 0;

    // If retry is available (not the last iteration), add prefix to the logs to indicate the upload will be retried
    // even though there was an error, and log errors as warnings
    bool can_retry = retry + 1 < max_retry;
    const char *retry_notif = can_retry ? "Upload will be RETRIED: " : "";
    auto loglevel =
        can_retry ? armonik::api::common::logger::Level::Warning : armonik::api::common::logger::Level::Error;

    try {
      reader.Rewind();
      auto channel = pool.GetChannel();

      grpc::ClientContext context{};
      armonik::api::grpc::v1::results::UploadResultDataRequest request{};
      armonik::api::grpc::v1::results::UploadResultDataResponse response{};

      auto client = armonik::api::grpc::v1::results::Results::NewStub(channel.channel);
      auto stream = client->UploadResultData(&context, &response);

      // Send message header with the result identifier
      request.mutable_id()->set_session_id(session);
      request.mutable_id()->set_result_id(result_id);
      stream->Write(request);
      request.clear_id();

      // Chunk the content to send it in multiple messages
      while (reader.Read(*request.mutable_data_chunk(), data_chunk_max_size)) {
        sent += request.data_chunk().size();
        if (!stream->Write(request)) {
          throw armonik::api::common::exceptions::ArmoniKApiException("Unable to continue upload result " + result_id);
        }
      }

      if (!stream->WritesDone()) {
        throw armonik::api::common::exceptions::ArmoniKApiException("Unable to upload result " + result_id);
      }
      auto status = stream->Finish();
      if (!status.ok()) {
        throw armonik::api::common::exceptions::ArmoniKApiException("Unable to finish upload result " + result_id +
                                                                    ": " + status.error_message());
      }

      // If the uploaded size is different than the actual data size, upload must be retried
      // Otherwise, we are good to go.
      if (response.result().size() == std::int64_t(sent)) {
        return sent;
      }

      // If the uploaded size is different, delete the uploaded data from the object storage
      try {
        pool.WithChannel([&](std::shared_ptr<grpc::Channel> channel) {
          armonik::api::client::ResultsClient(armonik::api::grpc::v1::results::Results::NewStub(channel))
              .delete_results_data(session, {std::move(*response.mutable_result()->mutable_result_id())});
        });
      } catch (const std::exception &e) {
        logger.warning("Unable to clean data for " + result_id + ": " + e.what());
      } catch (...) {
        logger.warning("Unable to clean data for " + result_id + ": unknown exception");
      }

      std::stringstream ss;
      ss << retry_notif << "Corrupted result " << result_id << " upload: mismatched between client size (" << sent
         << " B) and server size (" << response.result().size() << ")";

      throw std::make_exception_ptr(armonik::api::common::exceptions::ArmoniKApiException(ss.str()));
    } catch (const std::exception &e) {
      logger.log(loglevel, std::string(retry_notif) + "Failed to upload result " + result_id + ": " + e.what());
      eptr = std::current_exception();
    } catch (...) {
      logger.log(loglevel, std::string(retry_notif) + "Failed to upload result " + result_id + ": unknown exception");
      eptr = std::current_exception();
    }
  }

  std::rethrow_exception(eptr);
}

/**
 * @brief Upload a large result held in memory, retrying the upload in case of error
 */
void upload_large_result(ArmoniK::Sdk::Client::Internal::ChannelPool &pool, const std::string &session,
                         const std::string &result_id, armonik::api::string_view data, std::size_t data_chunk_max_size,
                         armonik::api::common::logger::ILogger &logger) {
  StringViewReader reader(data);
  upload_large_result(pool, session, result_id, reader, data_chunk_max_size, logger);
}

// Submit()/SubmitRaw() reserve upload_byte_budget_ bytes for the whole call up front, on the calling
// thread, before spawning anything. A result handler chaining a new submission from inside
// WaitResults() runs on thread_pool_, so that reservation blocks a pool worker whose freedom is
// needed to finish (and release the budget for) whichever call currently holds it. With enough
// concurrent chained calls, every worker ends up blocked this way, deadlocking the pool.
void EnsureNotCalledFromWorkerThread(const char *entry_point) {
  if (ThreadPool::IsWorkerThread()) {
    throw armonik::api::common::exceptions::ArmoniKApiException(std::string(entry_point) +
                                                                " was called from a result handler (risk of deadlock)");
  }
}
} // namespace

const std::string &SessionServiceImpl::getSession() const { return session; }

std::vector<std::string> SessionServiceImpl::SubmitRaw(const std::vector<std::string> &serialized_payloads,
                                                       const std::vector<std::vector<std::string>> &data_dependencies,
                                                       std::shared_ptr<IServiceInvocationHandler> handler,
                                                       const Common::TaskOptions &task_options) {
  EnsureNotCalledFromWorkerThread("SubmitRaw");

  const std::size_t message_overhead = 128;
  std::size_t data_chunk_max_size = DataChunkMaxSize();

  // Number of bytes to be sent in the next CreateResult request
  std::size_t data_batched = 0;

  std::vector<std::string> input_result_ids(serialized_payloads.size());
  std::vector<std::string> output_result_ids(serialized_payloads.size());
  std::vector<std::string> task_ids(serialized_payloads.size());

  // Reserve this call's total upload bytes up front, before any thread-pool work is spawned, to
  // bound peak allocated memory per SubmitRaw call. Released once the call returns (or throws).
  std::int64_t total_upload_bytes = 0;
  for (const auto &payload : serialized_payloads) {
    total_upload_bytes += static_cast<std::int64_t>(payload.size());
  }
  if (upload_byte_budget_.Acquire(total_upload_bytes)) {
    logger_.warning("Uploading " + std::to_string(total_upload_bytes) +
                    " bytes alone exceeds GrpcClient__UploadByteBudget (" +
                    std::to_string(upload_byte_budget_.capacity()) +
                    "). Consider raising it to bound peak upload memory more evenly.");
  }
  struct BudgetGuard {
    ByteBudget &budget;
    std::int64_t bytes;
    ~BudgetGuard() { budget.Release(bytes); }
  } budget_guard{upload_byte_budget_, total_upload_bytes};

  ThreadPool::JoinSet join_set(thread_pool_);

  // Batch Result metadata creation (for outputs and large inputs) and upload inputs
  Batcher<std::pair<std::size_t, bool>> create_metadata_and_upload_batcher(
      submit_batch_size_, [&](std::vector<std::pair<std::size_t, bool>> &&batch) {
        auto batch_ptr = std::make_shared<std::vector<std::pair<std::size_t, bool>>>(std::move(batch));
        join_set.Spawn([&, batch_ptr]() {
          auto &batch = *batch_ptr;
          std::vector<std::string> names(batch.size());
          for (std::size_t j = 0; j < batch.size(); ++j) {
            std::size_t i = batch[j].first;
            bool is_output = batch[j].second;

            names[j] = (is_output ? "output-" : "input-") + std::to_string(i);
          }

          auto reply = channel_pool.WithChannel([&](std::shared_ptr<grpc::Channel> channel) {
            return armonik::api::client::ResultsClient(armonik::api::grpc::v1::results::Results::NewStub(channel))
                .create_results_metadata(session, names);
          });

          // threadsafe as the index is unique among all batches
          for (std::size_t j = 0; j < batch.size(); ++j) {
            std::size_t i = batch[j].first;
            bool is_output = batch[j].second;

            if (is_output) {
              output_result_ids[i] = std::move(reply[names[j]]);
            } else {
              input_result_ids[i] = std::move(reply[names[j]]);

              // Upload result using stream. The upload bytes for this whole SubmitRaw call were
              // already reserved from upload_byte_budget_ up front (see budget_guard above).
              join_set.Spawn([&, i]() {
                upload_large_result(channel_pool, session, input_result_ids[i], serialized_payloads[i],
                                    data_chunk_max_size, logger_);
              });
            }
          }
        });
      });

  // Batch Result data creation (for small inputs)
  Batcher<std::size_t> create_data_batcher(submit_batch_size_, [&](std::vector<std::size_t> &&batch) {
    // Reset the number of bytes to be sent in the current batch
    data_batched = 0;

    auto batch_ptr = std::make_shared<std::vector<std::size_t>>(std::move(batch));
    join_set.Spawn([&, batch_ptr]() {
      auto &batch = *batch_ptr;

      std::vector<std::pair<std::string, std::string>> results(batch.size());
      for (std::size_t j = 0; j < batch.size(); ++j) {
        std::size_t i = batch[j];
        results[j] = {"input-" + std::to_string(i), serialized_payloads[i]};
      }
      auto reply = channel_pool.WithChannel([&](std::shared_ptr<grpc::Channel> channel) {
        return armonik::api::client::ResultsClient(armonik::api::grpc::v1::results::Results::NewStub(channel))
            .create_results(session, results);
      });

      // threadsafe as the index is unique among all batches
      for (std::size_t j = 0; j < batch.size(); ++j) {
        std::size_t i = batch[j];
        input_result_ids[i] = std::move(reply[results[j].first]);
      }
    });
  });

  // Batch task submission
  Batcher<std::size_t> submit_batcher(submit_batch_size_, [&](std::vector<std::size_t> &&batch) {
    auto batch_ptr = std::make_shared<std::vector<std::size_t>>(std::move(batch));
    join_set.Spawn([&, batch_ptr]() {
      auto &batch = *batch_ptr;
      std::vector<armonik::api::common::TaskCreation> requests(batch.size());
      for (std::size_t j = 0; j < batch.size(); ++j) {
        std::size_t i = batch[j];
        const auto &deps = data_dependencies[i];
        auto &request = requests[j];

        request = armonik::api::common::TaskCreation();
        request.payload_id = input_result_ids[i];
        request.expected_output_keys.push_back(output_result_ids[i]);
        request.data_dependencies.insert(request.data_dependencies.end(), deps.begin(), deps.end());
      }

      auto reply = channel_pool.WithChannel([&](std::shared_ptr<grpc::Channel> channel) {
        return armonik::api::client::TasksClient(armonik::api::grpc::v1::tasks::Tasks::NewStub(channel))
            .submit_tasks(session, std::move(requests), static_cast<armonik::api::grpc::v1::TaskOptions>(task_options));
      });

      // threadsafe as the index is unique among all batches
      for (std::size_t j = 0; j < batch.size(); ++j) {
        std::size_t i = batch[j];
        task_ids[i] = std::move(reply[j].task_id);

        std::stringstream ss;
        ss << "Submitted task " << task_ids[i] << " with result " << output_result_ids[i];
        logger_.debug(ss.str());
      }
    });
  });

  // Create all results
  for (std::size_t i = 0; i < serialized_payloads.size(); ++i) {
    auto payload_size = serialized_payloads[i].size();

    create_metadata_and_upload_batcher.Add({i, true});
    if (payload_size + message_overhead >= data_chunk_max_size) {
      create_metadata_and_upload_batcher.Add({i, false});
    } else {
      // If current batch would be too large, send it right now
      if (data_batched + payload_size + message_overhead >= data_chunk_max_size) {
        create_data_batcher.ProcessBatch();
      }

      data_batched += payload_size + message_overhead;
      create_data_batcher.Add(i);
    }
  }

  // Ensure all results are created
  create_metadata_and_upload_batcher.ProcessBatch();
  create_data_batcher.ProcessBatch();
  join_set.Wait();

  // Submit all tasks
  for (std::size_t i = 0; i < serialized_payloads.size(); ++i) {
    submit_batcher.Add(i);
  }

  // Ensure all tasks are actually submitted
  submit_batcher.ProcessBatch();
  join_set.Wait();

  std::lock_guard<std::mutex> lock(maps_mutex);

  for (std::size_t i = 0; i < serialized_payloads.size(); ++i) {
    const auto &result_id = output_result_ids[i];
    const auto &task_id = task_ids[i];
    result_handlers[result_id] = handler;
    resultId_taskId[result_id] = task_id;
    taskId_resultId[task_id] = result_id;
  }

  return task_ids;
}

ARMONIK_SDK_SUPPRESS_DEPRECATED_BEGIN
std::vector<std::string> SessionServiceImpl::Submit(const std::vector<Common::TaskPayload> &task_requests,
                                                    std::shared_ptr<IServiceInvocationHandler> handler,
                                                    const Common::TaskOptions &task_options) {
  std::vector<std::string> serialized(task_requests.size());
  std::vector<std::vector<std::string>> deps(task_requests.size());
  ParallelFor(thread_pool_, task_requests.size(), [&](std::size_t i) {
    serialized[i] = task_requests[i].Serialize();
    deps[i] = task_requests[i].data_dependencies;
  });
  return SubmitRaw(serialized, deps, std::move(handler), task_options);
}
ARMONIK_SDK_SUPPRESS_DEPRECATED_END

std::vector<std::string> SessionServiceImpl::Submit(const std::vector<Common::TaskDefinition> &task_requests,
                                                    std::shared_ptr<IServiceInvocationHandler> handler) {
  return Submit(task_requests, std::move(handler), taskOptions);
}

std::vector<std::string> SessionServiceImpl::Submit(const std::vector<Common::TaskDefinition> &task_requests,
                                                    std::shared_ptr<IServiceInvocationHandler> handler,
                                                    const Common::TaskOptions &task_options) {
  EnsureNotCalledFromWorkerThread("Submit");

  const std::size_t message_overhead = 128;
  const std::size_t data_chunk_max_size = DataChunkMaxSize();

  // Flatten all raw-data inputs across all tasks so they can be batch-created
  struct InputRef {
    std::size_t task_idx;
    std::string name;
    std::string result_key;
  };
  std::vector<InputRef> raw_inputs;
  for (std::size_t i = 0; i < task_requests.size(); ++i) {
    for (const auto &kv : task_requests[i].inputs) {
      const auto &name = kv.first;
      const auto &blob = kv.second;
      if (blob.IsRawData()) {
        raw_inputs.push_back({i, name, "input-" + std::to_string(raw_inputs.size())});
      }
    }
  }

  // Parallel to raw_inputs: result IDs assigned after creation
  std::vector<std::string> raw_result_ids(raw_inputs.size());

  if (!raw_inputs.empty()) {
    // Reserve all raw-input upload bytes up front, before any thread-pool work is spawned, to
    // bound peak allocated memory per Submit call. Released once this block exits (or throws).
    std::int64_t total_upload_bytes = 0;
    for (const auto &ri : raw_inputs) {
      total_upload_bytes += static_cast<std::int64_t>(task_requests[ri.task_idx].inputs.at(ri.name).GetData().size());
    }
    if (upload_byte_budget_.Acquire(total_upload_bytes)) {
      logger_.warning("Uploading " + std::to_string(total_upload_bytes) +
                      " bytes alone exceeds GrpcClient__UploadByteBudget (" +
                      std::to_string(upload_byte_budget_.capacity()) +
                      "). Consider raising it to bound peak upload memory more evenly.");
    }
    struct BudgetGuard {
      ByteBudget &budget;
      std::int64_t bytes;
      ~BudgetGuard() { budget.Release(bytes); }
    } budget_guard{upload_byte_budget_, total_upload_bytes};

    ThreadPool::JoinSet join_set(thread_pool_);

    // Large inputs: create metadata then stream-upload
    Batcher<std::size_t> large_batcher(submit_batch_size_, [&](std::vector<std::size_t> &&batch) {
      auto batch_ptr = std::make_shared<std::vector<std::size_t>>(std::move(batch));
      join_set.Spawn([&, batch_ptr]() {
        auto &batch = *batch_ptr;
        std::vector<std::string> keys;
        keys.reserve(batch.size());
        for (std::size_t j : batch) {
          keys.push_back(raw_inputs[j].result_key);
        }

        auto reply = channel_pool.WithChannel([&](std::shared_ptr<grpc::Channel> channel) {
          return armonik::api::client::ResultsClient(armonik::api::grpc::v1::results::Results::NewStub(channel))
              .create_results_metadata(session, keys);
        });

        for (std::size_t k = 0; k < batch.size(); ++k) {
          std::size_t j = batch[k];
          raw_result_ids[j] = reply.at(keys[k]); // threadsafe: each j is unique across batches

          // Upload this raw input. The upload bytes for this whole call were already reserved
          // from upload_byte_budget_ up front (see budget_guard above).
          join_set.Spawn([&, j]() {
            const auto &ri = raw_inputs[j];
            const auto &data = task_requests[ri.task_idx].inputs.at(ri.name).GetData();
            upload_large_result(channel_pool, session, raw_result_ids[j], data, data_chunk_max_size, logger_);
          });
        }
      });
    });

    // Small inputs: inline create (metadata + data in one RPC)
    std::size_t data_batched = 0;
    Batcher<std::size_t> small_batcher(submit_batch_size_, [&](std::vector<std::size_t> &&batch) {
      data_batched = 0;
      auto batch_ptr = std::make_shared<std::vector<std::size_t>>(std::move(batch));
      join_set.Spawn([&, batch_ptr]() {
        auto &batch = *batch_ptr;

        std::vector<std::pair<std::string, std::string>> pairs;
        pairs.reserve(batch.size());
        for (std::size_t j : batch) {
          const auto &ri = raw_inputs[j];
          pairs.push_back({ri.result_key, task_requests[ri.task_idx].inputs.at(ri.name).GetData()});
        }

        auto reply = channel_pool.WithChannel([&](std::shared_ptr<grpc::Channel> channel) {
          return armonik::api::client::ResultsClient(armonik::api::grpc::v1::results::Results::NewStub(channel))
              .create_results(session, pairs);
        });

        for (std::size_t k = 0; k < batch.size(); ++k) {
          raw_result_ids[batch[k]] = reply.at(pairs[k].first); // threadsafe: each j is unique
        }
      });
    });

    for (std::size_t j = 0; j < raw_inputs.size(); ++j) {
      const auto &data = task_requests[raw_inputs[j].task_idx].inputs.at(raw_inputs[j].name).GetData();

      if (data.size() + message_overhead >= data_chunk_max_size) {
        large_batcher.Add(j);
      } else {
        if (data_batched + data.size() + message_overhead >= data_chunk_max_size) {
          small_batcher.ProcessBatch();
        }
        data_batched += data.size() + message_overhead;
        small_batcher.Add(j);
      }
    }

    large_batcher.ProcessBatch();
    small_batcher.ProcessBatch();
    join_set.Wait();
  }

  // Build ConventionPayloads: existing-blob inputs resolved directly, raw inputs resolved from upload
  std::vector<Common::ConventionPayload> payloads(task_requests.size());
  for (std::size_t i = 0; i < task_requests.size(); ++i) {
    for (const auto &kv : task_requests[i].inputs) {
      const auto &name = kv.first;
      const auto &blob = kv.second;
      if (!blob.IsRawData()) {
        payloads[i].inputs[name] = blob.GetBlobId();
      }
    }
  }
  for (std::size_t j = 0; j < raw_inputs.size(); ++j) {
    payloads[raw_inputs[j].task_idx].inputs[raw_inputs[j].name] = raw_result_ids[j];
  }

  // Serialize payloads
  std::vector<std::string> serialized(payloads.size());
  ParallelFor(thread_pool_, payloads.size(), [&](std::size_t i) { serialized[i] = payloads[i].Serialize(); });

  // Build per-task deps: library blob + all input blob IDs so the DynamicWorker can resolve them
  std::vector<std::string> library_deps;
  auto blob_it = task_options.options.find(Common::DynamicLibrary::KeyLibraryBlobId);
  if (blob_it != task_options.options.end() && !blob_it->second.empty()) {
    library_deps.push_back(blob_it->second);
  }

  std::vector<std::vector<std::string>> deps(task_requests.size(), library_deps);
  for (std::size_t j = 0; j < raw_inputs.size(); ++j) {
    deps[raw_inputs[j].task_idx].push_back(std::move(raw_result_ids[j]));
  }
  for (std::size_t i = 0; i < task_requests.size(); ++i) {
    for (const auto &kv : task_requests[i].inputs) {
      const auto &blob = kv.second;
      if (!blob.IsRawData()) {
        deps[i].push_back(blob.GetBlobId());
      }
    }
  }

  return SubmitRaw(serialized, deps, std::move(handler), task_options);
}

std::size_t SessionServiceImpl::DataChunkMaxSize() {
  // Fetch the server configuration once. If the call throws, the next call tries again.
  std::call_once(data_chunk_max_size_once_, [this] {
    data_chunk_max_size_ =
        override_message_size_
            ? static_cast<std::size_t>(override_message_size_)
            : channel_pool.WithChannel([](std::shared_ptr<grpc::Channel> channel) {
                return static_cast<std::size_t>(
                    armonik::api::client::ResultsClient(armonik::api::grpc::v1::results::Results::NewStub(channel))
                        .get_service_configuration()
                        .data_chunk_max_size);
              });
  });
  return data_chunk_max_size_;
}

std::string SessionServiceImpl::UploadLibrary(std::istream &content) {
  const std::size_t data_chunk_max_size = DataChunkMaxSize();

  // Create a single result entry to hold the library blob
  auto reply = channel_pool.WithChannel([&](std::shared_ptr<grpc::Channel> channel) {
    return armonik::api::client::ResultsClient(armonik::api::grpc::v1::results::Results::NewStub(channel))
        .create_results_metadata(session, {"library"});
  });
  const std::string result_id = reply.at("library");

  StreamReader reader(content);
  const auto size = upload_large_result(channel_pool, session, result_id, reader, data_chunk_max_size, logger_);

  logger_.info("Uploaded library blob: " + result_id + " (" + std::to_string(size) + " bytes)");
  return result_id;
}

SessionServiceImpl::SessionServiceImpl(const Common::Properties &properties,
                                       armonik::api::common::logger::Logger &logger, const std::string &session_id)
    : taskOptions(properties.taskOptions), channel_pool(properties, logger),
      thread_pool_(properties.configuration.get_control_plane().getThreadPoolSize(), logger),
      logger_(logger.local({{"sdk_version", ArmoniK::Sdk::Common::getVersion()}})),
      wait_batch_size_(properties.configuration.get_control_plane().getWaitBatchSize()),
      download_max_retry_(properties.configuration.get_control_plane().getDownloadMaxRetry()),
      submit_batch_size_(properties.configuration.get_control_plane().getSubmitBatchSize()),
      override_message_size_(properties.configuration.get_control_plane().getOverrideMessageSize()),
      download_byte_budget_(properties.configuration.get_control_plane().getDownloadByteBudget()),
      upload_byte_budget_(properties.configuration.get_control_plane().getUploadByteBudget()) {
  // Creates a new session
  session = session_id.empty() ? channel_pool.WithChannel([&](std::shared_ptr<grpc::Channel> channel) {
    return armonik::api::client::SessionsClient(armonik::api::grpc::v1::sessions::Sessions::NewStub(channel))
        .create_session(static_cast<armonik::api::grpc::v1::TaskOptions>(properties.taskOptions),
                        {properties.taskOptions.partition_id});
  })
                               : session_id;
}

ARMONIK_SDK_SUPPRESS_DEPRECATED_BEGIN
std::vector<std::string> SessionServiceImpl::Submit(const std::vector<Common::TaskPayload> &task_requests,
                                                    std::shared_ptr<IServiceInvocationHandler> handler) {
  return Submit(task_requests, std::move(handler), taskOptions);
}
ARMONIK_SDK_SUPPRESS_DEPRECATED_END

void SessionServiceImpl::WaitResults(std::set<std::string> task_ids, WaitBehavior behavior,
                                     const WaitOptions &options) {
  auto function_stop = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.timeout);

  bool breakOnError = behavior & WaitBehavior::BreakOnError;
  bool stopOnFirst = behavior & WaitBehavior::Any;

  std::map<std::string, armonik::api::grpc::v1::results::ResultRaw> results;
  std::atomic<bool> hasError(false);

  ThreadPool::JoinSet join_set(thread_pool_);

  {
    std::lock_guard<std::mutex> _(maps_mutex);

    if (task_ids.empty()) {
      // If the task set is empty, wait for all the results that have been submitted at the moment of the wait
      for (auto &handler : result_handlers) {
        armonik::api::grpc::v1::results::ResultRaw result{};
        result.set_result_id(handler.first);
        result.set_status(armonik::api::grpc::v1::result_status::RESULT_STATUS_NOTFOUND);
        results.emplace(handler.first, std::move(result));
      }
    } else {
      // Otherwise, wait for the results of the specified tasks only
      for (auto &tid : task_ids) {
        auto result_id = taskId_resultId.find(tid);
        if (result_id == taskId_resultId.end()) {
          logger_.warning("Task ID " + tid + " has no associated result ID, skipping wait.");
          continue;
        }

        armonik::api::grpc::v1::results::ResultRaw result{};
        result.set_result_id(result_id->second);
        result.set_status(armonik::api::grpc::v1::result_status::RESULT_STATUS_NOTFOUND);
        results.emplace(result_id->second, std::move(result));
      }
    }
  }
  size_t initial_result_size = results.size();

  // Batcher to get results in batches
  Batcher<std::string> batcher(wait_batch_size_, [&](std::vector<std::string> &&batch) {
    auto batch_ptr = std::make_shared<std::vector<std::string>>(std::move(batch));
    join_set.Spawn([&, batch_ptr]() {
      auto &batch = *batch_ptr;
      armonik::api::grpc::v1::results::Filters filters{};
      for (auto &result_id : batch) {

        auto filter = filters.add_or_()->add_and_();
        filter->mutable_field()->mutable_result_raw_field()->set_field(
            armonik::api::grpc::v1::results::RESULT_RAW_ENUM_FIELD_RESULT_ID);
        filter->mutable_filter_string()->set_value(std::move(result_id));
        filter->mutable_filter_string()->set_operator_(armonik::api::grpc::v1::FILTER_STRING_OPERATOR_EQUAL);
      }

      auto response = channel_pool.WithChannel([&](std::shared_ptr<grpc::Channel> channel) {
        armonik::api::client::ResultsClient resultsClient(armonik::api::grpc::v1::results::Results::NewStub(channel));
        int total = 0;
        return resultsClient.list_results(std::move(filters), total, 0, filters.or__size());
      });

      for (auto &result : response) {
        // threadsafe as the result is known to be present in the map and we have unique keys
        results[result.result_id()] = std::move(result);
      }
    });
  });

  // Wait all the specified results
  while (!results.empty()) {
    // Get all the results using batched requests
    for (auto &result : results) {
      batcher.Add(result.first);
    }
    batcher.ProcessBatch();
    join_set.Wait();

    for (auto result_it = results.begin(); result_it != results.end();) {
      auto &result = result_it->second;
      auto status = result.status();

      // reset status to not found for next iteration if not completed or aborted
      result.set_status(armonik::api::grpc::v1::result_status::RESULT_STATUS_NOTFOUND);

      // Skip results that are not yet ready
      if (status == armonik::api::grpc::v1::result_status::RESULT_STATUS_CREATED) {
        ++result_it;
        continue;
      }

      // Only a COMPLETED result actually downloads payload bytes; reserve the budget for those
      // before spawning. This blocks here, in the single-threaded driving loop, rather than
      // inside a pool worker, so a tight budget throttles admission instead of occupying (and
      // potentially starving) thread_pool_ slots shared with unrelated Submit() work.
      std::int64_t result_bytes =
          status == armonik::api::grpc::v1::result_status::RESULT_STATUS_COMPLETED ? result.size() : 0;
      if (download_byte_budget_.Acquire(result_bytes)) {
        logger_.warning("Downloading a " + std::to_string(result_bytes) +
                        " byte result alone exceeds GrpcClient__DownloadByteBudget (" +
                        std::to_string(download_byte_budget_.capacity()) +
                        "). Consider raising it to bound peak download memory more evenly.");
      }

      auto result_ptr = std::make_shared<armonik::api::grpc::v1::results::ResultRaw>(std::move(result));
      join_set.Spawn([&, result_ptr, status, result_bytes]() {
        struct BudgetGuard {
          ByteBudget &budget;
          std::int64_t bytes;
          ~BudgetGuard() { budget.Release(bytes); }
        } budget_guard{download_byte_budget_, result_bytes};

        auto &result = *result_ptr;
        std::shared_ptr<IServiceInvocationHandler> handler{};
        std::string task_id{};

        { // Extract the handler and taskid information
          std::lock_guard<std::mutex> _(maps_mutex);
          auto handler_it = result_handlers.find(result.result_id());
          if (handler_it != result_handlers.end()) {
            handler = std::move(handler_it->second);
            result_handlers.erase(handler_it);
          }
          auto task_id_it = resultId_taskId.find(result.result_id());
          if (task_id_it != resultId_taskId.end()) {
            task_id = std::move(task_id_it->second);
            resultId_taskId.erase(task_id_it);
            taskId_resultId.erase(task_id);
          }
        }

        // function to be called upon errors
        auto handle_error = [&](const std::exception &e, const std::string &reason) {
          hasError.store(true, std::memory_order_relaxed);
          std::stringstream message;
          message << "Error while handling result " << result.result_id() << " for task "
                  << (task_id.empty() ? "[UNKNOWN]" : task_id);
          if (!reason.empty()) {
            message << " : " << reason;
          }
          message << " : " << e.what();
          logger_.error(message.str());
          if (handler) {
            try {
              handler->HandleError(e, task_id);
            } catch (const std::exception &he) {
              logger_.error(std::string("Handler threw in HandleError: ") + he.what());
            } catch (...) {
              logger_.error("Handler threw unknown exception in HandleError");
            }
          } else {
            logger_.warning("No handler registered for result " + result.result_id());
          }
        };

        std::string payload, owner_task_id;
        switch (status) {
        // Unreachable, generate an error to avoid missing results
        case armonik::api::grpc::v1::result_status::RESULT_STATUS_CREATED:
          handle_error(armonik::api::common::exceptions::ArmoniKApiException("Unreachable: result in CREATED status"),
                       "");
          break;

        // If the result is completed, we download it
        case armonik::api::grpc::v1::result_status::RESULT_STATUS_COMPLETED: {
          // Download the payload.
          //
          // gRPC's own transparent retry (the service-config retryPolicy set up in
          // ChannelFactory) only covers an attempt that fails before any response chunk has
          // reached us: once download_result_data_in's stream has started delivering data, a
          // dropped connection or a transient storage-backend error surfaces here instead,
          // and gRPC will not silently redial mid-stream. Re-issue the whole RPC from
          // scratch: download_result_data_in reads already-completed, immutable data, so
          // repeating it has no side effects and is always safe. The payload buffer is
          // cleared (keeping its capacity) before each attempt to avoid appending stale
          // data from a previous partial download.
          std::exception_ptr download_error;
          payload.reserve(result.size());
          for (int attempt = 1; attempt <= download_max_retry_; ++attempt) {
            try {
              payload.clear();
              channel_pool.WithChannel([&](std::shared_ptr<grpc::Channel> channel) {
                armonik::api::client::ResultsClient(armonik::api::grpc::v1::results::Results::NewStub(channel))
                    .download_result_data_in(session, result.result_id(), payload);
              });
              download_error = nullptr;
              break;
            } catch (const std::exception &e) {
              download_error = std::current_exception();
              if (attempt < download_max_retry_) {
                logger_.warning("Download attempt " + std::to_string(attempt) + "/" +
                                std::to_string(download_max_retry_) + " failed for result " + result.result_id() +
                                " (expected size " + std::to_string(result.size()) + " B): " + e.what() +
                                ". Retrying.");
              }
            }
          }
          if (download_error) {
            try {
              std::rethrow_exception(download_error);
            } catch (const std::exception &e) {
              handle_error(e, "Failed to download result data after " + std::to_string(download_max_retry_) +
                                  " attempt(s), expected size " + std::to_string(result.size()) + " B");
            }
            break;
          }

          // Call the response handler with the payload
          try {
            if (handler) {
              handler->HandleResponse(std::move(payload), task_id, result.result_id());
            } else {
              logger_.debug("No handler to deliver result " + result.result_id());
            }
          } catch (const std::exception &e) {
            handle_error(e, "Failed to execute result handler");
          }
          break;
        }

        // If the result is aborted, we retrieve the task error
        case armonik::api::grpc::v1::result_status::RESULT_STATUS_ABORTED:
          // The considered task is either the owner task of the result, or the original task
          owner_task_id = result.owner_task_id();
          if (result.owner_task_id().empty()) {
            owner_task_id = task_id;
          }

          if (owner_task_id.empty()) {
            handle_error(armonik::api::common::exceptions::ArmoniKApiException("Result is aborted"), "");
          } else {
            armonik::api::grpc::v1::TaskError error{};
            error.set_task_id(owner_task_id);

            // Retrieve the task error details
            try {
              channel_pool.WithChannel([&](std::shared_ptr<grpc::Channel> channel) {
                auto task = armonik::api::client::TasksClient(armonik::api::grpc::v1::tasks::Tasks::NewStub(channel))
                                .get_task(owner_task_id);
                auto task_error = error.add_errors();
                task_error->set_detail(std::move(*task.mutable_output()->mutable_error()));
                task_error->set_task_status(task.status());
              });
            } catch (const std::exception &e) {
              error.add_errors()->set_detail(e.what());
            }
            handle_error(armonik::api::common::exceptions::ArmoniKTaskError("Result is aborted", error), "");
          }
          break;

        // In all other cases, we just call the handler on a generic error
        case armonik::api::grpc::v1::result_status::RESULT_STATUS_DELETED:
          handle_error(armonik::api::common::exceptions::ArmoniKApiException("Result is deleted"), "");
          break;
        case armonik::api::grpc::v1::result_status::RESULT_STATUS_NOTFOUND:
          handle_error(armonik::api::common::exceptions::ArmoniKApiException("Result was not found"), "");
          break;
        default:
          handle_error(armonik::api::common::exceptions::ArmoniKApiException("Result status is unknown"), "");
          break;
        }
      });

      result_it = results.erase(result_it);
    }

    join_set.Wait();

    // If we wait for any and at least one is done, or if we break on error and had an error, then return
    if ((stopOnFirst && results.size() < initial_result_size) ||
        (breakOnError && hasError.load(std::memory_order_relaxed))) {
      break;
    }
    if (std::chrono::steady_clock::now() > function_stop) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(options.polling_ms));
  }
}

void SessionServiceImpl::CloseSession() {
  auto reply = channel_pool.WithChannel([&](const std::shared_ptr<::grpc::Channel> &channel) {
    return armonik::api::client::SessionsClient(armonik::api::grpc::v1::sessions::Sessions::NewStub(channel))
        .close_session(session);
  });
}

void SessionServiceImpl::CancelSession() {
  auto reply = channel_pool.WithChannel([&](const std::shared_ptr<::grpc::Channel> &channel) {
    return armonik::api::client::SessionsClient(armonik::api::grpc::v1::sessions::Sessions::NewStub(channel))
        .cancel_session(session);
  });
}

void SessionServiceImpl::PurgeSession() {
  auto reply = channel_pool.WithChannel([&](const std::shared_ptr<::grpc::Channel> &channel) {
    return armonik::api::client::SessionsClient(armonik::api::grpc::v1::sessions::Sessions::NewStub(channel))
        .purge_session(session);
  });
}

void SessionServiceImpl::DropSession() {
  // Clear all the maps
  {
    std::lock_guard<std::mutex> _(maps_mutex);
    taskId_resultId.clear();
    resultId_taskId.clear();
    result_handlers.clear();
  }
  // Cancel the session
  auto reply = channel_pool.WithChannel([&](const std::shared_ptr<::grpc::Channel> &channel) {
    return armonik::api::client::SessionsClient(armonik::api::grpc::v1::sessions::Sessions::NewStub(channel))
        .cancel_session(session);
  });

  // Delete the data of every result of the session on the server side. The session must be cancelled first.
  channel_pool.WithChannel([&](const std::shared_ptr<::grpc::Channel> &channel) {
    return armonik::api::client::SessionsClient(armonik::api::grpc::v1::sessions::Sessions::NewStub(channel))
        .purge_session(session);
  });
}

void SessionServiceImpl::CleanupTasks(std::vector<std::string> task_ids) {
  // Remove the given tasks from the maps
  {
    std::lock_guard<std::mutex> _(maps_mutex);
    for (auto &&t : task_ids) {
      auto loc = taskId_resultId.find(t);
      if (loc != taskId_resultId.end()) {
        resultId_taskId.erase(loc->second);
        result_handlers.erase(loc->second);
        taskId_resultId.erase(loc->first);
      }
    }
  }
  const size_t batch_size = 500;
  auto tasks_iterator = task_ids.begin();
  // Cancel the given tasks
  while (tasks_iterator != task_ids.end()) {
    channel_pool.WithChannel([&](const std::shared_ptr<::grpc::Channel> &channel) {
      std::vector<std::string> batched_ids;
      for (size_t i = 0; i < batch_size && tasks_iterator != task_ids.end(); ++i) {
        batched_ids.push_back(*tasks_iterator);
        tasks_iterator++;
      }
      armonik::api::client::TasksClient(armonik::api::grpc::v1::tasks::Tasks::NewStub(channel))
          .cancel_tasks(batched_ids);
    });
  }

  tasks_iterator = task_ids.begin();

  while (tasks_iterator != task_ids.end()) {
    // List batch of results from the given tasks
    auto map_results = channel_pool.WithChannel([&](const std::shared_ptr<::grpc::Channel> &channel) {
      std::vector<std::string> batched_ids;
      for (size_t i = 0; i < batch_size && tasks_iterator != task_ids.end(); ++i) {
        batched_ids.push_back(std::move(*tasks_iterator));
        tasks_iterator++;
      }
      return armonik::api::client::TasksClient(armonik::api::grpc::v1::tasks::Tasks::NewStub(channel))
          .get_result_ids(batched_ids);
    });

    // Delete results
    channel_pool.WithChannel([&](const std::shared_ptr<::grpc::Channel> &channel) {
      auto results = armonik::api::client::ResultsClient(armonik::api::grpc::v1::results::Results::NewStub(channel));
      std::vector<std::string> resultIds;
      resultIds.reserve(map_results.size());
      for (auto &&taskId_resultIds : map_results) {
        resultIds.insert(resultIds.end(), std::make_move_iterator(taskId_resultIds.second.begin()),
                         std::make_move_iterator(taskId_resultIds.second.end()));
      }
      results.delete_results_data(session, resultIds);
    });
  }
}

} // namespace Internal
} // namespace Client
} // namespace Sdk
} // namespace ArmoniK
