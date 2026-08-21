#!/usr/bin/env python3
"""kuzpkg-untracked - reconstruct packages that were installed outside kuzpkg.

Detection is deliberately evidence based:
  * Arch Linux package metadata is used when a pacman database is available.
  * LFS/BLFS/GLFS hints add real files only when those files exist.
  * Filesystem scanning catches manually-built libraries, binaries, pkg-config
    files and development headers.
  * lib32 packages are treated as separate packages and are scanned from
    /lib32, /usr/lib32 and the x86_64 multilib package namespace.

The result is a candidate list or reconstructed .kuzpkg.tar.zst archives.
Nothing is installed or removed by this utility.
"""
from __future__ import annotations

import argparse
import curses
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import tarfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

ARCHIVE_SUFFIX = ".kuzpkg.tar.zst"
LIB_DIRS = ("/lib", "/lib64", "/lib32", "/libx32", "/usr/lib", "/usr/lib64", "/usr/lib32", "/usr/libx32", "/usr/local/lib")
BIN_DIRS = ("/bin", "/sbin", "/usr/bin", "/usr/sbin", "/usr/local/bin", "/usr/local/sbin")
PC_DIRS = ("/usr/lib/pkgconfig", "/usr/lib64/pkgconfig", "/usr/lib32/pkgconfig", "/usr/libx32/pkgconfig", "/usr/share/pkgconfig", "/usr/local/lib/pkgconfig")
INCLUDE_DIRS = ("/usr/include", "/usr/local/include")
EXCLUDED = {"home", "root", "proc", "run", "sys", "dev", "tmp", "mnt", "media", "srv"}
ARCH_LOCAL_DB = "/var/lib/pacman/local"

# Packages from LFS/BLFS plus common GLFS development/runtime packages.
KNOWN_BINARIES: dict[str, list[str]] = {
    "coreutils": "ls cat cp mv rm mkdir rmdir touch chmod chown chgrp ln df du echo printf sort uniq cut tr wc head tail basename dirname pwd sleep true false yes date env expr install mktemp nice nohup od readlink realpath seq shred shuf split stat sync tac tee timeout tty uname unlink who whoami id groups logname nproc numfmt base64 md5sum sha1sum sha224sum sha256sum sha384sum sha512sum comm fmt fold join nl dd dir vdir test factor hostid pathchk stty sum tsort users".split(),
    "util-linux": "mount umount fdisk sfdisk blkid lsblk losetup swapon swapoff mkswap fsck dmesg hwclock kill lscpu findmnt flock nsenter unshare uuidgen wipefs column script rev look logger agetty setterm rename renice chcpu choom ionice lslocks lslogins lsns mountpoint namei taskset wdctl".split(),
    "procps-ng": "ps top free pkill pgrep pmap pwdx slabtop sysctl tload uptime vmstat w watch".split(),
    "findutils": ["find", "xargs", "locate", "updatedb"],
    "e2fsprogs": "mke2fs mkfs.ext2 mkfs.ext3 mkfs.ext4 e2fsck fsck.ext2 fsck.ext3 fsck.ext4 tune2fs dumpe2fs resize2fs debugfs badblocks e2label filefrag lsattr chattr".split(),
    "shadow": "passwd login useradd userdel usermod groupadd groupdel groupmod chage chfn chsh gpasswd newgrp newusers pwck grpck vipw vigr faillog lastlog nologin chpasswd".split(),
    "binutils": "ld as ar nm objdump objcopy ranlib strip readelf addr2line size strings c++filt gprof elfedit gold lto-dump".split(),
    "bash": ["bash"], "gawk": ["gawk", "awk"], "sed": ["sed"], "grep": ["grep"],
    "gzip": ["gzip", "gunzip"], "bzip2": ["bzip2", "bunzip2"], "xz": ["xz", "unxz"],
    "tar": ["tar"], "make": ["make"], "m4": ["m4"], "bison": ["bison"], "flex": ["flex"],
    "diffutils": ["diff", "cmp", "diff3"], "patch": ["patch"], "gettext": ["gettext"],
    "texinfo": ["makeinfo", "install-info"], "perl": ["perl"], "python": ["python", "python3"],
}
KNOWN_LIBS: dict[str, list[str]] = {
    "glibc": "c m pthread dl rt resolv util crypt nsl nss_files nss_dns".split(),
    "util-linux": "uuid blkid mount smartcols fdisk".split(), "zlib": ["z"],
    "openssl": ["ssl", "crypto"], "curl": ["curl"], "pcre2": ["pcre2-8", "pcre2-posix"],
    "expat": ["expat"], "libxml2": ["xml2"],
    "ncurses": "ncurses ncursesw panel panelw form formw menu menuw".split(),
    "readline": ["readline", "history"], "libffi": ["ffi"], "libcap": ["cap", "psx"],
    "libstdc++": ["stdc++"], "libgcc": ["gcc_s"], "libxcrypt": ["crypt"],
    "libarchive": ["archive"], "sqlite": ["sqlite3"], "libpng": ["png16"],
    "libjpeg-turbo": ["jpeg", "turbojpeg"], "freetype2": ["freetype"],
    "fontconfig": ["fontconfig"], "libedit": ["edit"], "elfutils": ["elf", "dw", "debuginfod"],
    "llvm": ["LLVM"], "libtool": ["ltdl"],
}
KNOWN_DATA: dict[str, list[str]] = {
    "ca-certificates": ["/etc/ssl/certs/ca-certificates.crt", "/etc/ssl/cert.pem"],
    "linux-api-headers": ["/usr/include/linux/version.h"],
    "bash": ["/usr/share/doc/bash"],
}
# BLFS/GLFS packages whose binary name is commonly not identical to the source name.
PACKAGE_ALIASES = {
    "openssl": {"openssl"}, "pkgconf": {"pkg-config", "pkgconf"}, "gawk": {"awk", "gawk"},
    "python": {"python", "python3"}, "perl": {"perl"}, "libtool": {"libtool"},
    "glib": {"glib-compile-schemas", "glib-compile-resources", "gdbus-codegen"},
}

