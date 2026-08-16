#!/usr/bin/env python3
"""
kuzpkg-untracked.py

Simple, dependency-light filesystem adopter/reconstructor for kuzpkg.

It does NOT use Arch repositories, ALPM, pacman, or repo metadata.
It scans the real filesystem, groups evidence into package candidates,
shows them in a small ncurses/2009-style TUI, and can reconstruct:

    <pkgname>-<pkgver>-<pkgrel>-<arch>.kuzpkg.tar.zst

The archive contains .PKGINFO plus the actual files discovered for the
candidate.  It also knows about common LFS/BLFS packages, the Linux kernel,
firmware/module trees, and language ecosystems such as Python, Ruby, Perl,
Node.js, Rust/Cargo, Go, Java, PHP, Lua and Tcl.
"""

from __future__ import annotations

import argparse
import curses
import json
import os
import platform
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

ARCHIVE_SUFFIX = ".kuzpkg.tar.zst"

# Deliberately boring: no repository configuration and no network discovery.
# These are filesystem roots that normally do not describe installed package
# payloads and may contain huge/volatile trees or user data.
EXCLUDED_ROOTS = {
    "home", "root", "proc", "run", "sys", "dev", "tmp", "mnt", "srv"
}

LIB_DIRS = (
    "/lib", "/lib64", "/lib32", "/libx32",
    "/usr/lib", "/usr/lib64", "/usr/lib32", "/usr/libx32",
)
BIN_DIRS = ("/bin", "/sbin", "/usr/bin", "/usr/sbin")
INCLUDE_DIRS = ("/include", "/usr/include", "/usr/local/include")
PKGCONFIG_DIRS = (
    "/usr/lib/pkgconfig", "/usr/lib64/pkgconfig", "/usr/lib32/pkgconfig",
    "/usr/libx32/pkgconfig", "/usr/share/pkgconfig", "/usr/local/lib/pkgconfig",
)

# Known multi-binary LFS packages.  These are hints; only files that really
# exist are claimed.
KNOWN_BINARIES: dict[str, list[str]] = {
    "coreutils": "ls cat cp mv rm mkdir rmdir touch chmod chown chgrp ln df du echo printf sort uniq cut tr wc head tail basename dirname pwd sleep true false yes date env expr install mktemp nice nohup od readlink realpath seq shred shuf split stat sync tac tee timeout tty uname unlink who whoami id groups logname nproc numfmt base64 md5sum sha1sum sha224sum sha256sum sha384sum sha512sum comm fmt fold join nl dd dir vdir test expr factor hostid pathchk pinky stty sum tsort uname users uptime users".split(),
    "util-linux": "mount umount fdisk sfdisk blkid lsblk losetup swapon swapoff mkswap fsck dmesg hwclock kill lscpu findmnt flock nsenter unshare uuidgen wipefs column script rev look logger agetty setterm rename renice chcpu choom ionice lslocks lslogins lsns mountpoint namei taskset wdctl".split(),
    "procps-ng": "ps top free pkill pgrep pmap pwdx slabtop sysctl tload uptime vmstat w watch".split(),
    "findutils": ["find", "xargs", "locate", "updatedb"],
    "e2fsprogs": "mke2fs mkfs.ext2 mkfs.ext3 mkfs.ext4 e2fsck fsck.ext2 fsck.ext3 fsck.ext4 tune2fs dumpe2fs resize2fs debugfs badblocks e2label filefrag lsattr chattr uuidgen".split(),
    "shadow": "passwd login useradd userdel usermod groupadd groupdel groupmod chage chfn chsh gpasswd newgrp newusers pwck grpck vipw vigr faillog lastlog nologin chpasswd".split(),
    "binutils": "ld as ar nm objdump objcopy ranlib strip readelf addr2line size strings c++filt gprof elfedit gold lto-dump".split(),
}

