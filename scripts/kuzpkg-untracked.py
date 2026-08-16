#!/usr/bin/env python3
# Generic filesystem adoption/discovery helper for kuzpkg.
#
# Package names are NOT taken directly from arbitrary filesystem filenames.
# Local filesystem artifacts are treated as evidence only.
#
# Valid package names are obtained from Arch Linux repository directory
# listings. Only files matching:
#
#   name-pkgver-pkgrel-arch.pkg.tar.zst
#
# are considered, and only "name" is extracted.
#
# Example:
#
#   git-2.51.0-1-x86_64.pkg.tar.zst
#
# becomes:
#
#   git
#
# The scanner then checks whether local filesystem evidence can reasonably
# correspond to one of those real Arch package names.


import argparse
import html.parser
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
from collections import defaultdict
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.parse import urljoin
from urllib.request import Request, urlopen


# ============================================================================
# Configuration
# ============================================================================

ARCH_REPOSITORIES = (
    "core",
    "extra",
    "multilib",
    "core-testing",
    "extra-testing",
    "multilib-testing",
    "core-staging",
    "extra-staging",
    "multilib-staging",
)

ARCH_MIRROR_BASES = (
    # ICM mirror layout currently exposed by the server.
    "https://ftp.icm.edu.pl/pub/Linux/dist/archlinux",
    "http://ftp.icm.edu.pl/pub/Linux/dist/archlinux",

    # Also accept the path form supplied by the user.
    "https://ftp.icm.edu.pl/Linux/dist/archlinux",
    "http://ftp.icm.edu.pl/Linux/dist/archlinux",
)

ARCH_CACHE_DEFAULT = (
    "/var/cache/kuzpkg/arch-package-names.json"
)

ARCH_ARCH = "x86_64"

ARCH_PACKAGE_RE = re.compile(
    r"^(.+)-"
    r"([0-9][A-Za-z0-9:+._]*?)-"
    r"([0-9]+)-"
    r"([A-Za-z0-9_]+)"
    r"\.pkg\.tar"
    r"(?:\.[A-Za-z0-9]+)+$"
)

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

    "/usr/share/nodejs",

    "/usr/lib/cargo",
    "/usr/local/lib/cargo",
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

GENERIC_BINARY_SUFFIXES = {
    ".so",
    ".a",
    ".dll",
    ".dylib",
    ".node",
    ".jar",
    ".wasm",
}

PYTHON_SUFFIXES = {
    ".py",
    ".pyc",
    ".pyo",
    ".pyd",
}

VERSION_RE = re.compile(
    r"(?:version|release|v)?\s*"
    r"([0-9]+(?:\.[0-9A-Za-z]+)+"
    r"(?:[-+._][0-9A-Za-z.-]+)?)",
    re.IGNORECASE,
)

