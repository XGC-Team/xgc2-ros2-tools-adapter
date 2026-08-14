#include "xgc_ros2_tools_adapter/tools_adapter.hpp"

#include <rmw/validate_full_topic_name.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "xgc_ros2_tools_adapter/error.hpp"
#include "xgc_ros2_tools_adapter/generated_contract.hpp"

namespace xgc_ros2_tools_adapter {
namespace {

constexpr std::uint32_t kMaximumDomainID = 232;
constexpr std::uint32_t kMaximumWaitMilliseconds = 300000;
constexpr std::size_t kMaximumConfigurationBytes = 16u * 1024u;
constexpr std::size_t kMaximumRosNameBytes = 1024;
constexpr std::size_t kMaximumRosTypeBytes = 256;

const std::regex &messageTypePattern() {
  static const std::regex pattern(
      "^[A-Za-z][A-Za-z0-9_]*/msg/[A-Za-z][A-Za-z0-9_]*$");
  return pattern;
}

const std::regex &serviceTypePattern() {
  static const std::regex pattern(
      "^[A-Za-z][A-Za-z0-9_]*/srv/[A-Za-z][A-Za-z0-9_]*$");
  return pattern;
}

const std::regex &rmwPattern() {
  static const std::regex pattern("^[A-Za-z][A-Za-z0-9_]*$");
  return pattern;
}

const std::regex &scopeKeyPattern() {
  static const std::regex pattern("^sha256:[0-9a-f]{64}$");
  return pattern;
}

void requireObject(const Json::Value &value, const std::string &field) {
  if (!value.isObject()) {
    permanentError("invalid_request", field + " must be a JSON object");
  }
}

void rejectUnknownFields(const Json::Value &value,
                         const std::set<std::string> &allowed,
                         const std::string &description) {
  for (const auto &member : value.getMemberNames()) {
    if (allowed.count(member) == 0) {
      permanentError("invalid_request",
                     description + " contains unknown field '" + member + "'");
    }
  }
}

std::string requiredString(const Json::Value &value, const std::string &field) {
  if (!value.isMember(field) || !value[field].isString() ||
      value[field].asString().empty()) {
    permanentError("invalid_request", field + " must be a non-empty string");
  }
  return value[field].asString();
}

std::string requiredPossiblyEmptyString(const Json::Value &value,
                                        const std::string &field) {
  if (!value.isMember(field) || !value[field].isString()) {
    permanentError("invalid_configuration", field + " must be a string");
  }
  return value[field].asString();
}

std::uint32_t requiredUInt(const Json::Value &value, const std::string &field,
                           std::uint32_t minimum, std::uint32_t maximum) {
  if (!value.isMember(field) || !value[field].isIntegral() ||
      (value[field].isInt64() && value[field].asInt64() < 0)) {
    permanentError("invalid_request", field + " must be an unsigned integer");
  }
  const Json::UInt64 parsed = value[field].asUInt64();
  if (parsed < minimum || parsed > maximum) {
    permanentError("invalid_request", field + " must be between " +
                                          std::to_string(minimum) + " and " +
                                          std::to_string(maximum));
  }
  return static_cast<std::uint32_t>(parsed);
}

void validateGraphName(const std::string &value, const std::string &field) {
  if (value.empty() || value.front() != '/' ||
      value.size() > kMaximumRosNameBytes) {
    permanentError("invalid_ros_name",
                   field + " must be an absolute ROS2 graph name");
  }
  int validation_result = RMW_TOPIC_VALID;
  std::size_t invalid_index = 0;
  const rmw_ret_t result = rmw_validate_full_topic_name(
      value.c_str(), &validation_result, &invalid_index);
  if (result != RMW_RET_OK || validation_result != RMW_TOPIC_VALID) {
    permanentError("invalid_ros_name",
                   field + " is not a valid absolute ROS2 graph name");
  }
}

void validateType(const std::string &value, const std::regex &pattern,
                  const std::string &field, const std::string &syntax) {
  if (value.size() > kMaximumRosTypeBytes ||
      !std::regex_match(value, pattern)) {
    permanentError("invalid_ros_type", field + " must use " + syntax);
  }
}

void setEnvironment(const char *name, const std::string &value) {
  const int result =
      value.empty() ? ::unsetenv(name) : ::setenv(name, value.c_str(), 1);
  if (result != 0) {
    throw std::runtime_error("unable to configure " + std::string(name) + ": " +
                             std::strerror(errno));
  }
}

const xgc::adapter::v1::CapabilityEndpointContract *
findEndpoint(const xgc::adapter::v1::CapabilityContract &value,
             const std::string &endpoint_id) {
  for (const auto &endpoint : value.endpoints()) {
    if (endpoint.endpoint_id() == endpoint_id) {
      return &endpoint;
    }
  }
  return nullptr;
}

bool schemaMatches(const xgc::v1::SchemaReference &actual,
                   const contract::Schema &expected) {
  return actual.message_id() == expected.message_id &&
         actual.type_name() == expected.type_name &&
         actual.schema_version() == expected.version &&
         actual.schema_fingerprint() == expected.fingerprint;
}

xgc::v1::SchemaReference schemaReference(const contract::Schema &value) {
  xgc::v1::SchemaReference result;
  result.set_message_id(value.message_id);
  result.set_type_name(value.type_name);
  result.set_schema_version(value.version);
  result.set_schema_fingerprint(value.fingerprint);
  return result;
}

bool validateExpectedContract(
    const xgc::adapter::v1::CapabilityContract &actual,
    const contract::Endpoint &expected, std::string *error) {
  const auto reject = [error](const std::string &message) {
    if (error != nullptr) {
      *error = message;
    }
    return false;
  };
  if (actual.capability_id() != expected.capability_id ||
      actual.contract_version() != expected.contract_version ||
      actual.contract_digest() != expected.contract_digest ||
      actual.endpoints_size() != 1) {
    return reject(
        "capability identity or digest differs from compiled contract");
  }
  const auto *endpoint = findEndpoint(actual, expected.endpoint_id);
  if (endpoint == nullptr || !endpoint->has_input_schema() ||
      !endpoint->has_output_schema() ||
      !schemaMatches(endpoint->input_schema(), expected.input_schema) ||
      !schemaMatches(endpoint->output_schema(), expected.output_schema) ||
      endpoint->interaction_mode() !=
          xgc::adapter::v1::INTERACTION_MODE_OPERATION ||
      endpoint->side_effect_class() !=
          xgc::adapter::v1::SIDE_EFFECT_CLASS_NON_IDEMPOTENT ||
      endpoint->idempotency_mode() !=
          xgc::adapter::v1::IDEMPOTENCY_MODE_REQUIRED ||
      !endpoint->cancellation_supported() || !endpoint->deadline_required()) {
    return reject("capability endpoint differs from compiled contract");
  }
  const auto &limits = endpoint->limits();
  if (endpoint->default_timeout_ms() != expected.default_timeout_ms ||
      endpoint->maximum_timeout_ms() != expected.maximum_timeout_ms ||
      limits.maximum_request_bytes() != expected.limits.maximum_request_bytes ||
      limits.maximum_response_bytes() !=
          expected.limits.maximum_response_bytes ||
      limits.maximum_concurrency() != expected.limits.maximum_concurrency ||
      limits.maximum_streams() != 0 ||
      limits.maximum_stream_chunk_bytes() != 0 ||
      limits.maximum_stream_chunk_messages() != 0) {
    return reject("capability endpoint limits differ from compiled contract");
  }
  return true;
}

bool validateCapabilityGrant(const xgc::adapter::v1::EnabledCapability &grant,
                             const contract::Endpoint &expected,
                             std::string *error) {
  if (grant.capability_id() != expected.capability_id ||
      grant.contract_version() != expected.contract_version ||
      grant.contract_digest() != expected.contract_digest ||
      grant.enabled_endpoint_ids_size() != 1 ||
      grant.enabled_endpoint_ids(0) != expected.endpoint_id) {
    if (error != nullptr) {
      *error = "enabled capability grant differs from compiled endpoint";
    }
    return false;
  }
  return true;
}

void validateInvocationContext(const xgc::adapter::v1::WorkContext &context,
                               const contract::Endpoint &expected) {
  if (context.capability_id() != expected.capability_id ||
      context.contract_version() != expected.contract_version ||
      context.contract_digest() != expected.contract_digest ||
      context.endpoint_id() != expected.endpoint_id) {
    permanentError("invalid_work_context",
                   "work context does not match its capability endpoint");
  }
}

std::chrono::system_clock::time_point
workDeadline(const xgc::adapter::v1::WorkContext &context) {
  return std::chrono::system_clock::time_point(
      std::chrono::nanoseconds(context.deadline().deadline_unix_nanos()));
}

void requireBeforeDeadline(const xgc::adapter::v1::WorkContext &context,
                           const std::string &code,
                           const std::string &message) {
  if (std::chrono::system_clock::now() >= workDeadline(context)) {
    deadlineError(code, message);
  }
}

std::uint64_t
elapsedMilliseconds(std::chrono::steady_clock::time_point started) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started)
          .count());
}

} // namespace