KNOWN_LIBS: dict[str, list[str]] = {
    "glibc": "c m pthread dl rt resolv util crypt nsl nss_files nss_dns".split(),
    "util-linux": "uuid blkid mount smartcols fdisk".split(),
    "zlib": ["z"],
    "openssl": ["ssl", "crypto"],
    "curl": ["curl"],
    "pcre2": ["pcre2-8", "pcre2-posix"],
    "expat": ["expat"],
    "libxml2": ["xml2"],
    "ncurses": "ncurses ncursesw panel panelw form formw menu menuw".split(),
    "readline": ["readline", "history"],
    "libffi": ["ffi"],
    "libcap": ["cap", "psx"],
    "libstdc++": ["stdc++"],
    "libgcc": ["gcc_s"],
}

KNOWN_DATA: dict[str, list[str]] = {
    "ca-certificates": ["/etc/ssl/certs/ca-certificates.crt", "/etc/ssl/cert.pem"],
    "linux-api-headers": ["/usr/include/linux/version.h"],
    "bash": ["/usr/share/doc/bash"],
}

# Kernel payload hints. The actual running kernel is detected separately and
# these trees are grouped under the linux package rather than being interpreted
# as thousands of unrelated package names.
KERNEL_ROOTS = (
    "/boot", "/lib/modules", "/usr/lib/modules", "/usr/src/linux-headers"
)
KERNEL_FILES = (
    "vmlinuz", "bzImage", "Image", "kernel", "System.map", "config"
)

ECOSYSTEM_MARKERS: dict[str, tuple[str, ...]] = {
    "python": ("site-packages", "dist-packages", ".dist-info", ".egg-info", ".py"),
    "rust": ("/.cargo/registry/", "/.cargo/git/", "Cargo.toml", "Cargo.lock", ".rlib", ".rmeta"),
    "ruby": ("/gems/", ".gemspec", ".gem", "vendor_ruby", ".rb"),
    "perl": (".pm", ".pod", "/perl"),
    "node": ("node_modules", "package.json", ".node", ".mjs", ".cjs"),
    "go": ("go.mod", "go.sum", "/pkg/mod/", ".a"),
    "java": (".jar", ".class", "pom.xml", "build.gradle", ".war", ".ear"),
    "php": ("composer.json", "composer.lock", ".phar", ".php"),
    "lua": (".lua", ".luac"),
    "tcl": (".tcl", ".tm"),
    "wasm": (".wasm",),
}

VERSION_RE = re.compile(
    r"(?<![A-Za-z0-9])v?"
    r"(\d+(?:\.\d+){1,8}(?:[-+._~][0-9A-Za-z._~+-]+)?)"
)
PYTHON_TAG_RE = re.compile(r"(?:-py\d+)?\.cpython[-_]\d+(?:[-_][A-Za-z0-9]+)*$")


@dataclass
class Candidate:
    name: str
    files: set[str] = field(default_factory=set)
    kinds: set[str] = field(default_factory=set)
    version_hint: str = ""
    version_verified: bool = False
    score: int = 0

    def add(self, path: str, kind: str, score: int = 1) -> None:
        self.files.add(path)
        self.kinds.add(kind)
        self.score += score


# ---------------------------------------------------------------------------
# Paths and filesystem helpers
# ---------------------------------------------------------------------------

def normalize_root(root: str) -> Path:
    p = Path(root).resolve()
    if not p.is_dir():
        raise ValueError(f"root is not a directory: {p}")
    return p


def relpath(path: Path, root: Path) -> str:
    if root == Path("/"):
        return "/" + str(path).lstrip("/")
    try:
        return "/" + str(path.relative_to(root)).lstrip("/")
    except ValueError:
        return str(path)


def root_join(root: Path, absolute_path: str) -> Path:
    return Path(absolute_path) if root == Path("/") else root / absolute_path.lstrip("/")


