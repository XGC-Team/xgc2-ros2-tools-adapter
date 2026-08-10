from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "generate_runtime_manifests", ROOT / "tools" / "generate_runtime_manifests.py"
)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class RuntimeManifestTests(unittest.TestCase):
    def test_on_demand_shared_provider_contract_is_exact(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "adapter"
            executable.write_bytes(b"adapter-binary")
            adapter, process = MODULE.build_manifests(
                executable,
                "xgc_ros2_tools_adapter",
                "xgc_ros2_tools_adapter_node",
                ROOT / "schemas",
                "0.1.0",
            )
        installed = adapter["adapters"][0]
        definition = installed["definition"]
        self.assertEqual("xgc2-ros2-tools-adapter", definition["id"])
        self.assertEqual("on-demand", definition["activation"]["mode"])
        self.assertEqual("shared", definition["scope"]["sharing"])
        self.assertEqual(["domain-id"], definition["scope"]["requiredAttributes"])
        self.assertEqual(
            {"xgc.ros2.topic.publish", "xgc.ros2.service.call"},
            {item["ref"]["id"] for item in installed["capabilityManifest"]["capabilities"]},
        )
        command = process["definitions"][0]["command"]
        self.assertEqual(
            {
                "executable": "ros2",
                "args": [
                    "run",
                    "xgc_ros2_tools_adapter",
                    "xgc_ros2_tools_adapter_node",
                    "--adapter-bootstrap-file",
                    "${adapterBootstrapFile}",
                ],
            },
            command,
        )
        self.assertNotIn("bash", str(command))
        self.assertNotIn("ros2 service call", str(command))
        self.assertNotIn("/opt/ros", str(command))
        self.assertNotIn("/home/", str(command))

    def test_package_command_rejects_absolute_or_noncanonical_ros_names(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "adapter"
            executable.write_bytes(b"adapter-binary")
            invalid_names = [
                (
                    "/home/operator/ws/install/xgc_ros2_tools_adapter",
                    "xgc_ros2_tools_adapter_node",
                ),
                (
                    "xgc_ros2_tools_adapter",
                    "/opt/ros/jazzy/lib/xgc_ros2_tools_adapter/"
                    "xgc_ros2_tools_adapter_node",
                ),
                ("XGC_ROS2_TOOLS_ADAPTER", "xgc_ros2_tools_adapter_node"),
                ("xgc_ros2_tools_adapter", "../xgc_ros2_tools_adapter_node"),
            ]
            for ros_package, ros_executable in invalid_names:
                with self.subTest(
                    ros_package=ros_package, ros_executable=ros_executable
                ):
                    with self.assertRaisesRegex(ValueError, "must be canonical"):
                        MODULE.build_manifests(
                            executable,
                            ros_package,
                            ros_executable,
                            ROOT / "schemas",
                            "0.1.0",
                        )


if __name__ == "__main__":
    unittest.main()
