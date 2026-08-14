#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
ROS_DISTRO="${XGC2_ROS_DISTRO:-jazzy}"
UBUNTU_CODENAME="${XGC2_UBUNTU_CODENAME:-noble}"
DOCKER_IMAGE="${DOCKER_IMAGE:-ros:${ROS_DISTRO}-ros-base-${UBUNTU_CODENAME}}"
WORK_DIR="${WORK_DIR:-${REPO_ROOT}/.work/docker-${ROS_DISTRO}}"
OUTPUT_DIR="${OUTPUT_DIR:-${REPO_ROOT}/debs}"
EXPECTED_ARCH="${EXPECTED_ARCH:-}"
PREPARE_ACTION="${PREPARE_ACTION:-ci}"
XGC2_APT_OVERLAY_URL="${XGC2_APT_OVERLAY_URL:-}"
XGC2_DEPENDENCY_SET_DIGEST="${XGC2_DEPENDENCY_SET_DIGEST:-$(
  python3 -c 'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["planDependencySetDigest"])' \
    "${REPO_ROOT}/.xgc2/dependency-lock.json"
)}"
ADAPTER_RUNTIME_CLIENT_DEB_VERSION="${ADAPTER_RUNTIME_CLIENT_DEB_VERSION:-}"
XGC2_PROTOBUF_DEB_VERSION="${XGC2_PROTOBUF_DEB_VERSION:-}"
HOST_UID="$(id -u)"
HOST_GID="$(id -g)"
readonly HOST_UID HOST_GID REPO_ROOT SCRIPT_DIR

while [[ $# -gt 0 ]]; do
  case "$1" in
    --image) DOCKER_IMAGE="$2"; shift 2 ;;
    --work-dir) WORK_DIR="$2"; shift 2 ;;
    --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

[[ "$ROS_DISTRO" == jazzy && "$UBUNTU_CODENAME" == noble ]] || {
  echo "ROS2 Tools Adapter supports only Jazzy/Noble" >&2
  exit 2
}
[[ "$PREPARE_ACTION" =~ ^(ci|release|compatibility-verify)$ ]] || {
  echo "invalid PREPARE_ACTION: $PREPARE_ACTION" >&2
  exit 2
}
[[ "$XGC2_DEPENDENCY_SET_DIGEST" =~ ^[0-9a-f]{64}$ ]] || {
  echo "XGC2_DEPENDENCY_SET_DIGEST must be 64 lowercase hexadecimal characters" >&2
  exit 2
}
if [[ -n "$XGC2_APT_OVERLAY_URL" ]]; then
  "$SCRIPT_DIR/configure_xgc2_apt.sh" --validate-url "$XGC2_APT_OVERLAY_URL"
  DEPENDENCY_MODE=staging-apt
else
  DEPENDENCY_MODE=locked-source
  ADAPTER_RUNTIME_CLIENT_DEB_VERSION="${ADAPTER_RUNTIME_CLIENT_DEB_VERSION:-0.6.0-8~noble}"
  XGC2_PROTOBUF_DEB_VERSION="${XGC2_PROTOBUF_DEB_VERSION:-0.5.0-11~noble}"
fi
if [[ "$PREPARE_ACTION" == compatibility-verify && "$DEPENDENCY_MODE" != staging-apt ]]; then
  echo "compatibility-verify requires XGC2_APT_OVERLAY_URL" >&2
  exit 2
