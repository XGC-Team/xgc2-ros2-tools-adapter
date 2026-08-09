#pragma once

#include <json/json.h>

#include <memory>
#include <string>

#include "rclcpp/serialized_message.hpp"
#include "rcpputils/shared_library.hpp"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_runtime_c/service_type_support_struct.h"
#include "rosidl_typesupport_introspection_cpp/message_introspection.hpp"
#include "rosidl_typesupport_introspection_cpp/service_introspection.hpp"

namespace xgc_ros2_tools_adapter {

struct MessageType {
  std::shared_ptr<rcpputils::SharedLibrary> introspection_library;
  std::shared_ptr<rcpputils::SharedLibrary> serialization_library;
  const rosidl_message_type_support_t* introspection_support = nullptr;
  const rosidl_message_type_support_t* serialization_support = nullptr;
  const rosidl_typesupport_introspection_cpp::MessageMembers* members = nullptr;
};

struct ServiceType {
  std::shared_ptr<rcpputils::SharedLibrary> introspection_library;
  const rosidl_service_type_support_t* introspection_support = nullptr;
  const rosidl_typesupport_introspection_cpp::ServiceMembers* members = nullptr;
};

class DynamicMessage {
 public:
  explicit DynamicMessage(
      const rosidl_typesupport_introspection_cpp::MessageMembers* members);
  ~DynamicMessage();
  DynamicMessage(const DynamicMessage&) = delete;
  DynamicMessage& operator=(const DynamicMessage&) = delete;
  DynamicMessage(DynamicMessage&& other) noexcept;
  DynamicMessage& operator=(DynamicMessage&& other) noexcept;

  void* data() noexcept { return data_; }
  const void* data() const noexcept { return data_; }
  const rosidl_typesupport_introspection_cpp::MessageMembers* members() const {
    return members_;
  }

 private:
  void reset() noexcept;
  const rosidl_typesupport_introspection_cpp::MessageMembers* members_ = nullptr;
  void* data_ = nullptr;
};

class JsonCodec {
 public:
  MessageType loadMessageType(const std::string& type_name) const;
  ServiceType loadServiceType(const std::string& type_name) const;
  DynamicMessage messageFromJson(
      const rosidl_typesupport_introspection_cpp::MessageMembers* members,
      const Json::Value& value) const;
  Json::Value messageToJson(
      const rosidl_typesupport_introspection_cpp::MessageMembers* members,
      const void* message) const;
  rclcpp::SerializedMessage serialize(const MessageType& type,
                                      const DynamicMessage& message) const;
};

Json::Value parseStrictJson(const std::string& input,
                            const std::string& description);
std::string writeJson(const Json::Value& value);

}  // namespace xgc_ros2_tools_adapter
