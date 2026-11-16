#include "ChannelPool.h"

#include <armonik/client/channel/ChannelFactory.h>
#include <armonik/common/options/ControlPlane.h>
#include <armonik/common/utils/ChannelArguments.h>
#include <armonik/sdk/common/Version.h>
#include <grpcpp/create_channel.h>
#include <utility>

namespace ArmoniK {
namespace Sdk {
namespace Client {
namespace Internal {

std::shared_ptr<grpc::Channel> ChannelPool::AcquireChannel() {
  std::shared_ptr<grpc::Channel> channel;
  {
    std::lock_guard<std::mutex> _(channel_mutex_);
    if (!channel_pool_.empty()) {
      channel = channel_pool_.front();
      channel_pool_.pop();
    }
  }

  if (channel != nullptr) {
    if (ShutdownOnFailure(channel)) {
      logger_.debug("Shutdown unhealthy channel");
    } else {
      logger_.debug("Acquired already existing channel from pool");
      return channel;
    }
  }
  channel = channel_factory_.create_channel();
  logger_.debug("Created and acquired new channel from pool");
  return channel;
}

void ChannelPool::ReleaseChannel(std::shared_ptr<grpc::Channel> channel) {
  if (ShutdownOnFailure(channel)) {
    logger_.debug("Shutdown unhealthy channel");
  } else {
    logger_.debug("Released channel to pool");
    std::lock_guard<std::mutex> _(channel_mutex_);
    channel_pool_.push(channel);
  }
}

bool ChannelPool::ShutdownOnFailure(const std::shared_ptr<grpc::Channel> &channel) {
  // Read the state without try_to_connect: a check must not start a connection on an idle channel. The next RPC on
  // the channel connects it.
  switch (channel->GetState(false)) {
  case GRPC_CHANNEL_CONNECTING:
    break;
  case GRPC_CHANNEL_IDLE:
    break;

  case GRPC_CHANNEL_SHUTDOWN:
    return true;
    break;

  case GRPC_CHANNEL_TRANSIENT_FAILURE:
    return true;
    break;

  case GRPC_CHANNEL_READY:
    break;

  default:
    return false;
    break;
  }
  return false;
}

ChannelPool::ChannelPool(ArmoniK::Sdk::Common::Properties properties, armonik::api::common::logger::Logger &logger)
    : properties_(std::move(properties)),
      channel_factory_(static_cast<armonik::api::common::utils::Configuration>(properties_.configuration), logger),
      logger_(logger.local({{"sdk_version", ArmoniK::Sdk::Common::getVersion()}})) {}
ChannelPool::~ChannelPool() = default;

ChannelPool::ChannelGuard::ChannelGuard(Internal::ChannelPool *pool) : pool_(pool) {
  if (pool_ != nullptr) {
    channel = pool_->AcquireChannel();
  }
}

ChannelPool::ChannelGuard::~ChannelGuard() {
  if (pool_ != nullptr) {
    pool_->ReleaseChannel(channel);
  }
}

ChannelPool::ChannelGuard ChannelPool::GetChannel() { return ChannelGuard(this); }

} // namespace Internal
} // namespace Client
} // namespace Sdk
} // namespace ArmoniK