fi
[[ "$WORK_DIR" == /* && "$WORK_DIR" != / ]] || {
  echo "--work-dir must be an absolute, non-root path" >&2
  exit 2
}
[[ "$OUTPUT_DIR" == /* && "$OUTPUT_DIR" != / ]] || {
  echo "--output-dir must be an absolute, non-root path" >&2
  exit 2
}
[[ "$HOST_UID" =~ ^[0-9]+$ && "$HOST_GID" =~ ^[0-9]+$ ]] || {
  echo "host uid/gid must be numeric" >&2
  exit 1
}

mkdir -p "$WORK_DIR" "$OUTPUT_DIR"
WORK_DIR="$(realpath -m "$WORK_DIR")"
OUTPUT_DIR="$(realpath -m "$OUTPUT_DIR")"
if find "$OUTPUT_DIR" -mindepth 1 -maxdepth 1 -print -quit | grep -q .; then
  echo "Debian output directory is not isolated: $OUTPUT_DIR" >&2
  exit 1
fi
STAGING_OUTPUT_DIR="$(mktemp -d "${OUTPUT_DIR}.staging.XXXXXX")"
cleanup() {
  rm -rf "$STAGING_OUTPUT_DIR"
}
trap cleanup EXIT

docker pull "$DOCKER_IMAGE"
# The single-quoted argument is an inner Bash program evaluated in the build
# container. Its continuations are not host-side shell continuations.
# shellcheck disable=SC1004
docker run --rm \
  -e "ADAPTER_RUNTIME_CLIENT_DEB_VERSION=${ADAPTER_RUNTIME_CLIENT_DEB_VERSION}" \
  -e "DEBIAN_FRONTEND=noninteractive" \
  -e "DEPENDENCY_MODE=${DEPENDENCY_MODE}" \
  -e "EXPECTED_ARCH=${EXPECTED_ARCH}" \
  -e "HOST_GID=${HOST_GID}" \
  -e "HOST_UID=${HOST_UID}" \
  -e "PREPARE_ACTION=${PREPARE_ACTION}" \
  -e "ROS_DISTRO=${ROS_DISTRO}" \
  -e "UBUNTU_CODENAME=${UBUNTU_CODENAME}" \
  -e "XGC2_APT_OVERLAY_URL=${XGC2_APT_OVERLAY_URL}" \
  -e "XGC2_DEPENDENCY_SET_DIGEST=${XGC2_DEPENDENCY_SET_DIGEST}" \
  -e "XGC2_PROTOBUF_DEB_VERSION=${XGC2_PROTOBUF_DEB_VERSION}" \
  -v "$REPO_ROOT:/workspace/source:ro" \
  -v "$WORK_DIR:/workspace/work" \
  -v "$STAGING_OUTPUT_DIR:/workspace/out" \
  "$DOCKER_IMAGE" bash -lc '
    set -euo pipefail
    # shellcheck disable=SC2317 # EXIT invokes this callback indirectly.
    return_mount_ownership() {
      chown -R "${HOST_UID}:${HOST_GID}" /workspace/work /workspace/out
    }
    trap return_mount_ownership EXIT

    actual_arch="$(dpkg --print-architecture)"
    [[ -z "$EXPECTED_ARCH" || "$actual_arch" == "$EXPECTED_ARCH" ]] || {
      echo "container architecture $actual_arch != $EXPECTED_ARCH" >&2
      exit 1
    }
    /workspace/source/.xgc2/scripts/configure_xgc2_apt.sh "$UBUNTU_CODENAME"

    runtime_package=libxgc2-adapter-runtime-client-dev
    protobuf_package=xgc2-protobuf-dev
    [[ -z "$ADAPTER_RUNTIME_CLIENT_DEB_VERSION" ]] || \
      runtime_package="${runtime_package}=${ADAPTER_RUNTIME_CLIENT_DEB_VERSION}"
    [[ -z "$XGC2_PROTOBUF_DEB_VERSION" ]] || \
      protobuf_package="${protobuf_package}=${XGC2_PROTOBUF_DEB_VERSION}"
    apt-get install -y --no-install-recommends \
      build-essential clang-format cmake dpkg-dev fakeroot file libjsoncpp-dev \
      pkg-config python3 python3-yaml ripgrep rsync shellcheck \
      "$runtime_package" "$protobuf_package" \
      ros-jazzy-ament-cmake ros-jazzy-ament-cmake-gtest \
      ros-jazzy-rcl-interfaces ros-jazzy-rclcpp ros-jazzy-rcpputils \
      ros-jazzy-rmw ros-jazzy-rosidl-runtime-cpp \
      ros-jazzy-rosidl-typesupport-introspection-cpp \
      ros-jazzy-std-msgs ros-jazzy-std-srvs
    [[ -z "$ADAPTER_RUNTIME_CLIENT_DEB_VERSION" ]] || \
      test "$(dpkg-query -W -f="\${Version}" libxgc2-adapter-runtime-client-dev)" = \
        "$ADAPTER_RUNTIME_CLIENT_DEB_VERSION"
    [[ -z "$XGC2_PROTOBUF_DEB_VERSION" ]] || \
      test "$(dpkg-query -W -f="\${Version}" xgc2-protobuf-dev)" = \
        "$XGC2_PROTOBUF_DEB_VERSION"

    find /workspace/work -mindepth 1 -maxdepth 1 \
      \( -name build -o -name install-root -o -name source \) \
      -exec rm -rf {} +
    mkdir -p /workspace/work/source /workspace/work/install-root
    rsync -a --delete \
      --exclude .git --exclude .ci --exclude .work --exclude build \
      --exclude debs --exclude install --exclude log \
      /workspace/source/ /workspace/work/source/
    set +u
    # shellcheck disable=SC1091
    source /opt/ros/jazzy/setup.bash
    set -u
    /workspace/work/source/.xgc2/scripts/check_package_compliance.sh
    cmake -S /workspace/work/source -B /workspace/work/build \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_INSTALL_PREFIX=/opt/ros/jazzy \
      -DBUILD_TESTING=ON
    cmake --build /workspace/work/build --parallel "$(nproc)"
    ctest --test-dir /workspace/work/build --output-on-failure
    DESTDIR=/workspace/work/install-root \
      cmake --install /workspace/work/build
    /workspace/work/source/.xgc2/scripts/package_debs.sh \
      --install-root /workspace/work/install-root \
      --output-dir /workspace/out \
      --ros-distro jazzy
    deb="$(find /workspace/out -maxdepth 1 -type f \
      -name "ros-jazzy-xgc2-ros2-tools-adapter_*.deb" -print)"
    [[ -n "$deb" && "$(wc -l <<<"$deb")" == 1 ]] || {
      echo "builder did not produce exactly one product Deb" >&2
      exit 1
    }
    apt-get install -y "$deb"
    expected_version="$(awk -F": *" "/^version:/ {print \$2; exit}" \
      /workspace/work/source/.xgc2/product.yml)"
    EXPECTED_VERSION="$expected_version" \
      /workspace/work/source/.xgc2/scripts/check_installed_packages.sh
    /workspace/work/source/.xgc2/scripts/write_dependency_evidence.py \
      --lock /workspace/work/source/.xgc2/dependency-lock.json \
      --output /workspace/out/xgc2-dependency-evidence.json \
      --prepare-action "$PREPARE_ACTION" \
      --dependency-mode "$DEPENDENCY_MODE" \
      --dependency-set-digest "$XGC2_DEPENDENCY_SET_DIGEST" \
      --distribution noble \
      --architecture "$actual_arch" \
      ${XGC2_APT_OVERLAY_URL:+--apt-overlay-url "$XGC2_APT_OVERLAY_URL"}
  '

mapfile -t staged_files < <(
  find "$STAGING_OUTPUT_DIR" -mindepth 1 -maxdepth 1 -type f -print | sort
)
(( ${#staged_files[@]} == 2 )) || {
  echo "builder produced an unexpected artifact set" >&2
  exit 1
}
for staged_file in "${staged_files[@]}"; do
  mv "$staged_file" "$OUTPUT_DIR/"
done
find "$OUTPUT_DIR" -mindepth 1 -maxdepth 1 -type f -print | sort