def excluded(path: Path, root: Path) -> bool:
    try:
        rel = path.relative_to(root)
    except ValueError:
        return True
    return bool(rel.parts and rel.parts[0] in EXCLUDED_ROOTS)


def walk_files(root: Path) -> Iterable[Path]:
    """Safe recursive walk; skips volatile/user trees and symlink directories."""
    for current, dirs, files in os.walk(root, topdown=True, followlinks=False):
        cur = Path(current)
        dirs[:] = [
            d for d in dirs
            if not excluded(cur / d, root) and not (cur / d).is_symlink()
        ]
        for name in files:
            path = cur / name
            try:
                if path.is_symlink() or path.is_file():
                    yield path
            except OSError:
                continue


def safe_read_text(path: Path, limit: int = 65536) -> str:
    try:
        with path.open("r", encoding="utf-8", errors="replace") as f:
            return f.read(limit)
    except OSError:
        return ""


def executable(path: Path) -> bool:
    try:
        return path.is_file() and os.access(path, os.X_OK)
    except OSError:
        return False


def which_in_root(binary: str, root: Path) -> Path | None:
    for directory in BIN_DIRS:
        p = root_join(root, directory) / binary
        if executable(p):
            return p
    return None


# ---------------------------------------------------------------------------
# Version probing
# ---------------------------------------------------------------------------

def probe_version(path: Path) -> tuple[str, bool]:
    if executable(path):
        for option in ("--version", "-version", "-V"):
            try:
                proc = subprocess.run(
                    [str(path), option],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    timeout=2,
                    check=False,
                    env={**os.environ, "LC_ALL": "C", "LANG": "C"},
                )
                match = VERSION_RE.search(proc.stdout or "")
                if match:
                    return match.group(1), True
            except (OSError, subprocess.SubprocessError):
                pass
    return "", False


def version_from_text(text: str) -> str:
    match = VERSION_RE.search(text)
    return match.group(1) if match else ""


def kernel_version(root: Path) -> tuple[str, bool]:
    # Prefer the actual uname string only when scanning the host root.
    if root == Path("/"):
        value = platform.release()
        if value:
            return value, True

    for path in (
        root_join(root, "/proc/version"),
        root_join(root, "/usr/include/linux/version.h"),
    ):
        text = safe_read_text(path)
        version = version_from_text(text)
        if version:
            return version, True

    return "", False


# ---------------------------------------------------------------------------
# Package-name inference
# ---------------------------------------------------------------------------

def strip_shared_library(name: str) -> str:
    base = name.split(".so", 1)[0]
    if base.startswith("lib"):
        base = base[3:]
    return base


def python_name(path: Path) -> str:
    name = path.name
    if name.endswith(".dist-info"):
        name = name[:-10]
    elif name.endswith(".egg-info"):
        name = name[:-9]
    elif name.endswith(".py"):
        name = name[:-3]
    name = PYTHON_TAG_RE.sub("", name)
    name = re.sub(r"[-_.]?(?:\d+)(?:[._-]\d+)*$", "", name)
    return name or "python-module"


def node_name(path: Path) -> str:
    parts = path.parts
    lower = [p.lower() for p in parts]
    if "node_modules" in lower:
        i = lower.index("node_modules")
        if i + 1 < len(parts):
            name = parts[i + 1]
            if name.startswith("@") and i + 2 < len(parts):
                return f"{name}-{parts[i + 2]}"
            return name
    return path.stem or "node-module"


def cargo_name(path: Path) -> str:
    data = safe_read_text(path)
    m = re.search(r"(?m)^name\s*=\s*['\"]([^'\"]+)['\"]", data)
    return m.group(1) if m else (path.parent.name or "rust-crate")