PYTHON_ABI_RE = re.compile(
    r"\.cpython-\d+"
    r"(?:-[A-Za-z0-9_]+)*$",
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

BINARY_SEPARATOR_RE = re.compile(
    r"[-_]"
)


# ============================================================================
# Terminal output
# ============================================================================

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

    current = max(
        0,
        min(current, total),
    )

    filled = int(
        width * current / total
    )

    return (
        f"[{FILL * filled}"
        f"{EMPTY * (width - filled)}] "
        f"{current}/{total}"
    )


# ============================================================================
# Generic HTTP / repository listing support
# ============================================================================

class LinkParser(html.parser.HTMLParser):
    def __init__(self):
        super().__init__()
        self.links = []

    def handle_starttag(self, tag, attrs):
        if tag.lower() != "a":
            return

        for key, value in attrs:
            if key.lower() == "href" and value:
                self.links.append(value)


def fetch_url(url, timeout=30):
    request = Request(
        url,
        headers={
            "User-Agent": (
                "kuzpkg-untracked/"
                "1.0"
            )
        },
    )

    try:
        with urlopen(
            request,
            timeout=timeout,
        ) as response:
            return response.read()

    except HTTPError as exc:
        raise RuntimeError(
            f"HTTP {exc.code} while fetching {url}"
        ) from exc

    except URLError as exc:
        raise RuntimeError(
            f"network error while fetching {url}: "
            f"{exc.reason}"
        ) from exc

    except OSError as exc:
        raise RuntimeError(
            f"cannot fetch {url}: {exc}"
        ) from exc


def parse_repository_listing(
    html_data,
    base_url,
):
    parser = LinkParser()

    try:
        text = html_data.decode(
            "utf-8",
            errors="replace",
        )
    except AttributeError:
        text = str(html_data)

    parser.feed(text)

    package_names = set()

    for href in parser.links:
        filename = urljoin(
            base_url,
            href,
        ).rstrip("/").split("/")[-1]

        match = ARCH_PACKAGE_RE.match(
            filename
        )

        if not match:
            continue

        name = match.group(1)

        if not name:
            continue

        package_names.add(name)

    return package_names


def repository_url_candidates(
    repository,
    architecture,
):
    suffix = (
        f"/{repository}/os/"
        f"{architecture}/"
    )

    for base in ARCH_MIRROR_BASES:
        yield base.rstrip("/") + suffix


def load_arch_package_names(
    cache_path,
    refresh=False,
    quiet=False,
):
    cache_path = Path(cache_path)

    if (
        cache_path.exists()
        and not refresh
    ):
        try:
            data = json.loads(
                cache_path.read_text(
                    errors="replace"
                )
            )

            if (
                isinstance(data, dict)
                and isinstance(
                    data.get("packages"),
                    list,
                )
            ):
                packages = set(
                    str(value)
                    for value in data["packages"]
                )

                if packages:
                    return packages

        except (
            OSError,
            json.JSONDecodeError,
            TypeError,
        ):
            pass

    all_packages = set()
    successful_repositories = []
    last_errors = []

    if not quiet:
        println(
            f"{BOLD}"
            "Loading Arch package indexes..."
            f"{RESET}"
        )

    for repository in ARCH_REPOSITORIES:
        success = False

        for url in repository_url_candidates(
            repository,
            ARCH_ARCH,
        ):
            try:
                html_data = fetch_url(url)

                packages = parse_repository_listing(
                    html_data,
                    url,
                )

                if not packages:
                    raise RuntimeError(
                        "repository listing contained "
                        "no Arch package archives"
                    )

                all_packages.update(packages)

                successful_repositories.append(
                    repository
                )

                success = True

                if not quiet:
                    println(
                        f"  {repository:<18} "
                        f"{len(packages):>6} packages"
                    )

                break

            except RuntimeError as exc:
                last_errors.append(
                    f"{repository}: {exc}"
                )

        if not success and not quiet:
            println(
                f"  {repository:<18} "
                "unavailable"
            )

    if not all_packages:
        detail = (
            "; ".join(last_errors[-3:])
            if last_errors
            else "no usable mirror listings"
        )

        raise RuntimeError(
            "could not load any Arch package "
            f"repository indexes: {detail}"
        )

    cache_data = {
        "generated": int(time.time()),
        "architecture": ARCH_ARCH,
        "repositories": (
            successful_repositories
        ),
        "packages": sorted(
            all_packages
        ),
    }

    try:
        cache_path.parent.mkdir(
            parents=True,
            exist_ok=True,
        )

        temporary_path = (
            cache_path.with_suffix(
                cache_path.suffix + ".tmp"
            )
        )

        temporary_path.write_text(
            json.dumps(
                cache_data,
                indent=2,
            )
            + "\n"
        )

        temporary_path.replace(
            cache_path
        )

    except OSError:
        # Cache failure should not stop discovery.
        pass

    if not quiet:
        println(
            f"{CHECK}, "
            f"{len(all_packages)} unique "
            "Arch package names"
        )

    return all_packages


# ============================================================================
# kuzpkg helpers
# ============================================================================

def run_kuzpkg(root, args):
    command = (
        ["kuzpkg"]
        + (
            []
            if root == "/"
            else ["--root", root]
        )
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
    return norm(
        Path(path).relative_to(
            Path(root)
        )
    )


def load_local_db(root):
    packages = set(
        run_kuzpkg(
            root,
            ["-Qq"],
        )
    )

    owned = set()

    for package in sorted(packages):
        for item in run_kuzpkg(
            root,
            ["-Qql", package],
        ):
            if item.startswith(
                package + " "
            ):
                item = item.split(
                    None,
                    1,
                )[1]

            owned.add(
                norm(item)
            )

    return packages, owned


# ============================================================================
# Filesystem scanning
# ============================================================================

def excluded_dir(path, root):
    path = Path(path)
    root = Path(root)

    try:
        relative = path.relative_to(root)
    except ValueError:
        return True

    if not relative.parts:
        return False

    return (
        relative.parts[0]
        in EXCLUDED_TOPLEVEL
    )


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
            or excluded_dir(
                base,
                root,
            )
        ):
            continue

        for current, dirnames, filenames in os.walk(
            base,
            topdown=True,
            followlinks=False,
        ):
            current_path = Path(current)

            dirnames[:] = [
                dirname
                for dirname in dirnames
                if not (
                    current_path / dirname
                ).is_symlink()
            ]

            for filename in filenames:
                path = (
                    current_path
                    / filename
                )

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
            dirname
            for dirname in dirnames
            if (
                dirname not in EXCLUDED_TOPLEVEL
                and not (
                    current_path / dirname
                ).is_symlink()
            )
        ]

        for filename in filenames:
            path = (
                current_path
                / filename
            )

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


