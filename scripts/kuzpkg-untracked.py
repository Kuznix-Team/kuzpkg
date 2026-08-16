#!/usr/bin/env python3
# Generic filesystem adoption/discovery helper for kuzpkg.
# Archives use:
#
#   name-pkgver-pkgrel-arch.kuzpkg.tar.zst
#
# The scanner is intentionally package-oriented. It does not treat arbitrary
# files such as .png, .postinst, .rbs, .pod, .md, .html, or random .pyc files
# as independent packages.

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


# ============================================================================
# Configuration
# ============================================================================

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

# Only these top-level trees are scanned by the broad filesystem scanner.
SCAN_TOPLEVEL = {
    "bin",
    "lib",
    "lib32",
    "lib64",
    "libx32",
    "sbin",
    "usr",
}

# Trees that must never be recursively scanned.
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

# Recognized module/install roots.
MODULE_ROOTS = (
    "/usr/lib/python",
    "/usr/lib64/python",
    "/usr/local/lib/python",
    "/usr/local/lib64/python",
    "/usr/lib/site-packages",
    "/usr/lib64/site-packages",
    "/usr/local/lib/site-packages",
    "/usr/local/lib64/site-packages",
    "/usr/lib/dist-packages",
    "/usr/lib64/dist-packages",
    "/usr/local/lib/dist-packages",
    "/usr/local/lib64/dist-packages",
    "/usr/lib/ruby",
    "/usr/lib64/ruby",
    "/usr/local/lib/ruby",
    "/usr/local/lib64/ruby",
    "/usr/lib/node_modules",
    "/usr/local/lib/node_modules",
    "/usr/lib/cargo",
    "/usr/local/lib/cargo",
    "/usr/share/nodejs",
)

MODULE_EXTENSIONS = {
    ".py",
    ".pyc",
    ".pyo",
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
}

MODULE_METADATA = {
    "pyproject.toml": "python",
    "setup.py": "python",
    "setup.cfg": "python",
    "PKG-INFO": "python",
    "METADATA": "python",
    "Cargo.toml": "rust-cargo",
    "Cargo.lock": "rust-cargo",
    "Gemfile": "ruby",
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

GENERIC_BINARY_SUFFIXES = {
    ".so",
    ".a",
    ".dll",
    ".dylib",
    ".jar",
    ".wasm",
}

PACKAGE_DATA_SUFFIXES = {
    ".pc",
}

PYTHON_SUFFIXES = {
    ".py",
    ".pyc",
    ".pyo",
    ".pyd",
}

NODE_SUFFIXES = {
    ".js",
    ".mjs",
    ".cjs",
    ".node",
}

RUBY_SUFFIXES = {
    ".rb",
    ".rake",
    ".gem",
}

PERL_SUFFIXES = {
    ".pm",
}

RUST_SUFFIXES = {
    ".rs",
    ".rlib",
    ".rmeta",
    ".crate",
}

JAVA_SUFFIXES = {
    ".jar",
    ".class",
    ".war",
    ".ear",
}

PHP_SUFFIXES = {
    ".php",
    ".phar",
}

LUA_SUFFIXES = {
    ".lua",
    ".luac",
}

TCL_SUFFIXES = {
    ".tcl",
    ".tm",
}

VERSION_RE = re.compile(
    r"(?:version|release|v)?\s*"
    r"([0-9]+(?:\.[0-9A-Za-z]+)+(?:[-+._][0-9A-Za-z.-]+)?)",
    re.IGNORECASE,
)

PYTHON_ABI_RE = re.compile(
    r"\.cpython-\d+(?:-[A-Za-z0-9_]+)*$",
    re.IGNORECASE,
)

PYTHON_OPT_RE = re.compile(
    r"\.opt-[12]$",
    re.IGNORECASE,
)

SONAME_RE = re.compile(
    r"\.so(?:\.[0-9A-Za-z._-]+)*$",
    re.IGNORECASE,
)


# ============================================================================
# Terminal output
# ============================================================================

def detect_unicode_support():
    if os.environ.get("KUZPKG_ASCII") == "1":
        return False

    encoding = (
        getattr(sys.stdout, "encoding", None) or ""
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


def stdout_write(text="", flush=False):
    sys.stdout.write(str(text))

    if flush:
        sys.stdout.flush()


def stderr_write(text="", flush=False):
    sys.stderr.write(str(text))

    if flush:
        sys.stderr.flush()


def println(text=""):
    sys.stdout.write(str(text) + "\n")


def progress_bar(current, total, width=32):
    if total <= 0:
        return f"[{EMPTY * width}] 0/0"

    current = max(0, min(current, total))
    filled = int(width * current / total)

    return (
        f"[{FILL * filled}"
        f"{EMPTY * (width - filled)}] "
        f"{current}/{total}"
    )


# ============================================================================
# kuzpkg helpers
# ============================================================================

def run_kuzpkg(root, args):
    command = (
        ["kuzpkg"]
        + ([] if root == "/" else ["--root", root])
        + list(args)
    )

    try:
        process = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
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
    root = Path(root)

    if root == Path("/"):
        return Path("/") / path.lstrip("/")

    return root / path.lstrip("/")


def rel(path, root):
    path = Path(path)
    root = Path(root)

    return norm(path.relative_to(root))


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
                item = item.split(None, 1)[1]

            owned.add(norm(item))

    return packages, owned


# ============================================================================
# Filesystem helpers
# ============================================================================

def is_under_root(path, root):
    try:
        Path(path).relative_to(Path(root))
        return True
    except ValueError:
        return False


def excluded_dir(path, root):
    path = Path(path)
    root = Path(root)

    try:
        relative = path.relative_to(root)
    except ValueError:
        return True

    if not relative.parts:
        return False

    return relative.parts[0] in EXCLUDED_TOPLEVEL


def scan_tree(root, predicate):
    root = Path(root)
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
                directory not in EXCLUDED_TOPLEVEL
                and not (
                    current_path / directory
                ).is_symlink()
            )
        ]

        for filename in filenames:
            path = current_path / filename

            try:
                valid = (
                    path.is_file()
                    or path.is_symlink()
                )
            except OSError:
                valid = False

            if valid and predicate(path):
                result.append(path)

    return result


