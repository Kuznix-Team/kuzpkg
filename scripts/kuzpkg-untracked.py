#!/usr/bin/env python3
# Generic filesystem adoption/discovery helper for kuzpkg.
# Archives use: name-pkgver-pkgrel-arch.kuzpkg.tar.zst

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
    ("/usr/lib", ""), ("/usr/lib64", ""), ("/lib", ""), ("/lib64", ""),
    ("/lib32", "lib32-"), ("/usr/lib32", "lib32-"),
    ("/libx32", "libx32-"), ("/usr/libx32", "libx32-"),
)
BIN_DIRS = ("/usr/bin", "/usr/sbin", "/bin", "/sbin")
PC_DIRS = (
    ("/usr/lib/pkgconfig", ""), ("/usr/lib64/pkgconfig", ""), ("/usr/share/pkgconfig", ""),
    ("/usr/lib32/pkgconfig", "lib32-"), ("/lib32/pkgconfig", "lib32-"),
    ("/usr/libx32/pkgconfig", "libx32-"), ("/libx32/pkgconfig", "libx32-"),
)
FIREFOX_DIRS = ("/lib/firefox", "/usr/lib/firefox")
VERSION_RE = re.compile(r"(?:version|release|v)?\s*([0-9]+(?:\.[0-9A-Za-z]+)+(?:[-+._][0-9A-Za-z.-]+)?)", re.I)


def run_kuzpkg(root, args):
    cmd = ["kuzpkg"] + ([] if root == "/" else ["--root", root]) + args
    try:
        p = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=False)
    except OSError as exc:
        raise RuntimeError(f"cannot execute kuzpkg: {exc}") from exc
    return [x.strip() for x in p.stdout.splitlines() if x.strip()]


def norm(path):
    return str(path).replace(os.sep, "/").lstrip("/").rstrip("/")


def root_path(root, path):
    return Path(root) / path.lstrip("/") if root != "/" else Path("/") / path.lstrip("/")


def rel(path, root):
    return norm(path.relative_to(root))


def load_local_db(root):
    packages = set(run_kuzpkg(root, ["-Qq"]))
    owned = set()
    for pkg in sorted(packages):
        for item in run_kuzpkg(root, ["-Qql", pkg]):
            if item.startswith(pkg + " "):
                item = item.split(None, 1)[1]
            owned.add(norm(item))
    return packages, owned


def scan_files(root, dirs, predicate):
    result = []
    for directory, prefix in dirs:
        base = root_path(str(root), directory)
        if not base.exists():
            continue
        for current, dirnames, filenames in os.walk(base, followlinks=False):
            dirnames[:] = [d for d in dirnames if not (Path(current) / d).is_symlink()]
            for filename in filenames:
                p = Path(current) / filename
                if (p.is_file() or p.is_symlink()) and predicate(p):
                    result.append((p, prefix))
    return result


def scan_include_dirs(root):
    base = root_path(str(root), "/usr/include")
    if not base.exists():
        return []
    return [p for p in base.iterdir() if p.is_dir() and not p.is_symlink()]


def scan_firefox(root):
    result = []
    for directory in FIREFOX_DIRS:
        base = root_path(str(root), directory)
        if not base.is_dir():
            continue
        for current, dirnames, filenames in os.walk(base, followlinks=False):
            dirnames[:] = [d for d in dirnames if not (Path(current) / d).is_symlink()]
            result.extend(Path(current) / f for f in filenames)
    return result


def package_guess(path):
    name = path.name
    if name.endswith(".pc"):
        return name[:-3]
    so = re.sub(r"\.so(?:\.[0-9A-Za-z._-]+)*$", "", name)
    if so != name:
        return so[3:] if so.startswith("lib") and len(so) > 3 else so
    if "." in name and name.startswith("lib"):
        return name.split(".", 1)[0][3:]
    return name


def version_from_text(text):
    for line in text.splitlines()[:40]:
        m = VERSION_RE.search(line)
        if m:
            return m.group(1)
    return None


def probe_version(path):
    if not path.is_file() or not os.access(path, os.X_OK):
        return None
    for option in (("--version",), ("-version",)):
        try:
            p = subprocess.run([str(path), *option], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, timeout=3, check=False)
            v = version_from_text(p.stdout)
            if v:
                return v
        except (OSError, subprocess.SubprocessError):
            pass
    return None


def detect_version(name, evidence, root):
    base = name.removeprefix("lib32-").removeprefix("libx32-")
    candidates = [root_path(str(root), d) / base for d in BIN_DIRS + FIREFOX_DIRS]
    candidates += evidence
    seen = set()
    for p in candidates:
        if str(p) in seen:
            continue
        seen.add(str(p))
        v = probe_version(p)
        if v:
            return v, "version-probe"
    if name == "firefox":
        for metadata in ("/usr/lib/firefox/platform.ini", "/lib/firefox/platform.ini"):
            p = root_path(str(root), metadata)
            try:
                m = re.search(r"^Version=([^\n]+)", p.read_text(errors="replace"), re.M)
                if m:
                    return m.group(1).strip(), "firefox-platform.ini"
            except OSError:
                pass
    for p in evidence:
        for parent in (p.parent, *p.parents):
            for metadata in ("VERSION", "version", "VERSION.txt", "PKG-INFO"):
                m = parent / metadata
                try:
                    if m.is_file():
                        v = version_from_text(m.read_text(errors="replace"))
                        if v:
                            return v, "metadata"
                except OSError:
                    pass
            if parent == root:
                break
    return "0.0.0+detected", "unverified"


