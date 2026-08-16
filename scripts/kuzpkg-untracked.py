#!/usr/bin/env python3
# Generic filesystem adoption/discovery helper for kuzpkg.
#
# Package discovery sources:
#
#   1. Installed kuzpkg database
#   2. Arch Linux repository .db metadata
#   3. Arch Linux repository .files metadata
#   4. Arch repository archive listings as fallback
#   5. Local filesystem evidence
#   6. Debian/dpkg database mapping
#
# Arch .files metadata fields used:
#
#   %NAME%
#   %BASE%
#   %DESC%
#   %LICENSE%
#   %DEPENDS%
#   %MAKEDEPENDS%
#   %FILES%
#
# Other .files fields are ignored.
#
# Generated archive:
#
#   name-pkgver-pkgrel-arch.kuzpkg.tar.zst
#
# Contains:
#
#   .PKGINFO
#   .KUZPKG-METADATA.json
#   discovered filesystem files


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


# ============================================================================
# Terminal output
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


def progress_bar(
    current,
    total,
    width=32,
):
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

def fetch_url(
    url,
    timeout=30,
):
    request = Request(
        url,
        headers={
            "User-Agent": (
                "kuzpkg-untracked/3.0"
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
# ALPM description parser
# ============================================================================

SUPPORTED_FILES_FIELDS = {
    "NAME",
    "BASE",
    "DESC",
    "LICENSE",
    "DEPENDS",
    "MAKEDEPENDS",
    "FILES",
}


def parse_alpm_description(text):
    """
    Parse an ALPM desc/files-style record.

    Only these fields are retained:

        NAME
        BASE
        DESC
        LICENSE
        DEPENDS
        MAKEDEPENDS
        FILES

    All other fields are ignored.
    """

    result = {}
    current_field = None

    for line in text.splitlines():
        line = line.rstrip("\n")

        if (
            line.startswith("%")
            and line.endswith("%")
        ):
            current_field = line[1:-1]

            if current_field in SUPPORTED_FILES_FIELDS:
                result[current_field] = []

            continue

        if (
            current_field in SUPPORTED_FILES_FIELDS
            and line
        ):
            result.setdefault(
                current_field,
                [],
            ).append(line)

    return {
        field: values
        for field, values in result.items()
        if values
    }


# ============================================================================
# ALPM decompression
# ============================================================================

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
        zstd = shutil.which(
            "zstd"
        )

        if not zstd:
            raise RuntimeError(
                "zstd is required to "
                "read Arch repository "
                "databases"
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
                "zstd database"
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
    for base_url in arch_repo_urls(
        repository
    ):
        yield (
            base_url
            + f"{repository}.files"
        )

        yield (
            base_url
            + f"{repository}.files.tar.gz"
        )

        yield (
            base_url
            + f"{repository}.files.tar.xz"
        )

        yield (
            base_url
            + f"{repository}.files.tar.zst"
        )


def arch_db_repo_urls(repository):
    for base_url in arch_repo_urls(
        repository
    ):
        yield (
            base_url
            + f"{repository}.db"
        )

        yield (
            base_url
            + f"{repository}.db.tar.gz"
        )

        yield (
            base_url
            + f"{repository}.db.tar.xz"
        )

        yield (
            base_url
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
    """
    Parse [repo].files.

    Uses only:

        NAME
        BASE
        DESC
        LICENSE
        DEPENDS
        MAKEDEPENDS
        FILES

    Example stored package:

        {
            "name": "acl",
            "base": "acl",
            "desc": "...",
            "license": [...],
            "depends": [...],
            "makedepends": [...],
            "files": [...]
        }
    """

    tar_data = decompress_database(
        data,
        filename,
    )

    packages = {}

    with tempfile.TemporaryDirectory(
        prefix="kuzpkg-arch-files-"
    ) as temporary_directory:
        temporary_directory = Path(
            temporary_directory
        )

        tar_path = (
            temporary_directory
            / "files.tar"
        )

        tar_path.write_bytes(
            tar_data
        )

        with tarfile.open(
            tar_path,
            "r",
        ) as archive:
            members = archive.getmembers()

            package_dirs = [
                member
                for member in members
                if member.isdir()
            ]

            for member in package_dirs:
                package_directory = (
                    member.name.rstrip("/")
                )

                if not package_directory:
                    continue

                desc_path = (
                    f"{package_directory}/desc"
                )

                files_path = (
                    f"{package_directory}/files"
                )

                desc_member = (
                    archive.extractfile(
                        desc_path
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

                metadata = (
                    parse_alpm_description(
                        desc_text
                    )
                )

                names = metadata.get(
                    "NAME",
                    [],
                )

                if not names:
                    continue

                name = names[0]

                bases = metadata.get(
                    "BASE",
                    [],
                )

                descriptions = metadata.get(
                    "DESC",
                    [],
                )

                licenses = metadata.get(
                    "LICENSE",
                    [],
                )

                depends = metadata.get(
                    "DEPENDS",
                    [],
                )

                makedepends = metadata.get(
                    "MAKEDEPENDS",
                    [],
                )

                file_list = []

                files_member = (
                    archive.extractfile(
                        files_path
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

                    in_files_section = False

                    for line in (
                        files_text.splitlines()
                    ):
                        if line == "%FILES%":
                            in_files_section = True
                            continue

                        if (
                            in_files_section
                            and line
                            and not line.startswith("%")
                        ):
                            file_list.append(
                                line
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

    cached_json = (
        cache_dir
        / f"{repository}.json"
    )

    if (
        cached_json.exists()
        and not refresh
    ):
        try:
            data = json.loads(
                cached_json.read_text(
                    errors="replace"
                )
            )

            if isinstance(
                data,
                dict,
            ):
                return data

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
                data = (
                    cache_file.read_bytes()
                )
            else:
                data = fetch_url(url)

                temporary = (
                    cache_file.with_suffix(
                        cache_file.suffix
                        + ".tmp"
                    )
                )

                temporary.write_bytes(
                    data
                )

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
                    cached_json.with_suffix(
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
                    cached_json
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

            for name, info in packages.items():
                info = dict(info)
                info["repo"] = repository
                info["source"] = "files"

                combined[name] = info

            if not quiet:
                println(
                    f"  {repository}: "
                    f"{len(packages)} packages"
                )

        except RuntimeError as exc:
            if not quiet:
                println(
                    f"  {repository}: "
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
    """
    Parse [repo].db.

    The .db database is used mainly to supplement repository metadata.
    .files remains the authoritative source for:
        NAME
        BASE
        DESC
        LICENSE
        DEPENDS
        MAKEDEPENDS
        FILES
    """

    tar_data = decompress_database(
        data,
        filename,
    )

    packages = {}

    with tempfile.TemporaryDirectory(
        prefix="kuzpkg-arch-db-"
    ) as temporary_directory:
        temporary_directory = Path(
            temporary_directory
        )

        tar_path = (
            temporary_directory
            / "db.tar"
        )

        tar_path.write_bytes(
            tar_data
        )

        with tarfile.open(
            tar_path,
            "r",
        ) as archive:
            for member in archive.getmembers():
                if not member.isdir():
                    continue

                package_directory = (
                    member.name.rstrip("/")
                )

                if not package_directory:
                    continue

                desc_member = (
                    archive.extractfile(
                        f"{package_directory}/desc"
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

                metadata = (
                    parse_alpm_description(
                        desc_text
                    )
                )

                names = metadata.get(
                    "NAME",
                    [],
                )

                if not names:
                    continue

                name = names[0]

                version_values = (
                    parse_alpm_description(
                        desc_text
                    ).get(
                        "VERSION",
                        [],
                    )
                )

                packages[name] = {
                    "name": name,
                    "repo": repository,
                    "version": (
                        version_values[0]
                        if version_values
                        else ""
                    ),
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

    cached_json = (
        cache_dir
        / f"{repository}.json"
    )

    if (
        cached_json.exists()
        and not refresh
    ):
        try:
            data = json.loads(
                cached_json.read_text(
                    errors="replace"
                )
            )

            if isinstance(
                data,
                dict,
            ):
                return data

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
                data = (
                    cache_file.read_bytes()
                )
            else:
                data = fetch_url(url)

                temporary = (
                    cache_file.with_suffix(
                        cache_file.suffix
                        + ".tmp"
                    )
                )

                temporary.write_bytes(
                    data
                )

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

            for name, info in packages.items():
                info = dict(info)
                info["repo"] = repository
                info["source"] = "db"

                if name not in combined:
                    combined[name] = info
                else:
                    combined[name].update(
                        info
                    )

            if not quiet:
                println(
                    f"  {repository}: "
                    f"{len(packages)} packages"
                )

        except RuntimeError as exc:
            if not quiet:
                println(
                    f"  {repository}: "
                    f"unavailable: {exc}"
                )

    return combined


# ============================================================================
# Combine Arch .db + .files
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

    # .db provides fallback package information.
    for name, info in db_packages.items():
        combined[name] = dict(info)

    # .files takes precedence for the fields we explicitly use.
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
                    "",
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
# Arch file ownership index
# ============================================================================

def build_arch_file_index(
    arch_packages,
):
    index = defaultdict(
        set
    )

    for package_name, info in (
        arch_packages.items()
    ):
        for filename in info.get(
            "files",
            [],
        ):
            filename = str(
                filename
            ).strip()

            if not filename:
                continue

            normalized = (
                filename
                .lstrip("/")
            )

            if not normalized:
                continue

            index[
                normalized
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
# DPKG database
# ============================================================================

def parse_dpkg_status(root):
    root = Path(root)

    status_path = (
        root
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

        package_name = fields.get(
            "Package"
        )

        status = fields.get(
            "Status",
            "",
        )

        if not package_name:
            continue

        if not status.startswith(
            "install ok installed"
        ):
            continue

        packages[package_name] = fields

    return packages


def dpkg_package_files(
    root,
    package_name,
):
    root = Path(root)

    files_dir = (
        root
        / DPKG_INFO_DEFAULT.lstrip("/")
    )

    files_path = (
        files_dir
        / f"{package_name}.list"
    )

    if not files_path.exists():
        return []

    try:
        return [
            line.strip()
            for line in (
                files_path.read_text(
                    errors="replace"
                ).splitlines()
            )
            if line.strip()
        ]

    except OSError:
        return []


def load_dpkg_packages(
    root,
):
    packages = parse_dpkg_status(
        root
    )

    for name, fields in packages.items():
        fields = dict(fields)

        fields["Files"] = (
            dpkg_package_files(
                root,
                name,
            )
        )

        packages[name] = fields

    return packages


# ============================================================================
# DPKG -> Arch matching
# ============================================================================

def normalize_distribution_name(
    name,
):
    value = name.strip()

    value = re.sub(
        r":(?:amd64|arm64|i386|armhf|all)$",
        "",
        value,
        flags=re.IGNORECASE,
    )

    value = value.lower()

    aliases = {
        "gcc-13": "gcc",
        "gcc-14": "gcc",
        "gcc-15": "gcc",
        "gcc-16": "gcc",
        "python3": "python",
        "python3-dev": "python",
        "python3-pip": "python-pip",
        "libssl3": "openssl",
        "libssl-dev": "openssl",
        "libz-dev": "zlib",
        "zlib1g": "zlib",
        "zlib1g-dev": "zlib",
        "libncurses6": "ncurses",
        "libncurses-dev": "ncurses",
        "libreadline8": "readline",
        "libreadline-dev": "readline",
        "libexpat1": "expat",
        "libexpat1-dev": "expat",
        "libffi8": "libffi",
        "libffi-dev": "libffi",
        "libxml2": "libxml2",
        "libxml2-dev": "libxml2",
        "libxslt1.1": "libxslt",
        "libxslt1-dev": "libxslt",
        "libjpeg62-turbo": "libjpeg-turbo",
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


def arch_name_candidates_from_dpkg(
    dpkg_name,
):
    normalized = (
        normalize_distribution_name(
            dpkg_name
        )
    )

    candidates = [
        dpkg_name,
        normalized,
    ]

    if normalized.endswith(
        "-dev"
    ):
        candidates.append(
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
            candidates.append(
                normalized[
                    len(prefix):
                ]
            )

    result = []

    for candidate in candidates:
        candidate = candidate.strip()

        if (
            candidate
            and candidate not in result
        ):
            result.append(
                candidate
            )

    return result


def map_dpkg_package_to_arch(
    dpkg_name,
    arch_packages,
):
    for candidate in (
        arch_name_candidates_from_dpkg(
            dpkg_name
        )
    ):
        if candidate in arch_packages:
            return (
                candidate,
                "dpkg-name",
            )

    normalized = (
        normalize_distribution_name(
            dpkg_name
        )
    )

    best = None
    best_score = -1

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

        elif (
            arch_normalized in normalized
            or normalized in arch_normalized
        ):
            if min(
                len(arch_normalized),
                len(normalized),
            ) >= 5:
                score = 40

        if score > best_score:
            best = arch_name
            best_score = score

    if (
        best is not None
        and best_score >= 70
    ):
        return (
            best,
            "dpkg-name-fuzzy",
        )

    return (
        None,
        None,
    )


# ============================================================================
# Package version detection
# ============================================================================

def version_from_text(text):
    for line in text.splitlines()[:100]:
        match = VERSION_RE.search(
            line
        )

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
    root = Path(root)

    # The Arch repository .files data does not need to be re-probed
    # when the package metadata already has a version from .db.
    info = arch_packages.get(
        name
    )

    if info:
        repository_version = (
            info.get("version", "")
        )

        if repository_version:
            return (
                repository_version,
                "arch-repository",
            )

    base = (
        name
        .removeprefix("lib32-")
        .removeprefix("libx32-")
    )

    candidates = []

    for directory in (
        *BIN_DIRS,
        *FIREFOX_DIRS,
    ):
        candidates.append(
            root
            / directory.lstrip("/")
            / base
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

        version = probe_version(
            path
        )

        if version:
            return (
                version,
                "version-probe",
            )

    if name == "firefox":
        for filename in (
            "/usr/lib/firefox/platform.ini",
            "/lib/firefox/platform.ini",
        ):
            path = (
                root
                / filename.lstrip("/")
            )

            try:
                match = re.search(
                    r"^Version=([^\n]+)",
                    path.read_text(
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

    return (
        "0.0.0+detected",
        "unverified",
    )


# ============================================================================
# Candidate storage
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
    item["types"].add(
        kind
    )
    item["sources"].add(
        source
    )

    if len(
        item["evidence"]
    ) < 100:
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
    # Exact Arch file ownership.
    # ------------------------------------------------------------------
    for path in scan_tree(
        root,
        lambda path: True,
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

        if (
            package_name
            in installed_kuzpkg
        ):
            continue

        add_candidate(
            candidates,
            package_name,
            path,
            "arch-file-owner",
            "arch-db-files",
        )

    # ------------------------------------------------------------------
    # Shared libraries.
    # ------------------------------------------------------------------
    for path, prefix in scan_files(
        root,
        LIB_DIRS,
        lambda path: (
            ".so" in path.name
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
            guess = package_guess(
                path
            )

            variants = []

            if guess:
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

        if (
            package_name
            in installed_kuzpkg
        ):
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
            guess = package_guess(
                path
            )

            if (
                guess
                and guess in arch_packages
            ):
                package_name = guess

        if not package_name:
            continue

        if (
            package_name
            in installed_kuzpkg
        ):
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
        lambda path: (
            path.name.endswith(".pc")
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
            guess = package_guess(
                path
            )

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

        if (
            package_name
            in installed_kuzpkg
        ):
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
    firefox_paths = scan_firefox(
        root
    )

    if (
        firefox_paths
        and "firefox" in arch_packages
        and "firefox" not in installed_kuzpkg
    ):
        for path in firefox_paths:
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
    # Module roots.
    # ------------------------------------------------------------------
    for path in scan_module_roots(
        root
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
            guess = package_guess(
                path
            )

            if (
                guess
                and guess in arch_packages
            ):
                package_name = guess

        if not package_name:
            continue

        if (
            package_name
            in installed_kuzpkg
        ):
            continue

        add_candidate(
            candidates,
            package_name,
            path,
            "module",
            "filesystem",
        )

    # ------------------------------------------------------------------
    # DPKG -> Arch mapping.
    # ------------------------------------------------------------------
    if include_dpkg:
        for dpkg_name, info in (
            dpkg_packages.items()
        ):
            package_name, source = (
                map_dpkg_package_to_arch(
                    dpkg_name,
                    arch_packages,
                )
            )

            if not package_name:
                continue

            if (
                package_name
                in installed_kuzpkg
            ):
                continue

            files = info.get(
                "Files",
                [],
            )

            existing_files = []

            for filename in files:
                path = (
                    root
                    / filename.lstrip("/")
                )

                if path.exists():
                    existing_files.append(
                        path
                    )

            if existing_files:
                for path in existing_files[:100]:
                    add_candidate(
                        candidates,
                        package_name,
                        path,
                        "dpkg-mapped",
                        source,
                    )
            else:
                synthetic_path = (
                    root
                    / (
                        "<dpkg:"
                        f"{dpkg_name}>"
                    )
                )

                add_candidate(
                    candidates,
                    package_name,
                    synthetic_path,
                    "dpkg-mapped",
                    source,
                )

    # ------------------------------------------------------------------
    # Minimum threshold.
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
                if (
                    Path(path).exists()
                    or str(path).find(
                        "<dpkg:"
                    ) >= 0
                )
            },
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

    result.sort(
        key=lambda item: (
            item[0].lower(),
            -item[1],
        )
    )

    return result


# ============================================================================
# .PKGINFO creation
# ============================================================================

def pkginfo_escape(
    value,
):
    return (
        str(value)
        .replace(
            "\\",
            "\\\\",
        )
        .replace(
            "\n",
            " ",
        )
    )


def get_arch_package_info(
    name,
    arch_packages,
):
    info = arch_packages.get(
        name
    )

    if info is None:
        return {
            "name": name,
            "base": name,
            "desc": "",
            "license": [],
            "depends": [],
            "makedepends": [],
            "version": "",
            "repo": "",
        }

    return info


def make_pkginfo(
    name,
    package_info,
    version,
    pkgrel,
    architecture,
):
    pkginfo = []

    pkginfo.append(
        "# Generated by kuzpkg-untracked"
    )

    pkginfo.append(
        f"pkgname = "
        f"{pkginfo_escape(name)}"
    )

    base = package_info.get(
        "base",
        name,
    )

    if base:
        pkginfo.append(
            f"pkgbase = "
            f"{pkginfo_escape(base)}"
        )

    pkginfo.append(
        f"pkgver = "
        f"{pkginfo_escape(version)}"
    )

    pkginfo.append(
        f"pkgrel = "
        f"{pkginfo_escape(pkgrel)}"
    )

    pkginfo.append(
        f"arch = "
        f"{pkginfo_escape(architecture)}"
    )

    description = (
        package_info.get(
            "desc",
            "",
        )
        or ""
    )

    if description:
        pkginfo.append(
            f"pkgdesc = "
            f"{pkginfo_escape(description)}"
        )

    for license_name in (
        package_info.get(
            "license",
            [],
        )
    ):
        pkginfo.append(
            f"license = "
            f"{pkginfo_escape(license_name)}"
        )

    for dependency in (
        package_info.get(
            "depends",
            [],
        )
    ):
        pkginfo.append(
            f"depend = "
            f"{pkginfo_escape(dependency)}"
        )

    # MAKEDEPENDS are intentionally included in
    # the generated metadata even though they are
    # build-time dependencies rather than runtime
    # dependencies.
    for dependency in (
        package_info.get(
            "makedepends",
            [],
        )
    ):
        pkginfo.append(
            f"makedepend = "
            f"{pkginfo_escape(dependency)}"
        )

    repo = package_info.get(
        "repo",
        "",
    )

    if repo:
        pkginfo.append(
            f"x_kuzpkg_repo = "
            f"{pkginfo_escape(repo)}"
        )

    return (
        "\n".join(pkginfo)
        + "\n"
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
    output_directory,
    pkgrel,
    architecture,
    overwrite,
    arch_packages,
):
    root = Path(root)
    output_directory = Path(
        output_directory
    )

    package_info = get_arch_package_info(
        name,
        arch_packages,
    )

    version = (
        package_info.get(
            "version",
            "",
        )
        or ""
    )

    version_source = (
        "arch-repository"
        if version
        else "unverified"
    )

    if not version:
        version, version_source = (
            detect_version(
                name,
                evidence,
                root,
                arch_packages,
            )
        )

    filename = (
        f"{safe(name)}-"
        f"{safe(version)}-"
        f"{safe(pkgrel)}-"
        f"{safe(architecture)}"
        ".kuzpkg.tar.zst"
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
            version_source,
            False,
        )

    zstd = shutil.which(
        "zstd"
    )

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
        "format": (
            "kuzpkg-detected-v6"
        ),
        "name": name,
        "base": package_info.get(
            "base",
            name,
        ),
        "version": version,
        "pkgrel": pkgrel,
        "arch": architecture,
        "repo": package_info.get(
            "repo",
            "",
        ),
        "description": package_info.get(
            "desc",
            "",
        ),
        "license": package_info.get(
            "license",
            [],
        ),
        "depends": package_info.get(
            "depends",
            [],
        ),
        "makedepends": package_info.get(
            "makedepends",
            [],
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

    pkginfo_text = make_pkginfo(
        name,
        package_info,
        version,
        pkgrel,
        architecture,
    )

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

        pkginfo_path = (
            temporary_directory
            / ".PKGINFO"
        )

        metadata_path.write_text(
            json.dumps(
                metadata,
                indent=2,
            )
            + "\n"
        )

        pkginfo_path.write_text(
            pkginfo_text
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

                added.add(
                    relative
                )

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
            "Arch repository .db/.files metadata "
            "and local dpkg data"
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
            "minimum number of local "
            "evidence items"
        ),
    )

    parser.add_argument(
        "-q",
        "--quiet",
        action="store_true",
        help=(
            "print only detected package names"
        ),
    )

    parser.add_argument(
        "--no-package",
        action="store_true",
        help=(
            "detect only; do not create "
            "archives"
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
        help="kuzpkg package release",
    )

    parser.add_argument(
        "--arch",
        help=(
            "output architecture; "
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
            "cache directory for "
            "Arch .db files"
        ),
    )

    parser.add_argument(
        "--arch-files-cache",
        default=ARCH_FILES_CACHE_DEFAULT,
        help=(
            "cache directory for "
            "Arch .files files"
        ),
    )

    parser.add_argument(
        "--refresh-arch",
        action="store_true",
        help=(
            "refresh Arch .db/.files "
            "databases"
        ),
    )

    parser.add_argument(
        "--no-dpkg",
        action="store_true",
        help=(
            "disable Debian/dpkg "
            "mapping"
        ),
    )

    args = parser.parse_args()

    if args.minimum < 1:
        parser.error(
            "--minimum must be at least 1"
        )

    # ------------------------------------------------------------------
    # Arch repository metadata
    # ------------------------------------------------------------------
    try:
        arch_packages = (
            load_arch_packages(
                args.arch_db_cache,
                args.arch_files_cache,
                refresh=args.refresh_arch,
                quiet=args.quiet,
            )
        )

    except RuntimeError as exc:
        stderr_write(
            "kuzpkg-untracked: "
            "error loading Arch metadata: "
            f"{exc}\n",
            flush=True,
        )
        return 1

    # ------------------------------------------------------------------
    # Arch ownership index from .files
    # ------------------------------------------------------------------
    arch_file_index = (
        build_arch_file_index(
            arch_packages
        )
    )

    # ------------------------------------------------------------------
    # DPKG
    # ------------------------------------------------------------------
    dpkg_packages = {}

    if not args.no_dpkg:
        dpkg_packages = (
            load_dpkg_packages(
                args.root
            )
        )

        if (
            dpkg_packages
            and not args.quiet
        ):
            println(
                f"Loaded "
                f"{len(dpkg_packages)} "
                "installed dpkg records"
            )

    # ------------------------------------------------------------------
    # Detection
    # ------------------------------------------------------------------
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
            include_dpkg=(
                not args.no_dpkg
            ),
        )

        root = Path(
            args.root
        ).resolve()

        output_arch, arch_source = (
            detect_arch(
                args.arch
            )
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
    # Quiet
    # ------------------------------------------------------------------
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

    # ------------------------------------------------------------------
    # Empty
    # ------------------------------------------------------------------
    if not candidates:
        println(
            "No untracked package "
            "candidates found"
        )
        return 0

    # ------------------------------------------------------------------
    # Discovery-only
    # ------------------------------------------------------------------
    if args.no_package:
        for (
            name,
            count,
            types,
            sources,
            evidence,
        ) in candidates:
            package_info = (
                arch_packages.get(
                    name,
                    {},
                )
            )

            version, source = (
                detect_version(
                    name,
                    evidence,
                    root,
                    arch_packages,
                )
            )

            println(
                f"{name:<44} "
                f"{version:<24} "
                f"{count:>4} evidence "
                f"[repo="
                f"{package_info.get('repo', '')}; "
                f"{source}; "
                f"{','.join(sorted(sources))}]"
            )

        return 0

    # ------------------------------------------------------------------
    # Package creation
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
        sources,
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
                version_source,
                was_created,
            ) = make_archive(
                name,
                count,
                types,
                sources,
                evidence,
                root,
                output_directory,
                args.pkgrel,
                output_arch,
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

    # ------------------------------------------------------------------
    # Final
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
        "Compressed all detected "
        "packages to "
        f"{output_directory}"
    )

    return 0


if __name__ == "__main__":
    raise SystemExit(
        main()
    )