NativeContext NativeContext::FromInstanceSpec(
    const xgc::adapter::v1::AdapterInstanceSpec &spec) {
  if (!spec.has_configuration() ||
      spec.configuration().encoding() != xgc::v1::PAYLOAD_ENCODING_JSON ||
      !spec.configuration().has_schema() ||
      !schemaMatches(spec.configuration().schema(), contract::kConfiguration)) {
    permanentError(
        "invalid_configuration",
        "instance configuration must use the compiled NativeContext schema");
  }
  if (spec.configuration().value().size() > kMaximumConfigurationBytes ||
      spec.secrets_size() != 0) {
    permanentError("invalid_configuration",
                   "instance configuration is too large or contains secrets");
  }
  const Json::Value value =
      parseStrictJson(spec.configuration().value(), "instance configuration");
  requireObject(value, "instance configuration");
  rejectUnknownFields(value, {"domainId", "rmwImplementation"},
                      "instance configuration");
  if (!value.isMember("domainId") || !value["domainId"].isIntegral() ||
      value["domainId"].asInt64() < 0 ||
      value["domainId"].asUInt64() > kMaximumDomainID) {
    permanentError("invalid_configuration",
                   "domainId must be between 0 and 232");
  }
  NativeContext context;
  context.domain_id = value["domainId"].asUInt();
  context.rmw_implementation =
      requiredPossiblyEmptyString(value, "rmwImplementation");
  if ((!context.rmw_implementation.empty() &&
       !std::regex_match(context.rmw_implementation, rmwPattern())) ||
      context.rmw_implementation.size() > 255) {
    permanentError(
        "invalid_configuration",
        "rmwImplementation must be a canonical implementation identifier");
  }
  if (!spec.has_scope() || spec.scope().kind() != "ros2-native-context" ||
      !std::regex_match(spec.scope().key(), scopeKeyPattern())) {
    permanentError("invalid_scope",
                   "instance scope must be a canonical ros2-native-context");
  }
  context.scope_key = spec.scope().key();
  const auto &attributes = spec.scope().attributes();
  const auto domain = attributes.find("domain-id");
  const auto rmw = attributes.find("rmw-implementation");
  const std::string scoped_rmw = rmw == attributes.end() ? "" : rmw->second;
  if (domain == attributes.end() ||
      domain->second != std::to_string(context.domain_id) ||
      scoped_rmw != context.rmw_implementation ||
      attributes.size() !=
          static_cast<std::size_t>(1 + !context.rmw_implementation.empty())) {
    permanentError("invalid_scope",
                   "ROS2 scope attributes do not match instance configuration");
  }
  return context;
}

