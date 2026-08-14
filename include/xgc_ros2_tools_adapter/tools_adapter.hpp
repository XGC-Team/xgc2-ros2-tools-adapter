#pragma once

#include <json/json.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "rclcpp/rclcpp.hpp"
#include "xgc/adapter/v1/adapter.pb.h"
#include "xgc/v1/message.pb.h"
#include "xgc2/adapter_runtime/client.hpp"
#include "xgc_ros2_tools_adapter/json_codec.hpp"

namespace xgc_ros2_tools_adapter {

struct NativeContext {
  std::uint32_t domain_id = 0;
  std::string rmw_implementation;
  std::string scope_key;

  static NativeContext
  FromInstanceSpec(const xgc::adapter::v1::AdapterInstanceSpec &spec);
  void ApplyEnvironment() const;
  bool operator==(const NativeContext &other) const noexcept;
};

class ToolsAdapter {
public:
  ToolsAdapter(rclcpp::Node::SharedPtr node, NativeContext native_context);

  bool ApplyInstanceSpec(const xgc::adapter::v1::AdapterInstanceSpec &spec,
                         std::string *error);
  void ClearInstanceSpec() noexcept;
  bool StartPublishCapability(const xgc::adapter::v1::AdapterInstanceSpec &spec,
                              const xgc::adapter::v1::EnabledCapability &grant,
                              std::string *error);
  void StopPublishCapability() noexcept;
  bool StartServiceCapability(const xgc::adapter::v1::AdapterInstanceSpec &spec,
                              const xgc::adapter::v1::EnabledCapability &grant,
                              std::string *error);
  void StopServiceCapability() noexcept;

  xgc2::adapter_runtime::OperationResult
  Publish(const xgc::adapter::v1::OperationRequest &request,
          const xgc2::adapter_runtime::CancellationToken &cancellation);
  xgc2::adapter_runtime::OperationResult
  CallService(const xgc::adapter::v1::OperationRequest &request,
              const xgc2::adapter_runtime::CancellationToken &cancellation);

private:
  Json::Value parseInput(const xgc::v1::Payload &payload,
                         const xgc::v1::SchemaReference &expected_schema,
                         std::uint32_t maximum_request_bytes) const;
  void validateSubject(const xgc::adapter::v1::WorkContext &context) const;
  xgc2::adapter_runtime::OperationResult
  success(const Json::Value &value,
          const xgc::v1::SchemaReference &output_schema,
          std::uint32_t maximum_response_bytes) const;
  xgc2::adapter_runtime::OperationResult
  failure(const std::exception &exception) const;

  rclcpp::Node::SharedPtr node_;
  NativeContext native_context_;
  JsonCodec codec_;
  std::atomic<bool> instance_spec_applied_{false};
  std::atomic<bool> publish_enabled_{false};
  std::atomic<bool> service_enabled_{false};
};

bool BindCapabilities(xgc2::adapter_runtime::ClientConfig *config,
                      ToolsAdapter *adapter, std::string *error);

} // namespace xgc_ros2_tools_adapter
