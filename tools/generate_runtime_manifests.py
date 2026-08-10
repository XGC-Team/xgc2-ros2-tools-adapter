#!/usr/bin/env python3
"""Generate exact ROS2 Tools Adapter capability and process manifests."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
from typing import Any


SCHEMAS = {
    "configuration": (4301, "xgc.ros2.tools.v1.NativeContext", "native-context.schema.json"),
    "publish_input": (4302, "xgc.ros2.tools.v1.PublishRequest", "publish-request.schema.json"),
    "publish_output": (4303, "xgc.ros2.tools.v1.PublishResult", "publish-result.schema.json"),
    "service_input": (4304, "xgc.ros2.tools.v1.ServiceCallRequest", "service-call-request.schema.json"),
    "service_output": (4305, "xgc.ros2.tools.v1.ServiceCallResult", "service-call-result.schema.json"),
}


def canonical_json(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def digest_file(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(chunk)
    return "sha256:" + hasher.hexdigest()


def schema_reference(schema_dir: Path, key: str) -> dict[str, Any]:
    message_id, type_name, filename = SCHEMAS[key]
    schema = json.loads((schema_dir / filename).read_text(encoding="utf-8"))
    if schema.get("$schema") != "https://json-schema.org/draft/2020-12/schema":
        raise ValueError(f"{filename} must use JSON Schema draft 2020-12")
    if schema.get("$id") != type_name or schema.get("type") != "object":
        raise ValueError(f"{filename} identity/root does not match {type_name}")
    normalized = json.dumps(schema, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
    fingerprint = int.from_bytes(hashlib.sha256(normalized).digest()[:8], "big")
    if fingerprint == 0:
        raise ValueError(f"zero schema fingerprint for {filename}")
    return {
        "messageId": message_id,
        "typeName": type_name,
        "schemaVersion": 1,
        "schemaFingerprint": fingerprint,
    }


def endpoint(endpoint_id: str, input_schema: dict[str, Any], output_schema: dict[str, Any], maximum_response_bytes: int) -> dict[str, Any]:
    return {
        "endpointId": endpoint_id,
        "interaction": "operation",
        "inputSchema": input_schema,
        "outputSchema": output_schema,
        "sideEffect": "non-idempotent",
        "idempotency": "required",
        "cancellationSupported": True,
        "deadlineRequired": True,
        "defaultTimeoutMillis": 30000,
        "maximumTimeoutMillis": 300000,
        "limits": {
            "maximumRequestBytes": 8388608,
            "maximumResponseBytes": maximum_response_bytes,
            "maximumConcurrency": 16,
            "maximumStreams": 0,
            "maximumStreamChunkBytes": 0,
            "maximumStreamChunkMessages": 0,
        },
    }


def capability(capability_id: str, value: dict[str, Any]) -> dict[str, Any]:
    body = {"ref": {"id": capability_id, "version": 1}, "endpoints": [value]}
    return {"ref": body["ref"], "contractDigest": digest_bytes(canonical_json(body)), "endpoints": body["endpoints"]}


def build_contracts(schema_dir: Path) -> tuple[dict[str, dict[str, Any]], list[dict[str, Any]]]:
    references = {key: schema_reference(schema_dir, key) for key in SCHEMAS}
    contracts = [
        capability("xgc.ros2.topic.publish", endpoint("publish", references["publish_input"], references["publish_output"], 1048576)),
        capability("xgc.ros2.service.call", endpoint("call", references["service_input"], references["service_output"], 8388608)),
    ]
    contracts.sort(key=lambda item: item["ref"]["id"])
    return references, contracts


def build_manifests(
    executable: Path,
    ros_package: str,
    ros_executable: str,
    schema_dir: Path,
    version: str,
) -> tuple[dict[str, Any], dict[str, Any]]:
    if not executable.is_file():
        raise ValueError(f"Adapter executable does not exist: {executable}")
    if re.fullmatch(r"[a-z][a-z0-9_]*", ros_package) is None:
        raise ValueError("ROS package name must be canonical")
    if re.fullmatch(r"[a-z][a-z0-9_]*", ros_executable) is None:
        raise ValueError("ROS executable name must be canonical")
    if re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version) is None:
        raise ValueError("version must use MAJOR.MINOR.PATCH")
    references, contracts = build_contracts(schema_dir)
    capability_manifest = {"formatVersion": 1, "capabilities": contracts}
    adapter = {
        "apiVersion": "xgc.adapter.definition/v1",
        "adapters": [{
            "definition": {
                "id": "xgc2-ros2-tools-adapter",
                "version": version,
                "processDefinitionId": "xgc2-ros2-tools-adapter",
                "buildDigest": digest_file(executable),
                "trustedManifestDigest": digest_bytes(canonical_json(capability_manifest)),
                "configuration": {"schema": references["configuration"], "allowedEncodings": ["json"]},
                "activation": {"mode": "on-demand", "idleTimeoutNanos": 300000000000},
                "scope": {
                    "kind": "ros2-native-context",
                    "requiredAttributes": ["domain-id"],
                    "optionalAttributes": ["rmw-implementation"],
                    "allowAdditionalAttributes": False,
                    "sharing": "shared",
                },
            },
            "capabilityManifest": capability_manifest,
        }],
    }
    process = {
        "apiVersion": "xgc.execution.process/v1",
        "definitions": [{
            "id": "xgc2-ros2-tools-adapter",
            "version": version,
            "label": "XGC2 ROS2 Tools Adapter",
            "description": "Typed ROS2 topic publish and service call capabilities for Adapter Runtime.",
            "drivers": ["host"],
            "parameters": {
                "properties": {"adapterBootstrapFile": {"type": "string", "description": "Trusted mode-0600 binary AdapterProcessBootstrap path."}},
                "required": ["adapterBootstrapFile"],
                "additionalProperties": False,
            },
            "command": {
                "executable": "ros2",
                "args": [
                    "run",
                    ros_package,
                    ros_executable,
                    "--adapter-bootstrap-file",
                    "${adapterBootstrapFile}",
                ],
            },
            "readiness": {"kind": "process"},
            "liveness": {"kind": "process"},
            "stop": {"gracePeriod": 5000000000},
            "restart": {"mode": "never"},
            "internal": True,
        }],
    }
    return adapter, process


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--ros-package", required=True)
    parser.add_argument("--ros-executable", required=True)
    parser.add_argument("--schema-dir", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--adapter-output", required=True)
    parser.add_argument("--process-output", required=True)
    args = parser.parse_args()
    adapter, process = build_manifests(
        Path(args.executable),
        args.ros_package,
        args.ros_executable,
        Path(args.schema_dir),
        args.version,
    )
    write_json(Path(args.adapter_output), adapter)
    write_json(Path(args.process_output), process)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
