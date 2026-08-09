#include "xgc_ros2_tools_adapter/tools_adapter.hpp"

#include <gtest/gtest.h>
#include <json/json.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <std_msgs/msg/string.hpp>
#include <std_srvs/srv/set_bool.hpp>
#include <string>
#include <thread>

#include "xgc_ros2_tools_adapter/error.hpp"
#include "xgc_ros2_tools_adapter/generated_contract.hpp"

namespace xgc_ros2_tools_adapter {
namespace {

constexpr char kScopeKey[] =
    "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

xgc::v1::SchemaReference schema(const contract::Schema& source) {
  xgc::v1::SchemaReference result;
  result.set_message_id(source.message_id);
  result.set_type_name(source.type_name);
  result.set_schema_version(source.version);
  result.set_schema_fingerprint(source.fingerprint);
  return result;
}

xgc::adapter::v1::AdapterInstanceSpec instanceSpec(
    std::uint32_t domain_id = 0, const std::string& rmw_implementation = "") {
  xgc::adapter::v1::AdapterInstanceSpec spec;
  *spec.mutable_configuration()->mutable_schema() =
      schema(contract::kConfiguration);
  spec.mutable_configuration()->set_encoding(xgc::v1::PAYLOAD_ENCODING_JSON);
  spec.mutable_configuration()->set_value(
      "{\"domainId\":" + std::to_string(domain_id) +
      ",\"rmwImplementation\":\"" + rmw_implementation + "\"}");
  spec.mutable_scope()->set_kind("ros2-native-context");
  spec.mutable_scope()->set_key(kScopeKey);
  (*spec.mutable_scope()->mutable_attributes())["domain-id"] =
      std::to_string(domain_id);
  if (!rmw_implementation.empty()) {
    (*spec.mutable_scope()->mutable_attributes())["rmw-implementation"] =
        rmw_implementation;
  }
  return spec;
}

xgc::adapter::v1::EnabledCapability grant(const contract::Endpoint& endpoint) {
  xgc::adapter::v1::EnabledCapability result;
  result.set_capability_id(endpoint.capability_id);
  result.set_contract_version(endpoint.contract_version);
  result.set_contract_digest(endpoint.contract_digest);
  result.add_enabled_endpoint_ids(endpoint.endpoint_id);
  return result;
}

xgc::adapter::v1::OperationRequest operation(
    const std::string& json, const contract::Endpoint& endpoint) {
  xgc::adapter::v1::OperationRequest request;
  auto* context = request.mutable_context();
  context->set_capability_id(endpoint.capability_id);
  context->set_contract_version(endpoint.contract_version);
  context->set_contract_digest(endpoint.contract_digest);
  context->set_endpoint_id(endpoint.endpoint_id);
  context->mutable_deadline()->set_deadline_unix_nanos(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          (std::chrono::system_clock::now() + std::chrono::seconds(5))
              .time_since_epoch())
          .count());
  context->mutable_subject()->set_kind("ros2-native-context");
  context->mutable_subject()->set_key(kScopeKey);
  (*context->mutable_subject()->mutable_attributes())["domain-id"] = "0";
  *request.mutable_input()->mutable_schema() = schema(endpoint.input_schema);
  request.mutable_input()->set_encoding(xgc::v1::PAYLOAD_ENCODING_JSON);
  request.mutable_input()->set_value(json);
  return request;
}

Json::Value parseJson(const std::string& input) {
  Json::CharReaderBuilder builder;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value value;
  std::string errors;
  EXPECT_TRUE(
      reader->parse(input.data(), input.data() + input.size(), &value, &errors))
      << errors;
  return value;
}

TEST(NativeContext, ParsesExactConfigurationAndRejectsScopeDrift) {
  const NativeContext context =
      NativeContext::FromInstanceSpec(instanceSpec(42, "rmw_cyclonedds_cpp"));
  EXPECT_EQ(42u, context.domain_id);
  EXPECT_EQ("rmw_cyclonedds_cpp", context.rmw_implementation);
  EXPECT_EQ(kScopeKey, context.scope_key);

  auto drifted = instanceSpec(42, "rmw_cyclonedds_cpp");
  (*drifted.mutable_scope()->mutable_attributes())["domain-id"] = "43";
  EXPECT_THROW(NativeContext::FromInstanceSpec(drifted), ToolsError);

  auto extra = instanceSpec();
  (*extra.mutable_scope()->mutable_attributes())["shell"] = "forbidden";
  EXPECT_THROW(NativeContext::FromInstanceSpec(extra), ToolsError);
}

class ToolsAdapterIntegration : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    int argc = 0;
    char** argv = nullptr;
    rclcpp::init(argc, argv);
    adapter_node_ = std::make_shared<rclcpp::Node>(
        "xgc_ros2_tools_adapter_integration_adapter");
    peer_node_ = std::make_shared<rclcpp::Node>(
        "xgc_ros2_tools_adapter_integration_peer");
    executor_ = std::make_shared<rclcpp::executors::MultiThreadedExecutor>(
        rclcpp::ExecutorOptions(), 4);
    executor_->add_node(adapter_node_);
    executor_->add_node(peer_node_);
    executor_thread_ = std::thread([] { executor_->spin(); });
  }

  static void TearDownTestSuite() {
    executor_->cancel();
    if (executor_thread_.joinable()) {
      executor_thread_.join();
    }
    executor_->remove_node(adapter_node_);
    executor_->remove_node(peer_node_);
    peer_node_.reset();
    adapter_node_.reset();
    executor_.reset();
    rclcpp::shutdown();
  }

  std::unique_ptr<ToolsAdapter> createAdapter() {
    return std::make_unique<ToolsAdapter>(adapter_node_,
                                          NativeContext{0, "", kScopeKey});
  }

  void activate(ToolsAdapter* adapter, const contract::Endpoint& endpoint) {
    auto spec = instanceSpec();
    std::string error;
    ASSERT_TRUE(adapter->ApplyInstanceSpec(spec, &error)) << error;
    if (&endpoint == &contract::kPublish) {
      ASSERT_TRUE(
          adapter->StartPublishCapability(spec, grant(endpoint), &error))
          << error;
    } else {
      ASSERT_TRUE(
          adapter->StartServiceCapability(spec, grant(endpoint), &error))
          << error;
    }
  }

  inline static rclcpp::Node::SharedPtr adapter_node_;
  inline static rclcpp::Node::SharedPtr peer_node_;
  inline static std::shared_ptr<rclcpp::executors::MultiThreadedExecutor>
      executor_;
  inline static std::thread executor_thread_;
};