def scan_firefox(root):
    result = []

    for directory in FIREFOX_DIRS:
        base = root_path(
            str(root),
            directory,
        )

        if (
            not base.exists()
            or not base.is_dir()
            or excluded_dir(
                base,
                root,
            )
        ):
            continue

        for current, dirnames, filenames in os.walk(
            base,
            topdown=True,
            followlinks=False,
        ):
            current_path = Path(current)

            dirnames[:] = [
                dirname
                for dirname in dirnames
                if not (
                    current_path / dirname
                ).is_symlink()
            ]

            for filename in filenames:
                result.append(
                    current_path / filename
                )

    return result


def scan_module_roots(root):
    result = []

    for directory in MODULE_ROOTS:
        base = root_path(
            str(root),
            directory,
        )

        if (
            not base.exists()
            or not base.is_dir()
            or excluded_dir(
                base,
                root,
            )
        ):
            continue

        for current, dirnames, filenames in os.walk(
            base,
            topdown=True,
            followlinks=False,
        ):
            current_path = Path(current)

            dirnames[:] = [
                dirname
                for dirname in dirnames
                if not (
                    current_path / dirname
                ).is_symlink()
            ]

            for filename in filenames:
                path = (
                    current_path
                    / filename
                )

                try:
                    valid = (
                        path.is_file()
                        or path.is_symlink()
                    )
                except OSError:
                    valid = False

                if not valid:
                    continue

                kind = module_kind(path)

                if kind:
                    result.append(
                        (
                            path,
                            kind,
                        )
                    )

    return result


# ============================================================================
# Module identification
# ============================================================================

def module_kind(path):
    path = Path(path)

    suffix = path.suffix.lower()

    lower_parts = {
        part.lower()
        for part in path.parts
    }

    name = path.name

    if name in {
        "pyproject.toml",
        "setup.py",
        "setup.cfg",
        "PKG-INFO",
        "METADATA",
    }:
        return "python"

    if (
        suffix in PYTHON_SUFFIXES
        or "site-packages" in lower_parts
        or "dist-packages" in lower_parts
    ):
        return "python"

    if (
        suffix in {
            ".rs",
            ".rlib",
            ".rmeta",
            ".crate",
        }
        or "cargo" in lower_parts
        or "rustlib" in lower_parts
    ):
        return "rust-cargo"

    if (
        suffix in {
            ".rb",
            ".rake",
            ".gem",
        }
        or "gems" in lower_parts
        or "vendor_ruby" in lower_parts
    ):
        return "ruby"

    if (
        suffix in {
            ".js",
            ".mjs",
            ".cjs",
            ".node",
        }
        or "node_modules" in lower_parts
        or "nodejs" in lower_parts
    ):
        return "nodejs"

    if suffix == ".pm":
        return "perl"

    if suffix == ".go":
        return "go"

    if suffix in {
        ".jar",
        ".class",
        ".war",
        ".ear",
    }:
        return "java-jvm"

    if suffix in {
        ".php",
        ".phar",
    }:
        return "php"

    if suffix in {
        ".lua",
        ".luac",
    }:
        return "lua"

    if suffix in {
        ".tcl",
        ".tm",
    }:
        return "tcl"

    if suffix == ".wasm":
        return "wasm"

    return None


# ============================================================================
# Local filename -> package-name hint
# ============================================================================

