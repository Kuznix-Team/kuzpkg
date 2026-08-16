#!/usr/bin/env python3
#
# kuzpkg-untracked.py
#
# Copyright (C) 2026 Kuznix
#
# Generic filesystem adoption/discovery helper for kuzpkg.
#
# It is intentionally package-manager agnostic: it can discover software
# installed by Arch/pacman, LFS/BLFS, source builds, RPM/DEB/APK systems,
# custom installers, and other projects by inspecting the installed files.
# It does NOT claim that arbitrary filesystem evidence can always identify
# the original upstream package with certainty; ambiguous results are kept
# visible instead of silently inventing metadata.
#
# Native artifacts: package
# /lib32 and /usr/lib32: lib32-package
# /libx32 and /usr/libx32: libx32-package
# Firefox trees: firefox
#
# Re-running is safe. Already-owned files are ignored and existing generated
# archives are skipped unless --overwrite is supplied.
#

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from collections import defaultdict
from pathlib import Path

LIB_DIRS = (
    ("/usr/lib", ""),
    ("/usr/lib64", ""),
    ("/lib", ""),
    ("/lib64", ""),
    ("/lib32", "lib32-"),
    ("/usr/lib32", "lib32-"),
    ("/libx32", "libx32-"),
    ("/usr/libx32", "libx32-"),
)
BIN_DIRS = ("/usr/bin", "/usr/sbin", "/bin", "/sbin")
PC_DIRS = (
    ("/usr/lib/pkgconfig", ""),
    ("/usr/lib64/pkgconfig", ""),
    ("/usr/share/pkgconfig", ""),
    ("/usr/lib32/pkgconfig", "lib32-"),
    ("/lib32/pkgconfig", "lib32-"),
    ("/usr/libx32/pkgconfig", "libx32-"),
    ("/libx32/pkgconfig", "libx32-"),
)
FIREFOX_DIRS = ("/lib/firefox", "/usr/lib/firefox")

VERSION_RE = re.compile(r"(?:version|release|v)?\s*([0-9]+(?:\.[0-9A-Za-z]+)+(?:[-+._][0-9A-Za-z.-]+)?)", re.I)


def run_kuzpkg(root: str, args: list[str]) -> list[str]:
    cmd = ["kuzpkg"]
    if root != "/":
        cmd += ["--root", root]
    cmd += args
    try:
        proc = subprocess.run(cmd, text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL, check=False)
    except OSError as exc:
        raise RuntimeError(f"cannot execute kuzpkg: {exc}") from exc
    return [line.strip() for line in proc.stdout.splitlines() if line.strip()]


def norm_rel(path: str) -> str:
    path = path.replace(os.sep, "/")
    return path.lstrip("/").rstrip("/")


def load_local_db(root: str) -> tuple[set[str], set[str]]:
    packages = set(run_kuzpkg(root, ["-Qq"]))
    owned: set[str] = set()
    for pkg in sorted(packages):
        for line in run_kuzpkg(root, ["-Qql", pkg]):
            if line.startswith(pkg + " "):
                line = line.split(None, 1)[1]
            owned.add(norm_rel(line))
    return packages, owned


def under_root(root: Path, path: str) -> Path:
    rel = path.lstrip("/")
    return root / rel if str(root) != "/" else Path("/") / rel


def relative(path: Path, root: Path) -> str:
    base = root if str(root) != "/" else Path("/")
    return norm_rel(str(path.relative_to(base)))


def scan_files(root: Path, dirs, predicate) -> list[tuple[Path, str]]:
    found = []
    for directory, prefix in dirs:
        base = under_root(root, directory)
        if not base.exists():
            continue
        try:
            for current, dirnames, filenames in os.walk(base, followlinks=False):
                dirnames[:] = [d for d in dirnames if not (Path(current) / d).is_symlink()]
                for name in filenames:
                    p = Path(current) / name
                    if (p.is_file() or p.is_symlink()) and predicate(p):
                        found.append((p, prefix))
        except OSError:
            pass
    return found


def scan_include_dirs(root: Path):
    base = under_root(root, "/usr/include")
    if not base.exists():
        return []
    try:
        return [p for p in base.iterdir() if p.is_dir() and not p.is_symlink()]
    except OSError:
        return []