TEST_F(ToolsAdapterIntegration, PublishesTypedJsonWithoutRosCli) {
  std::mutex mutex;
  std::condition_variable received;
  std::string received_value;
  auto subscriber = peer_node_->create_subscription<std_msgs::msg::String>(
      "/xgc_ros2_tools_adapter_test/topic", 10,
      [&](std_msgs::msg::String::ConstSharedPtr message) {
        std::lock_guard<std::mutex> lock(mutex);
        received_value = message->data;
        received.notify_all();
      });

  auto adapter = createAdapter();
  activate(adapter.get(), contract::kPublish);
  const auto request = operation(R"({
    "topic":"/xgc_ros2_tools_adapter_test/topic",
    "messageType":"std_msgs/msg/String",
    "message":{"data":"adapter-runtime"},
    "qosDepth":10,
    "reliability":"reliable",
    "durability":"volatile",
    "waitForSubscribersMs":2000
  })",
                                 contract::kPublish);
  const auto result =
      adapter->Publish(request, xgc2::adapter_runtime::CancellationToken());
  ASSERT_EQ(xgc::adapter::v1::OPERATION_PHASE_SUCCEEDED, result.phase)
      << result.error.message();
  EXPECT_EQ("published", parseJson(result.output.value())["event"].asString());

  std::unique_lock<std::mutex> lock(mutex);
  ASSERT_TRUE(received.wait_for(lock, std::chrono::seconds(2),
                                [&] { return !received_value.empty(); }));
  EXPECT_EQ("adapter-runtime", received_value);
  (void)subscriber;
}

TEST_F(ToolsAdapterIntegration, CallsTypedServiceWithoutRosCli) {
  auto service = peer_node_->create_service<std_srvs::srv::SetBool>(
      "/xgc_ros2_tools_adapter_test/set_bool",
      [](const std::shared_ptr<std_srvs::srv::SetBool::Request> request,
         std::shared_ptr<std_srvs::srv::SetBool::Response> response) {
        response->success = request->data;
        response->message = request->data ? "enabled" : "disabled";
      });

  auto adapter = createAdapter();
  activate(adapter.get(), contract::kService);
  const auto request = operation(R"({
    "service":"/xgc_ros2_tools_adapter_test/set_bool",
    "serviceType":"std_srvs/srv/SetBool",
    "request":{"data":false},
    "waitForServiceMs":2000,
    "callTimeoutMs":2000
  })",
                                 contract::kService);
  const auto result =
      adapter->CallService(request, xgc2::adapter_runtime::CancellationToken());
  ASSERT_EQ(xgc::adapter::v1::OPERATION_PHASE_SUCCEEDED, result.phase)
      << result.error.message();
  const Json::Value output = parseJson(result.output.value());
  EXPECT_EQ("response", output["event"].asString());
  EXPECT_FALSE(output["response"]["success"].asBool());
  EXPECT_EQ("disabled", output["response"]["message"].asString());
  (void)service;
}

TEST_F(ToolsAdapterIntegration, RejectsUseOutsideCapabilityLifecycle) {
  auto adapter = createAdapter();
  const auto request = operation(R"({
    "topic":"/xgc_ros2_tools_adapter_test/rejected",
    "messageType":"std_msgs/msg/String",
    "message":{"data":"must-not-publish"},
    "qosDepth":10,
    "reliability":"reliable",
    "durability":"volatile",
    "waitForSubscribersMs":0
  })",
                                 contract::kPublish);
  const auto result =
      adapter->Publish(request, xgc2::adapter_runtime::CancellationToken());
  EXPECT_EQ(xgc::adapter::v1::OPERATION_PHASE_REJECTED, result.phase);
  EXPECT_EQ(xgc::adapter::v1::ERROR_CLASS_REJECTED, result.error.class_());
  EXPECT_EQ("publish_capability_disabled", result.error.code());
}

TEST_F(ToolsAdapterIntegration,
       ReportsMissingTypeSupportAsPermanentInputError) {
  auto adapter = createAdapter();
  activate(adapter.get(), contract::kPublish);
  const auto request = operation(R"({
    "topic":"/xgc_ros2_tools_adapter_test/missing_type",
    "messageType":"missing_package/msg/Missing",
    "message":{},
    "qosDepth":10,
    "reliability":"reliable",
    "durability":"volatile",
    "waitForSubscribersMs":0
  })",
                                 contract::kPublish);
  const auto result =
      adapter->Publish(request, xgc2::adapter_runtime::CancellationToken());
  EXPECT_EQ(xgc::adapter::v1::OPERATION_PHASE_FAILED, result.phase);
  EXPECT_EQ(xgc::adapter::v1::ERROR_CLASS_PERMANENT, result.error.class_());
  EXPECT_EQ("ros_type_support_unavailable", result.error.code());
}

}  // namespace
}  // namespace xgc_ros2_tools_adapter
