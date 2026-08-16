#!/usr/bin/env python3
# Generic filesystem adoption/discovery helper for kuzpkg.
#
# Discovery sources:
#
#   1. installed kuzpkg LocalDB
#   2. Arch Linux .files databases
#   3. Arch Linux .db databases
#   4. local filesystem evidence
#   5. Debian/dpkg package database
#
# Arch repositories:
#
#   gnome-unstable
#   kde-unstable
#   core
#   extra
#   multilib
#   core-testing
#   extra-testing
#   multilib-testing
#   core-staging
#   extra-staging
#   multilib-staging
#
# .files fields used:
#
#   %NAME%
#   %BASE%
#   %DESC%
#   %LICENSE%
#   %DEPENDS%
#   %MAKEDEPENDS%
#   %FILES%
#
# .db fields used:
#
#   %NAME%
#   %BASE%
#   %VERSION%
#
# Generated archive:
#
#   name-pkgver-pkgrel-arch.kuzpkg.tar.zst
#
# Generated metadata:
#
#   .PKGINFO
#   .KUZPKG-METADATA.json


import argparse
import bz2
import gzip
import html.parser
import json
import lzma
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
from urllib.request import Request, urlopen


# ============================================================================
# Configuration
# ============================================================================

# Keep unstable repositories first so that when the same package name occurs
# in multiple enabled repositories, the more specific unstable repository
# remains the preferred metadata source.
ARCH_REPOSITORIES = (
    "gnome-unstable",
    "kde-unstable",
    "core-testing",
    "extra-testing",
    "multilib-testing",
    "core-staging",
    "extra-staging",
    "multilib-staging",
    "core",
    "extra",
    "multilib",
)

ARCH_MIRRORS = (
    "https://ftp.icm.edu.pl/pub/Linux/dist/archlinux",
    "http://ftp.icm.edu.pl/pub/Linux/dist/archlinux",
    "https://ftp.icm.edu.pl/Linux/dist/archlinux",
    "http://ftp.icm.edu.pl/Linux/dist/archlinux",
)

ARCH_ARCH = "x86_64"

ARCH_DB_CACHE_DEFAULT = (
    "/var/cache/kuzpkg/arch-repo-db"
)

ARCH_FILES_CACHE_DEFAULT = (
    "/var/cache/kuzpkg/arch-repo-files"
)

DPKG_STATUS_DEFAULT = (
    "/var/lib/dpkg/status"
)