def scan_firefox(root: Path):
    found = []
    for directory in FIREFOX_DIRS:
        base = under_root(root, directory)
        if not base.is_dir():
            continue
        try:
            for current, dirnames, filenames in os.walk(base, followlinks=False):
                dirnames[:] = [d for d in dirnames if not (Path(current) / d).is_symlink()]
                for name in filenames:
                    p = Path(current) / name
                    if p.is_file() or p.is_symlink():
                        found.append(p)
        except OSError:
            pass
    return found


def package_guess(path: Path):
    name = path.name
    if name.endswith(".pc"):
        return name[:-3], "pkgconfig"
    so = re.sub(r"\.so(?:\.[0-9A-Za-z._-]+)*$", "", name)
    if so != name:
        return (so[3:] if so.startswith("lib") and len(so) > 3 else so), "library"
    if name in {"firefox", "firefox-bin", "firefox-esr"}:
        return "firefox", "firefox"
    if "." in name and name.startswith("lib"):
        return name.split(".", 1)[0][3:], "library"
    return name, "binary"


def version_from_text(text: str):
    for line in text.splitlines()[:30]:
        match = VERSION_RE.search(line)
        if match:
            return match.group(1)
    return None


def probe_version(path: Path):
    if not path.is_file() or not os.access(path, os.X_OK):
        return None
    for option in (("--version",), ("-version",), (("--version", "--verbose"))):
        try:
            proc = subprocess.run([str(path), *option], stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, timeout=3,
                                  check=False)
            version = version_from_text(proc.stdout)
            if version:
                return version
        except (OSError, subprocess.SubprocessError):
            continue
    return None


def detect_version(name: str, evidence: list[Path], root: Path):
    # Prefer the named executable, then evidence executables.
    candidates = []
    for directory in BIN_DIRS + FIREFOX_DIRS:
        candidates.append(under_root(root, directory) / name)
    candidates.extend(evidence)
    seen = set()
    for candidate in candidates:
        key = str(candidate)
        if key in seen:
            continue
        seen.add(key)
        version = probe_version(candidate)
        if version:
            return version, "version-probe"

    # Common source/package metadata files. This is useful for non-Arch and
    # non-package-manager installations as well.
    for p in evidence:
        for parent in [p.parent, *p.parents]:
            for metadata in ("VERSION", "version", "VERSION.txt", "PKG-INFO"):
                m = parent / metadata
                try:
                    if m.is_file():
                        version = version_from_text(m.read_text(errors="replace"))
                        if version:
                            return version, "metadata"
                except OSError:
                    pass
            if parent == root:
                break
    return "0.0.0+detected", "unverified"


def discover(root: str, minimum: int):
    root_path = Path(root).resolve()
    local_packages, owned = load_local_db(root)
    candidates = defaultdict(lambda: {"count": 0, "types": set(), "evidence": []})

    artifacts = []
    artifacts += [(p, "library", prefix) for p, prefix in
                  scan_files(root_path, LIB_DIRS, lambda p: ".so" in p.name)]
    artifacts += [(p, "binary", "") for p in
                  scan_files(root_path, tuple((d, "") for d in BIN_DIRS),
                             lambda p: os.access(p, os.X_OK) and "." not in p.name)]
    artifacts += [(p, "pkgconfig", prefix) for p, prefix in
                  scan_files(root_path, PC_DIRS, lambda p: p.name.endswith(".pc"))]
    artifacts += [(p, "include", "") for p in scan_include_dirs(root_path)]

    # Firefox is a directory tree rather than a normal single library/binary
    # artifact. Treat its complete tree as one package candidate.
    for p in scan_firefox(root_path):
        artifacts.append((p, "firefox", ""))

    seen = set()
    for path, category, prefix in artifacts:
        rel = relative(path, root_path)
        if rel in seen or rel in owned:
            continue
        seen.add(rel)

        if category == "firefox":
            guess = "firefox"
        else:
            guess, _ = package_guess(path)
        if not guess:
            continue

        package_name = prefix + guess
        if package_name in local_packages:
            continue
        item = candidates[package_name]
        item["count"] += 1
        item["types"].add(category)
        if len(item["evidence"]) < 25:
            item["evidence"].append(path)

    result = []
    for name, item in candidates.items():
        if item["count"] >= minimum:
            result.append((name, item["count"], item["types"], item["evidence"]))
    result.sort(key=lambda x: (-x[1], x[0]))
    return result


