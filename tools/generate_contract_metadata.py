#!/usr/bin/env python3
"""Generate the exact capability metadata compiled into the ROS2 Adapter."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Any

from generate_runtime_manifests import build_contracts


def cpp_string(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def schema(value: dict[str, Any]) -> str:
    return "{" + ", ".join([
        f"{value['messageId']}u", cpp_string(value["typeName"]),
        f"{value['schemaVersion']}u", f"{value['schemaFingerprint']}ULL",
    ]) + "}"


def endpoint(value: dict[str, Any]) -> str:
    item = value["endpoints"][0]
    limits = item["limits"]
    return "{" + ", ".join([
        cpp_string(value["ref"]["id"]), f"{value['ref']['version']}u",
        cpp_string(value["contractDigest"]), cpp_string(item["endpointId"]),
        schema(item["inputSchema"]), schema(item["outputSchema"]),
        f"{item['defaultTimeoutMillis']}u", f"{item['maximumTimeoutMillis']}u",
        "{" + ", ".join(f"{limits[key]}u" for key in [
            "maximumRequestBytes", "maximumResponseBytes", "maximumConcurrency",
            "maximumStreams", "maximumStreamChunkBytes", "maximumStreamChunkMessages",
        ]) + "}",
    ]) + "}"


def generate(schema_dir: Path) -> str:
    references, contracts = build_contracts(schema_dir)
    by_id = {item["ref"]["id"]: item for item in contracts}
    return f'''#pragma once
#include <cstdint>
namespace xgc_ros2_tools_adapter {{ namespace contract {{
struct Schema {{ std::uint32_t message_id; const char* type_name; std::uint32_t version; std::uint64_t fingerprint; }};
struct Limits {{ std::uint32_t maximum_request_bytes; std::uint32_t maximum_response_bytes; std::uint32_t maximum_concurrency; std::uint32_t maximum_streams; std::uint32_t maximum_stream_chunk_bytes; std::uint32_t maximum_stream_chunk_messages; }};
struct Endpoint {{ const char* capability_id; std::uint32_t contract_version; const char* contract_digest; const char* endpoint_id; Schema input_schema; Schema output_schema; std::uint32_t default_timeout_ms; std::uint32_t maximum_timeout_ms; Limits limits; }};
inline constexpr Schema kConfiguration = {schema(references['configuration'])};
inline constexpr Endpoint kPublish = {endpoint(by_id['xgc.ros2.topic.publish'])};
inline constexpr Endpoint kService = {endpoint(by_id['xgc.ros2.service.call'])};
}} }}  // namespace xgc_ros2_tools_adapter::contract
'''


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--schema-dir", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(generate(Path(args.schema_dir)), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
