"""Source-tree release policy tests.

Generated binaries are staged locally and published as GitHub Release assets;
they are deliberately absent from git.
"""
from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def tracked_files() -> set[str]:
    out = subprocess.check_output(
        ["git", "ls-files"], cwd=ROOT, text=True
    )
    return set(out.splitlines())


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_generated_release_artifacts_are_not_tracked():
    tracked = tracked_files()
    forbidden_prefixes = ("dist/", "release/")
    assert not [path for path in tracked if path.startswith(forbidden_prefixes)]
    assert "BSMenubar/BSMenubar-bin" not in tracked


def test_release_staging_is_ignored():
    text = read(".gitignore")
    assert "/dist/" in text
    assert "/release/" in text
    assert "!dist/" not in text


def test_release_scripts_exist_and_parse():
    scripts = [
        "scripts/package-release.sh",
        "scripts/release-checksums.sh",
        "scripts/github-release.sh",
        "scripts/install.sh",
    ]
    for script in scripts:
        path = ROOT / script
        assert path.is_file(), f"missing {script}"
        subprocess.run(["bash", "-n", str(path)], check=True)


def test_container_build_does_not_override_custom_dist_with_repo_dist():
    build = read("build.sh")
    assert "--out /tmp/bs-container-dist" in build
    assert "--out /work/dist" not in build


def test_github_release_script_is_fail_closed():
    text = read("scripts/github-release.sh")
    for required in (
        "git status --porcelain",
        "git rev-parse HEAD",
        "git ls-remote origin",
        "gh release create",
        "--verify-tag",
        "bs_tray.ps1",
        "SHA256SUMS",
        "SBOM-binaries.json",
    ):
        assert required in text
    assert "codeberg" not in text.lower()


def test_installers_use_release_assets_and_mandatory_hashes():
    sh = read("scripts/install.sh")
    ps = read("scripts/install.ps1")
    assert "github.com/MindDragonLabs/BridgeSessions/releases/download/v${TAG}" in sh
    assert "refusing unverified binary" in sh
    assert sh.index("SHA-256 verified") < sh.index('REPORTED_VERSION=$("${TMP_BIN}" --version)')
    assert sh.index('curl -fsSL --progress-bar "${BASE}/${BIN}"') < sh.index('echo "→ Stopping existing daemon..."')
    assert 'RECORDED_HASH' in sh and 'INSTALLED_HASH' in sh
    assert 'previous installation restored' in sh
    assert "github.com/MindDragonLabs/BridgeSessions/releases/download/v$TAG" in ps
    assert "Get-FileHash" in ps
    assert "Move-Item $TMP_PATH $BIN_PATH -Force" in ps
    assert "SecurityProtocolType]::Tls12" in ps
    assert 'Get-ReleaseAssetHash "bs_tray.ps1"' in ps
    assert "$env:BRIDGESESSIONS_DIST_DIR" in ps
    assert "Staged Windows binary is missing" in ps
    assert "Staged SHA256SUMS is missing" in ps
    assert "Test-IsRunningUnderBridgeSessions" in ps
    assert ps.index("if (Test-IsRunningUnderBridgeSessions)") < ps.index(
        "DETECT AND KILL ALL RUNNING DAEMONS"
    )
    assert '$expectedInstalledHash = Get-ReleaseAssetHash "bridgesessions-windows-x86_64.exe"' in ps
    assert "$installedHash -eq $expectedInstalledHash" in ps
    assert ps.index("$expectedInstalledHash = Get-ReleaseAssetHash") < ps.index(
        "DETECT AND KILL ALL RUNNING DAEMONS"
    )
    assert "Installed binary hash differs from release; reinstalling" in ps
    assert ps.count("/Change /TN") >= 2
    assert "Set-ScheduledTask could not preserve the existing principal" in ps
    assert "Could not update BridgeSessions task action" in ps
    assert "Could not update BridgeSessions-CuaHelper task action" in ps
    assert "BRIDGESESSIONS_TASK_PASSWORD" in ps
    assert "daemonSetParams.Password" in ps
    assert "Password-aware update of the BridgeSessions task failed" in ps
    assert "protected management session" in ps
    assert "Refusing upgrade through BridgeSessions shell" in ps
    assert '$env:BRIDGESESSIONS_SKIP_UI -eq "1"' in ps
    # Windows PowerShell 5.1 treats UTF-8 without a BOM as the active ANSI
    # code page; keep this streamed installer ASCII-safe on Server 2016.
    ps.encode("ascii")


def test_windows_release_stages_and_publishes_verified_tray_companion():
    build = read("build.sh")
    release = read(".github/workflows/release.yml")
    ci = read(".github/workflows/ci.yml")
    checksums = read("scripts/release-checksums.sh")
    assert build.count('"bs_tray.ps1"') >= 2
    assert "dist/bs_tray.ps1" in release
    assert "dist/bs_tray.ps1" in ci
    assert "candidates=(bridgesessions bridgesessions-* bs_tray.ps1)" in checksums
    assert "windows-installer-validation" in ci
    assert "windows installer validation" in release
    assert "tests/test_install_order.ps1" in release
    assert "guard, linux, macos, windows, windows-installer-validation" in release


