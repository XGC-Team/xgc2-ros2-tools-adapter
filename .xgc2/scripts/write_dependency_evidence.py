#!/usr/bin/env python3
"""Write exact installed dependency evidence for a ROS2 Tools build."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path
from typing import Any


HEX40 = re.compile(r"^[0-9a-f]{40}$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")
HTTPS_URL = re.compile(
    r"^https://[A-Za-z0-9.-]+(?::[0-9]{1,5})?"
    r"(?:/[A-Za-z0-9._~%+:/=-]*)?$"
)


def canonical_digest(value: Any) -> str:
    encoded = json.dumps(
        value, ensure_ascii=False, separators=(",", ":"), sort_keys=True
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def load_lock(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict) or set(value) != {
        "schema",
        "planDependencySetDigest",
        "dependencies",
    }:
        raise ValueError("dependency lock fields are not exact")
    if value["schema"] != "xgc2.dependency-lock.v1":
        raise ValueError("unsupported dependency lock schema")
    dependencies = value["dependencies"]
    if not isinstance(dependencies, list) or len(dependencies) != 2:
        raise ValueError("dependency lock must contain exactly two providers")
    required = {
        "id",
        "planAction",
        "policy",
        "repository",
        "sourceSha",
        "version",
        "package",
        "packageVersion",
    }
    for dependency in dependencies:
        if not isinstance(dependency, dict) or set(dependency) != required:
            raise ValueError("locked dependency fields are not exact")
        if dependency["planAction"] != "verify" or dependency["policy"] != "rebuild":
            raise ValueError("locked dependency plan identity is invalid")
        if HEX40.fullmatch(str(dependency["sourceSha"])) is None:
            raise ValueError("locked dependency source SHA is invalid")
    plan_identity = [
        {
            "id": dependency["id"],
            "action": dependency["planAction"],
            "source_sha": dependency["sourceSha"],
            "version": dependency["version"],
            "policy": dependency["policy"],
        }
        for dependency in sorted(dependencies, key=lambda item: str(item["id"]))
    ]
    actual_digest = canonical_digest(plan_identity)
    if value["planDependencySetDigest"] != actual_digest:
        raise ValueError("dependency lock plan digest is invalid")
    return value


def installed_version(package: str) -> str:
    return subprocess.check_output(
        ["dpkg-query", "-W", "-f=${Version}", package], text=True
    ).strip()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument(
        "--prepare-action",
        required=True,
        choices=("ci", "release", "compatibility-verify"),
    )
    parser.add_argument(
        "--dependency-mode", required=True, choices=("locked-source", "staging-apt")
    )
    parser.add_argument("--dependency-set-digest", required=True)
    parser.add_argument("--distribution", required=True, choices=("noble",))
    parser.add_argument("--architecture", required=True, choices=("amd64", "arm64"))
    parser.add_argument("--apt-overlay-url", default="")
    args = parser.parse_args()

    if HEX64.fullmatch(args.dependency_set_digest) is None:
        parser.error("dependency-set-digest must be 64 lowercase hexadecimal characters")
    lock = load_lock(Path(args.lock))
    if args.dependency_mode == "locked-source":
        if args.apt_overlay_url:
            parser.error("locked-source mode forbids apt-overlay-url")
        if args.dependency_set_digest != lock["planDependencySetDigest"]:
            parser.error("locked-source dependency-set digest does not match the lock")
    else:
        if HTTPS_URL.fullmatch(args.apt_overlay_url) is None:
            parser.error("staging-apt mode requires a strict HTTPS overlay URL")
        if args.prepare_action == "ci":
            parser.error("CI cannot use staging-apt dependency mode")

    evidence: list[dict[str, str]] = []
    for dependency in lock["dependencies"]:
        package = str(dependency["package"])
        version = installed_version(package)
        if (
            args.dependency_mode == "locked-source"
            and version != dependency["packageVersion"]
        ):
            parser.error(
                f"{package} version {version} != locked {dependency['packageVersion']}"
            )
        source = (
            f"https://github.com/{dependency['repository']}.git@"
            f"{dependency['sourceSha']}"
            if args.dependency_mode == "locked-source"
            else args.apt_overlay_url.rstrip("/")
        )
        evidence.append({"package": package, "version": version, "source": source})

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(
            {
                "schema": "xgc2.dependency-evidence.v1",
                "prepareAction": args.prepare_action,
                "dependencySetDigest": args.dependency_set_digest,
                "dependencyMode": args.dependency_mode,
                "distribution": args.distribution,
                "architecture": args.architecture,
                "dependencies": sorted(evidence, key=lambda item: item["package"]),
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
