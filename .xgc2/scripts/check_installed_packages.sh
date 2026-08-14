#!/usr/bin/env bash
set -euo pipefail

ROS_DISTRO="${XGC2_ROS_DISTRO:-jazzy}"
[[ "$ROS_DISTRO" == jazzy ]] || {
  echo "unsupported ROS distribution: $ROS_DISTRO" >&2
  exit 2
}
PACKAGE="ros-${ROS_DISTRO}-xgc2-ros2-tools-adapter"
ROS_PACKAGE="xgc_ros2_tools_adapter"
EXPECTED_VERSION="${EXPECTED_VERSION:-0.1.0-1}"
PREFIX="/opt/ros/${ROS_DISTRO}"
EXECUTABLE="${PREFIX}/lib/${ROS_PACKAGE}/${ROS_PACKAGE}_node"
ADAPTER_MANIFEST="/usr/share/xgc2/adapter-definitions/xgc2-ros2-tools-adapter.json"
PROCESS_MANIFEST="/usr/share/xgc2/process-definitions/xgc2-ros2-tools-adapter.json"

test "$(dpkg-query -W -f='${Version}' "$PACKAGE")" = "$EXPECTED_VERSION"
test -x "$EXECUTABLE"
test -f "${PREFIX}/share/${ROS_PACKAGE}/package.xml"
test -f "$ADAPTER_MANIFEST"
test -f "$PROCESS_MANIFEST"
file -b "$EXECUTABLE" | grep -q '^ELF'
if ! ldd "$EXECUTABLE" | awk '/not found/ {missing=1} END {exit missing ? 1 : 0}'; then
  echo "installed Adapter has an unresolved shared library" >&2
  ldd "$EXECUTABLE" >&2 || true
  exit 1
fi
runtime_libraries="$(ldd "$EXECUTABLE")"
grep -Eq 'libxgc2_adapter_runtime_client[.]so[.]2 => /' <<<"$runtime_libraries"
grep -Eq 'libxgc2_adapter_runtime_protocol[.]so[.]2 => /' <<<"$runtime_libraries"
depends="$(dpkg-query -W -f='${Depends}' "$PACKAGE")"
for dependency in \
  libjsoncpp25 \
  libxgc2-adapter-runtime-client2 \
  ros-jazzy-rclcpp \
  ros-jazzy-rcpputils \
  ros-jazzy-rmw \
  ros-jazzy-rosidl-runtime-cpp \
  ros-jazzy-rosidl-typesupport-introspection-cpp; do
  grep -Eq "(^|, )[[:space:]]*${dependency}([[:space:](,]|$)" <<<"$depends" || {
    echo "missing declared runtime dependency: $dependency" >&2
    exit 1
  }
done
if grep -Eq '(^|, )(libxgc2-adapter-runtime-client-dev|xgc2-protobuf-dev)([[:space:](,]|$)' \
  <<<"$depends"; then
  echo "installed package leaks a build-only dependency" >&2
  exit 1
fi

set +u
# shellcheck disable=SC1090,SC1091
source "${PREFIX}/setup.bash"
set -u
test "$(ros2 pkg prefix "$ROS_PACKAGE")" = "$PREFIX"
"$EXECUTABLE" --help | grep -q -- '--adapter-bootstrap-file PATH'
removed_socket_flag="--sock""et"
if "$EXECUTABLE" "$removed_socket_flag" /tmp/removed.sock >/dev/null 2>&1; then
  echo "Adapter accepted a removed private socket argument" >&2
  exit 1
fi

for workspace_file in \
  setup.bash setup.ps1 setup.sh setup.zsh \
  local_setup.bash local_setup.ps1 local_setup.sh local_setup.zsh \
  _local_setup_util_ps1.py _local_setup_util_sh.py \
  .colcon_install_layout COLCON_IGNORE; do
  if dpkg-query -L "$PACKAGE" | grep -Fqx "${PREFIX}/${workspace_file}"; then
    echo "$PACKAGE must not own ROS workspace file ${workspace_file}" >&2
    exit 1
  fi
done

python3 - "$EXECUTABLE" "$ADAPTER_MANIFEST" "$PROCESS_MANIFEST" <<'PY'
import hashlib
import json
import sys
from pathlib import Path

executable, adapter_path, process_path = map(Path, sys.argv[1:])
adapter = json.loads(adapter_path.read_text(encoding="utf-8"))
process = json.loads(process_path.read_text(encoding="utf-8"))
installed = adapter["adapters"][0]
definition = installed["definition"]

def require(condition, message):
    if not condition:
        raise SystemExit(message)

require(adapter["apiVersion"] == "xgc.adapter.definition/v1", "Adapter API mismatch")
require(definition["id"] == "xgc2-ros2-tools-adapter", "Adapter id mismatch")
require(definition["version"] == "0.1.0", "Adapter version mismatch")
require(
    definition["buildDigest"]
    == "sha256:" + hashlib.sha256(executable.read_bytes()).hexdigest(),
    "Adapter build digest mismatch",
)
capabilities = installed["capabilityManifest"]["capabilities"]
require(
    {(item["ref"]["id"], item["endpoints"][0]["endpointId"]) for item in capabilities}
    == {
        ("xgc.ros2.topic.publish", "publish"),
        ("xgc.ros2.service.call", "call"),
    },
    "Adapter capability set mismatch",
)
entry = process["definitions"][0]
require(process["apiVersion"] == "xgc.execution.process/v1", "process API mismatch")
require(entry["id"] == "xgc2-ros2-tools-adapter", "process id mismatch")
require(entry["internal"] is True, "process must remain internal")
require(
    entry["command"]
    == {
        "executable": "ros2",
        "args": [
            "run",
            "xgc_ros2_tools_adapter",
            "xgc_ros2_tools_adapter_node",
            "--adapter-bootstrap-file",
            "${adapterBootstrapFile}",
        ],
    },
    "process command mismatch",
)
PY

echo "Installed ROS2 Tools Adapter check passed"