def test_windows_fleet_e2e_exercises_distinct_new_terminal_sessions():
    script = read("scripts/e2e-fleet-test.sh")
    assert "cmd.exe /Q' --detach" in script
    assert "new_terminal_isolation" in script
    assert 'sessions "$peer" --kill "$session"' in script


def test_release_workflow_pins_all_jobs_and_source_archives_to_verified_tag_commit():
    workflow = read(".github/workflows/release.yml")
    assert 'refs/tags/$tag:refs/tags/$tag' in workflow
    assert 'refs/tags/$tag^{commit}' in workflow
    assert 'REQUESTED_TAG: ${{ github.event.inputs.tag || github.ref_name }}' in workflow
    assert 'if [[ ! "$tag" =~ ^v[0-9A-Za-z][0-9A-Za-z.-]*$ ]]' in workflow
    assert 'echo "commit=$commit" >> "$GITHUB_OUTPUT"' in workflow
    assert 'commit: ${{ steps.check.outputs.commit }}' in workflow
    assert workflow.count('ref: ${{ needs.guard.outputs.commit }}') == 5
    assert '"$commit"' in workflow
    assert '--clobber' not in workflow
    assert 'refusing overwrite' in workflow
    assert 'a[name] != b[name]' in workflow
    assert '--verify-tag' in workflow
    assert 'ldd "$bin" >' in workflow and "grep -F 'not found'" in workflow
    assert "unexpected non-system macOS dependency" in workflow


def test_macos_release_requires_developer_id_signing_and_notarization():
    workflow = read(".github/workflows/release.yml")
    assert "MACOS_SIGNING_P12_B64" in workflow
    assert "MACOS_SIGNING_P12_PASSWORD" in workflow
    assert "APP_STORE_CONNECT_KEY_ID" in workflow
    assert "APP_STORE_CONNECT_ISSUER" in workflow
    assert "APP_STORE_CONNECT_KEY_P8" in workflow
    assert "./scripts/notarize-macos.sh" in workflow
    assert "missing APP_STORE_CONNECT_KEY_P8 secret" in workflow
    assert "Remove temporary signing credentials" in workflow


def test_checksum_validator_checks_exact_source_version_and_binary_platforms():
    checksums = read("scripts/release-checksums.sh")
    assert 'path.endswith(".zip")' in checksums
    assert 'names.count(member) != 1' in checksums
    assert 'members[0].isfile()' in checksums
    assert 'value != version + "\\n"' in checksums
    assert "unexpected artifact format" in checksums
    assert "grep -Fx -- \"$VERSION\"" in checksums


def test_fleet_e2e_checks_base_version_and_supports_mixed_version_runs():
    script = read("scripts/e2e-fleet-test.sh")
    assert 'fleet --json' in script
    assert 'peers.get(sys.argv[1], {}).get("version", "")' in script
    assert 'EXPECTED_PEER_VERSION="${BS_E2E_PEER_VERSION:-$EXPECTED_VERSION}"' in script
    assert '"$out" == "$EXPECTED_PEER_VERSION"+*' in script
    assert 'expected_peer_version' in script
    assert '"found version in output"' not in script


def test_reliability_loop_provisions_pytest_when_active_python_lacks_it():
    loop = read("scripts/reliability_loop.py")
    assert 'importlib.util.find_spec("pytest")' in loop
    assert '"--with", "pytest"' in loop


def test_static_builder_uses_supported_dependency_lines():
    text = read("scripts/Dockerfile.static-linux")
    assert "openssl-3.5.7" in text
    assert "spdlog-1.17.0" in text
    assert "fmt-12.2.0" in text
    assert "Catch2-3.8.0" not in text
    assert "openssl-3.3.2" not in text
    assert "spdlog-1.15.0" not in text

    pins = read("cmake/Dependencies.cmake")
    for pin in (
        '"v1.17.0"', '"v1.5.7"', '"v3.15.0"',
        '"v2.7.2"', '"v3.12.0"', '"openssl-3.5.7"',
    ):
        assert pin in pins
    assert "openssl-3.0.16" not in pins

    workflow = read(".github/workflows/release.yml")
    assert "./build.sh linux --distro native --openssl fetch --out dist --no-tests" in workflow


def test_version_is_single_line_and_cmake_reads_it():
    version = read("VERSION").strip()
    assert re.fullmatch(r"\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?", version)
    cmake = read("CMakeLists.txt")
    assert 'file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION"' in cmake
    assert 'add_compile_definitions(BS_VERSION="${BRIDGESESSIONS_VERSION}")' in cmake



def test_release_automation_is_executable():
    for rel in ("scripts/github-release.sh", "scripts/package-release.sh"):
        assert os.access(ROOT / rel, os.X_OK), f"{rel} must be executable"