void NativeContext::ApplyEnvironment() const {
  setEnvironment("ROS_DOMAIN_ID", std::to_string(domain_id));
  setEnvironment("RMW_IMPLEMENTATION", rmw_implementation);
}

bool NativeContext::operator==(const NativeContext &other) const noexcept {
  return domain_id == other.domain_id &&
         rmw_implementation == other.rmw_implementation &&
         scope_key == other.scope_key;
}

ToolsAdapter::ToolsAdapter(rclcpp::Node::SharedPtr node,
                           NativeContext native_context)
    : node_(std::move(node)), native_context_(std::move(native_context)) {
  if (!node_) {
    throw std::invalid_argument("ROS2 node is required");
  }
}

bool ToolsAdapter::ApplyInstanceSpec(
    const xgc::adapter::v1::AdapterInstanceSpec &spec, std::string *error) {
  try {
    if (!(NativeContext::FromInstanceSpec(spec) == native_context_)) {
      if (error != nullptr) {
        *error = "a running ROS2 process cannot change native context";
      }
      return false;
    }
    instance_spec_applied_.store(true, std::memory_order_release);
    return true;
  } catch (const std::exception &exception) {
    if (error != nullptr) {
      *error = exception.what();
    }
    return false;
  }
}

void ToolsAdapter::ClearInstanceSpec() noexcept {
  StopPublishCapability();
  StopServiceCapability();
  instance_spec_applied_.store(false, std::memory_order_release);
}

