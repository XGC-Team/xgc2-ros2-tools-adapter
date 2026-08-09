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
                "/opt/ros/jazzy/lib/xgc_ros2_tools_adapter/xgc_ros2_tools_adapter_node",
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
            "/opt/ros/jazzy/lib/xgc_ros2_tools_adapter/xgc_ros2_tools_adapter_node",
            command["executable"],
        )
        self.assertNotIn("bash", str(command))
        self.assertNotIn("ros2 service call", str(command))


if __name__ == "__main__":
    unittest.main()