def normalize_python_name(name):
    value = name

    for suffix in (
        ".pyc",
        ".pyo",
        ".py",
        ".pyd",
    ):
        if value.lower().endswith(
            suffix
        ):
            value = value[
                :-len(suffix)
            ]
            break

    value = PYTHON_OPT_RE.sub(
        "",
        value,
    )

    value = PYTHON_ABI_RE.sub(
        "",
        value,
    )

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
        return normalize_python_name(
            name
        )

    # ------------------------------------------------------------
    # Python package metadata
    # ------------------------------------------------------------
    if lower.endswith(".egg-info"):
        return name[:-9]

    if lower.endswith(".dist-info"):
        return name[:-10]

    # ------------------------------------------------------------
    # Shared object
    # ------------------------------------------------------------
    shared_object = SONAME_RE.sub(
        "",
        name,
    )

    if shared_object != name:
        if shared_object.startswith(
            "lib"
        ):
            return shared_object[3:]

        return shared_object

    # ------------------------------------------------------------
    # Static library
    # ------------------------------------------------------------
    if lower.endswith(".a"):
        value = name[:-2]

        if value.startswith("lib"):
            value = value[3:]

        return value

    # ------------------------------------------------------------
    # Node native module
    # ------------------------------------------------------------
    if lower.endswith(".node"):
        return name[:-5]

    # ------------------------------------------------------------
    # Rust
    # ------------------------------------------------------------
    for suffix_value in (
        ".rlib",
        ".rmeta",
        ".crate",
    ):
        if lower.endswith(
            suffix_value
        ):
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
        if lower.endswith(
            suffix_value
        ):
            return name[
                :-len(suffix_value)
            ]

    # ------------------------------------------------------------
    # Ruby gemspec / gem
    # ------------------------------------------------------------
    if lower.endswith(
        ".gemspec"
    ):
        return name[:-8]

    if lower.endswith(".gem"):
        return name[:-4]

    # ------------------------------------------------------------
    # Ordinary executable.
    #
    # Keep the exact executable name here. It will be validated against
    # actual Arch package names later.
    # ------------------------------------------------------------
    if (
        os.access(
            path,
            os.X_OK,
        )
        and "." not in name
    ):
        return name

    return None


# ============================================================================
# Repository-aware package matching
# ============================================================================

def exact_repo_match(
    candidate,
    repo_packages,
):
    if candidate in repo_packages:
        return candidate

    return None


def package_name_variants(
    candidate,
):
    values = []

    candidate = candidate.strip()

    if not candidate:
        return values

    values.append(candidate)

    # Python module names.
    normalized_python = (
        normalize_python_name(
            candidate
        )
    )

    if (
        normalized_python
        not in values
    ):
        values.append(
            normalized_python
        )

    # Remove common library prefixes for lookup.
    if candidate.startswith(
        "lib"
    ) and len(candidate) > 3:
        stripped = candidate[3:]

        if stripped not in values:
            values.append(stripped)

    # Remove multilib local prefix.
    for prefix in (
        "lib32-",
        "libx32-",
    ):
        if candidate.startswith(prefix):
            stripped = candidate[
                len(prefix):
            ]

            if stripped not in values:
                values.append(stripped)

    return values