DPKG_INFO_DEFAULT = (
    "/var/lib/dpkg/info"
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
    ("/libx32/pkgconfig", "lib32-"),
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

ARCH_PACKAGE_RE = re.compile(
    r"^(.+)-"
    r"([0-9][A-Za-z0-9:+._]*?)-"
    r"([0-9]+)-"
    r"([A-Za-z0-9_]+)"
    r"\.pkg\.tar"
    r"(?:\.[A-Za-z0-9]+)+$"
)

SUPPORTED_FILES_FIELDS = {
    "NAME",
    "BASE",
    "DESC",
    "LICENSE",
    "DEPENDS",
    "MAKEDEPENDS",
    "FILES",
}

SUPPORTED_DB_FIELDS = {
    "NAME",
    "BASE",
    "VERSION",
}


# ============================================================================
# Terminal
# ============================================================================

def detect_unicode_support():
    if os.environ.get("KUZPKG_ASCII") == "1":
        return False

    encoding = (
        getattr(
            sys.stdout,
            "encoding",
            None,
        )
        or ""
    ).lower().replace("-", "")

    return encoding in {
        "utf8",
        "utf8sig",
    }


USE_UNICODE = detect_unicode_support()

BOLD = (
    "\033[1m"
    if sys.stdout.isatty()
    else ""
)

DIM = (
    "\033[2m"
    if sys.stdout.isatty()
    else ""
)

RESET = (
    "\033[0m"
    if sys.stdout.isatty()
    else ""
)

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
# HTTP
# ============================================================================

def fetch_url(url, timeout=30):
    request = Request(
        url,
        headers={
            "User-Agent":
                "kuzpkg-untracked/4.0",
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
            f"HTTP {exc.code}: {url}"
        ) from exc

    except URLError as exc:
        raise RuntimeError(
            f"network error: {exc.reason}"
        ) from exc

    except OSError as exc:
        raise RuntimeError(
            f"cannot fetch {url}: {exc}"
        ) from exc


# ============================================================================
# ALPM parsing
# ============================================================================

def parse_alpm_fields(
    text,
    allowed_fields,
):
    result = {}
    current = None

    for raw_line in text.splitlines():
        line = raw_line.rstrip("\n")

        if (
            line.startswith("%")
            and line.endswith("%")
        ):
            current = line[1:-1]

            if current in allowed_fields:
                result[current] = []

            continue

        if (
            current in allowed_fields
            and line
        ):
            result.setdefault(
                current,
                [],
            ).append(line)

    return result


def decompress_database(
    data,
    filename,
):
    lower = filename.lower()

    if lower.endswith(".gz"):
        return gzip.decompress(data)

    if lower.endswith(".bz2"):
        return bz2.decompress(data)

    if lower.endswith(".xz"):
        return lzma.decompress(data)

    if lower.endswith(".zst"):
        zstd = shutil.which("zstd")

        if not zstd:
            raise RuntimeError(
                "zstd is required to read "
                "Arch repository databases"
            )

        process = subprocess.run(
            [
                zstd,
                "-d",
                "-q",
                "-c",
            ],
            input=data,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

        if process.returncode != 0:
            raise RuntimeError(
                "failed to decompress "
                "Arch repository zstd database"
            )

        return process.stdout

    return data


# ============================================================================
# Arch repository URLs
# ============================================================================

def arch_repo_urls(repository):
    suffix = (
        f"/{repository}/os/"
        f"{ARCH_ARCH}/"
    )

    for mirror in ARCH_MIRRORS:
        yield (
            mirror.rstrip("/")
            + suffix
        )


def arch_files_repo_urls(repository):
    for base in arch_repo_urls(
        repository
    ):
        yield (
            base
            + f"{repository}.files"
        )
        yield (
            base
            + f"{repository}.files.tar.gz"
        )
        yield (
            base
            + f"{repository}.files.tar.xz"
        )
        yield (
            base
            + f"{repository}.files.tar.zst"
        )


def arch_db_repo_urls(repository):
    for base in arch_repo_urls(
        repository
    ):
        yield (
            base
            + f"{repository}.db"
        )
        yield (
            base
            + f"{repository}.db.tar.gz"
        )
        yield (
            base
            + f"{repository}.db.tar.xz"
        )
        yield (
            base
            + f"{repository}.db.tar.zst"
        )


# ============================================================================
# Arch .files database
# ============================================================================

def parse_arch_files_database(
    data,
    filename,
    repository,
):
    tar_data = decompress_database(
        data,
        filename,
    )

    packages = {}

    with tempfile.TemporaryDirectory(
        prefix="kuzpkg-arch-files-",
    ) as temp_dir:
        temp_dir = Path(temp_dir)
        tar_path = temp_dir / "files.tar"
        tar_path.write_bytes(tar_data)

        with tarfile.open(
            tar_path,
            "r:*",
        ) as archive:
            for member in archive.getmembers():
                if not member.isdir():
                    continue

                package_dir = (
                    member.name.rstrip("/")
                )

                if not package_dir:
                    continue

                desc_member = (
                    archive.extractfile(
                        f"{package_dir}/desc"
                    )
                )

                if desc_member is None:
                    continue

                desc_text = (
                    desc_member
                    .read()
                    .decode(
                        "utf-8",
                        errors="replace",
                    )
                )

                fields = parse_alpm_fields(
                    desc_text,
                    SUPPORTED_FILES_FIELDS,
                )

                names = fields.get(
                    "NAME",
                    [],
                )

                if not names:
                    continue

                name = names[0]

                bases = fields.get(
                    "BASE",
                    [],
                )

                descriptions = fields.get(
                    "DESC",
                    [],
                )

                licenses = fields.get(
                    "LICENSE",
                    [],
                )

                depends = fields.get(
                    "DEPENDS",
                    [],
                )

                makedepends = fields.get(
                    "MAKEDEPENDS",
                    [],
                )

                file_list = []

                files_member = (
                    archive.extractfile(
                        f"{package_dir}/files"
                    )
                )

                if files_member is not None:
                    files_text = (
                        files_member
                        .read()
                        .decode(
                            "utf-8",
                            errors="replace",
                        )
                    )

                    in_files = False

                    for line in (
                        files_text.splitlines()
                    ):
                        if line == "%FILES%":
                            in_files = True
                            continue

                        if (
                            in_files
                            and line
                            and not line.startswith("%")
                        ):
                            file_list.append(line)

                if not file_list:
                    file_list = fields.get(
                        "FILES",
                        [],
                    )

                packages[name] = {
                    "name": name,
                    "base": (
                        bases[0]
                        if bases
                        else name
                    ),
                    "desc": (
                        descriptions[0]
                        if descriptions
                        else ""
                    ),
                    "license": licenses,
                    "depends": depends,
                    "makedepends": makedepends,
                    "files": file_list,
                    "version": "",
                    "repo": repository,
                    "source": "files",
                }

    return packages


def download_arch_files_database(
    repository,
    cache_dir,
    refresh=False,
):
    cache_dir = Path(cache_dir)

    cache_dir.mkdir(
        parents=True,
        exist_ok=True,
    )

    json_cache = (
        cache_dir
        / f"{repository}.json"
    )

    if (
        json_cache.exists()
        and not refresh
    ):
        try:
            cached = json.loads(
                json_cache.read_text(
                    errors="replace",
                )
            )

            if isinstance(
                cached,
                dict,
            ):
                return cached

        except (
            OSError,
            json.JSONDecodeError,
        ):
            pass

    errors = []

    for url in arch_files_repo_urls(
        repository
    ):
        filename = (
            url.rstrip("/")
            .split("/")[-1]
        )

        cache_file = (
            cache_dir
            / f"{repository}-{filename}"
        )

        try:
            if (
                cache_file.exists()
                and not refresh
            ):
                data = cache_file.read_bytes()
            else:
                data = fetch_url(url)

                temporary = (
                    cache_file.with_suffix(
                        cache_file.suffix
                        + ".tmp"
                    )
                )

                temporary.write_bytes(data)
                temporary.replace(
                    cache_file
                )

            packages = (
                parse_arch_files_database(
                    data,
                    filename,
                    repository,
                )
            )

            if not packages:
                raise RuntimeError(
                    "empty .files database"
                )

            try:
                temporary_json = (
                    json_cache.with_suffix(
                        ".json.tmp"
                    )
                )

                temporary_json.write_text(
                    json.dumps(
                        packages,
                        indent=2,
                    )
                    + "\n"
                )

                temporary_json.replace(
                    json_cache
                )

            except OSError:
                pass

            return packages

        except (
            OSError,
            RuntimeError,
            tarfile.TarError,
        ) as exc:
            errors.append(
                f"{url}: {exc}"
            )

    raise RuntimeError(
        f"cannot obtain "
        f"{repository}.files: "
        + "; ".join(
            errors[-3:]
        )
    )


def load_arch_files_databases(
    cache_dir,
    refresh=False,
    quiet=False,
):
    combined = {}

    for repository in ARCH_REPOSITORIES:
        try:
            if not quiet:
                println(
                    f"Loading "
                    f"Arch [{repository}].files..."
                )

            packages = (
                download_arch_files_database(
                    repository,
                    cache_dir,
                    refresh=refresh,
                )
            )

            for name, info in (
                packages.items()
            ):
                # Do not replace a package from a
                # higher-priority repository.
                if name not in combined:
                    info = dict(info)
                    info["repo"] = repository
                    info["source"] = "files"
                    combined[name] = info

            if not quiet:
                println(
                    f"  {repository:<18} "
                    f"{len(packages):>6} packages"
                )

        except RuntimeError as exc:
            if not quiet:
                println(
                    f"  {repository:<18} "
                    f"unavailable: {exc}"
                )

    return combined


# ============================================================================
# Arch .db database
# ============================================================================

def parse_arch_db_database(
    data,
    filename,
    repository,
):
    tar_data = decompress_database(
        data,
        filename,
    )

    packages = {}

    with tempfile.TemporaryDirectory(
        prefix="kuzpkg-arch-db-",
    ) as temp_dir:
        temp_dir = Path(temp_dir)
        tar_path = temp_dir / "db.tar"
        tar_path.write_bytes(tar_data)

        with tarfile.open(
            tar_path,
            "r:*",
        ) as archive:
            for member in archive.getmembers():
                if not member.isdir():
                    continue

                package_dir = (
                    member.name.rstrip("/")
                )

                if not package_dir:
                    continue

                desc_member = (
                    archive.extractfile(
                        f"{package_dir}/desc"
                    )
                )

                if desc_member is None:
                    continue

                desc_text = (
                    desc_member
                    .read()
                    .decode(
                        "utf-8",
                        errors="replace",
                    )
                )

                fields = parse_alpm_fields(
                    desc_text,
                    SUPPORTED_DB_FIELDS,
                )

                names = fields.get(
                    "NAME",
                    [],
                )

                if not names:
                    continue

                bases = fields.get(
                    "BASE",
                    [],
                )

                versions = fields.get(
                    "VERSION",
                    [],
                )

                name = names[0]

                packages[name] = {
                    "name": name,
                    "base": (
                        bases[0]
                        if bases
                        else name
                    ),
                    "version": (
                        versions[0]
                        if versions
                        else ""
                    ),
                    "repo": repository,
                    "source": "db",
                }

    return packages


def download_arch_db_database(
    repository,
    cache_dir,
    refresh=False,
):
    cache_dir = Path(cache_dir)

    cache_dir.mkdir(
        parents=True,
        exist_ok=True,
    )

    json_cache = (
        cache_dir
        / f"{repository}.json"
    )

    if (
        json_cache.exists()
        and not refresh
    ):
        try:
            cached = json.loads(
                json_cache.read_text(
                    errors="replace",
                )
            )

            if isinstance(
                cached,
                dict,
            ):
                return cached

        except (
            OSError,
            json.JSONDecodeError,
        ):
            pass

    errors = []

    for url in arch_db_repo_urls(
        repository
    ):
        filename = (
            url.rstrip("/")
            .split("/")[-1]
        )

        cache_file = (
            cache_dir
            / f"{repository}-{filename}"
        )

        try:
            if (
                cache_file.exists()
                and not refresh
            ):
                data = cache_file.read_bytes()
            else:
                data = fetch_url(url)

                temporary = (
                    cache_file.with_suffix(
                        cache_file.suffix
                        + ".tmp"
                    )
                )

                temporary.write_bytes(data)
                temporary.replace(
                    cache_file
                )

            packages = (
                parse_arch_db_database(
                    data,
                    filename,
                    repository,
                )
            )

            if not packages:
                raise RuntimeError(
                    "empty .db database"
                )

            try:
                temporary_json = (
                    json_cache.with_suffix(
                        ".json.tmp"
                    )
                )

                temporary_json.write_text(
                    json.dumps(
                        packages,
                        indent=2,
                    )
                    + "\n"
                )

                temporary_json.replace(
                    json_cache
                )

            except OSError:
                pass

            return packages

        except (
            OSError,
            RuntimeError,
            tarfile.TarError,
        ) as exc:
            errors.append(
                f"{url}: {exc}"
            )

    raise RuntimeError(
        f"cannot obtain "
        f"{repository}.db: "
        + "; ".join(
            errors[-3:]
        )
    )


def load_arch_db_databases(
    cache_dir,
    refresh=False,
    quiet=False,
):
    combined = {}

    for repository in ARCH_REPOSITORIES:
        try:
            if not quiet:
                println(
                    f"Loading "
                    f"Arch [{repository}].db..."
                )

            packages = (
                download_arch_db_database(
                    repository,
                    cache_dir,
                    refresh=refresh,
                )
            )

            for name, info in (
                packages.items()
            ):
                # Preserve repository priority.
                if name not in combined:
                    info = dict(info)
                    info["repo"] = repository
                    info["source"] = "db"
                    combined[name] = info

            if not quiet:
                println(
                    f"  {repository:<18} "
                    f"{len(packages):>6} packages"
                )

        except RuntimeError as exc:
            if not quiet:
                println(
                    f"  {repository:<18} "
                    f"unavailable: {exc}"
                )

    return combined


# ============================================================================
# Combine Arch metadata
# ============================================================================

def load_arch_packages(
    db_cache,
    files_cache,
    refresh=False,
    quiet=False,
):
    files_packages = (
        load_arch_files_databases(
            files_cache,
            refresh=refresh,
            quiet=quiet,
        )
    )

    db_packages = (
        load_arch_db_databases(
            db_cache,
            refresh=refresh,
            quiet=quiet,
        )
    )

    combined = {}

    # Start with .db metadata.
    for name, info in db_packages.items():
        combined[name] = dict(info)

    # .files provides:
    #
    # NAME
    # BASE
    # DESC
    # LICENSE
    # DEPENDS
    # MAKEDEPENDS
    # FILES
    #
    # It deliberately takes precedence for these fields.
    for name, info in files_packages.items():
        if name not in combined:
            combined[name] = {}

        combined[name].update(
            {
                "name": info.get(
                    "name",
                    name,
                ),
                "base": info.get(
                    "base",
                    name,
                ),
                "desc": info.get(
                    "desc",
                    "",
                ),
                "license": info.get(
                    "license",
                    [],
                ),
                "depends": info.get(
                    "depends",
                    [],
                ),
                "makedepends": info.get(
                    "makedepends",
                    [],
                ),
                "files": info.get(
                    "files",
                    [],
                ),
                "repo": info.get(
                    "repo",
                    combined[name].get(
                        "repo",
                        "",
                    ),
                ),
                "source": "files",
            }
        )

    if not combined:
        raise RuntimeError(
            "no Arch repository metadata "
            "could be loaded"
        )

    if not quiet:
        println(
            f"{CHECK}, "
            f"{len(combined)} unique "
            "Arch packages"
        )

    return combined


# ============================================================================
# kuzpkg LocalDB
# ============================================================================

def run_kuzpkg(
    root,
    args,
):
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


def rel(path, root):
    return norm(
        Path(path).relative_to(
            Path(root)
        )
    )


def load_local_db(root):
    """
    Read installed kuzpkg package names and
    owned files.

    This is intentionally tolerant: if kuzpkg
    itself is unavailable, filesystem/Arch/dpkg
    detection can still continue.
    """

    packages = set()

    try:
        packages = set(
            run_kuzpkg(
                root,
                ["-Qq"],
            )
        )
    except RuntimeError:
        packages = set()

    owned = set()

    for package in sorted(packages):
        try:
            files = run_kuzpkg(
                root,
                ["-Qql", package],
            )
        except RuntimeError:
            continue

        for item in files:
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
# Arch file ownership index
# ============================================================================

def build_arch_file_index(
    arch_packages,
):
    index = defaultdict(set)

    for package_name, info in (
        arch_packages.items()
    ):
        for filename in info.get(
            "files",
            [],
        ):
            filename = str(filename).strip()

            if not filename:
                continue

            index[
                filename.lstrip("/")
            ].add(
                package_name
            )

    return index


def find_arch_package_for_file(
    relative_path,
    index,
):
    normalized = (
        str(relative_path)
        .lstrip("/")
        .replace(
            os.sep,
            "/",
        )
    )

    packages = index.get(
        normalized
    )

    if packages:
        return sorted(
            packages
        )[0]

    return None


# ============================================================================
# Filesystem scanning
# ============================================================================

def excluded_dir(
    path,
    root,
):
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


def scan_files(
    root,
    directories,
    predicate,
):
    root = Path(root)
    result = []

    for directory, prefix in directories:
        base = (
            root
            / directory.lstrip("/")
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
                        (
                            path,
                            prefix,
                        )
                    )

    return result


def scan_tree(
    root,
    predicate,
):
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
                dirname
                not in EXCLUDED_TOPLEVEL
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
        base = (
            Path(root)
            / directory.lstrip("/")
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
                    current_path
                    / filename
                )

    return result


def scan_module_roots(root):
    result = []

    for directory in MODULE_ROOTS:
        base = (
            Path(root)
            / directory.lstrip("/")
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

                if valid:
                    result.append(path)

    return result


# ============================================================================
# Package guessing
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

    return value


def package_guess(path):
    path = Path(path)

    name = path.name
    lower = name.lower()
    suffix = path.suffix.lower()

    if lower.endswith(".pc"):
        return name[:-3]

    if suffix in PYTHON_SUFFIXES:
        return normalize_python_name(
            name
        )

    if lower.endswith(".egg-info"):
        return name[:-9]

    if lower.endswith(".dist-info"):
        return name[:-10]

    shared = SONAME_RE.sub(
        "",
        name,
    )

    if shared != name:
        if shared.startswith("lib"):
            return shared[3:]
        return shared

    if lower.endswith(".a"):
        value = name[:-2]

        if value.startswith("lib"):
            value = value[3:]

        return value

    if lower.endswith(".node"):
        return name[:-5]

    for extension in (
        ".rlib",
        ".rmeta",
        ".crate",
        ".jar",
        ".war",
        ".ear",
    ):
        if lower.endswith(extension):
            return name[
                :-len(extension)
            ]

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

    if (
        executable
        and "." not in name
    ):
        return name

    return None


# ============================================================================
# Version
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
    arch_packages,
):
    info = arch_packages.get(
        name,
        {},
    )

    repository_version = info.get(
        "version",
        "",
    )

    if repository_version:
        return (
            repository_version,
            "arch-repository",
        )

    for path in evidence:
        version = probe_version(path)

        if version:
            return (
                version,
                "version-probe",
            )

    root = Path(root)

    for directory in (
        *BIN_DIRS,
        *FIREFOX_DIRS,
    ):
        candidate = (
            root
            / directory.lstrip("/")
            / name
        )

        version = probe_version(
            candidate
        )

        if version:
            return (
                version,
                "version-probe",
            )

    return (
        "0.0.0+detected",
        "unverified",
    )


# ============================================================================
# DPKG
# ============================================================================

def parse_dpkg_status(root):
    status_path = (
        Path(root)
        / DPKG_STATUS_DEFAULT.lstrip("/")
    )

    if not status_path.exists():
        return {}

    try:
        text = status_path.read_text(
            errors="replace"
        )
    except OSError:
        return {}

    packages = {}

    for paragraph in re.split(
        r"\n\s*\n",
        text,
    ):
        fields = {}
        current = None

        for line in paragraph.splitlines():
            if (
                line.startswith(" ")
                or line.startswith("\t")
            ):
                if current:
                    fields[current] += (
                        "\n"
                        + line.lstrip()
                    )
                continue

            if ":" not in line:
                continue

            key, value = line.split(
                ":",
                1,
            )

            current = key
            fields[key] = value.strip()

        name = fields.get(
            "Package"
        )

        status = fields.get(
            "Status",
            "",
        )

        if not name:
            continue

        if not status.startswith(
            "install ok installed"
        ):
            continue

        packages[name] = fields

    return packages


def dpkg_package_files(
    root,
    package_name,
):
    list_path = (
        Path(root)
        / DPKG_INFO_DEFAULT.lstrip("/")
        / f"{package_name}.list"
    )

    if not list_path.exists():
        return []

    try:
        return [
            line.strip()
            for line in (
                list_path.read_text(
                    errors="replace",
                ).splitlines()
            )
            if line.strip()
        ]

    except OSError:
        return []


def load_dpkg_packages(root):
    packages = parse_dpkg_status(root)

    for name, info in packages.items():
        info = dict(info)

        info["Files"] = (
            dpkg_package_files(
                root,
                name,
            )
        )

        packages[name] = info

    return packages


# ============================================================================
# Debian -> Arch mapping
# ============================================================================

def normalize_distribution_name(
    name,
):
    value = name.strip().lower()

    value = re.sub(
        r":(?:amd64|arm64|i386|armhf|all)$",
        "",
        value,
    )

    aliases = {
        "python3": "python",
        "python3-dev": "python",
        "python3-pip": "python-pip",
        "libssl3": "openssl",
        "libssl-dev": "openssl",
        "zlib1g": "zlib",
        "zlib1g-dev": "zlib",
        "libreadline8": "readline",
        "libreadline-dev": "readline",
        "libncurses6": "ncurses",
        "libncurses-dev": "ncurses",
        "libexpat1": "expat",
        "libexpat1-dev": "expat",
        "libffi8": "libffi",
        "libffi-dev": "libffi",
        "libfontconfig1": "fontconfig",
        "libfreetype6": "freetype2",
        "libglib2.0-0": "glib2",
        "libglib2.0-dev": "glib2",
        "libgtk-3-0": "gtk3",
        "libgtk-3-dev": "gtk3",
        "libgtk-4-1": "gtk4",
        "libgtk-4-dev": "gtk4",
        "libc6": "glibc",
        "libc6-dev": "glibc",
        "libstdc++6": "gcc",
        "libgcc-s1": "gcc",
    }

    return aliases.get(
        value,
        value,
    )


def arch_candidates_from_dpkg(
    dpkg_name,
):
    normalized = normalize_distribution_name(
        dpkg_name
    )

    result = [
        dpkg_name,
        normalized,
    ]

    if normalized.endswith(
        "-dev"
    ):
        result.append(
            normalized[:-4]
        )

    for prefix in (
        "python3-",
        "python-",
        "ruby-",
        "node-",
        "nodejs-",
        "golang-",
    ):
        if normalized.startswith(
            prefix
        ):
            result.append(
                normalized[len(prefix):]
            )

    return list(
        dict.fromkeys(
            candidate
            for candidate in result
            if candidate
        )
    )


def map_dpkg_to_arch(
    dpkg_name,
    arch_packages,
):
    for candidate in (
        arch_candidates_from_dpkg(
            dpkg_name
        )
    ):
        if candidate in arch_packages:
            return (
                candidate,
                "dpkg-name",
            )

    normalized = normalize_distribution_name(
        dpkg_name
    )

    best = None
    best_score = 0

    for arch_name in arch_packages:
        arch_normalized = (
            normalize_distribution_name(
                arch_name
            )
        )

        score = 0

        if (
            arch_normalized
            == normalized
        ):
            score = 100

        elif (
            arch_normalized.startswith(
                normalized + "-"
            )
        ):
            score = 80

        elif (
            normalized.startswith(
                arch_normalized + "-"
            )
        ):
            score = 70

        if score > best_score:
            best = arch_name
            best_score = score

    if best is not None:
        return (
            best,
            "dpkg-fuzzy",
        )

    return (
        None,
        None,
    )


# ============================================================================
# Candidate collection
# ============================================================================

def add_candidate(
    candidates,
    package_name,
    path,
    kind,
    source,
):
    if not package_name:
        return

    item = candidates[
        package_name
    ]

    item["count"] += 1
    item["types"].add(kind)
    item["sources"].add(source)

    if len(item["evidence"]) < 100:
        item["evidence"].append(
            Path(path)
        )


# ============================================================================
# Discovery
# ============================================================================

def discover(
    root,
    minimum,
    arch_packages,
    arch_file_index,
    dpkg_packages,
    include_dpkg,
):
    root = Path(root).resolve()

    installed_kuzpkg, owned = (
        load_local_db(
            str(root)
        )
    )

    candidates = defaultdict(
        lambda: {
            "count": 0,
            "types": set(),
            "sources": set(),
            "evidence": [],
        }
    )

    # ------------------------------------------------------------------
    # Exact Arch .files ownership.
    # ------------------------------------------------------------------
    for path in scan_tree(
        root,
        lambda _: True,
    ):
        try:
            relative = rel(
                path,
                root,
            )
        except ValueError:
            continue

        if relative in owned:
            continue

        package_name = (
            find_arch_package_for_file(
                relative,
                arch_file_index,
            )
        )

        if not package_name:
            continue

        if package_name in installed_kuzpkg:
            continue

        add_candidate(
            candidates,
            package_name,
            path,
            "arch-file-owner",
            "arch-files",
        )

    # ------------------------------------------------------------------
    # Libraries.
    # ------------------------------------------------------------------
    for path, prefix in scan_files(
        root,
        LIB_DIRS,
        lambda path: ".so" in path.name,
    ):
        try:
            relative = rel(
                path,
                root,
            )
        except ValueError:
            continue

        if relative in owned:
            continue

        package_name = (
            find_arch_package_for_file(
                relative,
                arch_file_index,
            )
        )

        if not package_name:
            guess = package_guess(path)

            if guess:
                variants = []

                if prefix:
                    variants.append(
                        prefix + guess
                    )

                variants.append(
                    guess
                )

                for variant in variants:
                    if variant in arch_packages:
                        package_name = variant
                        break

        if not package_name:
            continue

        if package_name in installed_kuzpkg:
            continue

        add_candidate(
            candidates,
            package_name,
            path,
            "library",
            "filesystem",
        )

    # ------------------------------------------------------------------
    # Executables.
    # ------------------------------------------------------------------
    for path, _prefix in scan_files(
        root,
        tuple(
            (directory, "")
            for directory in BIN_DIRS
        ),
        lambda path: (
            os.access(path, os.X_OK)
            and "." not in path.name
        ),
    ):
        try:
            relative = rel(
                path,
                root,
            )
        except ValueError:
            continue

        if relative in owned:
            continue

        package_name = (
            find_arch_package_for_file(
                relative,
                arch_file_index,
            )
        )

        if not package_name:
            guess = package_guess(path)

            if guess in arch_packages:
                package_name = guess

        if not package_name:
            continue

        if package_name in installed_kuzpkg:
            continue

        add_candidate(
            candidates,
            package_name,
            path,
            "binary",
            "filesystem",
        )

    # ------------------------------------------------------------------
    # pkg-config.
    # ------------------------------------------------------------------
    for path, prefix in scan_files(
        root,
        PC_DIRS,
        lambda path: path.name.endswith(".pc"),
    ):
        try:
            relative = rel(
                path,
                root,
            )
        except ValueError:
            continue

        if relative in owned:
            continue

        package_name = (
            find_arch_package_for_file(
                relative,
                arch_file_index,
            )
        )

        if not package_name:
            guess = package_guess(path)

            if guess:
                variants = []

                if prefix:
                    variants.append(
                        prefix + guess
                    )

                variants.append(
                    guess
                )

                for variant in variants:
                    if variant in arch_packages:
                        package_name = variant
                        break

        if not package_name:
            continue

        if package_name in installed_kuzpkg:
            continue

        add_candidate(
            candidates,
            package_name,
            path,
            "pkgconfig",
            "filesystem",
        )

    # ------------------------------------------------------------------
    # Firefox.
    # ------------------------------------------------------------------
    if (
        "firefox" in arch_packages
        and "firefox" not in installed_kuzpkg
    ):
        for path in scan_firefox(root):
            try:
                relative = rel(
                    path,
                    root,
                )
            except ValueError:
                continue

            if relative in owned:
                continue

            add_candidate(
                candidates,
                "firefox",
                path,
                "firefox",
                "filesystem",
            )

    # ------------------------------------------------------------------
    # Language/module roots.
    # ------------------------------------------------------------------
    for path in scan_module_roots(root):
        try:
            relative = rel(
                path,
                root,
            )
        except ValueError:
            continue

        if relative in owned:
            continue

        package_name = (
            find_arch_package_for_file(
                relative,
                arch_file_index,
            )
        )

        if not package_name:
            guess = package_guess(path)

            if guess in arch_packages:
                package_name = guess

        if not package_name:
            continue

        if package_name in installed_kuzpkg:
            continue

        add_candidate(
            candidates,
            package_name,
            path,
            "module",
            "filesystem",
        )

    # ------------------------------------------------------------------
    # Debian / dpkg mapping.
    # ------------------------------------------------------------------
    if include_dpkg:
        for dpkg_name, info in dpkg_packages.items():
            package_name, map_source = (
                map_dpkg_to_arch(
                    dpkg_name,
                    arch_packages,
                )
            )

            if not package_name:
                continue

            if package_name in installed_kuzpkg:
                continue

            files = info.get(
                "Files",
                [],
            )

            existing = []

            for filename in files:
                path = (
                    root
                    / filename.lstrip("/")
                )

                if path.exists():
                    existing.append(path)

            if existing:
                for path in existing[:100]:
                    add_candidate(
                        candidates,
                        package_name,
                        path,
                        "dpkg-mapped",
                        map_source,
                    )

            else:
                add_candidate(
                    candidates,
                    package_name,
                    root / (
                        "<dpkg:"
                        f"{dpkg_name}>"
                    ),
                    "dpkg-mapped",
                    map_source,
                )

    # ------------------------------------------------------------------
    # Finalize.
    # ------------------------------------------------------------------
    result = []

    for name, item in candidates.items():
        if item["count"] < minimum:
            continue

        evidence = sorted(
            set(
                Path(path)
                for path in item["evidence"]
            ),
            key=str,
        )

        result.append(
            (
                name,
                item["count"],
                item["types"],
                item["sources"],
                evidence,
            )
        )

    # Highest evidence count first, then name.
    result.sort(
        key=lambda item: (
            -item[1],
            item[0].lower(),
        )
    )

    return result


# ============================================================================
# Metadata helpers
# ============================================================================

def pkginfo_escape(value):
    return (
        str(value)
        .replace("\\", "\\\\")
        .replace("\n", " ")
    )


def make_pkginfo(
    name,
    info,
    version,
    pkgrel,
    architecture,
):
    lines = [
        "# Generated by kuzpkg-untracked",
        f"pkgname = "
        f"{pkginfo_escape(name)}",
        f"pkgbase = "
        f"{pkginfo_escape(info.get('base', name))}",
        f"pkgver = "
        f"{pkginfo_escape(version)}",
        f"pkgrel = "
        f"{pkginfo_escape(pkgrel)}",
        f"arch = "
        f"{pkginfo_escape(architecture)}",
    ]

    description = info.get(
        "desc",
        "",
    )

    if description:
        lines.append(
            "pkgdesc = "
            + pkginfo_escape(
                description
            )
        )

    for license_name in info.get(
        "license",
        [],
    ):
        lines.append(
            "license = "
            + pkginfo_escape(
                license_name
            )
        )

    for dependency in info.get(
        "depends",
        [],
    ):
        lines.append(
            "depend = "
            + pkginfo_escape(
                dependency
            )
        )

    for dependency in info.get(
        "makedepends",
        [],
    ):
        lines.append(
            "makedepend = "
            + pkginfo_escape(
                dependency
            )
        )

    repo = info.get(
        "repo",
        "",
    )

    if repo:
        lines.append(
            "x_kuzpkg_repo = "
            + pkginfo_escape(repo)
        )

    return (
        "\n".join(lines)
        + "\n"
    )


def format_package_details(
    name,
    count,
    types,
    sources,
    evidence,
    info,
    version,
    version_source,
    show_files=False,
):
    println(
        f"{BOLD}{name}{RESET}"
    )

    println(
        f"  Repository : "
        f"{info.get('repo', 'unknown')}"
    )

    println(
        f"  Base       : "
        f"{info.get('base', name)}"
    )

    println(
        f"  Version    : "
        f"{version}"
        f" {DIM}({version_source}){RESET}"
    )

    println(
        f"  Description: "
        f"{info.get('desc', '')}"
    )

    licenses = info.get(
        "license",
        [],
    )

    println(
        "  License    : "
        + (
            ", ".join(licenses)
            if licenses
            else "(none)"
        )
    )

    depends = info.get(
        "depends",
        [],
    )

    makedepends = info.get(
        "makedepends",
        [],
    )

    println(
        f"  Depends    : "
        f"{len(depends)}"
    )

    println(
        f"  MakeDepends: "
        f"{len(makedepends)}"
    )

    println(
        f"  Evidence   : "
        f"{count} item(s)"
    )

    println(
        "  Types      : "
        + (
            ", ".join(
                sorted(types)
            )
            if types
            else "(none)"
        )
    )

    println(
        "  Sources    : "
        + (
            ", ".join(
                sorted(sources)
            )
            if sources
            else "(none)"
        )
    )

    if depends:
        println(
            "  Dependencies:"
        )

        for dependency in depends:
            println(
                f"    - {dependency}"
            )

    if makedepends:
        println(
            "  Build dependencies:"
        )

        for dependency in makedepends:
            println(
                f"    - {dependency}"
            )

    if show_files:
        files = info.get(
            "files",
            [],
        )

        println(
            f"  Repository files: "
            f"{len(files)}"
        )

        for filename in files:
            println(
                f"    {filename}"
            )

    println(
        "  Evidence paths:"
    )

    for path in evidence[:10]:
        println(
            f"    {path}"
        )

    if len(evidence) > 10:
        println(
            f"    ... "
            f"{len(evidence) - 10} more"
        )


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
    sources,
    evidence,
    root,
    output_dir,
    pkgrel,
    architecture,
    overwrite,
    arch_packages,
):
    root = Path(root)
    output_dir = Path(output_dir)

    info = arch_packages.get(
        name,
        {},
    )

    version = info.get(
        "version",
        "",
    )

    version_source = (
        "arch-repository"
        if version
        else "unverified"
    )

    if not version:
        version, version_source = detect_version(
            name,
            evidence,
            root,
            arch_packages,
        )

    filename = (
        f"{safe(name)}-"
        f"{safe(version)}-"
        f"{safe(pkgrel)}-"
        f"{safe(architecture)}"
        ".kuzpkg.tar.zst"
    )

    output = output_dir / filename

    if (
        output.exists()
        and not overwrite
    ):
        return (
            output,
            version,
            version_source,
            False,
        )

    zstd = shutil.which("zstd")

    if not zstd:
        raise RuntimeError(
            "zstd is required to create "
            ".kuzpkg.tar.zst archives"
        )

    output_dir.mkdir(
        parents=True,
        exist_ok=True,
    )

    pkginfo_text = make_pkginfo(
        name,
        info,
        version,
        pkgrel,
        architecture,
    )

    metadata = {
        "format": "kuzpkg-detected-v8",
        "name": name,
        "base": info.get(
            "base",
            name,
        ),
        "version": version,
        "version_source": version_source,
        "pkgrel": pkgrel,
        "arch": architecture,
        "repo": info.get(
            "repo",
            "",
        ),
        "description": info.get(
            "desc",
            "",
        ),
        "license": info.get(
            "license",
            [],
        ),
        "depends": info.get(
            "depends",
            [],
        ),
        "makedepends": info.get(
            "makedepends",
            [],
        ),
        "repository_file_count": len(
            info.get(
                "files",
                [],
            )
        ),
        "artifact_types": sorted(
            types
        ),
        "candidate_sources": sorted(
            sources
        ),
        "artifact_count": count,
        "origin": (
            "filesystem-and-"
            "repository-discovery"
        ),
    }

    with tempfile.TemporaryDirectory(
        prefix="kuzpkg-untracked-",
    ) as temp_dir:
        temp_dir = Path(temp_dir)

        tar_path = (
            temp_dir
            / "package.kuzpkg.tar"
        )

        pkginfo_path = (
            temp_dir
            / ".PKGINFO"
        )

        metadata_path = (
            temp_dir
            / ".KUZPKG-METADATA.json"
        )

        pkginfo_path.write_text(
            pkginfo_text
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
        ) as archive:
            archive.add(
                pkginfo_path,
                arcname=".PKGINFO",
            )

            archive.add(
                metadata_path,
                arcname=(
                    ".KUZPKG-METADATA.json"
                ),
            )

            for path in evidence:
                path = Path(path)

                if not path.exists():
                    continue

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
                    archive.add(
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
        version_source,
        True,
    )


# ============================================================================
# Main
# ============================================================================

def main():
    parser = argparse.ArgumentParser(
        description=(
            "Discover untracked packages using "
            "Arch repository .db/.files "
            "metadata and dpkg data"
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
            "minimum local evidence items "
            "required for a candidate"
        ),
    )

    parser.add_argument(
        "-q",
        "--quiet",
        action="store_true",
        help=(
            "print only package names"
        ),
    )

    parser.add_argument(
        "--details",
        action="store_true",
        help=(
            "show full metadata, "
            "dependencies, sources and evidence"
        ),
    )

    parser.add_argument(
        "--show-files",
        action="store_true",
        help=(
            "show the complete Arch %FILES% "
            "list in detailed output"
        ),
    )

    parser.add_argument(
        "--no-package",
        action="store_true",
        help=(
            "detect only; do not create archives"
        ),
    )

    parser.add_argument(
        "--output-dir",
        default="/var/lib/kuzpkg/pkg",
        help=(
            "output directory for "
            "kuzpkg archives"
        ),
    )

    parser.add_argument(
        "--pkgrel",
        default="1",
        help=(
            "kuzpkg package release"
        ),
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
            "overwrite existing archives"
        ),
    )

    parser.add_argument(
        "--arch-db-cache",
        default=ARCH_DB_CACHE_DEFAULT,
        help=(
            "cache directory for Arch .db"
        ),
    )

    parser.add_argument(
        "--arch-files-cache",
        default=ARCH_FILES_CACHE_DEFAULT,
        help=(
            "cache directory for Arch .files"
        ),
    )

    parser.add_argument(
        "--refresh-arch",
        action="store_true",
        help=(
            "refresh all Arch repository metadata"
        ),
    )

    parser.add_argument(
        "--no-dpkg",
        action="store_true",
        help=(
            "disable Debian/dpkg mapping"
        ),
    )

    args = parser.parse_args()

    if args.minimum < 1:
        parser.error(
            "--minimum must be at least 1"
        )

    # ==================================================================
    # Load Arch metadata
    # ==================================================================
    try:
        arch_packages = load_arch_packages(
            args.arch_db_cache,
            args.arch_files_cache,
            refresh=args.refresh_arch,
            quiet=args.quiet,
        )

    except RuntimeError as exc:
        stderr_write(
            "kuzpkg-untracked: "
            "error loading Arch metadata: "
            f"{exc}\n",
            flush=True,
        )
        return 1

    arch_file_index = (
        build_arch_file_index(
            arch_packages
        )
    )

    # ==================================================================
    # Load dpkg
    # ==================================================================
    dpkg_packages = {}

    if not args.no_dpkg:
        dpkg_packages = load_dpkg_packages(
            args.root
        )

        if (
            dpkg_packages
            and not args.quiet
        ):
            println(
                f"Loaded "
                f"{len(dpkg_packages)} "
                "installed dpkg package records"
            )

    # ==================================================================
    # Detect
    # ==================================================================
    try:
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
            arch_packages,
            arch_file_index,
            dpkg_packages,
            include_dpkg=not args.no_dpkg,
        )

        root = Path(
            args.root
        ).resolve()

        architecture, architecture_source = (
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

    # ==================================================================
    # Quiet mode
    # ==================================================================
    if args.quiet:
        for (
            name,
            count,
            types,
            sources,
            evidence,
        ) in candidates:
            println(name)

        return 0

    # ==================================================================
    # Empty
    # ==================================================================
    if not candidates:
        println(
            "No untracked package "
            "candidates found"
        )
        return 0

    # ==================================================================
    # Details / discovery-only
    # ==================================================================
    if args.no_package or args.details:
        for (
            name,
            count,
            types,
            sources,
            evidence,
        ) in candidates:
            info = arch_packages.get(
                name,
                {},
            )

            version, version_source = (
                detect_version(
                    name,
                    evidence,
                    root,
                    arch_packages,
                )
            )

            if args.details:
                format_package_details(
                    name,
                    count,
                    types,
                    sources,
                    evidence,
                    info,
                    version,
                    version_source,
                    show_files=args.show_files,
                )

            else:
                println(
                    f"{name:<44} "
                    f"{version:<24} "
                    f"{count:>4} evidence "
                    f"["
                    f"repo={info.get('repo', '')}; "
                    f"base={info.get('base', name)}; "
                    f"depends="
                    f"{len(info.get('depends', []))}; "
                    f"makedepends="
                    f"{len(info.get('makedepends', []))}; "
                    f"{version_source}"
                    f"]"
                )

        if args.no_package:
            return 0

    # ==================================================================
    # Package creation
    # ==================================================================
    output_dir = Path(
        args.output_dir
    ).resolve()

    total = len(candidates)

    created = 0
    skipped = 0
    failed = 0

    println(
        f"{BOLD}"
        f"Packaging {total} detected "
        f"package(s)..."
        f"{RESET}"
    )

    for index, (
        name,
        count,
        types,
        sources,
        evidence,
    ) in enumerate(
        candidates,
        start=1,
    ):
        info = arch_packages.get(
            name,
            {},
        )

        println(
            f"{BOLD}"
            f"Compressing {name}"
            f"{RESET}"
            f" "
            f"{DIM}("
            f"{info.get('repo', 'unknown')}, "
            f"{info.get('version', 'unknown')}"
            f"){RESET}"
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
                version_source,
                was_created,
            ) = make_archive(
                name,
                count,
                types,
                sources,
                evidence,
                root,
                output_dir,
                args.pkgrel,
                architecture,
                args.overwrite,
                arch_packages,
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
            tarfile.TarError,
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

    # ==================================================================
    # Final summary
    # ==================================================================
    println()
    println(
        f"{BOLD}Detection summary{RESET}"
    )
    println(
        f"  Candidates : {total}"
    )
    println(
        f"  Created    : {created}"
    )
    println(
        f"  Skipped    : {skipped}"
    )
    println(
        f"  Failed     : {failed}"
    )
    println(
        f"  Output     : {output_dir}"
    )
    println(
        f"  Architecture: "
        f"{architecture} ({architecture_source})"
    )
    println(
        f"  Repositories: "
        f"{len(ARCH_REPOSITORIES)}"
    )

    if failed:
        return 1

    println()
    println(
        "Compressed all detected packages "
        f"to {output_dir}"
    )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
