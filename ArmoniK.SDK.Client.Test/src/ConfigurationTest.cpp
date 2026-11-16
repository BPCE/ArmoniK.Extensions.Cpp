#include <gtest/gtest.h>

#include <armonik/sdk/common/Configuration.h>

#include <string>

namespace {
ArmoniK::Sdk::Common::ControlPlane control_plane_with(const std::string &key, const std::string &value) {
  ArmoniK::Sdk::Common::Configuration config;
  config.set(key, value);
  return config.get_control_plane();
}
} // namespace

// Warnings are printed once per key and value in the process, so each test uses values no other test uses.

TEST(ControlPlaneConfiguration, valid_values_are_used) {
  testing::internal::CaptureStderr();
  EXPECT_EQ(control_plane_with("GrpcClient__DownloadMaxRetry", "7").getDownloadMaxRetry(), 7);
  EXPECT_EQ(control_plane_with("GrpcClient__WaitBatchSize", " 42 ").getWaitBatchSize(), 42);
  EXPECT_EQ(control_plane_with("GrpcClient__DownloadByteBudget", "8589934592").getDownloadByteBudget(), 8589934592LL);
  EXPECT_EQ(testing::internal::GetCapturedStderr(), "");
}

TEST(ControlPlaneConfiguration, unset_and_zero_defaults_are_silent) {
  testing::internal::CaptureStderr();
  EXPECT_EQ(control_plane_with("GrpcClient__SubmitBatchSize", "").getSubmitBatchSize(), 200);
  EXPECT_EQ(control_plane_with("GrpcClient__ThreadPoolSize", "0").getThreadPoolSize(), 0);
  EXPECT_EQ(control_plane_with("GrpcClient__UploadByteBudget", "0").getUploadByteBudget(), 0);
  EXPECT_EQ(testing::internal::GetCapturedStderr(), "");
}

TEST(ControlPlaneConfiguration, invalid_values_fall_back_with_a_warning) {
  struct Case {
    const char *key;
    const char *value;
    const char *reason;
  };
  const Case cases[] = {
      {"GrpcClient__ThreadPoolSize", "abc", "not an integer"},
      {"GrpcClient__ThreadPoolSize", "12abc", "not an integer"},
      {"GrpcClient__ThreadPoolSize", "-5", "must be positive"},
      {"GrpcClient__DownloadMaxRetry", "0", "must be positive"},
      {"GrpcClient__WaitBatchSize", "3000000000", "out of range"},
      {"GrpcClient__DownloadByteBudget", "99999999999999999999", "out of range"},
  };
  for (const auto &c : cases) {
    testing::internal::CaptureStderr();
    control_plane_with(c.key, c.value);
    const auto err = testing::internal::GetCapturedStderr();
    EXPECT_NE(err.find(c.key), std::string::npos) << c.key << "=" << c.value << ": " << err;
    EXPECT_NE(err.find(c.reason), std::string::npos) << c.key << "=" << c.value << ": " << err;
  }
  EXPECT_EQ(control_plane_with("GrpcClient__ThreadPoolSize", "-6").getThreadPoolSize(), 0);
  EXPECT_EQ(control_plane_with("GrpcClient__DownloadMaxRetry", "-1").getDownloadMaxRetry(), 3);
  EXPECT_EQ(control_plane_with("GrpcClient__WaitBatchSize", "4000000000").getWaitBatchSize(), 200);
}

TEST(ControlPlaneConfiguration, a_warning_is_printed_once_per_value) {
  testing::internal::CaptureStderr();
  control_plane_with("GrpcClient__SubmitBatchSize", "twice");
  control_plane_with("GrpcClient__SubmitBatchSize", "twice");
  const auto err = testing::internal::GetCapturedStderr();
  const auto first = err.find("GrpcClient__SubmitBatchSize");
  ASSERT_NE(first, std::string::npos);
  EXPECT_EQ(err.find("GrpcClient__SubmitBatchSize", first + 1), std::string::npos) << err;
}
