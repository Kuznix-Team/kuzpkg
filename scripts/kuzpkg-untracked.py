#!/usr/bin/env python3
# Generic filesystem adoption/discovery helper for kuzpkg.
# Archives use: name-pkgver-pkgrel-arch.kuzpkg.tar.zst.
#
# The scanner is package-manager/language agnostic. It can discover native
# files, multilib files, Firefox, and language modules installed by Python,
# Rust/Cargo, Ruby, Perl, Node.js, Go, Java/JVM, PHP, Lua, Tcl, R, WASM,
# and other ecosystems.

import argparse
import json
import os
import platform
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

BIN_DIRS = (
    "/usr/bin",
    "/usr/sbin",
    "/bin",
    "/sbin",
)

PC_DIRS = (
    ("/usr/lib/pkgconfig", ""),
    ("/usr/lib64/pkgconfig", ""),
    ("/usr/share/pkgconfig", ""),
    ("/usr/lib32/pkgconfig", "lib32-"),
    ("/lib32/pkgconfig", "lib32-"),
    ("/usr/libx32/pkgconfig", "libx32-"),
    ("/libx32/pkgconfig", "libx32-"),
)

FIREFOX_DIRS = (
    "/lib/firefox",
    "/usr/lib/firefox",
)

EXCLUDED_TOPLEVEL = {
    "home",
    "root",
    "proc",
    "run",
    "sys",
    "dev",
    "tmp",
    "mnt",
    "srv",
}

MODULE_EXTENSIONS = {
    ".py",
    ".pyc",
    ".pyo",
    ".so",
    ".pyd",
    ".rs",
    ".rlib",
    ".rmeta",
    ".crate",
    ".rb",
    ".rake",
    ".gem",
    ".pm",
    ".pod",
    ".js",
    ".mjs",
    ".cjs",
    ".node",
    ".go",
    ".a",
    ".jar",
    ".class",
    ".war",
    ".ear",
    ".php",
    ".phar",
    ".lua",
    ".luac",
    ".tcl",
    ".tm",
    ".wasm",
    ".dll",
    ".dylib",
}

MODULE_METADATA = {
    "pyproject.toml": "python",
    "setup.py": "python",
    "setup.cfg": "python",
    "PKG-INFO": "python",
    "METADATA": "python",
    "Cargo.toml": "rust-cargo",
    "Cargo.lock": "rust-cargo",
    "gemfile": "ruby",
    "gemspec": "ruby",
    ".bundle": "ruby",
    "package.json": "nodejs",
    "package-lock.json": "nodejs",
    "yarn.lock": "nodejs",
    "pnpm-lock.yaml": "nodejs",
    "go.mod": "go",
    "go.sum": "go",
    "pom.xml": "java",
    "build.gradle": "java",
    "build.gradle.kts": "java",
    "composer.json": "php",
    "DESCRIPTION": "r",
    "NAMESPACE": "r",
}

VERSION_RE = re.compile(
    r"(?:version|release|v)?\s*"
    r"([0-9]+(?:\.[0-9A-Za-z]+)+(?:[-+._][0-9A-Za-z.-]+)?)",
    re.I,
)


# ---------------------------------------------------------------------------
# Terminal/font/progress support
# ---------------------------------------------------------------------------

def detect_unicode_support():
    if os.environ.get("KUZPKG_ASCII") == "1":
        return False

    encoding = (
        getattr(sys.stdout, "encoding", None)
        or ""
    ).lower().replace("-", "")

    return encoding in {
        "utf8",
        "utf8sig",
    }


USE_UNICODE = detect_unicode_support()

BOLD = "\033[1m" if sys.stdout.isatty() else ""
RESET = "\033[0m" if sys.stdout.isatty() else ""

FILL = "█" if USE_UNICODE else "#"
EMPTY = "░" if USE_UNICODE else "-"
CHECK = "✓" if USE_UNICODE else "done"


def write_stdout(text="", flush=False):
    sys.stdout.write(str(text))

    if flush:
        sys.stdout.flush()


def write_stderr(text="", flush=False):
    sys.stderr.write(str(text))

    if flush:
        sys.stderr.flush()