def detect_arch(explicit=None):
    if explicit:
        return explicit, "explicit"
    try:
        arch = os.uname().machine
    except AttributeError:
        arch = platform.machine()
    if arch:
        return arch, "uname"
    return "unknown", "unknown"


def discover(root, minimum):
    root = Path(root).resolve()
    packages, owned = load_local_db(str(root))
    candidates = defaultdict(lambda: {"count": 0, "types": set(), "evidence": []})
    artifacts = []
    artifacts += [(p, "library", pre) for p, pre in scan_files(root, LIB_DIRS, lambda p: ".so" in p.name)]
    artifacts += [(p, "binary", "") for p in scan_files(root, tuple((d, "") for d in BIN_DIRS),
                         lambda p: os.access(p, os.X_OK) and "." not in p.name)]
    artifacts += [(p, "pkgconfig", pre) for p, pre in scan_files(root, PC_DIRS, lambda p: p.name.endswith(".pc"))]
    artifacts += [(p, "include", "") for p in scan_include_dirs(root)]
    artifacts += [(p, "firefox", "") for p in scan_firefox(root)]

    seen = set()
    for path, kind, prefix in artifacts:
        key = rel(path, root)
        if key in seen or key in owned:
            continue
        seen.add(key)
        guess = "firefox" if kind == "firefox" else package_guess(path)
        if not guess:
            continue
        name = prefix + guess
        if name in packages:
            continue
        item = candidates[name]
        item["count"] += 1
        item["types"].add(kind)
        if len(item["evidence"]) < 25:
            item["evidence"].append(path)

    return sorted(((n, x["count"], x["types"], x["evidence"]) for n, x in candidates.items()
                   if x["count"] >= minimum), key=lambda x: (-x[1], x[0]))


def safe(s):
    return re.sub(r"[^A-Za-z0-9._+@-]+", "_", s)


def make_archive(name, count, types, evidence, root, outdir, pkgrel, arch, overwrite):
    version, source = detect_version(name, evidence, root)
    filename = f"{safe(name)}-{safe(version)}-{safe(pkgrel)}-{safe(arch)}.kuzpkg.tar.zst"
    output = outdir / filename
    if output.exists() and not overwrite:
        return output, version, source, False
    zstd = shutil.which("zstd")
    if not zstd:
        raise RuntimeError("zstd is required to create .kuzpkg.tar.zst archives")
    outdir.mkdir(parents=True, exist_ok=True)
    metadata = {
        "format": "kuzpkg-detected-v2", "name": name, "version": version,
        "version_verified": source != "unverified", "version_source": source,
        "pkgrel": pkgrel, "arch": arch, "artifact_types": sorted(types),
        "artifact_count": count, "origin": "filesystem-discovery"
    }
    with tempfile.TemporaryDirectory(prefix="kuzpkg-untracked-") as tmp:
        tarpath = Path(tmp) / "package.kuzpkg.tar"
        meta = Path(tmp) / ".KUZPKG-METADATA.json"
        meta.write_text(json.dumps(metadata, indent=2) + "\n")
        with tarfile.open(tarpath, "w") as tar:
            tar.add(meta, arcname=".KUZPKG-METADATA.json")
            added = set()
            for path in evidence:
                item = rel(path, root)
                if item not in added:
                    tar.add(path, arcname=item, recursive=False)
                    added.add(item)
        subprocess.run([zstd, "-q", "-f", "-o", str(output), str(tarpath)], check=True)
    return output, version, source, True


def main():
    parser = argparse.ArgumentParser(description="Discover software missing from kuzpkg LocalDB")
    parser.add_argument("--root", default="/", help="alternate installation root/chroot")
    parser.add_argument("--minimum", type=int, default=1)
    parser.add_argument("-q", "--quiet", action="store_true")
    parser.add_argument("--package", action="store_true", help="create .kuzpkg.tar.zst archives")
    parser.add_argument("--output-dir", default=".")
    parser.add_argument("--pkgrel", default="1", help="package release number")
    parser.add_argument("--arch", help="override architecture; default is uname -m")
    parser.add_argument("--overwrite", action="store_true")
    args = parser.parse_args()
    if args.minimum < 1:
        parser.error("--minimum must be at least 1")
    try:
        candidates = discover(args.root, args.minimum)
        root = Path(args.root).resolve()
        arch, arch_source = detect_arch(args.arch)
    except RuntimeError as exc:
        print(f"kuzpkg-untracked: error: {exc}", file=sys.stderr)
        return 1
    if not candidates:
        print("kuzpkg-untracked: no unregistered package candidates found")
        return 0
    print(f"kuzpkg-untracked: found {len(candidates)} candidate package(s) missing from LocalDB")
    for name, count, types, evidence in candidates:
        version, source = detect_version(name, evidence, root)
        if args.quiet:
            print(name)
        else:
            print(f"  + {name:<32} {version:<24} {count:>3} artifact(s) [{source}; arch={arch} ({arch_source})]")
            for p in evidence[:5]:
                print(f"      /{rel(p, root)}")
            if len(evidence) > 5:
                print(f"      ... and {len(evidence)-5} more")
        if args.package:
            try:
                output, _, _, created = make_archive(name, count, types, evidence, root,
                                                      Path(args.output_dir).resolve(), args.pkgrel,
                                                      arch, args.overwrite)
                print(f"      package: {output} ({'created' if created else 'exists; skipped'})")
            except (OSError, subprocess.SubprocessError, RuntimeError) as exc:
                print(f"      package: FAILED: {exc}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