def infer_candidate(path: Path, root: Path) -> tuple[str, str, int] | None:
    rp = relpath(path, root)
    low = rp.lower()
    name = path.name

    # Firefox is intentionally one package, including everything below both
    # /lib/firefox and /usr/lib/firefox.
    if low.startswith("/lib/firefox/") or low.startswith("/usr/lib/firefox/"):
        return "firefox", "firefox", 8

    # Linux kernel and module payload.
    if (
        low.startswith("/lib/modules/")
        or low.startswith("/usr/lib/modules/")
        or (
            low.startswith("/boot/")
            and any(token in name.lower() for token in KERNEL_FILES)
        )
        or low.endswith("/linux/version.h")
    ):
        return "linux", "kernel", 10

    # Python ecosystem.
    if "site-packages/" in low or "dist-packages/" in low or name.endswith((".py", ".pyc", ".pyo", ".pyd")) or name.endswith((".dist-info", ".egg-info")):
        return python_name(path), "python", 3

    # Node.js modules.
    if "node_modules/" in low or name == "package.json" or name.endswith((".node", ".mjs", ".cjs")):
        return node_name(path), "node", 3

    # Rust / Cargo. Cargo.toml is especially useful because it exposes the
    # real crate name instead of guessing from one compiled artifact.
    if name == "Cargo.toml":
        return cargo_name(path), "rust", 5
    if "/.cargo/registry/" in low or name.endswith((".rlib", ".rmeta")):
        return path.parent.name or "rust-crate", "rust", 2

    # Ruby.
    if "/gems/" in low or name.endswith((".gemspec", ".gem")):
        return path.parent.name or "ruby-gem", "ruby", 2

    # Perl.
    if name.endswith((".pm", ".pod")) or "/perl" in low:
        return path.stem or "perl-module", "perl", 2

    # Go.
    if name in ("go.mod", "go.sum") or "/pkg/mod/" in low:
        return path.parent.name or "go-module", "go", 2

    # Java / PHP / Lua / Tcl / WASM.
    suffix = path.suffix.lower()
    simple_types = {
        ".jar": "java", ".class": "java", ".war": "java", ".ear": "java",
        ".phar": "php", ".php": "php", ".lua": "lua", ".luac": "lua",
        ".tcl": "tcl", ".tm": "tcl", ".wasm": "wasm",
    }
    if suffix in simple_types:
        return path.stem or simple_types[suffix], simple_types[suffix], 2

    # Shared libraries. Avoid treating every soname as a unique package.
    if ".so" in name:
        return strip_shared_library(name), "library", 5

    # pkg-config is a strong development-file signal.
    if name.endswith(".pc"):
        return name[:-3], "pkgconfig", 4

    # Static libraries and common developer payloads.
    if suffix in {".a", ".la"}:
        base = name.rsplit(".", 1)[0]
        if base.startswith("lib"):
            base = base[3:]
        return base or "library", "static-library", 2

    return None


def add_known_hints(root: Path, candidates: dict[str, Candidate]) -> None:
    for package, binaries in KNOWN_BINARIES.items():
        for binary in binaries:
            p = which_in_root(binary, root)
            if p:
                c = candidates.setdefault(package, Candidate(package))
                c.add(relpath(p, root), "lfs-bin", 5)
                if not c.version_hint:
                    version, verified = probe_version(p)
                    if version:
                        c.version_hint, c.version_verified = version, verified

    for package, libs in KNOWN_LIBS.items():
        c = None
        for base in LIB_DIRS:
            directory = root_join(root, base)
            if not directory.exists():
                continue
            for lib in libs:
                for p in directory.glob(f"lib{lib}.so*"):
                    if not p.exists():
                        continue
                    c = c or candidates.setdefault(package, Candidate(package))
                    c.add(relpath(p, root), "lfs-lib", 4)

    for package, paths in KNOWN_DATA.items():
        c = candidates.setdefault(package, Candidate(package))
        for absolute in paths:
            p = root_join(root, absolute)
            if p.exists():
                c.add(relpath(p, root), "lfs-data", 3)

    # Kernel package gets the real version when possible.
    kver, verified = kernel_version(root)
    kpaths = []
    for base in ("/lib/modules", "/usr/lib/modules"):
        p = root_join(root, base)
        if p.exists():
            for child in p.iterdir():
                if child.is_dir():
                    kpaths.extend(relpath(x, root) for x in walk_files(child))
                    if len(kpaths) > 50000:
                        break
    for base in ("/boot",):
        p = root_join(root, base)
        if p.exists():
            for child in p.iterdir():
                low = child.name.lower()
                if any(low.startswith(x.lower()) for x in KERNEL_FILES):
                    kpaths.append(relpath(child, root))
    if kpaths:
        c = candidates.setdefault("linux", Candidate("linux"))
        for item in kpaths:
            c.add(item, "kernel", 5)
        if kver:
            c.version_hint, c.version_verified = kver, verified


