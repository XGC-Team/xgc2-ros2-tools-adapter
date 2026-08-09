#include "xgc_ros2_tools_adapter/json_codec.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <utility>

#include "rclcpp/serialization.hpp"
#include "rclcpp/typesupport_helpers.hpp"
#include "rosidl_runtime_cpp/message_initialization.hpp"
#include "rosidl_typesupport_introspection_cpp/field_types.hpp"
#include "xgc_ros2_tools_adapter/error.hpp"

namespace xgc_ros2_tools_adapter {
namespace {

using rosidl_typesupport_introspection_cpp::MessageMember;
using rosidl_typesupport_introspection_cpp::MessageMembers;

const MessageMembers* nestedMembers(const MessageMember& member) {
  if (member.members_ == nullptr || member.members_->data == nullptr) {
    permanentError("ros_type_support_invalid",
                   "nested ROS2 message introspection is unavailable");
  }
  return static_cast<const MessageMembers*>(member.members_->data);
}

void requireObject(const Json::Value& value, const std::string& path) {
  if (!value.isObject()) {
    permanentError("invalid_message", path + " must be a JSON object");
  }
}

void rejectUnknownFields(const Json::Value& value, const MessageMembers* members,
                         const std::string& path) {
  for (const auto& name : value.getMemberNames()) {
    bool found = false;
    for (std::uint32_t index = 0; index < members->member_count_; ++index) {
      if (name == members->members_[index].name_) {
        found = true;
        break;
      }
    }
    if (!found) {
      permanentError("unknown_message_field",
                     path + " contains unknown field '" + name + "'");
    }
  }
}

template <typename T>
void assignSigned(const Json::Value& value, void* output,
                  const std::string& path) {
  if (!value.isIntegral()) {
    permanentError("invalid_message_field", path + " must be an integer");
  }
  const Json::Int64 parsed = value.asInt64();
  if (parsed < static_cast<Json::Int64>(std::numeric_limits<T>::min()) ||
      parsed > static_cast<Json::Int64>(std::numeric_limits<T>::max())) {
    permanentError("message_field_out_of_range", path + " is out of range");
  }
  *static_cast<T*>(output) = static_cast<T>(parsed);
}

template <typename T>
void assignUnsigned(const Json::Value& value, void* output,
                    const std::string& path) {
  if (!value.isIntegral() || (value.isInt64() && value.asInt64() < 0)) {
    permanentError("invalid_message_field",
                   path + " must be an unsigned integer");
  }
  const Json::UInt64 parsed = value.asUInt64();
  if (parsed > static_cast<Json::UInt64>(std::numeric_limits<T>::max())) {
    permanentError("message_field_out_of_range", path + " is out of range");
  }
  *static_cast<T*>(output) = static_cast<T>(parsed);
}

template <typename T>
void assignFloating(const Json::Value& value, void* output,
                    const std::string& path) {
  if (!value.isNumeric()) {
    permanentError("invalid_message_field", path + " must be numeric");
  }
  const double parsed = value.asDouble();
  if (!std::isfinite(parsed) ||
      std::fabs(parsed) > static_cast<double>(std::numeric_limits<T>::max())) {
    permanentError("message_field_out_of_range", path + " is out of range");
  }
  *static_cast<T*>(output) = static_cast<T>(parsed);
}

void assignScalar(const MessageMember& member, const Json::Value& value,
                  void* output, const std::string& path);

void assignMember(const MessageMember& member, const Json::Value& value,
                  void* message, const std::string& path) {
  void* field = static_cast<unsigned char*>(message) + member.offset_;
  if (!member.is_array_) {
    assignScalar(member, value, field, path);
    return;
  }
  if (!value.isArray()) {
    permanentError("invalid_message_field", path + " must be an array");
  }
  const std::size_t requested = value.size();
  if (member.array_size_ != 0 && !member.is_upper_bound_ &&
      requested != member.array_size_) {
    permanentError("invalid_message_field",
                   path + " must contain exactly " +
                       std::to_string(member.array_size_) + " items");
  }
  if (member.is_upper_bound_ && member.array_size_ != 0 &&
      requested > member.array_size_) {
    permanentError("invalid_message_field",
                   path + " exceeds its bounded sequence size");
  }
  if ((member.array_size_ == 0 || member.is_upper_bound_) &&
      member.resize_function != nullptr) {
    member.resize_function(field, requested);
  }
  if (member.size_function == nullptr || member.get_function == nullptr ||
      member.size_function(field) != requested) {
    permanentError("ros_type_support_invalid",
                   path + " sequence introspection is incomplete");
  }
  for (std::size_t index = 0; index < requested; ++index) {
    assignScalar(member, value[static_cast<Json::ArrayIndex>(index)],
                 member.get_function(field, index),
                 path + "[" + std::to_string(index) + "]");
  }
}

void assignScalar(const MessageMember& member, const Json::Value& value,
                  void* output, const std::string& path) {
  using namespace rosidl_typesupport_introspection_cpp;
  switch (member.type_id_) {
    case ROS_TYPE_BOOL:
      if (!value.isBool()) {
        permanentError("invalid_message_field", path + " must be boolean");
      }
      *static_cast<bool*>(output) = value.asBool();
      return;
    case ROS_TYPE_FLOAT:
      assignFloating<float>(value, output, path);
      return;
    case ROS_TYPE_DOUBLE:
    case ROS_TYPE_LONG_DOUBLE:
      assignFloating<double>(value, output, path);
      return;
    case ROS_TYPE_CHAR:
    case ROS_TYPE_OCTET:
    case ROS_TYPE_UINT8:
      assignUnsigned<std::uint8_t>(value, output, path);
      return;
    case ROS_TYPE_INT8:
      assignSigned<std::int8_t>(value, output, path);
      return;
    case ROS_TYPE_UINT16:
      assignUnsigned<std::uint16_t>(value, output, path);
      return;
    case ROS_TYPE_INT16:
      assignSigned<std::int16_t>(value, output, path);
      return;
    case ROS_TYPE_UINT32:
      assignUnsigned<std::uint32_t>(value, output, path);
      return;
    case ROS_TYPE_INT32:
      assignSigned<std::int32_t>(value, output, path);
      return;
    case ROS_TYPE_UINT64:
      assignUnsigned<std::uint64_t>(value, output, path);
      return;
    case ROS_TYPE_INT64:
      assignSigned<std::int64_t>(value, output, path);
      return;
    case ROS_TYPE_STRING: {
      if (!value.isString()) {
        permanentError("invalid_message_field", path + " must be a string");
      }
      const std::string parsed = value.asString();
      if (member.string_upper_bound_ != 0 &&
          parsed.size() > member.string_upper_bound_) {
        permanentError("message_field_out_of_range",
                       path + " exceeds its string bound");
      }
      *static_cast<std::string*>(output) = parsed;
      return;
    }
    case ROS_TYPE_WSTRING: {
      if (!value.isString()) {
        permanentError("invalid_message_field", path + " must be a string");
      }
      std::u16string parsed;
      for (unsigned char character : value.asString()) {
        if (character > 0x7f) {
          permanentError("unsupported_message_field",
                         path + " contains a non-ASCII wide string");
        }
        parsed.push_back(static_cast<char16_t>(character));
      }
      if (member.string_upper_bound_ != 0 &&
          parsed.size() > member.string_upper_bound_) {
        permanentError("message_field_out_of_range",
                       path + " exceeds its wide-string bound");
      }
      *static_cast<std::u16string*>(output) = std::move(parsed);
      return;
    }
    case ROS_TYPE_MESSAGE: {
      const MessageMembers* nested = nestedMembers(member);
      requireObject(value, path);
      rejectUnknownFields(value, nested, path);
      for (std::uint32_t index = 0; index < nested->member_count_; ++index) {
        const MessageMember& child = nested->members_[index];
        if (value.isMember(child.name_)) {
          assignMember(child, value[child.name_], output,
                       path + "." + child.name_);
        }
      }
      return;
    }
    default:
      permanentError("unsupported_message_field",
                     path + " uses an unsupported ROS2 field type");
  }
}

Json::Value scalarToJson(const MessageMember& member, const void* input,
                         const std::string& path);

Json::Value memberToJson(const MessageMember& member, const void* message,
                         const std::string& path) {
  const void* field = static_cast<const unsigned char*>(message) + member.offset_;
  if (!member.is_array_) {
    return scalarToJson(member, field, path);
  }
  if (member.size_function == nullptr || member.get_const_function == nullptr) {
    permanentError("ros_type_support_invalid",
                   path + " sequence introspection is incomplete");
  }
  Json::Value result(Json::arrayValue);
  const std::size_t size = member.size_function(field);
  for (std::size_t index = 0; index < size; ++index) {
    result.append(scalarToJson(member, member.get_const_function(field, index),
                               path + "[" + std::to_string(index) + "]"));
  }
  return result;
}

Json::Value scalarToJson(const MessageMember& member, const void* input,
                         const std::string& path) {
  using namespace rosidl_typesupport_introspection_cpp;
  switch (member.type_id_) {
    case ROS_TYPE_BOOL:
      return Json::Value(*static_cast<const bool*>(input));
    case ROS_TYPE_FLOAT:
      return Json::Value(*static_cast<const float*>(input));
    case ROS_TYPE_DOUBLE:
    case ROS_TYPE_LONG_DOUBLE:
      return Json::Value(*static_cast<const double*>(input));
    case ROS_TYPE_CHAR:
    case ROS_TYPE_OCTET:
    case ROS_TYPE_UINT8:
      return Json::Value(static_cast<Json::UInt>(*static_cast<const std::uint8_t*>(input)));
    case ROS_TYPE_INT8:
      return Json::Value(static_cast<Json::Int>(*static_cast<const std::int8_t*>(input)));
    case ROS_TYPE_UINT16:
      return Json::Value(static_cast<Json::UInt>(*static_cast<const std::uint16_t*>(input)));
    case ROS_TYPE_INT16:
      return Json::Value(static_cast<Json::Int>(*static_cast<const std::int16_t*>(input)));
    case ROS_TYPE_UINT32:
      return Json::Value(static_cast<Json::UInt>(*static_cast<const std::uint32_t*>(input)));
    case ROS_TYPE_INT32:
      return Json::Value(static_cast<Json::Int>(*static_cast<const std::int32_t*>(input)));
    case ROS_TYPE_UINT64:
      return Json::Value(static_cast<Json::UInt64>(*static_cast<const std::uint64_t*>(input)));
    case ROS_TYPE_INT64:
      return Json::Value(static_cast<Json::Int64>(*static_cast<const std::int64_t*>(input)));
    case ROS_TYPE_STRING:
      return Json::Value(*static_cast<const std::string*>(input));
    case ROS_TYPE_WSTRING: {
      std::string value;
      for (char16_t character : *static_cast<const std::u16string*>(input)) {
        if (character > 0x7f) {
          permanentError("unsupported_message_field",
                         path + " contains a non-ASCII wide string");
        }
        value.push_back(static_cast<char>(character));
      }
      return Json::Value(value);
    }
    case ROS_TYPE_MESSAGE: {
      const MessageMembers* nested = nestedMembers(member);
      Json::Value result(Json::objectValue);
      for (std::uint32_t index = 0; index < nested->member_count_; ++index) {
        const MessageMember& child = nested->members_[index];
        result[child.name_] = memberToJson(child, input, path + "." + child.name_);
      }
      return result;
    }
    default:
      permanentError("unsupported_message_field",
                     path + " uses an unsupported ROS2 field type");
  }
}

}  // namespace

DynamicMessage::DynamicMessage(const MessageMembers* members) : members_(members) {
  if (members_ == nullptr || members_->init_function == nullptr ||
      members_->fini_function == nullptr || members_->size_of_ == 0) {
    permanentError("ros_type_support_invalid",
                   "ROS2 message introspection is incomplete");
  }
  data_ = ::operator new(members_->size_of_);
  try {
    members_->init_function(data_, rosidl_runtime_cpp::MessageInitialization::ALL);
  } catch (...) {
    ::operator delete(data_);
    data_ = nullptr;
    throw;
  }
}

DynamicMessage::~DynamicMessage() { reset(); }

DynamicMessage::DynamicMessage(DynamicMessage&& other) noexcept
    : members_(other.members_), data_(other.data_) {
  other.members_ = nullptr;
  other.data_ = nullptr;
}

DynamicMessage& DynamicMessage::operator=(DynamicMessage&& other) noexcept {
  if (this != &other) {
    reset();
    members_ = other.members_;
    data_ = other.data_;
    other.members_ = nullptr;
    other.data_ = nullptr;
  }
  return *this;
}

void DynamicMessage::reset() noexcept {
  if (data_ != nullptr && members_ != nullptr) {
    members_->fini_function(data_);
    ::operator delete(data_);
  }
  data_ = nullptr;
  members_ = nullptr;
}

MessageType JsonCodec::loadMessageType(const std::string& type_name) const {
  MessageType result;
  result.introspection_library = rclcpp::get_typesupport_library(
      type_name, "rosidl_typesupport_introspection_cpp");
  result.introspection_support = rclcpp::get_message_typesupport_handle(
      type_name, "rosidl_typesupport_introspection_cpp",
      *result.introspection_library);
  result.serialization_library =
      rclcpp::get_typesupport_library(type_name, "rosidl_typesupport_cpp");
  result.serialization_support = rclcpp::get_message_typesupport_handle(
      type_name, "rosidl_typesupport_cpp", *result.serialization_library);
  if (result.introspection_support == nullptr ||
      result.introspection_support->data == nullptr ||
      result.serialization_support == nullptr) {
    permanentError("ros_type_support_unavailable",
                   "ROS2 message type support is unavailable for " + type_name);
  }
  result.members = static_cast<const MessageMembers*>(
      result.introspection_support->data);
  return result;
}

ServiceType JsonCodec::loadServiceType(const std::string& type_name) const {
  ServiceType result;
  result.introspection_library = rclcpp::get_typesupport_library(
      type_name, "rosidl_typesupport_introspection_cpp");
  result.introspection_support = rclcpp::get_service_typesupport_handle(
      type_name, "rosidl_typesupport_introspection_cpp",
      *result.introspection_library);
  if (result.introspection_support == nullptr ||
      result.introspection_support->data == nullptr) {
    permanentError("ros_type_support_unavailable",
                   "ROS2 service type support is unavailable for " + type_name);
  }
  result.members = static_cast<
      const rosidl_typesupport_introspection_cpp::ServiceMembers*>(
      result.introspection_support->data);
  if (result.members->request_members_ == nullptr ||
      result.members->response_members_ == nullptr) {
    permanentError("ros_type_support_invalid",
                   "ROS2 service introspection is incomplete for " + type_name);
  }
  return result;
}

DynamicMessage JsonCodec::messageFromJson(const MessageMembers* members,
                                          const Json::Value& value) const {
  requireObject(value, "message");
  rejectUnknownFields(value, members, "message");
  DynamicMessage result(members);
  for (std::uint32_t index = 0; index < members->member_count_; ++index) {
    const MessageMember& member = members->members_[index];
    if (value.isMember(member.name_)) {
      assignMember(member, value[member.name_], result.data(),
                   std::string("message.") + member.name_);
    }
  }
  return result;
}

Json::Value JsonCodec::messageToJson(const MessageMembers* members,
                                    const void* message) const {
  Json::Value result(Json::objectValue);
  for (std::uint32_t index = 0; index < members->member_count_; ++index) {
    const MessageMember& member = members->members_[index];
    result[member.name_] = memberToJson(
        member, message, std::string("message.") + member.name_);
  }
  return result;
}

rclcpp::SerializedMessage JsonCodec::serialize(
    const MessageType& type, const DynamicMessage& message) const {
  if (type.serialization_support == nullptr || message.data() == nullptr) {
    permanentError("ros_type_support_invalid",
                   "ROS2 serialization type support is unavailable");
  }
  rclcpp::SerializedMessage output;
  rclcpp::SerializationBase serializer(type.serialization_support);
  serializer.serialize_message(message.data(), &output);
  return output;
}

Json::Value parseStrictJson(const std::string& input,
                            const std::string& description) {
  Json::CharReaderBuilder builder;
  builder["collectComments"] = false;
  builder["allowComments"] = false;
  builder["allowTrailingCommas"] = false;
  builder["strictRoot"] = true;
  builder["rejectDupKeys"] = true;
  builder["stackLimit"] = 128;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value value;
  std::string errors;
  if (!reader->parse(input.data(), input.data() + input.size(), &value,
                     &errors)) {
    permanentError("invalid_json", description + " is not strict JSON: " + errors);
  }
  return value;
}

std::string writeJson(const Json::Value& value) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  builder["commentStyle"] = "None";
  return Json::writeString(builder, value);
}

}  // namespace xgc_ros2_tools_adapter
