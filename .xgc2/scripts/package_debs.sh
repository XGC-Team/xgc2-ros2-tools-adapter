#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
INSTALL_ROOT=""
OUTPUT_DIR=""
ROS_DISTRO="${XGC2_ROS_DISTRO:-jazzy}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --install-root) INSTALL_ROOT="$2"; shift 2 ;;
    --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
    --ros-distro) ROS_DISTRO="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

[[ "$ROS_DISTRO" == jazzy ]] || {
  echo "unsupported ROS distribution: $ROS_DISTRO" >&2
  exit 2
}
[[ -n "$INSTALL_ROOT" && "$INSTALL_ROOT" == /* && "$INSTALL_ROOT" != / ]] || {
  echo "--install-root must be an absolute, non-root path" >&2
  exit 2
}
[[ -n "$OUTPUT_DIR" && "$OUTPUT_DIR" == /* && "$OUTPUT_DIR" != / ]] || {
  echo "--output-dir must be an absolute, non-root path" >&2
  exit 2
}

PACKAGE="ros-${ROS_DISTRO}-xgc2-ros2-tools-adapter"
ROS_PACKAGE="xgc_ros2_tools_adapter"
PREFIX="/opt/ros/${ROS_DISTRO}"
PREFIX_ROOT="${INSTALL_ROOT}${PREFIX}"
VERSION="${PACKAGE_VERSION:-$(
  awk -F': *' '/^version:[[:space:]]*/ {print $2; exit}' \
    "${REPO_ROOT}/.xgc2/product.yml"
)}"
ARCH="$(dpkg --print-architecture)"
PKG_ROOT="$(mktemp -d /tmp/xgc2-ros2-tools-adapter-deb.XXXXXX)"
cleanup() {
  rm -rf "$PKG_ROOT"
}
trap cleanup EXIT

[[ -n "$VERSION" ]] || { echo "product version is empty" >&2; exit 1; }
test -x "${PREFIX_ROOT}/lib/${ROS_PACKAGE}/${ROS_PACKAGE}_node"
test -d "${PREFIX_ROOT}/share/${ROS_PACKAGE}"
test -f "${PREFIX_ROOT}/share/ament_index/resource_index/packages/${ROS_PACKAGE}"
test -f "${INSTALL_ROOT}/usr/share/xgc2/adapter-definitions/xgc2-ros2-tools-adapter.json"
test -f "${INSTALL_ROOT}/usr/share/xgc2/process-definitions/xgc2-ros2-tools-adapter.json"

install -d -m 0755 \
  "$PKG_ROOT/DEBIAN" \
  "$PKG_ROOT/usr/share/doc/$PACKAGE" \
  "$PKG_ROOT$PREFIX/lib/$ROS_PACKAGE" \
  "$PKG_ROOT$PREFIX/share" \
  "$PKG_ROOT/usr/share/xgc2/adapter-definitions" \
  "$PKG_ROOT/usr/share/xgc2/process-definitions"
cp -a "${PREFIX_ROOT}/lib/${ROS_PACKAGE}/${ROS_PACKAGE}_node" \
  "$PKG_ROOT$PREFIX/lib/$ROS_PACKAGE/"
cp -a "${PREFIX_ROOT}/share/${ROS_PACKAGE}" \
  "$PKG_ROOT$PREFIX/share/"
for resource_index in package_run_dependencies packages parent_prefix_path; do
  resource="${PREFIX_ROOT}/share/ament_index/resource_index/${resource_index}/${ROS_PACKAGE}"
  test -f "$resource"
  install -D -m 0644 "$resource" \
    "$PKG_ROOT$PREFIX/share/ament_index/resource_index/${resource_index}/${ROS_PACKAGE}"
done
cp -a "${INSTALL_ROOT}/usr/share/xgc2/adapter-definitions/xgc2-ros2-tools-adapter.json" \
  "$PKG_ROOT/usr/share/xgc2/adapter-definitions/"
cp -a "${INSTALL_ROOT}/usr/share/xgc2/process-definitions/xgc2-ros2-tools-adapter.json" \
  "$PKG_ROOT/usr/share/xgc2/process-definitions/"