@dataclass
class ArchPackage:
    name: str
    version: str = ""
    arch: str = ""
    files: set[str] = field(default_factory=set)

@dataclass
class Candidate:
    name: str
    files: set[str] = field(default_factory=set)
    kinds: set[str] = field(default_factory=set)
    version_hint: str = ""
    version_verified: bool = False
    score: int = 0
    arch_name: str = ""
    source: str = "filesystem"

    def add(self, path: str, kind: str, score: int) -> None:
        self.files.add(path)
        self.kinds.add(kind)
        self.score += score

VERSION_RE = re.compile(r"(?<![A-Za-z0-9])v?(\d+(?:\.\d+){1,8}(?:[-+._~][0-9A-Za-z._~+-]+)?)")


def root_path(root: Path, absolute: str) -> Path:
    return Path(absolute) if root == Path("/") else root / absolute.lstrip("/")


def relpath(path: Path, root: Path) -> str:
    if root == Path("/"):
        return "/" + str(path).lstrip("/")
    return "/" + str(path.relative_to(root)).lstrip("/")


def normalize_root(value: str) -> Path:
    p = Path(value).resolve()
    if not p.is_dir():
        raise ValueError(f"root is not a directory: {p}")
    return p


def excluded(path: Path, root: Path) -> bool:
    try:
        rel = path.relative_to(root)
    except ValueError:
        return True
    return bool(rel.parts and rel.parts[0] in EXCLUDED)


def walk_files(root: Path) -> Iterable[Path]:
    for current, dirs, files in os.walk(root, topdown=True, followlinks=False):
        cur = Path(current)
        dirs[:] = [d for d in dirs if not excluded(cur / d, root) and not (cur / d).is_symlink()]
        for name in files:
            p = cur / name
            try:
                if p.is_file() or p.is_symlink():
                    yield p
            except OSError:
                continue


def executable(path: Path) -> bool:
    try:
        return path.is_file() and os.access(path, os.X_OK)
    except OSError:
        return False


def version_from_text(text: str) -> str:
    m = VERSION_RE.search(text or "")
    return m.group(1) if m else ""


