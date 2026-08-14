#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <rclcpp/rclcpp.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "xgc2/adapter_runtime/client.hpp"
#include "xgc_ros2_tools_adapter/tools_adapter.hpp"

namespace {

std::atomic<bool> stop_requested{false};
std::atomic<bool> session_lost{false};

void handleSignal(int) {
  stop_requested.store(true, std::memory_order_release);
}

std::string parseBootstrapPath(int argc, char **argv) {
  if (argc == 2 &&
      (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
    std::cout << "Usage: xgc_ros2_tools_adapter_node "
                 "--adapter-bootstrap-file PATH\n";
    std::exit(0);
  }
  if (argc == 3 && std::string(argv[1]) == "--adapter-bootstrap-file" &&
      argv[2][0] != '\0') {
    return argv[2];
  }
  throw std::invalid_argument(
      "exactly --adapter-bootstrap-file PATH is required");
}

void logRuntime(xgc2::adapter_runtime::LogLevel level,
                const std::string &message) {
  const auto logger = rclcpp::get_logger("xgc_ros2_tools_adapter.runtime");
  switch (level) {
  case xgc2::adapter_runtime::LogLevel::kDebug:
    RCLCPP_DEBUG(logger, "%s", message.c_str());
    break;
  case xgc2::adapter_runtime::LogLevel::kInfo:
    RCLCPP_INFO(logger, "%s", message.c_str());
    break;
  case xgc2::adapter_runtime::LogLevel::kWarning:
    RCLCPP_WARN(logger, "%s", message.c_str());
    break;
  case xgc2::adapter_runtime::LogLevel::kError:
    RCLCPP_ERROR(logger, "%s", message.c_str());
    break;
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    const std::string bootstrap_path = parseBootstrapPath(argc, argv);
    auto runtime_config =
        xgc2::adapter_runtime::ClientConfig::FromBootstrapFile(bootstrap_path);
    const auto native_context =
        xgc_ros2_tools_adapter::NativeContext::FromInstanceSpec(
            runtime_config.initial_spec());
    native_context.ApplyEnvironment();

    rclcpp::InitOptions init_options;
    init_options.set_domain_id(native_context.domain_id);
    rclcpp::init(0, nullptr, init_options, rclcpp::SignalHandlerOptions::None);
    auto node = std::make_shared<rclcpp::Node>("xgc_ros2_tools_adapter");
    auto executor = std::make_shared<rclcpp::executors::MultiThreadedExecutor>(
        rclcpp::ExecutorOptions(), 4);
    executor->add_node(node);

    xgc_ros2_tools_adapter::ToolsAdapter adapter(node, native_context);
    std::string error;
    if (!xgc_ros2_tools_adapter::BindCapabilities(&runtime_config, &adapter,
                                                  &error)) {
      throw std::runtime_error(error);
    }
    runtime_config.dispatch_workers = 16;

    xgc2::adapter_runtime::ClientCallbacks callbacks;
    callbacks.apply_instance_spec = [&adapter](const auto &spec,
                                               std::string *apply_error) {
      return adapter.ApplyInstanceSpec(spec, apply_error);
    };
    callbacks.clear_instance_spec = [&adapter] { adapter.ClearInstanceSpec(); };
    callbacks.stop_requested = [](const auto &request) {
      RCLCPP_INFO(rclcpp::get_logger("xgc_ros2_tools_adapter"),
                  "Adapter Runtime requested process stop: %s",
                  request.reason().c_str());
      stop_requested.store(true, std::memory_order_release);
    };
    callbacks.session_lost = [](const std::string &reason) {
      RCLCPP_ERROR(rclcpp::get_logger("xgc_ros2_tools_adapter"),
                   "Adapter Runtime session was lost: %s", reason.c_str());
      session_lost.store(true, std::memory_order_release);
      stop_requested.store(true, std::memory_order_release);
    };
    callbacks.log = logRuntime;

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    xgc2::adapter_runtime::Client runtime(std::move(runtime_config),
                                          std::move(callbacks));
    if (!runtime.Start(&error)) {
      throw std::runtime_error("Adapter Runtime startup failed: " + error);
    }
    // Start ROS dispatch only after the trusted bootstrap and Runtime session
    // are accepted. Bootstrap failures therefore cannot leave a joinable
    // executor thread during stack unwinding.
    std::thread executor_thread([executor] { executor->spin(); });
    RCLCPP_INFO(node->get_logger(), "XGC2 ROS2 Tools Adapter is ready");
    while (rclcpp::ok() && !stop_requested.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    runtime.Stop();
    executor->cancel();
    if (executor_thread.joinable()) {
      executor_thread.join();
    }
    executor->remove_node(node);
    rclcpp::shutdown();
    return session_lost.load(std::memory_order_acquire) ? 2 : 0;
  } catch (const std::exception &exception) {
    std::cerr << "xgc_ros2_tools_adapter: " << exception.what() << '\n';
    if (rclcpp::ok()) {
      rclcpp::shutdown();
    }
    return 1;
  }
}
