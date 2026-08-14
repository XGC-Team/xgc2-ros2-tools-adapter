from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
MANIFEST_TOOL = ROOT / ".xgc2/scripts/xgc2_artifact_manifest.py"
EVIDENCE_TOOL = ROOT / ".xgc2/scripts/write_dependency_evidence.py"
DEPENDENCY_LOCK = ROOT / ".xgc2/dependency-lock.json"
PRODUCT = "xgc2-ros2-tools-adapter"
PACKAGE = "ros-jazzy-xgc2-ros2-tools-adapter"
VERSION = "0.1.0-1"
SOURCE_SHA = "1" * 40


class ArtifactManifestTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def build_deb(
        self,
        directory: Path,
        *,
        package: str = PACKAGE,
        version: str = VERSION,
        architecture: str = "amd64",
    ) -> Path:
        package_root = self.root / f"package-{len(list(self.root.glob('package-*')))}"
        (package_root / "DEBIAN").mkdir(parents=True)
        (package_root / "usr/share/xgc2-tests").mkdir(parents=True)
        (package_root / "DEBIAN/control").write_text(
            "\n".join(
                [
                    f"Package: {package}",
                    f"Version: {version}",
                    "Section: misc",
                    "Priority: optional",
                    f"Architecture: {architecture}",
                    "Maintainer: XGC2 Tests <tests@example.com>",
                    "Description: ROS2 Tools artifact contract test",
                    "",
                ]
            ),
            encoding="utf-8",
        )
        (package_root / "usr/share/xgc2-tests/identity").write_text(
            f"{package}={version}\n", encoding="utf-8"
        )
        directory.mkdir(parents=True, exist_ok=True)
        output = directory / f"{package}_{version}_{architecture}.deb"
        subprocess.run(
            ["dpkg-deb", "--root-owner-group", "--build", package_root, output],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        return output

    def build_arguments(self, deb_dir: Path) -> list[str]:
        return [
            sys.executable,
            str(MANIFEST_TOOL),
            "build",
            "--deb-dir",
            str(deb_dir),
            "--output-dir",
            str(self.root / "manifests"),
            "--product",
            PRODUCT,
            "--expected-package",
            PACKAGE,
            "--product-version",
            VERSION,
            "--distribution",
            "noble",
            "--architecture",
            "amd64",
            "--source-sha",
            SOURCE_SHA,
            "--ci-run-id",
            "12345",
            "--ci-workflow",
            "ci",
            "--ci-workflow-ref",
            "repo/.github/workflows/ci.yml@refs/heads/jazzy",
        ]

    def test_build_and_verify_exact_product_deb(self) -> None:
        deb_dir = self.root / "debs"
        deb = self.build_deb(deb_dir)
        subprocess.run(self.build_arguments(deb_dir), check=True)
        manifest_path = self.root / "manifests" / f"{PRODUCT}_noble_amd64.build.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        self.assertEqual("xgc2.build-artifact.v1", manifest["schema"])
        self.assertEqual(PACKAGE, manifest["debs"][0]["package"])

        subprocess.run(
            [
                sys.executable,
                str(MANIFEST_TOOL),
                "verify-build",
                "--artifact-dir",
                str(self.root),
                "--deb-output-dir",
                str(self.root / "verified/debs"),
                "--manifest-output-dir",
                str(self.root / "verified/manifests"),
                "--product",
                PRODUCT,
                "--expected-package",
                PACKAGE,
                "--product-version",
                VERSION,
                "--distribution",
                "noble",
                "--architecture",
                "amd64",
                "--source-sha",
                SOURCE_SHA,
                "--ci-run-id",
                "12345",
            ],
            check=True,
        )
        self.assertEqual(deb.read_bytes(), (self.root / "verified/debs" / deb.name).read_bytes())

    def test_build_rejects_extra_or_wrong_deb(self) -> None:
        extra = self.root / "extra"
        self.build_deb(extra)
        self.build_deb(extra, package="unrelated-package")
        result = subprocess.run(
            self.build_arguments(extra), capture_output=True, text=True, check=False
        )
        self.assertNotEqual(0, result.returncode)
        self.assertIn("exactly one Deb", result.stderr)

        wrong = self.root / "wrong"
        self.build_deb(wrong, package="wrong-package")
        result = subprocess.run(
            self.build_arguments(wrong), capture_output=True, text=True, check=False
        )
        self.assertNotEqual(0, result.returncode)
        self.assertIn("package identity mismatch", result.stderr)

    def test_verify_rejects_tampered_deb(self) -> None:
        deb_dir = self.root / "debs"
        deb = self.build_deb(deb_dir)
        subprocess.run(self.build_arguments(deb_dir), check=True)
        deb.write_bytes(deb.read_bytes() + b"tamper")
        result = subprocess.run(
            [
                sys.executable,
                str(MANIFEST_TOOL),
                "verify-build",
                "--artifact-dir",
                str(self.root),
                "--deb-output-dir",
                str(self.root / "verified/debs"),
                "--manifest-output-dir",
                str(self.root / "verified/manifests"),
                "--product",
                PRODUCT,
                "--expected-package",
                PACKAGE,
                "--product-version",
                VERSION,
                "--distribution",
                "noble",
                "--architecture",
                "amd64",
                "--source-sha",
                SOURCE_SHA,
                "--ci-run-id",
                "12345",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertNotEqual(0, result.returncode)


class DependencyEvidenceTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        fake_bin = self.root / "bin"
        fake_bin.mkdir()
        dpkg_query = fake_bin / "dpkg-query"
        dpkg_query.write_text(
            "#!/usr/bin/env bash\n"
            "case \"${@: -1}\" in\n"
            "  libxgc2-adapter-runtime-client-dev) printf '%s' '0.6.0-8~noble' ;;\n"
            "  xgc2-protobuf-dev) printf '%s' '0.5.0-11~noble' ;;\n"
            "  *) exit 1 ;;\n"
            "esac\n",
            encoding="utf-8",
        )
        dpkg_query.chmod(0o755)
        self.environment = dict(os.environ)
        self.environment["PATH"] = f"{fake_bin}:{self.environment['PATH']}"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def run_evidence(self, *extra: str) -> subprocess.CompletedProcess[str]:
        lock = json.loads(DEPENDENCY_LOCK.read_text(encoding="utf-8"))
        return subprocess.run(
            [
                sys.executable,
                str(EVIDENCE_TOOL),
                "--lock",
                str(DEPENDENCY_LOCK),
                "--output",
                str(self.root / "evidence.json"),
                "--prepare-action",
                "ci",
                "--dependency-mode",
                "locked-source",
                "--dependency-set-digest",
                lock["planDependencySetDigest"],
                "--distribution",
                "noble",
                "--architecture",
                "amd64",
                *extra,
            ],
            env=self.environment,
            capture_output=True,
            text=True,
            check=False,
        )

    def test_locked_evidence_is_exact_and_source_bound(self) -> None:
        result = self.run_evidence()
        self.assertEqual(0, result.returncode, result.stderr)
        evidence = json.loads((self.root / "evidence.json").read_text(encoding="utf-8"))
        self.assertEqual("xgc2.dependency-evidence.v1", evidence["schema"])
        self.assertEqual(
            ["libxgc2-adapter-runtime-client-dev", "xgc2-protobuf-dev"],
            [item["package"] for item in evidence["dependencies"]],
        )
        self.assertTrue(all(".git@" in item["source"] for item in evidence["dependencies"]))

    def test_locked_evidence_rejects_wrong_digest(self) -> None:
        result = subprocess.run(
            [
                sys.executable,
                str(EVIDENCE_TOOL),
                "--lock",
                str(DEPENDENCY_LOCK),
                "--output",
                str(self.root / "evidence.json"),
                "--prepare-action",
                "ci",
                "--dependency-mode",
                "locked-source",
                "--dependency-set-digest",
                "0" * 64,
                "--distribution",
                "noble",
                "--architecture",
                "amd64",
            ],
            env=self.environment,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertNotEqual(0, result.returncode)
        self.assertIn("does not match the lock", result.stderr)


if __name__ == "__main__":
    unittest.main()