def probe_version(path: Path) -> tuple[str, bool]:
    if not executable(path):
        return "", False
    for opt in ("--version", "-version", "-V"):
        try:
            p = subprocess.run([str(path), opt], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, timeout=2, check=False,
                               env={**os.environ, "LC_ALL": "C", "LANG": "C"})
            v = version_from_text(p.stdout)
            if v:
                return v, True
        except (OSError, subprocess.SubprocessError):
            pass
    return "", False


def parse_arch_files_file(path: Path) -> set[str]:
    result: set[str] = set()
    try:
        text = path.read_text(errors="replace")
    except OSError:
        return result
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("%FILES%"):
            continue
        if line.startswith("%BACKUP%") or line.startswith("%CSIZE%"):
            break
        if line.startswith("%"):
            continue
        # pacman files DB entries are relative to /.
        if line.startswith("/"):
            result.add(line)
        else:
            result.add("/" + line)
    return result


def read_arch_local_db(root: Path) -> dict[str, ArchPackage]:
    """Read Arch package metadata without requiring pacman to be installed."""
    dbroot = root_path(root, ARCH_LOCAL_DB)
    packages: dict[str, ArchPackage] = {}
    if not dbroot.is_dir():
        return packages
    for entry in sorted(dbroot.iterdir()):
        if not entry.is_dir():
            continue
        desc = entry / "desc"
        files = entry / "files"
        try:
            lines = desc.read_text(errors="replace").splitlines()
        except OSError:
            continue
        fields: dict[str, list[str]] = {}
        current = ""
        for line in lines:
            if line.startswith("%") and line.endswith("%"):
                current = line.strip("%")
                fields[current] = []
            elif current:
                fields[current].append(line)
        name = (fields.get("NAME") or [""])[0]
        if not name:
            continue
        pkg = ArchPackage(name=name,
                          version=(fields.get("VERSION") or [""])[0],
                          arch=(fields.get("ARCH") or [""])[0])
        pkg.files = parse_arch_files_file(files)
        packages[name] = pkg
    return packages


