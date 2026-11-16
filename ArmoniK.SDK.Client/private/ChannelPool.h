#pragma once

#include <armonik/client/channel/ChannelFactory.h>
#include <armonik/common/logger/formatter.h>
#include <armonik/common/logger/logger.h>
#include <armonik/common/logger/writer.h>
#include <armonik/sdk/common/Properties.h>
#include <grpcpp/channel.h>
#include <mutex>
#include <queue>
#include <utility>

namespace ArmoniK {
namespace Sdk {
namespace Client {
namespace Internal {
/**
 * @brief A pool for Grpc channels
 */
class ChannelPool {
public:
  /**
   * @brief Creates a channel pool from the given properties
   * @param properties Properties
   */
  explicit ChannelPool(ArmoniK::Sdk::Common::Properties properties, armonik::api::common::logger::Logger &logger);

  /**
   * @brief Copy constructor
   *
   * @param other Other channel pool
   */
  ChannelPool(const ChannelPool &) = delete;

  /**
   * @brief Copy operator
   *
   * @param other Other channel pool
   */
  ChannelPool &operator=(const ChannelPool &) = delete;

  /**
   * @brief Move constructor
   *
   * @param other Other channel pool
   */
  ChannelPool(ChannelPool &&other) noexcept = default;
  /**
   * @brief Move assignment constructor
   *
   * @param other Other channel pool
   */
  ChannelPool &operator=(ChannelPool &&other) noexcept = default;

  /**
   * @brief Destroy the Channel Pool object
   *
   */
  ~ChannelPool();

  /**
   * @brief
   *
   * @return std::shared_ptr<grpc::Channel>
   */
  std::shared_ptr<grpc::Channel> AcquireChannel();

  /**
   * @brief Releases a channel to the pool
   *
   * @param channel The gRPC channel
   */
  void ReleaseChannel(std::shared_ptr<grpc::Channel> channel);

  /**
   * @brief Check whether a channel is shut down or failing, without starting a connection
   *
   * @param channel The gRPC channel
   * @return true if the channel must be dropped from the pool
   * @return false if not
   */
  static bool ShutdownOnFailure(const std::shared_ptr<grpc::Channel> &channel);

  /**
   * Calls the function with an acquired channel
   * The function must take a std::shared_ptr<::grpc::Channel> as a first argument
   * @tparam F function type
   * @tparam Args function arguments (apart from the channel)
   * @param f function to call
   * @param args arguments
   * @return what the function returns
   */
  template <typename F, typename... Args>
  typename std::result_of<F(std::shared_ptr<::grpc::Channel>, Args &&...)>::type WithChannel(F f, Args &&...args) {
    auto guard = GetChannel();
    return f(guard.channel, static_cast<Args &&>(args)...);
  }

  /**
   * @brief Helper class that acquires a channel from a pool when constructed, and releases it when disposed
   *
   */
  class ChannelGuard {
  public:
    /**
     * @brief Free channel
     *
     */
    std::shared_ptr<grpc::Channel> channel;

    /**
     * @brief Construct a new Channel Guard object
     *
     * @param properties
     */
    ChannelGuard(Internal::ChannelPool *pool);

    /**
     * @brief Copy constructor (deleted, the guard owns a single release of the channel)
     */
    ChannelGuard(const ChannelGuard &) = delete;

    /**
     * @brief Copy assignment operator (deleted, the guard owns a single release of the channel)
     */
    ChannelGuard &operator=(const ChannelGuard &) = delete;

    /**
     * @brief Move constructor
     */
    ChannelGuard(ChannelGuard &&other) noexcept : channel(std::move(other.channel)), pool_(other.pool_) {
      other.pool_ = nullptr;
    }

    /**
     * @brief Move assignment operator
     */
    ChannelGuard &operator=(ChannelGuard &&other) noexcept {
      if (this != &other) {
        if (pool_ != nullptr) {
          pool_->ReleaseChannel(channel);
        }
        channel = std::move(other.channel);
        pool_ = other.pool_;
        other.pool_ = nullptr;
      }
      return *this;
    }

    /**
     * @brief Destroy the Channel Guard object
     *
     */
    ~ChannelGuard();

    /**
     * @brief Implicit convert a ChannelGuard into a ChannelBase
     *
     * @param other the other channel guard
     * @return std::shared_ptr<grpc::Channel>&
     */
    explicit operator std::shared_ptr<grpc::Channel>() const { return this->channel; }

    ChannelPool *pool_;
  };

  /**
   * @brief Get the Channel object that will be automatically released when disposed
   *
   * @return ChannelGuard
   */
  ChannelGuard GetChannel();

private:
  ArmoniK::Sdk::Common::Properties properties_;
  /** Built once from properties_, which must be declared before it */
  armonik::api::client::ChannelFactory channel_factory_;
  std::queue<std::shared_ptr<grpc::Channel>> channel_pool_;
  std::mutex channel_mutex_;
  armonik::api::common::logger::LocalLogger logger_;
};

} // namespace Internal
} // namespace Client
} // namespace Sdk
} // namespace ArmoniK