def print_line(text=""):
    sys.stdout.write(str(text) + "\n")


def progress_bar(current, total, width=32):
    if total <= 0:
        return f"[{EMPTY * width}] 0/0"

    current = min(
        max(current, 0),
        total,
    )

    filled = int(
        width * current / total
    )

    return (
        f"[{FILL * filled}"
        f"{EMPTY * (width - filled)}] "
        f"{current}/{total}"
    )


# ---------------------------------------------------------------------------
# kuzpkg helpers
# ---------------------------------------------------------------------------

def run_kuzpkg(root, args):
    cmd = (
        ["kuzpkg"]
        + ([] if root == "/" else ["--root", root])
        + args
    )

    try:
        process = subprocess.run(
            cmd,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    except OSError as exc:
        raise RuntimeError(
            f"cannot execute kuzpkg: {exc}"
        ) from exc

    return [
        line.strip()
        for line in process.stdout.splitlines()
        if line.strip()
    ]


def norm(path):
    return (
        str(path)
        .replace(os.sep, "/")
        .lstrip("/")
        .rstrip("/")
    )


def root_path(root, path):
    if root != "/":
        return Path(root) / path.lstrip("/")

    return Path("/") / path.lstrip("/")


def rel(path, root):
    return norm(
        path.relative_to(root)
    )


def load_local_db(root):
    packages = set(
        run_kuzpkg(root, ["-Qq"])
    )

    owned = set()

    for package in sorted(packages):
        for item in run_kuzpkg(
            root,
            ["-Qql", package],
        ):
            if item.startswith(package + " "):
                item = item.split(
                    None,
                    1,
                )[1]

            owned.add(norm(item))

    return packages, owned


# ---------------------------------------------------------------------------
# Filesystem scanning
# ---------------------------------------------------------------------------

def excluded_dir(path, root):
    try:
        relative = path.relative_to(root)
    except ValueError:
        return True

    return bool(
        relative.parts
        and relative.parts[0]
        in EXCLUDED_TOPLEVEL
    )


def scan_tree(root, predicate=lambda p: True):
    result = []

    for current, dirnames, filenames in os.walk(
        root,
        topdown=True,
        followlinks=False,
    ):
        current_path = Path(current)

        if excluded_dir(
            current_path,
            root,
        ):
            dirnames[:] = []
            continue

        dirnames[:] = [
            directory
            for directory in dirnames
            if (
                directory
                not in EXCLUDED_TOPLEVEL
                and not (
                    current_path / directory
                ).is_symlink()
            )
        ]

        for filename in filenames:
            path = (
                current_path
                / filename
            )

            if (
                (
                    path.is_file()
                    or path.is_symlink()
                )
                and predicate(path)
            ):
                result.append(path)

    return result


def scan_files(root, directories, predicate):
    result = []

    for directory, prefix in directories:
        base = root_path(
            str(root),
            directory,
        )

        if (
            not base.exists()
            or excluded_dir(base, root)
        ):
            continue

        for current, dirnames, filenames in os.walk(
            base,
            followlinks=False,
        ):
            current_path = Path(current)

            dirnames[:] = [
                directory_name
                for directory_name in dirnames
                if not (
                    current_path / directory_name
                ).is_symlink()
            ]

            for filename in filenames:
                path = (
                    current_path
                    / filename
                )

                if (
                    (
                        path.is_file()
                        or path.is_symlink()
                    )
                    and predicate(path)
                ):
                    result.append(
                        (path, prefix)
                    )

    return result


def scan_include_dirs(root):
    base = root_path(
        str(root),
        "/usr/include",
    )

    if (
        not base.exists()
        or excluded_dir(base, root)
    ):
        return []

    return [
        path
        for path in base.iterdir()
        if (
            path.is_dir()
            and not path.is_symlink()
        )
    ]


def scan_firefox(root):
    result = []

    for directory in FIREFOX_DIRS:
        base = root_path(
            str(root),
            directory,
        )

        if (
            not base.is_dir()
            or excluded_dir(base, root)
        ):
            continue

        for current, dirnames, filenames in os.walk(
            base,
            followlinks=False,
        ):
            current_path = Path(current)

            dirnames[:] = [
                directory_name
                for directory_name in dirnames
                if not (
                    current_path / directory_name
                ).is_symlink()
            ]

            result.extend(
                current_path / filename
                for filename in filenames
            )

    return result


# ---------------------------------------------------------------------------
# Package identification
# ---------------------------------------------------------------------------

def package_guess(path):
    name = path.name
    lower = name.lower()

    if name.endswith(".pc"):
        return name[:-3]

    for parent in (
        path.parent,
        *path.parents,
    ):
        parent_name = parent.name

        if parent_name.lower() in {
            "site-packages",
            "dist-packages",
            "vendor",
            "vendor_ruby",
            "gems",
            "node_modules",
            "vendor_modules",
            "lib",
            "modules",
        }:
            break

        if (
            parent_name
            and parent_name not in {
                "python",
                "python3",
                "ruby",
                "perl",
                "node",
                "nodejs",
            }
            and lower.endswith(
                (
                    ".py",
                    ".rb",
                    ".pm",
                    ".lua",
                    ".tcl",
                    ".php",
                )
            )
        ):
            return parent_name

    shared_object = re.sub(
        r"\.so(?:\.[0-9A-Za-z._-]+)*$",
        "",
        name,
    )

    if shared_object != name:
        if (
            shared_object.startswith("lib")
            and len(shared_object) > 3
        ):
            return shared_object[3:]

        return shared_object

    if (
        "." in name
        and name.startswith("lib")
    ):
        return name.split(
            ".",
            1,
        )[0][3:]

    for suffix in (
        ".rlib",
        ".rmeta",
        ".crate",
        ".gem",
        ".node",
        ".jar",
        ".wasm",
    ):
        if name.endswith(suffix):
            return name[
                :-len(suffix)
            ]

    return name


def module_kind(path):
    lower_parts = {
        part.lower()
        for part in path.parts
    }

    suffix = path.suffix.lower()
    name = path.name

    if name in MODULE_METADATA:
        return MODULE_METADATA[name]

    if (
        "site-packages" in lower_parts
        or "dist-packages" in lower_parts
        or suffix in {
            ".py",
            ".pyc",
            ".pyo",
            ".pyd",
        }
    ):
        return "python"

    if (
        "cargo" in lower_parts
        or "rustlib" in lower_parts
        or suffix in {
            ".rlib",
            ".rmeta",
            ".crate",
        }
    ):
        return "rust-cargo"

    if (
        "gems" in lower_parts
        or "vendor_ruby" in lower_parts
        or suffix in {
            ".rb",
            ".gem",
        }
    ):
        return "ruby"

    if (
        "node_modules" in lower_parts
        or suffix in {
            ".js",
            ".mjs",
            ".cjs",
            ".node",
        }
    ):
        return "nodejs"

    if (
        suffix in {
            ".pm",
            ".pod",
        }
        or "perl" in lower_parts
    ):
        return "perl"

    if (
        suffix == ".go"
        or "gopath" in lower_parts
    ):
        return "go"

    if (
        suffix in {
            ".jar",
            ".class",
            ".war",
            ".ear",
        }
        or "java" in lower_parts
    ):
        return "java-jvm"

    if (
        suffix in {
            ".php",
            ".phar",
        }
        or "composer" in lower_parts
    ):
        return "php"

    if (
        suffix in {
            ".lua",
            ".luac",
        }
        or "lua" in lower_parts
    ):
        return "lua"

    if (
        suffix in {
            ".tcl",
            ".tm",
        }
        or "tcl" in lower_parts
    ):
        return "tcl"

    if suffix == ".wasm":
        return "wasm"

    if suffix in MODULE_EXTENSIONS:
        return "generic-module"

    return None


def scan_modules(root):
    return [
        (
            path,
            module_kind(path),
        )
        for path in scan_tree(
            root,
            lambda path: (
                module_kind(path)
                is not None
            ),
        )
    ]


# ---------------------------------------------------------------------------
# Version detection
# ---------------------------------------------------------------------------

def version_from_text(text):
    for line in text.splitlines()[:80]:
        match = VERSION_RE.search(line)

        if match:
            return match.group(1)

    return None


def probe_version(path):
    if (
        not path.is_file()
        or not os.access(path, os.X_OK)
    ):
        return None

    for option in (
        ("--version",),
        ("-version",),
    ):
        try:
            process = subprocess.run(
                [str(path), *option],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=3,
                check=False,
            )

            version = version_from_text(
                process.stdout
            )

            if version:
                return version

        except (
            OSError,
            subprocess.SubprocessError,
        ):
            pass

    return None


def detect_version(
    name,
    evidence,
    root,
):
    base = (
        name
        .removeprefix("lib32-")
        .removeprefix("libx32-")
    )

    candidates = [
        root_path(
            str(root),
            directory,
        ) / base
        for directory in (
            BIN_DIRS
            + FIREFOX_DIRS
        )
    ]

    candidates += evidence

    seen = set()

    for path in candidates:
        path = Path(path)
        path_key = str(path)

        if path_key in seen:
            continue

        seen.add(path_key)

        version = probe_version(path)

        if version:
            return (
                version,
                "version-probe",
            )

    if name == "firefox":
        for metadata in (
            "/usr/lib/firefox/platform.ini",
            "/lib/firefox/platform.ini",
        ):
            path = root_path(
                str(root),
                metadata,
            )

            try:
                match = re.search(
                    r"^Version=([^\n]+)",
                    path.read_text(
                        errors="replace"
                    ),
                    re.M,
                )

                if match:
                    return (
                        match.group(1).strip(),
                        "firefox-platform.ini",
                    )

            except OSError:
                pass

    for evidence_path in evidence:
        evidence_path = Path(
            evidence_path
        )

        parents = (
            evidence_path.parent,
            *evidence_path.parents,
        )

        for parent in parents:
            for metadata_name in (
                "VERSION",
                "version",
                "VERSION.txt",
                "PKG-INFO",
                "METADATA",
                "pyproject.toml",
                "Cargo.toml",
                "gemspec",
                "package.json",
            ):
                metadata_path = (
                    parent
                    / metadata_name
                )

                try:
                    if not metadata_path.is_file():
                        continue

                    text = metadata_path.read_text(
                        errors="replace"
                    )

                    if metadata_name == "package.json":
                        try:
                            value = json.loads(
                                text
                            ).get("version")

                            if value:
                                return (
                                    str(value),
                                    "package.json",
                                )

                        except json.JSONDecodeError:
                            pass

                    version = version_from_text(
                        text
                    )

                    if version:
                        return (
                            version,
                            "metadata",
                        )

                except OSError:
                    pass

            if parent == root:
                break

    return (
        "0.0.0+detected",
        "unverified",
    )


# ---------------------------------------------------------------------------
# Architecture
# ---------------------------------------------------------------------------

def detect_arch(explicit=None):
    if explicit:
        return (
            explicit,
            "explicit",
        )

    try:
        arch = os.uname().machine
    except AttributeError:
        arch = platform.machine()

    if arch:
        return (
            arch,
            "uname",
        )

    return (
        "unknown",
        "unknown",
    )


# ---------------------------------------------------------------------------
# Discovery
# ---------------------------------------------------------------------------

def discover(root, minimum):
    root = Path(root).resolve()

    packages, owned = load_local_db(
        str(root)
    )

    candidates = defaultdict(
        lambda: {
            "count": 0,
            "types": set(),
            "evidence": [],
        }
    )

    artifacts = []

    artifacts += [
        (path, "library", prefix)
        for path, prefix in scan_files(
            root,
            LIB_DIRS,
            lambda path: (
                ".so" in path.name
            ),
        )
    ]

    artifacts += [
        (path, "binary", "")
        for path in scan_files(
            root,
            tuple(
                (
                    directory,
                    "",
                )
                for directory in BIN_DIRS
            ),
            lambda path: (
                os.access(
                    path,
                    os.X_OK,
                )
                and "." not in path.name
            ),
        )
    ]

    artifacts += [
        (path, "pkgconfig", prefix)
        for path, prefix in scan_files(
            root,
            PC_DIRS,
            lambda path: (
                path.name.endswith(".pc")
            ),
        )
    ]

    artifacts += [
        (path, "include", "")
        for path in scan_include_dirs(root)
    ]

    artifacts += [
        (path, "firefox", "")
        for path in scan_firefox(root)
    ]

    artifacts += [
        (path, kind, "")
        for path, kind in scan_modules(root)
    ]

    generic_predicate = lambda path: (
        os.access(path, os.X_OK)
        or path.suffix.lower()
        in {
            ".dll",
            ".dylib",
            ".so",
            ".a",
            ".jar",
            ".wasm",
        }
    )

    artifacts += [
        (path, "generic", "")
        for path in scan_tree(
            root,
            generic_predicate,
        )
    ]

    seen = set()

    for path, kind, prefix in artifacts:
        try:
            key = rel(
                path,
                root,
            )
        except ValueError:
            continue

        if (
            key in seen
            or key in owned
        ):
            continue

        seen.add(key)

        if kind == "firefox":
            guess = "firefox"
        else:
            guess = package_guess(path)

        if not guess:
            continue

        name = prefix + guess

        if name in packages:
            continue

        item = candidates[name]

        item["count"] += 1
        item["types"].add(kind)

        if len(
            item["evidence"]
        ) < 25:
            item["evidence"].append(path)

    return sorted(
        (
            (
                name,
                item["count"],
                item["types"],
                item["evidence"],
            )
            for name, item
            in candidates.items()
            if item["count"] >= minimum
        ),
        key=lambda entry: (
            -entry[1],
            entry[0],
        ),
    )


# ---------------------------------------------------------------------------
# Archive creation
# ---------------------------------------------------------------------------

def safe(value):
    return re.sub(
        r"[^A-Za-z0-9._+@-]+",
        "_",
        value,
    )


def make_archive(
    name,
    count,
    types,
    evidence,
    root,
    outdir,
    pkgrel,
    arch,
    overwrite,
):
    version, source = detect_version(
        name,
        evidence,
        root,
    )

    filename = (
        f"{safe(name)}-"
        f"{safe(version)}-"
        f"{safe(pkgrel)}-"
        f"{safe(arch)}.kuzpkg.tar.zst"
    )

    output = (
        outdir
        / filename
    )

    if (
        output.exists()
        and not overwrite
    ):
        return (
            output,
            version,
            source,
            False,
        )

    zstd = shutil.which("zstd")

    if not zstd:
        raise RuntimeError(
            "zstd is required to create "
            ".kuzpkg.tar.zst archives"
        )

    outdir.mkdir(
        parents=True,
        exist_ok=True,
    )

    metadata = {
        "format": "kuzpkg-detected-v3",
        "name": name,
        "version": version,
        "version_verified": (
            source != "unverified"
        ),
        "version_source": source,
        "pkgrel": pkgrel,
        "arch": arch,
        "artifact_types": sorted(
            types
        ),
        "artifact_count": count,
        "origin": (
            "filesystem-discovery"
        ),
    }

    with tempfile.TemporaryDirectory(
        prefix="kuzpkg-untracked-"
    ) as temporary_directory:
        temporary_directory = Path(
            temporary_directory
        )

        tarpath = (
            temporary_directory
            / "package.kuzpkg.tar"
        )

        metadata_path = (
            temporary_directory
            / ".KUZPKG-METADATA.json"
        )

        metadata_path.write_text(
            json.dumps(
                metadata,
                indent=2,
            )
            + "\n"
        )

        with tarfile.open(
            tarpath,
            "w",
        ) as tar:
            tar.add(
                metadata_path,
                arcname=(
                    ".KUZPKG-METADATA.json"
                ),
            )

            added = set()

            for path in evidence:
                item = rel(
                    path,
                    root,
                )

                if item in added:
                    continue

                tar.add(
                    path,
                    arcname=item,
                    recursive=False,
                )

                added.add(item)

        subprocess.run(
            [
                zstd,
                "-q",
                "-f",
                "-o",
                str(output),
                str(tarpath),
            ],
            check=True,
        )

    return (
        output,
        version,
        source,
        True,
    )


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description=(
            "Discover software missing from "
            "kuzpkg LocalDB and package it"
        )
    )

    parser.add_argument(
        "--root",
        default="/",
        help=(
            "alternate installation "
            "root/chroot"
        ),
    )

    parser.add_argument(
        "--minimum",
        type=int,
        default=1,
        help=(
            "minimum number of detected "
            "artifacts"
        ),
    )

    parser.add_argument(
        "-q",
        "--quiet",
        action="store_true",
        help="only print package names",
    )

    parser.add_argument(
        "--no-package",
        action="store_true",
        help=(
            "only discover; do not "
            "create archives"
        ),
    )

    parser.add_argument(
        "--output-dir",
        default="/var/lib/kuzpkg/pkg",
        help=(
            "directory for generated "
            "packages"
        ),
    )

    parser.add_argument(
        "--pkgrel",
        default="1",
        help="package release number",
    )

    parser.add_argument(
        "--arch",
        help=(
            "override architecture; "
            "default is uname -m"
        ),
    )

    parser.add_argument(
        "--overwrite",
        action="store_true",
        help=(
            "overwrite existing package "
            "archives"
        ),
    )

    args = parser.parse_args()

    if args.minimum < 1:
        parser.error(
            "--minimum must be at least 1"
        )

    try:
        # Avoid print(..., end=...) entirely.
        write_stdout(
            f"{BOLD}Detecting packages...{RESET} ",
            flush=True,
        )

        candidates = discover(
            args.root,
            args.minimum,
        )

        root = Path(
            args.root
        ).resolve()

        arch, arch_source = detect_arch(
            args.arch
        )

        print_line(
            f"{CHECK}, "
            f"{len(candidates)} package(s)"
        )

    except RuntimeError as exc:
        write_stderr(
            "kuzpkg-untracked: "
            f"error: {exc}\n"
        )

        return 1

    if not candidates:
        print_line(
            "No unregistered package "
            "candidates found"
        )

        return 0

    # Discovery-only mode.
    if args.no_package:
        for (
            name,
            count,
            types,
            evidence,
        ) in candidates:
            version, source = detect_version(
                name,
                evidence,
                root,
            )

            if args.quiet:
                print_line(name)
                continue

            print_line(
                f"{name} "
                f"{version} "
                f"({count} artifact(s); "
                f"{source}; "
                f"arch={arch})"
            )

        return 0

    output_dir = Path(
        args.output_dir
    ).resolve()

    total = len(candidates)

    created = 0
    skipped = 0
    failed = 0

    for index, (
        name,
        count,
        types,
        evidence,
    ) in enumerate(
        candidates,
        1,
    ):
        print_line(
            f"{BOLD}"
            f"Compressing {name}..."
            f"{RESET}"
        )

        # Initial progress state.
        write_stdout(
            "  "
            + progress_bar(
                index - 1,
                total,
            )
            + "\r",
            flush=True,
        )

        try:
            (
                output,
                _,
                _,
                was_created,
            ) = make_archive(
                name,
                count,
                types,
                evidence,
                root,
                output_dir,
                args.pkgrel,
                arch,
                args.overwrite,
            )

            # Clear/rewrite the progress line.
            write_stdout(
                "  "
                + progress_bar(
                    index,
                    total,
                )
                + "\n",
                flush=True,
            )

            if was_created:
                created += 1
            else:
                skipped += 1

        except (
            OSError,
            subprocess.SubprocessError,
            RuntimeError,
        ) as exc:
            write_stderr(
                "  "
                + progress_bar(
                    index,
                    total,
                )
                + f" FAILED: {exc}\n",
                flush=True,
            )

            failed += 1

    if failed:
        print_line(
            f"Compressed {created} "
            f"detected package(s); "
            f"skipped {skipped}; "
            f"failed {failed}"
        )

        return 1

    print_line(
        "Compressed all detected "
        "packages to "
        f"{output_dir}"
    )

    return 0


if __name__ == "__main__":
    raise SystemExit(
        main()
    )