def find_repo_package(
    candidate,
    repo_packages,
):
    for variant in package_name_variants(
        candidate
    ):
        if variant in repo_packages:
            return variant

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

    for argument in (
        "--version",
        "-version",
    ):
        try:
            process = subprocess.run(
                [
                    str(path),
                    argument,
                ],
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
    root = Path(root)

    candidates = []

    base = name

    base = base.removeprefix(
        "lib32-"
    )
    base = base.removeprefix(
        "libx32-"
    )

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
                        errors="replace"
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

    metadata_names = (
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
            for metadata_name in metadata_names:
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
                            ).get(
                                "version"
                            )

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
# Candidate storage
# ============================================================================

def add_candidate(
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

def discover(
    root,
    minimum,
    repo_packages,
):
    root = Path(root).resolve()

    installed_packages, owned = (
        load_local_db(
            str(root)
        )
    )

    # Candidates are indexed by REAL Arch package name.
    candidates = defaultdict(
        lambda: {
            "count": 0,
            "types": set(),
            "evidence": [],
        }
    )

    # ------------------------------------------------------------------
    # Shared libraries
    # ------------------------------------------------------------------
    for path, prefix in scan_files(
        root,
        LIB_DIRS,
        lambda path: (
            ".so" in path.name
        ),
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

        guess = package_guess(path)

        if not guess:
            continue

        # For multilib evidence, try the prefixed package name first.
        prefixed = (
            prefix + guess
            if prefix
            else guess
        )

        package_name = find_repo_package(
            prefixed,
            repo_packages,
        )

        if package_name is None:
            package_name = find_repo_package(
                guess,
                repo_packages,
            )

        if package_name is None:
            continue

        if package_name in installed_packages:
            continue

        add_candidate(
            candidates,
            package_name,
            path,
            "library",
        )

    # ------------------------------------------------------------------
    # Executables
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
            key = rel(
                path,
                root,
            )
        except ValueError:
            continue

        if key in owned:
            continue

        guess = package_guess(path)

        if not guess:
            continue

        package_name = find_repo_package(
            guess,
            repo_packages,
        )

        if package_name is None:
            continue

        if package_name in installed_packages:
            continue

        add_candidate(
            candidates,
            package_name,
            path,
            "binary",
        )

    # ------------------------------------------------------------------
    # pkg-config files
    # ------------------------------------------------------------------
    for path, prefix in scan_files(
        root,
        PC_DIRS,
        lambda path: (
            path.name.endswith(".pc")
        ),
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

        guess = package_guess(path)

        if not guess:
            continue

        prefixed = (
            prefix + guess
            if prefix
            else guess
        )

        package_name = find_repo_package(
            prefixed,
            repo_packages,
        )

        if package_name is None:
            package_name = find_repo_package(
                guess,
                repo_packages,
            )

        if package_name is None:
            continue

        if package_name in installed_packages:
            continue

        add_candidate(
            candidates,
            package_name,
            path,
            "pkgconfig",
        )

    # ------------------------------------------------------------------
    # Firefox
    # ------------------------------------------------------------------
    firefox_paths = scan_firefox(root)

    if firefox_paths:
        package_name = find_repo_package(
            "firefox",
            repo_packages,
        )

        if (
            package_name
            and package_name
            not in installed_packages
        ):
            for path in firefox_paths:
                try:
                    key = rel(
                        path,
                        root,
                    )
                except ValueError:
                    continue

                if key in owned:
                    continue

                add_candidate(
                    candidates,
                    package_name,
                    path,
                    "firefox",
                )

    # ------------------------------------------------------------------
    # Language/module trees
    # ------------------------------------------------------------------
    for path, kind in scan_module_roots(
        root
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

        guess = package_guess(path)

        if not guess:
            continue

        package_name = find_repo_package(
            guess,
            repo_packages,
        )

        if package_name is None:
            continue

        if package_name in installed_packages:
            continue

        add_candidate(
            candidates,
            package_name,
            path,
            kind,
        )

    # ------------------------------------------------------------------
    # Generic binary files.
    #
    # Still restricted to real installation trees. No arbitrary .png,
    # .pod, .rbs, .postinst, .md, .html, etc.
    # ------------------------------------------------------------------
    generic_roots = (
        "/bin",
        "/sbin",
        "/usr/bin",
        "/usr/sbin",
        "/lib",
        "/lib32",
        "/lib64",
        "/libx32",
        "/usr/lib",
        "/usr/lib32",
        "/usr/lib64",
        "/usr/libx32",
    )

    def is_generic_binary(path):
        suffix = path.suffix.lower()

        if suffix in GENERIC_BINARY_SUFFIXES:
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
            or excluded_dir(
                base,
                root,
            )
        ):
            continue

        for path in scan_tree(
            base,
            is_generic_binary,
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

            guess = package_guess(path)

            if not guess:
                continue

            package_name = find_repo_package(
                guess,
                repo_packages,
            )

            if package_name is None:
                continue

            if package_name in installed_packages:
                continue

            add_candidate(
                candidates,
                package_name,
                path,
                "generic",
            )

    # ------------------------------------------------------------------
    # Final filter
    # ------------------------------------------------------------------
    result = []

    for name, item in candidates.items():
        if item["count"] < minimum:
            continue

        evidence = sorted(
            {
                Path(path)
                for path
                in item["evidence"]
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
            item[0].lower(),
            -item[1],
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
    output_directory,
    pkgrel,
    arch,
    overwrite,
):
    root = Path(root)
    output_directory = Path(
        output_directory
    )

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
        output_directory
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

    output_directory.mkdir(
        parents=True,
        exist_ok=True,
    )

    metadata = {
        "format": "kuzpkg-detected-v4",
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
        "origin": "filesystem-discovery",
        "package_name_source": (
            "arch-repository-filename"
        ),
    }

    with tempfile.TemporaryDirectory(
        prefix="kuzpkg-untracked-"
    ) as temporary_directory:
        temporary_directory = Path(
            temporary_directory
        )

        tar_path = (
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
                    relative = rel(
                        path,
                        root,
                    )
                except ValueError:
                    continue

                if relative in added:
                    continue

                try:
                    tar.add(
                        path,
                        arcname=relative,
                        recursive=False,
                    )
                except (
                    OSError,
                    ValueError,
                ):
                    continue

                added.add(relative)

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
            "Discover untracked software using "
            "Arch Linux package archive names"
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
            "minimum number of local artifacts "
            "required for a candidate"
        ),
    )

    parser.add_argument(
        "-q",
        "--quiet",
        action="store_true",
        help=(
            "print only validated package names"
        ),
    )

    parser.add_argument(
        "--no-package",
        action="store_true",
        help=(
            "detect only; do not create "
            "kuzpkg archives"
        ),
    )

    parser.add_argument(
        "--output-dir",
        default="/var/lib/kuzpkg/pkg",
        help=(
            "directory for generated "
            "kuzpkg packages"
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
            "output package architecture; "
            "default is uname -m"
        ),
    )

    parser.add_argument(
        "--overwrite",
        action="store_true",
        help=(
            "overwrite existing kuzpkg archives"
        ),
    )

    parser.add_argument(
        "--arch-cache",
        default=ARCH_CACHE_DEFAULT,
        help=(
            "cache containing Arch package names"
        ),
    )

    parser.add_argument(
        "--refresh-arch",
        action="store_true",
        help=(
            "refresh Arch package-name cache"
        ),
    )

    args = parser.parse_args()

    if args.minimum < 1:
        parser.error(
            "--minimum must be at least 1"
        )

    try:
        # --------------------------------------------------------------
        # Arch repository package names.
        # --------------------------------------------------------------
        repo_packages = (
            load_arch_package_names(
                args.arch_cache,
                refresh=args.refresh_arch,
                quiet=args.quiet,
            )
        )

        # --------------------------------------------------------------
        # Filesystem discovery.
        # --------------------------------------------------------------
        if not args.quiet:
            stdout_write(
                f"{BOLD}"
                "Detecting packages..."
                f"{RESET} ",
                flush=True,
            )

        candidates = discover(
            args.root,
            args.minimum,
            repo_packages,
        )

        root = Path(
            args.root
        ).resolve()

        output_arch, arch_source = (
            detect_arch(args.arch)
        )

        if not args.quiet:
            println(
                f"{CHECK}, "
                f"{len(candidates)} "
                "validated package(s)"
            )

    except RuntimeError as exc:
        stderr_write(
            "kuzpkg-untracked: "
            f"error: {exc}\n",
            flush=True,
        )
        return 1

    # ------------------------------------------------------------------
    # Quiet mode.
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
    # No candidates.
    # ------------------------------------------------------------------
    if not candidates:
        println(
            "No untracked Arch package "
            "candidates found"
        )
        return 0

    # ------------------------------------------------------------------
    # Discovery-only.
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
                f"{name:<42} "
                f"{version:<24} "
                f"{count:>4} artifact(s) "
                f"[{source}; "
                f"arch={output_arch}]"
            )

        return 0

    # ------------------------------------------------------------------
    # Packaging.
    # ------------------------------------------------------------------
    output_directory = Path(
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
                version,
                source,
                was_created,
            ) = make_archive(
                name,
                count,
                types,
                evidence,
                root,
                output_directory,
                args.pkgrel,
                output_arch,
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

    # ------------------------------------------------------------------
    # Final result.
    # ------------------------------------------------------------------
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
        f"to {output_directory}"
    )

    return 0


if __name__ == "__main__":
    raise SystemExit(
        main()
    )