bool ToolsAdapter::StartPublishCapability(
    const xgc::adapter::v1::AdapterInstanceSpec &spec,
    const xgc::adapter::v1::EnabledCapability &grant, std::string *error) {
  try {
    if (!instance_spec_applied_.load(std::memory_order_acquire) ||
        !(NativeContext::FromInstanceSpec(spec) == native_context_) ||
        !validateCapabilityGrant(grant, contract::kPublish, error)) {
      return false;
    }
    bool expected = false;
    if (!publish_enabled_.compare_exchange_strong(expected, true,
                                                  std::memory_order_acq_rel)) {
      if (error != nullptr) {
        *error = "publish capability is already active";
      }
      return false;
    }
    return true;
  } catch (const std::exception &exception) {
    if (error != nullptr) {
      *error = exception.what();
    }
    return false;
  }
}

void ToolsAdapter::StopPublishCapability() noexcept {
  publish_enabled_.store(false, std::memory_order_release);
}

bool ToolsAdapter::StartServiceCapability(
    const xgc::adapter::v1::AdapterInstanceSpec &spec,
    const xgc::adapter::v1::EnabledCapability &grant, std::string *error) {
  try {
    if (!instance_spec_applied_.load(std::memory_order_acquire) ||
        !(NativeContext::FromInstanceSpec(spec) == native_context_) ||
        !validateCapabilityGrant(grant, contract::kService, error)) {
      return false;
    }
    bool expected = false;
    if (!service_enabled_.compare_exchange_strong(expected, true,
                                                  std::memory_order_acq_rel)) {
      if (error != nullptr) {
        *error = "service capability is already active";
      }
      return false;
    }
    return true;
  } catch (const std::exception &exception) {
    if (error != nullptr) {
      *error = exception.what();
    }
    return false;
  }
}

void ToolsAdapter::StopServiceCapability() noexcept {
  service_enabled_.store(false, std::memory_order_release);
}

Json::Value
ToolsAdapter::parseInput(const xgc::v1::Payload &payload,
                         const xgc::v1::SchemaReference &expected_schema,
                         std::uint32_t maximum_request_bytes) const {
  if (payload.encoding() != xgc::v1::PAYLOAD_ENCODING_JSON ||
      !payload.has_schema() ||
      payload.schema().SerializeAsString() !=
          expected_schema.SerializeAsString()) {
    permanentError("unsupported_input_contract",
                   "capability input must use its compiled JSON schema");
  }
  if (payload.value().size() > maximum_request_bytes) {
    resourceExhaustedError("request_too_large",
                           "capability input exceeds its request limit");
  }
  Json::Value value = parseStrictJson(payload.value(), "capability input");
  requireObject(value, "capability input");
  return value;
}

void ToolsAdapter::validateSubject(
    const xgc::adapter::v1::WorkContext &context) const {
  if (!context.has_subject() ||
      context.subject().kind() != "ros2-native-context" ||
      context.subject().key() != native_context_.scope_key) {
    permanentError("invalid_subject",
                   "invocation subject must match this ROS2 native context");
  }
  const auto &attributes = context.subject().attributes();
  const auto domain = attributes.find("domain-id");
  const auto rmw = attributes.find("rmw-implementation");
  const std::string scoped_rmw = rmw == attributes.end() ? "" : rmw->second;
  if (domain == attributes.end() ||
      domain->second != std::to_string(native_context_.domain_id) ||
      scoped_rmw != native_context_.rmw_implementation ||
      attributes.size() !=
          static_cast<std::size_t>(
              1 + !native_context_.rmw_implementation.empty())) {
    permanentError(
        "invalid_subject",
        "invocation subject does not match this ROS2 native context");
  }
}

