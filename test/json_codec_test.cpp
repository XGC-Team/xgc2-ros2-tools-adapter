#include "xgc_ros2_tools_adapter/json_codec.hpp"

#include <gtest/gtest.h>
#include <json/json.h>

namespace xgc_ros2_tools_adapter {
namespace {

TEST(JsonCodec, EncodesInstalledMessageTypeWithoutCompileTimeBinding) {
  JsonCodec codec;
  const MessageType type = codec.loadMessageType("std_msgs/msg/String");
  Json::Value input(Json::objectValue);
  input["data"] = "adapter-runtime";
  DynamicMessage message = codec.messageFromJson(type.members, input);
  const Json::Value projected =
      codec.messageToJson(type.members, message.data());
  EXPECT_EQ("adapter-runtime", projected["data"].asString());
  const auto serialized = codec.serialize(type, message);
  EXPECT_GT(serialized.get_rcl_serialized_message().buffer_length, 0u);
}

TEST(JsonCodec, EncodesInstalledServiceRequestAndRejectsUnknownFields) {
  JsonCodec codec;
  const ServiceType type = codec.loadServiceType("std_srvs/srv/SetBool");
  Json::Value input(Json::objectValue);
  input["data"] = true;
  DynamicMessage request =
      codec.messageFromJson(type.members->request_members_, input);
  EXPECT_TRUE(
      codec
          .messageToJson(type.members->request_members_, request.data())["data"]
          .asBool());

  input["shell"] = "ros2 service call";
  EXPECT_THROW(codec.messageFromJson(type.members->request_members_, input),
               std::exception);
}

TEST(JsonCodec, RoundTripsBooleanSequencesThroughFetchAssignIntrospection) {
  JsonCodec codec;
  const MessageType type =
      codec.loadMessageType("rcl_interfaces/msg/ParameterValue");
  Json::Value input(Json::objectValue);
  input["bool_array_value"] = Json::Value(Json::arrayValue);
  input["bool_array_value"].append(true);
  input["bool_array_value"].append(false);
  DynamicMessage message = codec.messageFromJson(type.members, input);
  const Json::Value projected =
      codec.messageToJson(type.members, message.data());
  ASSERT_EQ(2u, projected["bool_array_value"].size());
  EXPECT_TRUE(projected["bool_array_value"][0].asBool());
  EXPECT_FALSE(projected["bool_array_value"][1].asBool());
}

}  // namespace
}  // namespace xgc_ros2_tools_adapter