cp "$REPO_ROOT/README.md" "$PKG_ROOT/usr/share/doc/$PACKAGE/README.md"
cp "$REPO_ROOT/LICENSE" "$PKG_ROOT/usr/share/doc/$PACKAGE/copyright"

executable="$PKG_ROOT$PREFIX/lib/$ROS_PACKAGE/${ROS_PACKAGE}_node"
install -d -m 0755 "$PKG_ROOT/debian"
printf '%s\n' \
  'Source: xgc2-ros2-tools-adapter' \
  'Section: misc' \
  'Priority: optional' \
  'Maintainer: XGC2 <lxk36@users.noreply.github.com>' \
  '' \
  "Package: ${PACKAGE}" \
  'Architecture: any' \
  >"$PKG_ROOT/debian/control"
shlibdeps_output="$(
  cd "$PKG_ROOT"
  dpkg-shlibdeps -O "-e${executable}"
)"
shlibdeps="${shlibdeps_output#shlibs:Depends=}"
[[ -n "$shlibdeps" && "$shlibdeps" != "$shlibdeps_output" ]] || {
  echo "dpkg-shlibdeps did not produce executable dependencies" >&2
  exit 1
}

# ROS libraries intentionally use unversioned SONAMEs and Ubuntu's ROS Debs do
# not all publish shlibs metadata. Keep every dependency that dpkg-shlibdeps
# can prove, then complete the exact runtime contract declared in product.yml.
append_runtime_dependency() {
  local package="$1"
  if ! grep -Eq "(^|, )${package}([[:space:](,]|$)" <<<"$shlibdeps"; then
    shlibdeps+=", ${package}"
  fi
}
for runtime_package in \
  libjsoncpp25 \
  libxgc2-adapter-runtime-client2 \
  ros-jazzy-rclcpp \
  ros-jazzy-rcpputils \
  ros-jazzy-rmw \
  ros-jazzy-rosidl-runtime-cpp \
  ros-jazzy-rosidl-typesupport-introspection-cpp; do
  append_runtime_dependency "$runtime_package"
done
grep -Eq '(^|, )libxgc2-adapter-runtime-client2([[:space:](,]|$)' <<<"$shlibdeps" || {
  echo "computed dependencies omit libxgc2-adapter-runtime-client2" >&2
  exit 1
}
if grep -Eq '(^|, )(libxgc2-adapter-runtime-client-dev|xgc2-protobuf-dev)([[:space:](,]|$)' \
  <<<"$shlibdeps"; then
  echo "computed runtime dependencies contain a build-only package" >&2
  exit 1
fi
rm -rf "$PKG_ROOT/debian"

printf '%s\n' \
  "Package: ${PACKAGE}" \
  "Version: ${VERSION}" \
  'Section: misc' \
  'Priority: optional' \
  "Architecture: ${ARCH}" \
  'Maintainer: XGC2 <lxk36@users.noreply.github.com>' \
  "Depends: ${shlibdeps}" \
  'Description: XGC2 typed ROS2 topic and service Adapter Runtime provider' \
  ' Provides bounded typed ROS2 publish and service-call capabilities through' \
  ' the target-local XGC2 Adapter Runtime.' \
  >"$PKG_ROOT/DEBIAN/control"

find "$PKG_ROOT" -type d -exec chmod 0755 {} +
find "$PKG_ROOT" -type f -exec chmod 0644 {} +
chmod 0755 "$PKG_ROOT/DEBIAN" "$executable"
mkdir -p "$OUTPUT_DIR"
if find "$OUTPUT_DIR" -maxdepth 1 -type f -name "${PACKAGE}_*.deb" -print -quit \
  | grep -q .; then
  echo "Debian output directory already contains this product" >&2
  exit 1
fi
deb="$OUTPUT_DIR/${PACKAGE}_${VERSION}_${ARCH}.deb"
fakeroot dpkg-deb --build "$PKG_ROOT" "$deb" >/dev/null
echo "$deb"