def scan_files(root, directories, predicate):
    root = Path(root)
    result = []

    for directory, prefix in directories:
        base = root_path(
            str(root),
            directory,
        )

        if (
            not base.exists()
            or not base.is_dir()
            or excluded_dir(base, root)
        ):
            continue

        for current, dirnames, filenames in os.walk(
            base,
            topdown=True,
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
                path = current_path / filename

                try:
                    valid = (
                        path.is_file()
                        or path.is_symlink()
                    )
                except OSError:
                    valid = False

                if valid and predicate(path):
                    result.append(
                        (path, prefix)
                    )

    return result


def scan_firefox(root):
    root = Path(root)
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
            topdown=True,
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


def scan_module_roots(root):
    root = Path(root)
    result = []

    for directory in MODULE_ROOTS:
        base = root_path(
            str(root),
            directory,
        )

        if (
            not base.exists()
            or not base.is_dir()
            or excluded_dir(base, root)
        ):
            continue

        for current, dirnames, filenames in os.walk(
            base,
            topdown=True,
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
                path = current_path / filename

                try:
                    if not (
                        path.is_file()
                        or path.is_symlink()
                    ):
                        continue
                except OSError:
                    continue

                kind = module_kind(path)

                if kind is None:
                    continue

                result.append(
                    (
                        path,
                        kind,
                        "",
                    )
                )

    return result


# ============================================================================
# Module classification
# ============================================================================

def module_kind(path):
    path = Path(path)

    suffix = path.suffix.lower()
    lower_parts = {
        part.lower()
        for part in path.parts
    }

    name = path.name

    metadata_kind = MODULE_METADATA.get(name)

    if metadata_kind is not None:
        return metadata_kind

    if (
        suffix in PYTHON_SUFFIXES
        or "site-packages" in lower_parts
        or "dist-packages" in lower_parts
    ):
        return "python"

    if (
        suffix in RUST_SUFFIXES
        or "cargo" in lower_parts
        or "rustlib" in lower_parts
    ):
        return "rust-cargo"

    if (
        suffix in RUBY_SUFFIXES
        or "gems" in lower_parts
        or "vendor_ruby" in lower_parts
    ):
        return "ruby"

    if (
        suffix in NODE_SUFFIXES
        or "node_modules" in lower_parts
        or "nodejs" in lower_parts
    ):
        return "nodejs"

    if (
        suffix in PERL_SUFFIXES
        or "perl" in lower_parts
    ):
        return "perl"

    if (
        suffix == ".go"
        or "gopath" in lower_parts
        or "pkg/mod" in lower_parts
    ):
        return "go"

    if (
        suffix in JAVA_SUFFIXES
        or "java" in lower_parts
    ):
        return "java-jvm"

    if (
        suffix in PHP_SUFFIXES
        or "composer" in lower_parts
    ):
        return "php"

    if (
        suffix in LUA_SUFFIXES
        or "lua" in lower_parts
    ):
        return "lua"

    if (
        suffix in TCL_SUFFIXES
        or "tcl" in lower_parts
    ):
        return "tcl"

    if suffix == ".wasm":
        return "wasm"

    return None


# ============================================================================
# Package-name normalization
# ============================================================================

def normalize_python_name(name):
    """
    Convert names such as:

        typing.cpython-314.pyc
        typing.cpython-313.opt-1.pyc
        typing.cpython-313.opt-2.pyc
        foo.cpython-314-x86_64-linux-gnu.so

    into stable package/module names.
    """

    value = name

    # Remove Python suffix.
    for suffix in (
        ".pyc",
        ".pyo",
        ".py",
    ):
        if value.lower().endswith(suffix):
            value = value[:-len(suffix)]
            break

    # Remove optimization suffix.
    value = PYTHON_OPT_RE.sub(
        "",
        value,
    )

    # Remove CPython ABI suffix.
    value = PYTHON_ABI_RE.sub(
        "",
        value,
    )

    # Remove common ABI suffixes from extension modules.
    value = re.sub(
        r"\.cpython-\d+"
        r"(?:-[A-Za-z0-9_]+)*$",
        "",
        value,
        flags=re.IGNORECASE,
    )

    # Remove common implementation suffixes.
    value = re.sub(
        r"\.(?:abi\d+|pypy\d+)$",
        "",
        value,
        flags=re.IGNORECASE,
    )

    return value


def package_guess(path):
    path = Path(path)

    name = path.name
    lower = name.lower()
    suffix = path.suffix.lower()

    # ------------------------------------------------------------
    # pkg-config
    # ------------------------------------------------------------
    if lower.endswith(".pc"):
        return name[:-3]

    # ------------------------------------------------------------
    # Python
    # ------------------------------------------------------------
    if suffix in PYTHON_SUFFIXES:
        return normalize_python_name(name)

    # ------------------------------------------------------------
    # Python metadata
    # ------------------------------------------------------------
    if name.endswith(".egg-info"):
        return name[:-9]

    if name.endswith(".dist-info"):
        return name[:-10]

    # ------------------------------------------------------------
    # Shared libraries
    # ------------------------------------------------------------
    shared_object = SONAME_RE.sub(
        "",
        name,
    )

    if shared_object != name:
        if shared_object.startswith("lib"):
            shared_object = shared_object[3:]

        return shared_object

    # ------------------------------------------------------------
    # Static libraries
    # ------------------------------------------------------------
    if lower.endswith(".a"):
        value = name[:-2]

        if value.startswith("lib"):
            value = value[3:]

        return value

    # ------------------------------------------------------------
    # Native modules
    # ------------------------------------------------------------
    if lower.endswith(".node"):
        return name[:-5]

    if lower.endswith(".pyd"):
        return normalize_python_name(name)

    # ------------------------------------------------------------
    # Rust
    # ------------------------------------------------------------
    for suffix_value in (
        ".rlib",
        ".rmeta",
        ".crate",
    ):
        if lower.endswith(suffix_value):
            return name[
                :-len(suffix_value)
            ]

    # ------------------------------------------------------------
    # Java
    # ------------------------------------------------------------
    for suffix_value in (
        ".jar",
        ".war",
        ".ear",
    ):
        if lower.endswith(suffix_value):
            return name[
                :-len(suffix_value)
            ]

    # ------------------------------------------------------------
    # Node
    # ------------------------------------------------------------
    if suffix in {
        ".js",
        ".mjs",
        ".cjs",
    }:
        for parent in (
            path.parent,
            *path.parents,
        ):
            parent_name = parent.name

            if parent_name == "node_modules":
                continue

            if (
                parent_name
                and parent_name not in {
                    "node",
                    "nodejs",
                }
            ):
                return parent_name

        return name.rsplit(
            ".",
            1,
        )[0]

    # ------------------------------------------------------------
    # Ruby
    # ------------------------------------------------------------
    if lower.endswith(".gemspec"):
        return name[:-8]

    if lower.endswith(".gem"):
        return name[:-4]

    # ------------------------------------------------------------
    # Explicit generic package-like names
    # ------------------------------------------------------------
    if name.startswith("lib") and "." in name:
        return name.split(
            ".",
            1,
        )[0][3:]

    return None


# ============================================================================
# Version detection
# ============================================================================

def version_from_text(text):
    for line in text.splitlines()[:100]:
        match = VERSION_RE.search(line)

        if match:
            return match.group(1)

    return None


def probe_version(path):
    path = Path(path)

    try:
        executable = (
            path.is_file()
            and os.access(
                path,
                os.X_OK,
            )
        )
    except OSError:
        executable = False

    if not executable:
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


def detect_version(name, evidence, root):
    root = Path(root)

    base = name
    base = base.removeprefix("lib32-")
    base = base.removeprefix("libx32-")

    candidates = []

    for directory in (
        *BIN_DIRS,
        *FIREFOX_DIRS,
    ):
        candidates.append(
            root_path(
                str(root),
                directory,
            ) / base
        )

    candidates.extend(
        Path(path)
        for path in evidence
    )

    seen = set()

    for path in candidates:
        path = Path(path)
        key = str(path)

        if key in seen:
            continue

        seen.add(key)

        version = probe_version(path)

        if version:
            return (
                version,
                "version-probe",
            )

    if name == "firefox":
        for metadata_name in (
            "/usr/lib/firefox/platform.ini",
            "/lib/firefox/platform.ini",
        ):
            metadata_path = root_path(
                str(root),
                metadata_name,
            )

            try:
                match = re.search(
                    r"^Version=([^\n]+)",
                    metadata_path.read_text(
                        errors="replace",
                    ),
                    re.MULTILINE,
                )

                if match:
                    return (
                        match.group(1).strip(),
                        "firefox-platform.ini",
                    )

            except OSError:
                pass

    metadata_files = (
        "VERSION",
        "version",
        "VERSION.txt",
        "PKG-INFO",
        "METADATA",
        "pyproject.toml",
        "Cargo.toml",
        "gemspec",
        "package.json",
    )

    for evidence_path in evidence:
        evidence_path = Path(
            evidence_path
        )

        for parent in (
            evidence_path.parent,
            *evidence_path.parents,
        ):
            for metadata_name in metadata_files:
                metadata_path = (
                    parent
                    / metadata_name
                )

                try:
                    if not metadata_path.is_file():
                        continue

                    text = metadata_path.read_text(
                        errors="replace",
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


# ============================================================================
# Architecture
# ============================================================================

def detect_arch(explicit=None):
    if explicit:
        return (
            explicit,
            "explicit",
        )

    try:
        architecture = os.uname().machine
    except AttributeError:
        architecture = platform.machine()

    if architecture:
        return (
            architecture,
            "uname",
        )

    return (
        "unknown",
        "unknown",
    )


# ============================================================================
# Candidate evidence
# ============================================================================

def candidate_add(
    candidates,
    name,
    path,
    kind,
):
    if not name:
        return

    item = candidates[name]

    item["count"] += 1
    item["types"].add(kind)

    if len(
        item["evidence"]
    ) < 50:
        item["evidence"].append(
            Path(path)
        )


# ============================================================================
# Discovery
# ============================================================================

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

    # ------------------------------------------------------------------
    # Native shared libraries
    # ------------------------------------------------------------------
    for path, prefix in scan_files(
        root,
        LIB_DIRS,
        lambda path: (
            ".so" in path.name
        ),
    ):
        try:
            key = rel(path, root)
        except ValueError:
            continue

        if key in owned:
            continue

        name = package_guess(path)

        if not name:
            continue

        name = prefix + name

        if name in packages:
            continue

        candidate_add(
            candidates,
            name,
            path,
            "library",
        )

    # ------------------------------------------------------------------
    # Native binaries
    # ------------------------------------------------------------------
    for path, prefix in scan_files(
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
    ):
        try:
            key = rel(path, root)
        except ValueError:
            continue

        if key in owned:
            continue

        name = package_guess(path)

        if not name:
            # For normal binaries, the filename itself is a useful
            # package hint.
            name = path.name

        if name in packages:
            continue

        candidate_add(
            candidates,
            name,
            path,
            "binary",
        )

    # ------------------------------------------------------------------
    # pkg-config
    # ------------------------------------------------------------------
    for path, prefix in scan_files(
        root,
        PC_DIRS,
        lambda path: (
            path.name.endswith(".pc")
        ),
    ):
        try:
            key = rel(path, root)
        except ValueError:
            continue

        if key in owned:
            continue

        name = package_guess(path)

        if not name:
            continue

        name = prefix + name

        if name in packages:
            continue

        candidate_add(
            candidates,
            name,
            path,
            "pkgconfig",
        )

    # ------------------------------------------------------------------
    # Firefox
    # ------------------------------------------------------------------
    firefox_paths = scan_firefox(root)

    if firefox_paths:
        for path in firefox_paths:
            try:
                key = rel(path, root)
            except ValueError:
                continue

            if key in owned:
                continue

            candidate_add(
                candidates,
                "firefox",
                path,
                "firefox",
            )

    # ------------------------------------------------------------------
    # Recognized language/module trees only.
    # ------------------------------------------------------------------
    for path, kind, prefix in scan_module_roots(root):
        try:
            key = rel(path, root)
        except ValueError:
            continue

        if key in owned:
            continue

        name = package_guess(path)

        if not name:
            continue

        name = prefix + name

        if name in packages:
            continue

        candidate_add(
            candidates,
            name,
            path,
            kind,
        )

    # ------------------------------------------------------------------
    # Generic binary discovery.
    #
    # Only scan selected package installation trees and only recognize
    # actual binary/archive suffixes. This deliberately excludes:
    #
    #   .png
    #   .md
    #   .html
    #   .postinst
    #   .postrm
    #   .prerm
    #   .rbs
    #   .pod
    #   random source files
    #
    # ------------------------------------------------------------------
    generic_roots = (
        "/bin",
        "/sbin",
        "/usr/bin",
        "/usr/sbin",
        "/usr/lib",
        "/usr/lib64",
        "/lib",
        "/lib64",
    )

    generic_suffixes = GENERIC_BINARY_SUFFIXES

    def generic_binary(path):
        suffix = path.suffix.lower()

        if suffix in generic_suffixes:
            return True

        try:
            return (
                path.is_file()
                and os.access(
                    path,
                    os.X_OK,
                )
                and "." not in path.name
            )
        except OSError:
            return False

    for directory in generic_roots:
        base = root_path(
            str(root),
            directory,
        )

        if (
            not base.exists()
            or not base.is_dir()
            or excluded_dir(base, root)
        ):
            continue

        for path in scan_tree(
            base,
            generic_binary,
        ):
            try:
                key = rel(
                    path,
                    root,
                )
            except ValueError:
                continue

            if key in owned:
                continue

            name = package_guess(path)

            if not name:
                if (
                    path.suffix.lower()
                    in generic_suffixes
                ):
                    stem = path.name

                    if stem.startswith("lib"):
                        stem = stem[3:]

                    stem = re.sub(
                        r"\.[^.]+$",
                        "",
                        stem,
                    )

                    name = stem
                else:
                    name = path.name

            if not name:
                continue

            if name in packages:
                continue

            candidate_add(
                candidates,
                name,
                path,
                "generic",
            )

    # ------------------------------------------------------------------
    # Filter tiny/noisy candidates.
    # ------------------------------------------------------------------
    result = []

    for name, item in candidates.items():
        if item["count"] < minimum:
            continue

        evidence = sorted(
            {
                Path(path)
                for path in item["evidence"]
            },
            key=str,
        )

        result.append(
            (
                name,
                item["count"],
                item["types"],
                evidence,
            )
        )

    result.sort(
        key=lambda item: (
            -item[1],
            item[0].lower(),
        )
    )

    return result


# ============================================================================
# Archive creation
# ============================================================================

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
    root = Path(root)
    outdir = Path(outdir)

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

    output = outdir / filename

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
        "artifact_types": sorted(types),
        "artifact_count": count,
        "origin": "filesystem-discovery",
    }

    with tempfile.TemporaryDirectory(
        prefix="kuzpkg-untracked-",
    ) as temp_directory:
        temp_directory = Path(
            temp_directory
        )

        tar_path = (
            temp_directory
            / "package.kuzpkg.tar"
        )

        metadata_path = (
            temp_directory
            / ".KUZPKG-METADATA.json"
        )

        metadata_path.write_text(
            json.dumps(
                metadata,
                indent=2,
            )
            + "\n"
        )

        added = set()

        with tarfile.open(
            tar_path,
            "w",
        ) as tar:
            tar.add(
                metadata_path,
                arcname=(
                    ".KUZPKG-METADATA.json"
                ),
            )

            for path in evidence:
                path = Path(path)

                try:
                    item = rel(
                        path,
                        root,
                    )
                except ValueError:
                    continue

                if item in added:
                    continue

                try:
                    tar.add(
                        path,
                        arcname=item,
                        recursive=False,
                    )
                except (
                    OSError,
                    ValueError,
                ):
                    continue

                added.add(item)

        subprocess.run(
            [
                zstd,
                "-q",
                "-f",
                "-o",
                str(output),
                str(tar_path),
            ],
            check=True,
        )

    return (
        output,
        version,
        source,
        True,
    )


# ============================================================================
# Main
# ============================================================================

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
            "minimum number of artifacts "
            "required for a candidate"
        ),
    )

    parser.add_argument(
        "-q",
        "--quiet",
        action="store_true",
        help=(
            "print only detected package names "
            "without verbose artifact information"
        ),
    )

    parser.add_argument(
        "--no-package",
        action="store_true",
        help=(
            "discover only; do not create "
            "kuzpkg archives"
        ),
    )

    parser.add_argument(
        "--output-dir",
        default="/var/lib/kuzpkg/pkg",
        help=(
            "directory for generated packages"
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
        stdout_write(
            f"{BOLD}"
            "Detecting packages..."
            f"{RESET} ",
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

        println(
            f"{CHECK}, "
            f"{len(candidates)} package(s)"
        )

    except RuntimeError as exc:
        stderr_write(
            "kuzpkg-untracked: "
            f"error: {exc}\n",
            flush=True,
        )
        return 1

    if not candidates:
        println(
            "No unregistered package "
            "candidates found"
        )
        return 0

    # ------------------------------------------------------------------
    # Quiet mode.
    #
    # Example:
    #
    #   python3 kuzpkg-untracked.py -q
    #
    # Output:
    #
    #   bash
    #   gcc
    #   python
    #
    # ------------------------------------------------------------------
    if args.quiet:
        for (
            name,
            count,
            types,
            evidence,
        ) in candidates:
            println(name)

        return 0

    # ------------------------------------------------------------------
    # Discovery-only mode.
    # ------------------------------------------------------------------
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

            println(
                f"{name:<40} "
                f"{version:<24} "
                f"{count:>4} artifact(s) "
                f"[{source}; "
                f"arch={arch}]"
            )

        return 0

    # ------------------------------------------------------------------
    # Packaging mode.
    # ------------------------------------------------------------------
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
        start=1,
    ):
        println(
            f"{BOLD}"
            f"Compressing {name}..."
            f"{RESET}"
        )

        stdout_write(
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
                _version,
                _source,
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

            stdout_write(
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
            stderr_write(
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
        println(
            f"Compressed {created} "
            f"detected package(s); "
            f"skipped {skipped}; "
            f"failed {failed}"
        )

        return 1

    println(
        "Compressed all detected packages "
        f"to {output_dir}"
    )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