def read_arch_pacman_q(root: Path) -> dict[str, ArchPackage]:
    """Use pacman when available; this also works when the DB is elsewhere."""
    if root != Path("/"):
        return {}
    pacman = shutil.which("pacman")
    if not pacman:
        return {}
    result: dict[str, ArchPackage] = {}
    try:
        q = subprocess.run([pacman, "-Q"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                           text=True, timeout=10, check=False)
    except (OSError, subprocess.SubprocessError):
        return result
    if q.returncode != 0:
        return result
    for line in q.stdout.splitlines():
        parts = line.rsplit(" ", 1)
        if len(parts) != 2:
            continue
        name, version = parts
        result[name] = ArchPackage(name=name, version=version, arch=platform.machine())
    # -Ql is intentionally per package only when a package is a candidate; a
    # complete -Ql for a large Arch installation can be very expensive.
    return result


def arch_package_db(root: Path) -> dict[str, ArchPackage]:
    db = read_arch_local_db(root)
    if db:
        return db
    return read_arch_pacman_q(root)


def normalize_arch_pkg_name(name: str) -> str:
    # lib32-* is retained: it represents a distinct payload and must not be
    # merged into the native package.
    return name


def arch_matches_files(root: Path, packages: dict[str, ArchPackage], candidates: dict[str, Candidate]) -> None:
    """Match Arch package file manifests against the real filesystem.

    This is much stronger than a libfoo.so naming guess: every matched path is
    an exact package-owned path from Arch metadata. It also naturally handles
    lib32-* packages and packages that contain many unrelated files.
    """
    if not packages:
        return
    for pkg in packages.values():
        matched = 0
        evidence: list[str] = []
        for rel in pkg.files:
            p = root_path(root, rel)
            try:
                if p.exists() or p.is_symlink():
                    matched += 1
                    evidence.append(rel)
            except OSError:
                pass
        if not matched:
            continue
        # Require multiple exact files unless the manifest is tiny. This avoids
        # adopting a package from a single generic file such as /usr/share/man.
        threshold = 1 if len(pkg.files) <= 3 else 2
        if matched < threshold:
            continue
        name = normalize_arch_pkg_name(pkg.name)
        c = candidates.setdefault(name, Candidate(name, source="arch-db"))
        for rel in evidence:
            c.add(rel, "arch-package", 6)
        if pkg.version:
            c.version_hint = pkg.version
            c.version_verified = True
        c.arch_name = pkg.arch
        c.source = "arch-db"


def infer_library(name: str) -> str:
    base = name.split(".so", 1)[0]
    return base[3:] if base.startswith("lib") else base


def infer_filesystem_candidate(path: Path, root: Path) -> tuple[str, str, int] | None:
    rel = relpath(path, root)
    low = rel.lower()
    name = path.name
    # Keep kernel/module trees together.
    if low.startswith(("/lib/modules/", "/usr/lib/modules/")) or (low.startswith("/boot/") and any(x in name.lower() for x in ("vmlinuz", "bzimage", "system.map", "config"))):
        return "linux", "kernel", 8
    if low.endswith("/linux/version.h"):
        return "linux-api-headers", "header", 5
    # Common GLFS/LFS/BLFS multi-file packages.
    for pkg, binaries in KNOWN_BINARIES.items():
        if name in binaries:
            return pkg, "known-bin", 5
    if ".so" in name:
        return infer_library(name), "library", 5
    if name.endswith(".pc"):
        return name[:-3], "pkgconfig", 5
    if name.endswith((".a", ".la")):
        base = name.rsplit(".", 1)[0]
        return (base[3:] if base.startswith("lib") else base), "static-library", 2
    # Development headers are valuable GLFS/LFS evidence.
    if any(low.startswith(d + "/") for d in ("/usr/include", "/usr/local/include")):
        if path.suffix in {".h", ".hpp", ".hh", ".inc"}:
            return path.parent.name or path.stem, "header", 2
    return None


def add_known_hints(root: Path, candidates: dict[str, Candidate]) -> None:
    for package, binaries in KNOWN_BINARIES.items():
        for binary in binaries:
            for directory in BIN_DIRS:
                p = root_path(root, directory) / binary
                if executable(p):
                    c = candidates.setdefault(package, Candidate(package, source="lfs-blfs-glfs"))
                    c.add(relpath(p, root), "lfs-bin", 5)
                    if not c.version_hint:
                        v, ok = probe_version(p)
                        if v:
                            c.version_hint, c.version_verified = v, ok
                    break
    for package, libs in KNOWN_LIBS.items():
        for directory in LIB_DIRS:
            d = root_path(root, directory)
            if not d.exists():
                continue
            for lib in libs:
                for p in d.glob(f"lib{lib}.so*"):
                    if p.exists() or p.is_symlink():
                        candidates.setdefault(package, Candidate(package, source="lfs-blfs-glfs")).add(relpath(p, root), "lfs-lib", 4)
    for package, paths in KNOWN_DATA.items():
        c = candidates.setdefault(package, Candidate(package, source="lfs-blfs-glfs"))
        for absolute in paths:
            p = root_path(root, absolute)
            if p.exists() or p.is_symlink():
                c.add(relpath(p, root), "lfs-data", 3)


def filesystem_scan(root: Path, candidates: dict[str, Candidate]) -> None:
    for path in walk_files(root):
        result = infer_filesystem_candidate(path, root)
        if not result:
            continue
        name, kind, score = result
        try:
            if path.stat().st_size == 0 and kind not in {"kernel", "header", "pkgconfig"}:
                continue
        except OSError:
            continue
        c = candidates.setdefault(name, Candidate(name, source="filesystem"))
        c.add(relpath(path, root), kind, score)
        if not c.version_hint and executable(path) and ".so" not in path.name:
            v, ok = probe_version(path)
            if v:
                c.version_hint, c.version_verified = v, ok


def discover(root: Path, use_arch: bool = True, use_hints: bool = True) -> dict[str, Candidate]:
    candidates: dict[str, Candidate] = {}
    if use_arch:
        packages = arch_package_db(root)
        arch_matches_files(root, packages, candidates)
    filesystem_scan(root, candidates)
    if use_hints:
        add_known_hints(root, candidates)
    return {n: c for n, c in candidates.items() if c.files}


def safe_token(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9+_.@-]+", "-", value).strip("-") or "unknown"


def package_version(c: Candidate) -> tuple[str, bool]:
    return (c.version_hint, c.version_verified) if c.version_hint else ("0.0.0+detected", False)


def pkginfo(c: Candidate, version: str, verified: bool, pkgrel: str, arch: str) -> str:
    return "\n".join([
        f"pkgname = {c.name}", f"pkgver = {version}", f"pkgrel = {pkgrel}", f"arch = {arch}",
        "pkgtype = reconstructed", "origin = untracked-adoption",
        f"version_verified = {'true' if verified else 'false'}", f"file_count = {len(c.files)}",
        f"evidence_score = {c.score}", f"detection_source = {c.source}",
        f"types = {','.join(sorted(c.kinds))}",
    ]) + "\n"


def create_archive(c: Candidate, root: Path, output: Path, pkgrel: str, arch: str, overwrite: bool) -> tuple[Path, bool, str]:
    version, verified = package_version(c)
    dest = output / f"{safe_token(c.name)}-{safe_token(version)}-{safe_token(pkgrel)}-{safe_token(arch)}{ARCHIVE_SUFFIX}"
    if dest.exists() and not overwrite:
        return dest, False, "already exists"
    zstd = shutil.which("zstd")
    if not zstd:
        return dest, False, "zstd command not found"
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="kuzpkg-untracked-") as td:
        stage = Path(td)
        info = stage / ".PKGINFO"
        info.write_text(pkginfo(c, version, verified, pkgrel, arch), encoding="utf-8")
        tar_path = stage / "payload.tar"
        with tarfile.open(tar_path, "w") as tar:
            tar.add(info, arcname=".PKGINFO", recursive=False)
            for rel in sorted(c.files):
                src = root_path(root, rel)
                try:
                    if src.exists() or src.is_symlink():
                        tar.add(src, arcname=rel.lstrip("/"), recursive=False)
                except OSError:
                    continue
        tmp = dest.with_name(dest.name + ".tmp")
        p = subprocess.run([zstd, "-q", "-f", str(tar_path), "-o", str(tmp)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False)
        if p.returncode != 0:
            try: tmp.unlink()
            except OSError: pass
            return dest, False, p.stderr.strip() or "zstd failed"
        tmp.replace(dest)
    return dest, True, "created"


def print_table(candidates: dict[str, Candidate]) -> None:
    print(f"{'PACKAGE':32} {'VERSION':24} {'FILES':>7}  {'SOURCE':18} TYPES")
    print("-" * 110)
    for c in sorted(candidates.values(), key=lambda x: (-x.score, x.name)):
        ver = c.version_hint or "0.0.0+detected"
        if not c.version_verified: ver += "*"
        print(f"{c.name:32} {ver:24} {len(c.files):7d}  {c.source:18} {','.join(sorted(c.kinds))}")


def package_all(candidates: dict[str, Candidate], root: Path, output: Path, pkgrel: str, arch: str, overwrite: bool) -> int:
    made = skipped = failed = 0
    for name in sorted(candidates):
        path, ok, msg = create_archive(candidates[name], root, output, pkgrel, arch, overwrite)
        if ok: made += 1; print(f"+ {path}")
        elif msg == "already exists": skipped += 1; print(f"= {path} (exists)")
        else: failed += 1; print(f"! {name}: {msg}", file=sys.stderr)
    print(f"done: created={made} skipped={skipped} failed={failed}")
    return 1 if failed else 0


def tui(stdscr, rows: list[tuple[str, Candidate]], root: Path, output: Path, pkgrel: str, arch: str, overwrite: bool) -> None:
    curses.curs_set(0); stdscr.keypad(True); cursor = 0; marked: set[int] = set(); message = "SPACE mark  A all  N none  P package  ENTER details  Q quit"
    while True:
        stdscr.erase(); h, w = stdscr.getmaxyx()
        stdscr.addnstr(0, 0, " kuzpkg-untracked | Arch + LFS/BLFS/GLFS adoption ".ljust(max(1, w - 1)), max(1, w - 1), curses.A_REVERSE)
        stdscr.addnstr(1, 0, f"candidates={len(rows)} selected={len(marked)}", max(1, w - 1))
        visible = max(1, h - 5); start = max(0, min(cursor - visible // 2, max(0, len(rows) - visible)))
        for y, i in enumerate(range(start, min(len(rows), start + visible)), 3):
            c = rows[i][1]; marker = "*" if i in marked else " "; flag = "" if c.version_verified else "!"
            line = f"{marker}{flag} {c.name:<30} {(c.version_hint or '0.0.0+detected'):<20} {len(c.files):>6} {c.source:<18}"
            if i == cursor: stdscr.addnstr(y, 0, line, max(1, w - 1), curses.A_REVERSE)
            else: stdscr.addnstr(y, 0, line, max(1, w - 1))
        stdscr.addnstr(h - 2, 0, message, max(1, w - 1)); stdscr.addnstr(h - 1, 0, " F1 help ".ljust(max(1, w - 1)), max(1, w - 1), curses.A_REVERSE); stdscr.refresh()
        key = stdscr.getch()
        if key in (ord("q"), ord("Q")): return
        if key == curses.KEY_UP: cursor = max(0, cursor - 1)
        elif key == curses.KEY_DOWN: cursor = min(max(0, len(rows) - 1), cursor + 1)
        elif key == ord(" ") and rows: marked.symmetric_difference_update({cursor})
        elif key in (ord("a"), ord("A")): marked.update(range(len(rows)))
        elif key in (ord("n"), ord("N")): marked.clear()
        elif key in (ord("p"), ord("P")):
            made = 0
            for i in sorted(marked):
                _, c = rows[i]; _, ok, msg = create_archive(c, root, output, pkgrel, arch, overwrite)
                if ok: made += 1
            message = f"created {made} archive(s) in {output}"
        elif key == curses.KEY_F1:
            stdscr.erase(); help_text = ["kuzpkg-untracked", "", "Arch package manifests are exact-path evidence when pacman metadata exists.", "LFS/BLFS/GLFS hints add only files that are actually present.", "lib32-* is kept separate from native packages.", "! means the version was inferred rather than verified.", "", "Press any key to return."]
            for i, line in enumerate(help_text[:h - 1]): stdscr.addnstr(i, 0, line, max(1, w - 1))
            stdscr.refresh(); stdscr.getch()


def run_tui(candidates: dict[str, Candidate], root: Path, output: Path, pkgrel: str, arch: str, overwrite: bool) -> int:
    rows = sorted(candidates.items(), key=lambda x: (-x[1].score, x[0])); curses.wrapper(tui, rows, root, output, pkgrel, arch, overwrite); return 0


def main() -> int:
    p = argparse.ArgumentParser(description="Reconstruct untracked LFS/BLFS/GLFS/Arch-compatible packages")
    p.add_argument("--root", default="/", help="filesystem root/chroot to scan")
    p.add_argument("--output-dir", default="./kuzpkg-reconstructed")
    p.add_argument("--pkgrel", default="1")
    p.add_argument("--arch", default=None)
    p.add_argument("--overwrite", action="store_true")
    p.add_argument("--no-tui", action="store_true")
    p.add_argument("--package-all", action="store_true")
    p.add_argument("--no-arch-db", action="store_true", help="disable Arch pacman local-database matching")
    p.add_argument("--no-lfs-hints", action="store_true", help="disable LFS/BLFS/GLFS package hints")
    args = p.parse_args()
    try: root = normalize_root(args.root)
    except ValueError as e: print(f"error: {e}", file=sys.stderr); return 2
    arch = args.arch or platform.machine() or "unknown"; output = Path(args.output_dir).resolve()
    print("kuzpkg-untracked: scanning filesystem and package metadata...")
    candidates = discover(root, use_arch=not args.no_arch_db, use_hints=not args.no_lfs_hints)
    print(f"kuzpkg-untracked: {len(candidates)} candidate packages")
    if not candidates: return 0
    if args.package_all: return package_all(candidates, root, output, args.pkgrel, arch, args.overwrite)
    if args.no_tui: print_table(candidates); return 0
    return run_tui(candidates, root, output, args.pkgrel, arch, args.overwrite)

if __name__ == "__main__":
    raise SystemExit(main())