def discover(root: Path, skip_known_bins: bool = False) -> dict[str, Candidate]:
    candidates: dict[str, Candidate] = {}

    for path in walk_files(root):
        result = infer_candidate(path, root)
        if not result:
            continue
        name, kind, score = result
        # Prevent a flood of one-byte/generated cache artifacts.
        try:
            size = path.stat().st_size
        except OSError:
            size = 0
        if size == 0 and kind not in {"kernel", "python", "pkgconfig"}:
            continue
        c = candidates.setdefault(name, Candidate(name))
        c.add(relpath(path, root), kind, score)

        if not c.version_hint and executable(path) and ".so" not in path.name:
            version, verified = probe_version(path)
            if version:
                c.version_hint, c.version_verified = version, verified

    if not skip_known_bins:
        add_known_hints(root, candidates)

    return {name: c for name, c in candidates.items() if c.files}


# ---------------------------------------------------------------------------
# Metadata and archive writing
# ---------------------------------------------------------------------------

def architecture() -> str:
    return getattr(os, "uname", lambda: None)().machine if hasattr(os, "uname") else platform.machine()


def safe_token(value: str) -> str:
    value = re.sub(r"[^A-Za-z0-9+_.@-]+", "-", value)
    return value.strip("-") or "unknown"


def package_version(candidate: Candidate) -> tuple[str, bool]:
    if candidate.version_hint:
        return candidate.version_hint, candidate.version_verified
    return "0.0.0+detected", False


def package_description(candidate: Candidate) -> str:
    kinds = ", ".join(sorted(candidate.kinds))
    return f"Reconstructed from existing filesystem; evidence: {kinds}"


def pkginfo_text(candidate: Candidate, version: str, verified: bool, pkgrel: str, arch: str) -> str:
    lines = [
        f"pkgname = {candidate.name}",
        f"pkgver = {version}",
        f"pkgrel = {pkgrel}",
        f"arch = {arch}",
        "pkgtype = reconstructed",
        "origin = untracked-adoption",
        f"version_verified = {'true' if verified else 'false'}",
        f"description = {package_description(candidate)}",
        f"file_count = {len(candidate.files)}",
        f"evidence_score = {candidate.score}",
        f"types = {','.join(sorted(candidate.kinds))}",
    ]
    return "\n".join(lines) + "\n"


def tar_add_path(tar, source: Path, arcname: str) -> None:
    # Avoid dereferencing symlinks so the recreated package retains the same
    # filesystem relationship rather than copying the symlink target.
    info = tar.gettarinfo(str(source), arcname=arcname, recursive=False)
    if stat.S_ISREG(info.mode):
        with source.open("rb") as f:
            tar.addfile(info, f)
    else:
        tar.addfile(info)


