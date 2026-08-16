#!/usr/bin/env bash
set -euo pipefail

test -r /opt/ros/jazzy/setup.bash || {
  echo "XGC2 build image is missing the ROS Jazzy setup" >&2
  exit 1
}
set +u
# shellcheck disable=SC1091
source /opt/ros/jazzy/setup.bash
set -u

for command in \
  clang-format-18 cmake ctest dpkg-deb dpkg-query fakeroot file \
  python3 rg ros2 rsync shellcheck; do
  command -v "$command" >/dev/null 2>&1 || {
    echo "XGC2 build image is missing required command: $command" >&2
    exit 1
  }
done

dpkg-query -W \
  clang-format build-essential cmake dpkg-dev fakeroot file libjsoncpp-dev \
  pkg-config python3 python3-yaml ripgrep rsync shellcheck \
  ros-jazzy-ament-cmake ros-jazzy-ament-cmake-gtest \
  ros-jazzy-rcl-interfaces ros-jazzy-rclcpp ros-jazzy-rcpputils \
  ros-jazzy-rmw ros-jazzy-rosidl-runtime-cpp \
  ros-jazzy-rosidl-typesupport-introspection-cpp \
  ros-jazzy-std-msgs ros-jazzy-std-srvs >/dev/null
