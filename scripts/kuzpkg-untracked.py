#!/usr/bin/env python3
#
# kuzpkg-untracked.py
#
# Copyright (C) 2026 Kuznix
#
# Discover software already present on disk but missing from kuzpkg's
# local database. Supports native, lib32, and x32 library trees.
#
# Unowned native artifacts are reported as: package
# Unowned /lib32 artifacts are reported as: lib32-package
# Unowned x32 artifacts are reported as: libx32-package
#
# This is deliberately discovery-only. It never modifies the local DB.

import argparse
import os
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

DEFAULT_LIB_DIRS = (
    ("/usr/lib", ""),
    ("/usr/lib64", ""),
    ("/lib", ""),
    ("/lib64", ""),
    ("/lib32", "lib32-"),
    ("/usr/lib32", "lib32-"),
    ("/libx32", "libx32-"),
    ("/usr/libx32", "libx32-"),
)
DEFAULT_BIN_DIRS = (
    "/usr/bin",
    "/usr/sbin",
    "/bin",
    "/sbin",
)
DEFAULT_PC_DIRS = (
    ("/usr/lib/pkgconfig", ""),
    ("/usr/lib64/pkgconfig", ""),
    ("/usr/share/pkgconfig", ""),
    ("/usr/lib32/pkgconfig", "lib32-"),
    ("/lib32/pkgconfig", "lib32-"),
    ("/usr/libx32/pkgconfig", "libx32-"),
    ("/libx32/pkgconfig", "libx32-"),
)


def run_kuzpkg(root: str, args: list[str]) -> list[str]:
    cmd = ["kuzpkg"]
    if root != "/":
        cmd += ["--root", root]
    cmd += args
    try:
        proc = subprocess.run(
            cmd,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    except OSError as exc:
        raise RuntimeError(f"cannot execute kuzpkg: {exc}") from exc
    return [line.strip() for line in proc.stdout.splitlines() if line.strip()]


def norm_rel(path: str) -> str:
    path = path.replace(os.sep, "/")
    if path.startswith("/"):
        path = path[1:]
    return path.rstrip("/")


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
    relative = path.lstrip("/")
    return root / relative if str(root) != "/" else Path("/") / relative


def scan_files(root: Path, dirs: tuple[str, ...], predicate) -> list[tuple[Path, str]]:
    found: list[tuple[Path, str]] = []
    for directory, prefix in dirs:
        base = under_root(root, directory)
        if not base.exists():
            continue
        try:
            for current, dirnames, filenames in os.walk(base, followlinks=False):
                dirnames[:] = [d for d in dirnames if not (Path(current) / d).is_symlink()]
                for name in filenames:
                    path = Path(current) / name
                    if (path.is_symlink() or path.is_file()) and predicate(path):
                        found.append((path, prefix))
        except OSError:
            continue
    return found


def scan_include_dirs(root: Path) -> list[Path]:
    base = under_root(root, "/usr/include")
    if not base.exists():
        return []
    try:
        return [p for p in base.iterdir() if p.is_dir() and not p.is_symlink()]
    except OSError:
        return []


def package_guess(path: Path) -> tuple[str, str]:
    name = path.name

    if name.endswith(".pc"):
        return name[:-3], "pkgconfig"

    so = re.sub(r"\.so(?:\.[0-9A-Za-z._-]+)*$", "", name)
    if so != name:
        if so.startswith("lib") and len(so) > 3:
            return so[3:], "library"
        return so, "library"

    if "." in name and name.startswith("lib"):
        return name.split(".", 1)[0][3:], "library"

    return name, "binary"


def relative_display(path: Path, root: Path) -> str:
    if str(root) == "/":
        return "/" + norm_rel(str(path))
    return "/" + norm_rel(str(path.relative_to(root)))


def discover(root: str, minimum: int) -> list[tuple[str, int, set[str], list[str]]]:
    root_path = Path(root).resolve()
    local_packages, owned = load_local_db(root)
    candidates: dict[str, dict] = defaultdict(lambda: {
        "count": 0,
        "types": set(),
        "evidence": [],
    })

    lib_pred = lambda p: ".so" in p.name
    bin_pred = lambda p: os.access(p, os.X_OK) and "." not in p.name
    pc_pred = lambda p: p.name.endswith(".pc")

    artifacts: list[tuple[Path, str, str]] = []
    artifacts += [(p, "library", prefix) for p, prefix in scan_files(root_path, DEFAULT_LIB_DIRS, lib_pred)]
    artifacts += [(p, "binary", "") for p in scan_files(root_path, ((d, "") for d in DEFAULT_BIN_DIRS), bin_pred)]
    artifacts += [(p, "pkgconfig", prefix) for p, prefix in scan_files(root_path, DEFAULT_PC_DIRS, pc_pred)]
    artifacts += [(p, "include", "") for p in scan_include_dirs(root_path)]

    seen: set[str] = set()
    for path, category, prefix in artifacts:
        rel = norm_rel(str(path.relative_to(root_path if str(root_path) != "/" else Path("/"))))
        if rel in seen or rel in owned:
            continue
        seen.add(rel)

        guess, _ = package_guess(path)
        if not guess:
            continue

        package_name = prefix + guess
        if package_name in local_packages:
            continue

        item = candidates[package_name]
        item["count"] += 1
        item["types"].add(category)
        if len(item["evidence"]) < 5:
            item["evidence"].append(relative_display(path, root_path))

    result = []
    for name, item in candidates.items():
        strong = bool({"binary", "library", "pkgconfig"} & item["types"])
        if item["count"] >= minimum or (minimum == 1 and strong):
            result.append((name, item["count"], item["types"], item["evidence"]))

    result.sort(key=lambda x: (-x[1], x[0]))
    return result


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Find software present on disk but missing from kuzpkg LocalDB"
    )
    parser.add_argument("--root", default="/", help="alternate installation root/chroot (default: /)")
    parser.add_argument("--minimum", type=int, default=1,
                        help="minimum number of unowned artifacts per candidate (default: 1)")
    parser.add_argument("-q", "--quiet", action="store_true",
                        help="print only candidate package names")
    args = parser.parse_args()

    if args.minimum < 1:
        parser.error("--minimum must be at least 1")

    try:
        candidates = discover(args.root, args.minimum)
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
        for path in evidence:
            print(f"      {path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