def create_archive(
    candidate: Candidate,
    root: Path,
    output_dir: Path,
    pkgrel: str,
    arch: str,
    overwrite: bool = False,
) -> tuple[Path, bool, str]:
    version, verified = package_version(candidate)
    filename = f"{safe_token(candidate.name)}-{safe_token(version)}-{safe_token(pkgrel)}-{safe_token(arch)}{ARCHIVE_SUFFIX}"
    destination = output_dir / filename

    if destination.exists() and not overwrite:
        return destination, False, "already exists"

    zstd = shutil.which("zstd")
    if not zstd:
        return destination, False, "zstd command not found"

    output_dir.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="kuzpkg-rebuild-") as temp:
        stage = Path(temp)
        info_path = stage / ".PKGINFO"
        info_path.write_text(
            pkginfo_text(candidate, version, verified, pkgrel, arch),
            encoding="utf-8",
        )

        tar_path = stage / "payload.tar"
        with __import__("tarfile").open(tar_path, "w") as tar:
            tar_add_path(tar, info_path, ".PKGINFO")
            for rel in sorted(candidate.files):
                source = root_join(root, rel)
                try:
                    if not (source.exists() or source.is_symlink()):
                        continue
                    tar_add_path(tar, source, rel.lstrip("/"))
                except (OSError, ValueError):
                    continue

        temporary = destination.with_suffix(destination.suffix + ".tmp")
        process = subprocess.run(
            [zstd, "-q", "-f", str(tar_path), "-o", str(temporary)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if process.returncode != 0:
            try:
                temporary.unlink()
            except OSError:
                pass
            return destination, False, process.stderr.strip() or "zstd failed"
        temporary.replace(destination)

    return destination, True, "created"


# ---------------------------------------------------------------------------
# Optional local database hints — intentionally generic and non-Arch.
# ---------------------------------------------------------------------------

def existing_kuzpkg_names() -> set[str]:
    """Read package names from a simple local database if kuzpkg exposes one.

    Supported forms:
      * `kuzpkg -Qq` output
      * `KUZPKG_LOCALDB` text file with one package name per line

    Failure simply means there is no filtering by installed metadata.
    """
    names: set[str] = set()
    env_file = os.environ.get("KUZPKG_LOCALDB")
    if env_file:
        try:
            for line in Path(env_file).read_text(errors="replace").splitlines():
                line = line.strip()
                if line and not line.startswith("#"):
                    names.add(line.split()[0])
        except OSError:
            pass

    tool = shutil.which("kuzpkg")
    if tool:
        try:
            p = subprocess.run(
                [tool, "-Qq"],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
                timeout=5,
                check=False,
            )
            if p.returncode == 0:
                names.update(x.strip() for x in p.stdout.splitlines() if x.strip())
        except (OSError, subprocess.SubprocessError):
            pass
    return names


# ---------------------------------------------------------------------------
# ncurses TUI
# ---------------------------------------------------------------------------


def short_types(candidate: Candidate, width: int = 24) -> str:
    text = ",".join(sorted(candidate.kinds))
    return text if len(text) <= width else text[: width - 1] + "…"


def tui_title(stdscr, text: str) -> None:
    h, w = stdscr.getmaxyx()
    stdscr.attron(curses.A_REVERSE)
    stdscr.addnstr(0, 0, (" " + text).ljust(max(w, len(text) + 1)), w - 1)
    stdscr.attroff(curses.A_REVERSE)


def draw_tui(stdscr, rows, cursor, marked, message, mode="browse") -> None:
    curses.erasechar if False else None
    stdscr.erase()
    h, w = stdscr.getmaxyx()
    tui_title(stdscr, "kuzpkg-untracked  |  filesystem adoption  |  2009")
    if h < 8 or w < 60:
        stdscr.addstr(2, 0, "terminal too small; resize to at least 60x8")
        stdscr.refresh()
        return

    stdscr.addnstr(1, 0, "↑/↓ move  SPACE mark  A all  N none  P package  R rescan  Q quit", w - 1)
    stdscr.addnstr(2, 0, f"mode={mode}  candidates={len(rows)}  selected={len(marked)}", w - 1)

    visible = max(1, h - 7)
    start = max(0, min(cursor - visible // 2, max(0, len(rows) - visible)))
    end = min(len(rows), start + visible)

    y = 4
    for index in range(start, end):
        name, candidate = rows[index]
        marker = "*" if index in marked else " "
        version = candidate.version_hint or "0.0.0+detected"
        verified = "!" if not candidate.version_verified else " "
        line = f"{marker}{verified} {name:<30} {version:<20} {len(candidate.files):>6} {short_types(candidate):<24}"
        if index == cursor:
            stdscr.attron(curses.A_REVERSE)
        stdscr.addnstr(y, 0, line, w - 1)
        if index == cursor:
            stdscr.attroff(curses.A_REVERSE)
        y += 1

    stdscr.addnstr(h - 2, 0, message[: w - 1], w - 1)
    stdscr.attron(curses.A_REVERSE)
    stdscr.addnstr(h - 1, 0, " F1 help   ENTER details   P create archives ".ljust(w - 1), w - 1)
    stdscr.attroff(curses.A_REVERSE)
    stdscr.refresh()


def details(stdscr, candidate: Candidate) -> None:
    while True:
        stdscr.erase()
        h, w = stdscr.getmaxyx()
        tui_title(stdscr, f"{candidate.name} details")
        lines = [
            f"version: {candidate.version_hint or '0.0.0+detected'}",
            f"verified: {'yes' if candidate.version_verified else 'no'}",
            f"evidence score: {candidate.score}",
            f"types: {', '.join(sorted(candidate.kinds))}",
            f"files: {len(candidate.files)}",
            "",
        ]
        lines.extend(sorted(candidate.files))
        for i, line in enumerate(lines[: max(0, h - 3)], start=2):
            stdscr.addnstr(i, 0, line, w - 1)
        stdscr.addnstr(h - 1, 0, "Press any key to return", w - 1, curses.A_REVERSE)
        stdscr.refresh()
        stdscr.getch()
        return


def run_tui(candidates: dict[str, Candidate], root: Path, output: Path, pkgrel: str, arch: str, overwrite: bool) -> int:
    rows = sorted(candidates.items(), key=lambda item: (-item[1].score, item[0]))
    marked: set[int] = set()
    state = {"cursor": 0, "message": "Ready."}

    def loop(stdscr):
        curses.curs_set(0)
        try:
            stdscr.keypad(True)
        except curses.error:
            pass

        while True:
            draw_tui(stdscr, rows, state["cursor"], marked, state["message"])
            key = stdscr.getch()
            cursor = state["cursor"]

            if key in (ord("q"), ord("Q")):
                return
            if key == curses.KEY_UP:
                state["cursor"] = max(0, cursor - 1)
            elif key == curses.KEY_DOWN:
                state["cursor"] = min(max(0, len(rows) - 1), cursor + 1)
            elif key == curses.KEY_PPAGE:
                state["cursor"] = max(0, cursor - 10)
            elif key == curses.KEY_NPAGE:
                state["cursor"] = min(max(0, len(rows) - 1), cursor + 10)
            elif key == ord(" ") and rows:
                marked.symmetric_difference_update({cursor})
            elif key in (ord("a"), ord("A")):
                marked.update(range(len(rows)))
            elif key in (ord("n"), ord("N")):
                marked.clear()
            elif key in (10, 13) and rows:
                details(stdscr, rows[cursor][1])
            elif key in (ord("p"), ord("P")):
                if not marked:
                    state["message"] = "Nothing selected. Press SPACE or A first."
                    continue
                created = 0
                skipped = 0
                errors = []
                for index in sorted(marked):
                    name, candidate = rows[index]
                    archive, ok, message = create_archive(
                        candidate, root, output, pkgrel, arch, overwrite
                    )
                    if ok:
                        created += 1
                    elif message == "already exists":
                        skipped += 1
                    else:
                        errors.append(f"{name}: {message}")
                if errors:
                    state["message"] = f"created={created} skipped={skipped} error={errors[0]}"
                else:
                    state["message"] = f"created={created} skipped={skipped} -> {output}"
            elif key in (ord("r"), ord("R")):
                state["message"] = "Rescan is available from the command line (restart the tool)."
            elif key == curses.KEY_F1:
                help_lines = [
                    "kuzpkg-untracked",
                    "",
                    "This tool scans the existing filesystem only.",
                    "No Arch repository code, pacman DB, or network access is used.",
                    "",
                    "SPACE  mark/unmark    A all    N none",
                    "P      build .kuzpkg.tar.zst",
                    "ENTER  show candidate files",
                    "Q      quit",
                    "",
                    "! beside a version means it was inferred, not verified.",
                ]
                stdscr.erase()
                for i, line in enumerate(help_lines[: max(1, stdscr.getmaxyx()[0] - 1)]):
                    stdscr.addnstr(i, 0, line, stdscr.getmaxyx()[1] - 1)
                stdscr.getch()

    curses.wrapper(loop)
    return 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def print_table(candidates: dict[str, Candidate]) -> None:
    rows = sorted(candidates.values(), key=lambda c: (-c.score, c.name))
    print(f"{'PACKAGE':32} {'VERSION':22} {'FILES':>7}  TYPES")
    print("-" * 90)
    for c in rows:
        ver = c.version_hint or "0.0.0+detected"
        flag = "" if c.version_verified else "*"
        print(f"{c.name:32} {(ver + flag):22} {len(c.files):7d}  {','.join(sorted(c.kinds))}")


def package_all(candidates: dict[str, Candidate], root: Path, output: Path, pkgrel: str, arch: str, overwrite: bool) -> int:
    made = 0
    skipped = 0
    failed = 0
    for name in sorted(candidates):
        path, ok, message = create_archive(candidates[name], root, output, pkgrel, arch, overwrite)
        if ok:
            made += 1
            print(f"+ {path}")
        elif message == "already exists":
            skipped += 1
            print(f"= {path} (exists)")
        else:
            failed += 1
            print(f"! {name}: {message}", file=sys.stderr)
    print(f"done: created={made} skipped={skipped} failed={failed}")
    return 1 if failed else 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Reconstruct untracked Linux software into .kuzpkg.tar.zst packages"
    )
    parser.add_argument("--root", default="/", help="filesystem root/chroot to scan")
    parser.add_argument("--output-dir", default="./kuzpkg-reconstructed", help="archive output directory")
    parser.add_argument("--pkgrel", default="1")
    parser.add_argument("--arch", default=None, help="architecture (default: uname -m)")
    parser.add_argument("--overwrite", action="store_true")
    parser.add_argument("--no-tui", action="store_true", help="print candidates only")
    parser.add_argument("--package-all", action="store_true", help="rebuild every candidate")
    parser.add_argument("--ignore-localdb", action="store_true", help="do not hide packages already known to kuzpkg")
    parser.add_argument("--skip-known-hints", action="store_true", help="disable LFS/BLFS package hints")
    args = parser.parse_args()

    try:
        root = normalize_root(args.root)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    arch = args.arch or platform.machine() or "unknown"
    output = Path(args.output_dir).resolve()

    print("kuzpkg-untracked: scanning filesystem...")
    candidates = discover(root, skip_known_bins=args.skip_known_hints)

    if not args.ignore_localdb:
        known = existing_kuzpkg_names()
        for name in list(candidates):
            if name in known:
                del candidates[name]

    print(f"kuzpkg-untracked: {len(candidates)} candidate packages")

    if not candidates:
        return 0

    if args.package_all:
        return package_all(candidates, root, output, args.pkgrel, arch, args.overwrite)

    if args.no_tui:
        print_table(candidates)
        return 0

    return run_tui(candidates, root, output, args.pkgrel, arch, args.overwrite)


if __name__ == "__main__":
    raise SystemExit(main())