xgc2::adapter_runtime::OperationResult ToolsAdapter::Publish(
    const xgc::adapter::v1::OperationRequest &request,
    const xgc2::adapter_runtime::CancellationToken &cancellation) {
  try {
    if (!publish_enabled_.load(std::memory_order_acquire)) {
      rejectedError("publish_capability_disabled",
                    "ROS2 publish capability is not active");
    }
    validateInvocationContext(request.context(), contract::kPublish);
    validateSubject(request.context());
    const Json::Value input = parseInput(
        request.input(), schemaReference(contract::kPublish.input_schema),
        contract::kPublish.limits.maximum_request_bytes);
    rejectUnknownFields(input,
                        {"topic", "messageType", "message", "qosDepth",
                         "reliability", "durability", "waitForSubscribersMs"},
                        "publish request");
    const std::string topic = requiredString(input, "topic");
    const std::string message_type = requiredString(input, "messageType");
    validateGraphName(topic, "topic");
    validateType(message_type, messageTypePattern(), "messageType",
                 "pkg/msg/Type syntax");
    if (!input.isMember("message")) {
      permanentError("invalid_request", "message is required");
    }
    requireObject(input["message"], "message");
    const std::uint32_t depth = requiredUInt(input, "qosDepth", 1, 1000);
    const std::string reliability = requiredString(input, "reliability");
    const std::string durability = requiredString(input, "durability");
    const std::uint32_t wait_ms = requiredUInt(input, "waitForSubscribersMs", 0,
                                               kMaximumWaitMilliseconds);
    if (reliability != "reliable" && reliability != "best-effort") {
      permanentError("invalid_request", "reliability is unsupported");
    }
    if (durability != "volatile" && durability != "transient-local") {
      permanentError("invalid_request", "durability is unsupported");
    }
    if (cancellation.IsCancellationRequested()) {
      cancelledError("publish_cancelled",
                     "publish was cancelled before dispatch");
    }
    requireBeforeDeadline(request.context(), "publish_deadline_elapsed",
                          "publish deadline elapsed before dispatch");

    const MessageType type = codec_.loadMessageType(message_type);
    DynamicMessage message =
        codec_.messageFromJson(type.members, input["message"]);
    rclcpp::SerializedMessage serialized = codec_.serialize(type, message);
    rclcpp::QoS qos{rclcpp::KeepLast(depth)};
    if (reliability == "reliable") {
      qos.reliable();
    } else {
      qos.best_effort();
    }
    if (durability == "volatile") {
      qos.durability_volatile();
    } else {
      qos.transient_local();
    }
    auto publisher = node_->create_generic_publisher(topic, message_type, qos);

    const auto wait_until =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    while (publisher->get_subscription_count() == 0 &&
           std::chrono::steady_clock::now() < wait_until) {
      if (cancellation.IsCancellationRequested()) {
        cancelledError("publish_cancelled",
                       "publish was cancelled while waiting for subscribers");
      }
      requireBeforeDeadline(
          request.context(), "publish_deadline_elapsed",
          "publish deadline elapsed while waiting for subscribers");
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const std::size_t subscriber_count = publisher->get_subscription_count();
    if (wait_ms > 0 && subscriber_count == 0) {
      transientError("subscriber_unavailable",
                     "no ROS2 subscriber matched before the wait timeout");
    }
    if (cancellation.IsCancellationRequested()) {
      cancelledError("publish_cancelled",
                     "publish was cancelled before dispatch");
    }
    requireBeforeDeadline(request.context(), "publish_deadline_elapsed",
                          "publish deadline elapsed before dispatch");
    try {
      publisher->publish(serialized);
    } catch (const std::exception &exception) {
      uncertainError("publish_outcome_unknown",
                     "ROS2 publish outcome is unknown: " +
                         std::string(exception.what()));
    }
    Json::Value output(Json::objectValue);
    output["event"] = "published";
    output["topic"] = topic;
    output["messageType"] = message_type;
    output["serializedBytes"] =
        Json::UInt64(serialized.get_rcl_serialized_message().buffer_length);
    output["subscriberCount"] = Json::UInt64(subscriber_count);
    return success(output, schemaReference(contract::kPublish.output_schema),
                   contract::kPublish.limits.maximum_response_bytes);
  } catch (const std::exception &exception) {
    return failure(exception);
  }
}

xgc2::adapter_runtime::OperationResult ToolsAdapter::CallService(
    const xgc::adapter::v1::OperationRequest &request,
    const xgc2::adapter_runtime::CancellationToken &cancellation) {
  try {
    if (!service_enabled_.load(std::memory_order_acquire)) {
      rejectedError("service_capability_disabled",
                    "ROS2 service capability is not active");
    }
    validateInvocationContext(request.context(), contract::kService);
    validateSubject(request.context());
    const Json::Value input = parseInput(
        request.input(), schemaReference(contract::kService.input_schema),
        contract::kService.limits.maximum_request_bytes);
    rejectUnknownFields(input,
                        {"service", "serviceType", "request",
                         "waitForServiceMs", "callTimeoutMs"},
                        "service request");
    const std::string service = requiredString(input, "service");
    const std::string service_type = requiredString(input, "serviceType");
    validateGraphName(service, "service");
    validateType(service_type, serviceTypePattern(), "serviceType",
                 "pkg/srv/Type syntax");
    if (!input.isMember("request")) {
      permanentError("invalid_request", "request is required");
    }
    requireObject(input["request"], "request");
    const std::uint32_t wait_ms =
        requiredUInt(input, "waitForServiceMs", 0, kMaximumWaitMilliseconds);
    const std::uint32_t call_ms =
        requiredUInt(input, "callTimeoutMs", 1, kMaximumWaitMilliseconds);
    const ServiceType type = codec_.loadServiceType(service_type);
    DynamicMessage service_request = codec_.messageFromJson(
        type.members->request_members_, input["request"]);
    auto client = node_->create_generic_client(service, service_type);
    const auto wait_until =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    while (!client->service_is_ready()) {
      if (cancellation.IsCancellationRequested()) {
        cancelledError("service_call_cancelled",
                       "service call was cancelled before dispatch");
      }
      requireBeforeDeadline(request.context(), "service_wait_deadline_elapsed",
                            "service wait deadline elapsed");
      if (std::chrono::steady_clock::now() >= wait_until) {
        transientError("service_unavailable",
                       "ROS2 service was unavailable before the wait timeout");
      }
      client->wait_for_service(std::chrono::milliseconds(20));
    }
    if (cancellation.IsCancellationRequested()) {
      cancelledError("service_call_cancelled",
                     "service call was cancelled before dispatch");
    }
    requireBeforeDeadline(request.context(), "service_call_deadline_elapsed",
                          "service call deadline elapsed before dispatch");
    const auto started = std::chrono::steady_clock::now();
    auto future = client->async_send_request(service_request.data());
    const auto call_until = started + std::chrono::milliseconds(call_ms);
    while (future.wait_for(std::chrono::milliseconds(20)) !=
           std::future_status::ready) {
      if (cancellation.IsCancellationRequested()) {
        client->remove_pending_request(future.request_id);
        uncertainError("service_outcome_unknown_after_cancel",
                       "service request was dispatched before cancellation");
      }
      if (std::chrono::steady_clock::now() >= call_until ||
          std::chrono::system_clock::now() >= workDeadline(request.context())) {
        client->remove_pending_request(future.request_id);
        uncertainError("service_outcome_unknown_after_timeout",
                       "service request was dispatched before its timeout");
      }
    }
    const auto response = future.get();
    if (!response) {
      uncertainError("service_response_missing",
                     "ROS2 service completed without a response object");
    }
    const Json::Value response_json =
        codec_.messageToJson(type.members->response_members_, response.get());
    Json::Value output(Json::objectValue);
    output["event"] = "response";
    output["service"] = service;
    output["serviceType"] = service_type;
    output["response"] = response_json;
    output["durationMs"] = Json::UInt64(elapsedMilliseconds(started));
    return success(output, schemaReference(contract::kService.output_schema),
                   contract::kService.limits.maximum_response_bytes);
  } catch (const std::exception &exception) {
    return failure(exception);
  }
}

xgc2::adapter_runtime::OperationResult
ToolsAdapter::success(const Json::Value &value,
                      const xgc::v1::SchemaReference &output_schema,
                      std::uint32_t maximum_response_bytes) const {
  xgc::v1::Payload payload;
  *payload.mutable_schema() = output_schema;
  payload.set_encoding(xgc::v1::PAYLOAD_ENCODING_JSON);
  payload.set_value(writeJson(value));
  if (payload.value().size() > maximum_response_bytes) {
    uncertainError("operation_result_too_large",
                   "native side effect committed but its result is too large");
  }
  return xgc2::adapter_runtime::OperationResult::Success(std::move(payload),
                                                         true);
}

xgc2::adapter_runtime::OperationResult
ToolsAdapter::failure(const std::exception &exception) const {
  const auto *typed = dynamic_cast<const ToolsError *>(&exception);
  if (typed == nullptr) {
    return xgc2::adapter_runtime::OperationResult::Failure(
        xgc::adapter::v1::ERROR_CLASS_TRANSIENT, "internal_error",
        exception.what());
  }
  xgc::adapter::v1::ErrorClass error_class =
      xgc::adapter::v1::ERROR_CLASS_PERMANENT;
  if (typed->errorClass() == "transient") {
    error_class = xgc::adapter::v1::ERROR_CLASS_TRANSIENT;
  } else if (typed->errorClass() == "uncertain") {
    error_class = xgc::adapter::v1::ERROR_CLASS_UNCERTAIN;
  } else if (typed->errorClass() == "rejected") {
    error_class = xgc::adapter::v1::ERROR_CLASS_REJECTED;
  } else if (typed->errorClass() == "resource-exhausted") {
    error_class = xgc::adapter::v1::ERROR_CLASS_RESOURCE_EXHAUSTED;
  } else if (typed->errorClass() == "cancelled") {
    error_class = xgc::adapter::v1::ERROR_CLASS_CANCELLED;
  } else if (typed->errorClass() == "deadline") {
    error_class = xgc::adapter::v1::ERROR_CLASS_DEADLINE;
  }
  return xgc2::adapter_runtime::OperationResult::Failure(
      error_class, typed->code(), typed->what());
}

bool BindCapabilities(xgc2::adapter_runtime::ClientConfig *config,
                      ToolsAdapter *adapter, std::string *error) {
  if (config == nullptr || adapter == nullptr ||
      config->capabilities().size() != 2) {
    if (error != nullptr) {
      *error =
          "ROS2 Tools Adapter requires config, application, and two contracts";
    }
    return false;
  }
  bool publish_bound = false;
  bool service_bound = false;
  for (const auto &binding : config->capabilities()) {
    const auto &bootstrap_contract = binding.contract;
    const contract::Endpoint *expected = nullptr;
    bool *bound = nullptr;
    if (bootstrap_contract.capability_id() ==
        contract::kPublish.capability_id) {
      expected = &contract::kPublish;
      bound = &publish_bound;
    } else if (bootstrap_contract.capability_id() ==
               contract::kService.capability_id) {
      expected = &contract::kService;
      bound = &service_bound;
    } else {
      if (error != nullptr) {
        *error = "trusted bootstrap advertises an unknown capability";
      }
      return false;
    }
    if (*bound ||
        !validateExpectedContract(bootstrap_contract, *expected, error)) {
      return false;
    }
    xgc2::adapter_runtime::CapabilityCallbacks callbacks;
    if (expected == &contract::kPublish) {
      callbacks.start = [adapter](const auto &spec, const auto &grant,
                                  std::string *start_error) {
        return adapter->StartPublishCapability(spec, grant, start_error);
      };
      callbacks.stop = [adapter] { adapter->StopPublishCapability(); };
      callbacks.operation = [adapter](const auto &work, const auto &token) {
        return adapter->Publish(work, token);
      };
    } else {
      callbacks.start = [adapter](const auto &spec, const auto &grant,
                                  std::string *start_error) {
        return adapter->StartServiceCapability(spec, grant, start_error);
      };
      callbacks.stop = [adapter] { adapter->StopServiceCapability(); };
      callbacks.operation = [adapter](const auto &work, const auto &token) {
        return adapter->CallService(work, token);
      };
    }
    if (!config->BindCapability(bootstrap_contract.capability_id(),
                                bootstrap_contract.contract_version(),
                                bootstrap_contract.contract_digest(),
                                std::move(callbacks), error)) {
      return false;
    }
    *bound = true;
  }
  return publish_bound && service_bound;
}

} // namespace xgc_ros2_tools_adapter