def archive_candidate(name: str, count: int, types: set[str], evidence: list[Path],
                      root: Path, output_dir: Path, overwrite: bool):
    version, version_source = detect_version(name.removeprefix("lib32-").removeprefix("libx32-"), evidence, root)
    archive_name = f"{name}-{version}.kuzpkg.tar.zst"
    output = output_dir / archive_name
    if output.exists() and not overwrite:
        return output, version, version_source, False

    output_dir.mkdir(parents=True, exist_ok=True)
    # Python's tarfile has no portable zstd writer on older Python versions.
    # Build an uncompressed tar in a temporary file, then use the system zstd.
    zstd = shutil.which("zstd")
    if not zstd:
        raise RuntimeError("zstd is required to create .kuzpkg.tar.zst archives")

    metadata = {
        "format": "kuzpkg-detected-v1",
        "name": name,
        "version": version,
        "version_verified": version_source != "unverified",
        "version_source": version_source,
        "architecture": "auto-detected",
        "origin": "filesystem-discovery",
        "artifact_types": sorted(types),
        "artifact_count": count,
    }

    with tempfile.TemporaryDirectory(prefix="kuzpkg-untracked-") as tmp:
        tar_path = Path(tmp) / f"{name}-{version}.kuzpkg.tar"
        metadata_path = Path(tmp) / ".KUZPKG-METADATA.json"
        metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
        with tarfile.open(tar_path, "w") as tar:
            tar.add(metadata_path, arcname=".KUZPKG-METADATA.json")
            added = set()
            for path in evidence:
                try:
                    rel = relative(path, root)
                except ValueError:
                    continue
                if rel in added:
                    continue
                added.add(rel)
                # Store paths exactly as they would appear under /.
                tar.add(path, arcname=rel, recursive=False)
        subprocess.run([zstd, "-q", "-f", "-o", str(output), str(tar_path)],
                       check=True)

    return output, version, version_source, True


def main():
    parser = argparse.ArgumentParser(
        description="Discover filesystem-installed software missing from kuzpkg LocalDB"
    )
    parser.add_argument("--root", default="/", help="alternate installation root/chroot")
    parser.add_argument("--minimum", type=int, default=1,
                        help="minimum unowned artifacts per candidate")
    parser.add_argument("-q", "--quiet", action="store_true", help="print candidate names only")
    parser.add_argument("--package", action="store_true",
                        help="create .kuzpkg.tar.zst archives for discovered candidates")
    parser.add_argument("--output-dir", default=".", help="directory for generated packages")
    parser.add_argument("--overwrite", action="store_true",
                        help="replace existing generated .kuzpkg.tar.zst files")
    args = parser.parse_args()
    if args.minimum < 1:
        parser.error("--minimum must be at least 1")

    try:
        candidates = discover(args.root, args.minimum)
        root_path = Path(args.root).resolve()
    except RuntimeError as exc:
        print(f"kuzpkg-untracked: error: {exc}", file=sys.stderr)
        return 1

    if not candidates:
        print("kuzpkg-untracked: no unregistered package candidates found")
        return 0

    print(f"kuzpkg-untracked: found {len(candidates)} candidate package(s) missing from LocalDB")
    for name, count, types, evidence in candidates:
        if args.quiet:
            print(name)
            continue
        print(f"  + {name:<32} {count:>3} unowned artifact(s) [{', '.join(sorted(types))}]")
        for path in evidence[:5]:
            print(f"      /{relative(path, root_path)}")
        if len(evidence) > 5:
            print(f"      ... and {len(evidence) - 5} more")

        if args.package:
            try:
                output, version, source, created = archive_candidate(
                    name, count, types, evidence, root_path,
                    Path(args.output_dir).resolve(), args.overwrite)
                action = "created" if created else "already exists; skipped"
                print(f"      package: {output} ({action}; version={version}, {source})")
            except (OSError, subprocess.SubprocessError, RuntimeError) as exc:
                print(f"      package: FAILED: {exc}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
