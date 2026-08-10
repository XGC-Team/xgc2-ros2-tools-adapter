cmake_minimum_required(VERSION 3.16)
project(xgc_ros2_tools_adapter VERSION 0.1.0 LANGUAGES CXX)

find_package(ament_cmake REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(rclcpp REQUIRED)
find_package(rcpputils REQUIRED)
find_package(rmw REQUIRED)
find_package(rosidl_runtime_cpp REQUIRED)
find_package(rosidl_typesupport_introspection_cpp REQUIRED)
find_package(xgc2_adapter_runtime_client 0.6.0 EXACT REQUIRED CONFIG)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)

set(XGC2_CONTRACT_HEADER
  "${CMAKE_CURRENT_BINARY_DIR}/generated/xgc_ros2_tools_adapter/generated_contract.hpp")
add_custom_command(
  OUTPUT "${XGC2_CONTRACT_HEADER}"
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "${CMAKE_CURRENT_BINARY_DIR}/generated/xgc_ros2_tools_adapter"
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_contract_metadata.py"
          --schema-dir "${CMAKE_CURRENT_SOURCE_DIR}/schemas"
          --output "${XGC2_CONTRACT_HEADER}"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_contract_metadata.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_runtime_manifests.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/schemas/native-context.schema.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/schemas/publish-request.schema.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/schemas/publish-result.schema.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/schemas/service-call-request.schema.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/schemas/service-call-result.schema.json"
  VERBATIM
)
add_custom_target(${PROJECT_NAME}_contract DEPENDS "${XGC2_CONTRACT_HEADER}")

add_library(${PROJECT_NAME}_core STATIC
  "${XGC2_CONTRACT_HEADER}"
  src/json_codec.cpp
  src/tools_adapter.cpp
)
add_dependencies(${PROJECT_NAME}_core ${PROJECT_NAME}_contract)
target_compile_features(${PROJECT_NAME}_core PUBLIC cxx_std_17)
target_compile_options(${PROJECT_NAME}_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_include_directories(${PROJECT_NAME}_core
  PUBLIC
    include
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    ${JSONCPP_INCLUDE_DIRS}
)
ament_target_dependencies(${PROJECT_NAME}_core
  rclcpp rcpputils rmw rosidl_runtime_cpp rosidl_typesupport_introspection_cpp)
target_link_libraries(${PROJECT_NAME}_core
  xgc2::adapter_runtime_client ${JSONCPP_LIBRARIES})

add_executable(${PROJECT_NAME}_node src/main.cpp)
target_compile_features(${PROJECT_NAME}_node PUBLIC cxx_std_17)
target_compile_options(${PROJECT_NAME}_node PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(${PROJECT_NAME}_node ${PROJECT_NAME}_core)
ament_target_dependencies(${PROJECT_NAME}_node rclcpp)

set(XGC2_INSTALLED_ARTIFACT
  "${CMAKE_INSTALL_PREFIX}/lib/${PROJECT_NAME}/${PROJECT_NAME}_node")
set(XGC2_RUNTIME_MANIFEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/runtime-manifests")
set(XGC2_ADAPTER_MANIFEST
  "${XGC2_RUNTIME_MANIFEST_DIR}/adapter-definitions/xgc2-ros2-tools-adapter.json")
set(XGC2_PROCESS_MANIFEST
  "${XGC2_RUNTIME_MANIFEST_DIR}/process-definitions/xgc2-ros2-tools-adapter.json")
add_custom_command(
  OUTPUT "${XGC2_ADAPTER_MANIFEST}" "${XGC2_PROCESS_MANIFEST}"
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_runtime_manifests.py"
          --executable "$<TARGET_FILE:${PROJECT_NAME}_node>"
          --ros-package "${PROJECT_NAME}"
          --ros-executable "${PROJECT_NAME}_node"
          --schema-dir "${CMAKE_CURRENT_SOURCE_DIR}/schemas"
          --version "${PROJECT_VERSION}"
          --adapter-output "${XGC2_ADAPTER_MANIFEST}"
          --process-output "${XGC2_PROCESS_MANIFEST}"
  DEPENDS ${PROJECT_NAME}_node
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_runtime_manifests.py"
  VERBATIM
)
add_custom_target(${PROJECT_NAME}_runtime_manifests ALL
  DEPENDS "${XGC2_ADAPTER_MANIFEST}" "${XGC2_PROCESS_MANIFEST}")

install(TARGETS ${PROJECT_NAME}_node
  RUNTIME DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY schemas/ DESTINATION share/${PROJECT_NAME}/schemas)
configure_file(cmake/install_runtime_manifests.cmake.in
               "${CMAKE_CURRENT_BINARY_DIR}/install_runtime_manifests.cmake" @ONLY)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/install_runtime_manifests.cmake")

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  find_package(rcl_interfaces REQUIRED)
  find_package(std_msgs REQUIRED)
  find_package(std_srvs REQUIRED)
  ament_add_gtest(${PROJECT_NAME}_tests
    test/json_codec_test.cpp
    test/tools_adapter_test.cpp
  )
  if(TARGET ${PROJECT_NAME}_tests)
    target_compile_features(${PROJECT_NAME}_tests PUBLIC cxx_std_17)
    target_compile_options(${PROJECT_NAME}_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(${PROJECT_NAME}_tests ${PROJECT_NAME}_core)
    ament_target_dependencies(${PROJECT_NAME}_tests rcl_interfaces std_msgs std_srvs)
  endif()
  add_test(NAME ${PROJECT_NAME}_manifest_tests
    COMMAND "${Python3_EXECUTABLE}" -m unittest discover -v
            -s "${CMAKE_CURRENT_SOURCE_DIR}/test" -p "test_*.py")
endif()

ament_package()
