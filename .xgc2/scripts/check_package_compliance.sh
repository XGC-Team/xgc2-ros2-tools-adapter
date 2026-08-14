#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
temporary="$(mktemp -d /tmp/xgc2-ros2-tools-compliance.XXXXXX)"
cleanup() {
  rm -rf "$temporary"
}
trap cleanup EXIT

for command in bash python3 rg shellcheck; do
  command -v "$command" >/dev/null 2>&1 || {
    echo "missing compliance dependency: $command" >&2
    exit 1
  }
done

for required in \
  LICENSE README.md CMakeLists.txt package.xml \
  .github/workflows/ci.yml .github/workflows/release.yml \
  .xgc2/dependency-lock.json .xgc2/product.yml \
  .xgc2/scripts/build_debs_in_docker.sh \
  .xgc2/scripts/check_cpp_quality.sh \
  .xgc2/scripts/check_installed_packages.sh \
  .xgc2/scripts/configure_xgc2_apt.sh \
  .xgc2/scripts/package_debs.sh \
  .xgc2/scripts/write_dependency_evidence.py \
  .xgc2/scripts/xgc2_artifact_manifest.py; do
  test -f "$REPO_ROOT/$required" || {
    echo "missing release file: $required" >&2
    exit 1
  }
done
for script in "$REPO_ROOT"/.xgc2/scripts/*.sh "$REPO_ROOT"/.xgc2/scripts/*.py; do
  test -x "$script" || {
    echo "release script is not executable: $script" >&2
    exit 1
  }
done

python3 - "$REPO_ROOT/.xgc2/product.yml" "$REPO_ROOT/package.xml" <<'PY'
import sys
from pathlib import Path
import xml.etree.ElementTree as ET

import yaml

product_path, package_path = map(Path, sys.argv[1:])
product = yaml.safe_load(product_path.read_text(encoding="utf-8"))
if not isinstance(product, dict):
    raise SystemExit("product metadata must be a mapping")
expected = {
    "schema": "xgc2.product.v1",
    "id": "xgc2-ros2-tools-adapter",
    "name": "XGC2 ROS2 Tools Adapter",
    "version": "0.1.0-1",
    "kind": "ros2-apt",
}
for key, value in expected.items():
    if product.get(key) != value:
        raise SystemExit(f"product {key} mismatch: {product.get(key)!r}")
apt = product.get("apt")
if not isinstance(apt, dict):
    raise SystemExit("apt metadata must be a mapping")
package = "ros-jazzy-xgc2-ros2-tools-adapter"
if apt.get("distribution") != "noble":
    raise SystemExit("APT distribution must be noble")
if apt.get("install") != [package] or apt.get("packages") != [package]:
    raise SystemExit("APT package identity is not exact")
if apt.get("package_architectures", {}).get(package) != ["amd64", "arm64"]:
    raise SystemExit("APT architecture contract is not dual-architecture")
release = product.get("release")
if not isinstance(release, dict):
    raise SystemExit("release metadata must be a mapping")
release_identity = {
    "repository": "lxk36/xgc2-ros2-tools-adapter",
    "ref": "jazzy",
    "workflow": "release.yml",
    "ci_workflow": "ci.yml",
}
for key, value in release_identity.items():
    if release.get(key) != value:
        raise SystemExit(f"release {key} mismatch")
if release.get("requires") != [
    "libxgc2-adapter-runtime-client-dev",
    "xgc2-protobuf",
]:
    raise SystemExit("release dependency set is not exact")
if release.get("dependency_policy") != {
    "libxgc2-adapter-runtime-client-dev": "rebuild",
    "xgc2-protobuf": "rebuild",
}:
    raise SystemExit("release dependency policy is not exact")

package_xml = ET.parse(package_path).getroot()
if package_xml.findtext("name") != "xgc_ros2_tools_adapter":
    raise SystemExit("ROS package name mismatch")
if package_xml.findtext("version") != "0.1.0":
    raise SystemExit("ROS package version mismatch")
if package_xml.findtext("license") != "BSD-3-Clause":
    raise SystemExit("ROS package license mismatch")
dependencies = {
    node.text for node in package_xml if node.tag.endswith("depend") and node.text
}
for dependency in {
    "libxgc2-adapter-runtime-client-dev",
    "libxgc2-adapter-runtime-client2",
    "rclcpp",
    "rosidl_typesupport_introspection_cpp",
}:
    if dependency not in dependencies:
        raise SystemExit(f"ROS dependency is missing: {dependency}")
PY

grep -Fq 'find_package(xgc2_adapter_runtime_client 0.6.0 EXACT REQUIRED CONFIG)' \
  "$REPO_ROOT/CMakeLists.txt"
grep -Fq 'xgc2::adapter_runtime_client' "$REPO_ROOT/CMakeLists.txt"
for workflow in ci.yml release.yml; do
  path="$REPO_ROOT/.github/workflows/$workflow"
  grep -Fq 'ubuntu-24.04-arm' "$path"
  grep -Fq '.xgc2/scripts/build_debs_in_docker.sh' "$path"
  grep -Fq 'xgc2_artifact_manifest.py build' "$path"
  grep -Fq 'xgc2_artifact_manifest.py verify-build' "$path"
  grep -Fq 'retention-days: 14' "$path"
  grep -Fq 'actions/upload-artifact@' "$path"
done
grep -Fq '.xgc2/scripts/check_cpp_quality.sh' "$REPO_ROOT/.github/workflows/ci.yml"
for input in expected_version expected_source_sha prepare_action apt_overlay_url \
  dependency_set_digest run_cpp_quality run_source_tests; do
  grep -Eq "^[[:space:]]+${input}:" "$REPO_ROOT/.github/workflows/release.yml"
done

for script in "$REPO_ROOT"/.xgc2/scripts/*.sh; do
  bash -n "$script"
done
shellcheck "$REPO_ROOT"/.xgc2/scripts/*.sh
PYTHONPYCACHEPREFIX="$temporary/pycache" python3 -m py_compile \
  "$REPO_ROOT"/tools/*.py \
  "$REPO_ROOT"/.xgc2/scripts/*.py
for document in "$REPO_ROOT"/schemas/*.json "$REPO_ROOT/.xgc2/dependency-lock.json"; do
  python3 -m json.tool "$document" >/dev/null
done
python3 - "$REPO_ROOT/.xgc2/scripts/write_dependency_evidence.py" \
  "$REPO_ROOT/.xgc2/dependency-lock.json" <<'PY'
import importlib.util
import sys
from pathlib import Path

script, lock = map(Path, sys.argv[1:])
spec = importlib.util.spec_from_file_location("dependency_evidence", script)
if spec is None or spec.loader is None:
    raise SystemExit("cannot load dependency evidence module")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
module.load_lock(lock)
PY
python3 -m unittest discover -v -s "$REPO_ROOT/test" -p 'test_*.py'

python3 "$REPO_ROOT/tools/generate_runtime_manifests.py" \
  --executable /bin/true \
  --ros-package xgc_ros2_tools_adapter \
  --ros-executable xgc_ros2_tools_adapter_node \
  --schema-dir "$REPO_ROOT/schemas" \
  --version 0.1.0 \
  --adapter-output "$temporary/adapter.json" \
  --process-output "$temporary/process.json"
python3 -m json.tool "$temporary/adapter.json" >/dev/null
python3 -m json.tool "$temporary/process.json" >/dev/null

forbidden_pattern='automation[._-]gate''way|\bgate''way\b|mav''ros|--sock''et|publish_''apt|APT_''REPO_[A-Z0-9_]+'
if rg -n -i "$forbidden_pattern" \
  "$REPO_ROOT/include" "$REPO_ROOT/src" "$REPO_ROOT/schemas" \
  "$REPO_ROOT/tools" "$REPO_ROOT/.xgc2" "$REPO_ROOT/.github"; then
  echo "removed identity, product publishing, or product secret remains" >&2
  exit 1
fi

echo "Package compliance check passed"
