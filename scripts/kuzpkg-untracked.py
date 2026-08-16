#!/usr/bin/env python3
# Generic filesystem adoption/discovery helper for kuzpkg.
# Archives use: name-pkgver-pkgrel-arch.kuzpkg.tar.zst.
#
# The scanner is package-manager/language agnostic. It can discover native
# files, multilib files, Firefox, and language modules installed by Python,
# Rust/Cargo, Ruby, Perl, Node.js, Go, Java/JVM, PHP, Lua, Tcl, and other
# ecosystems. Unknown language ecosystems are handled generically by module
# metadata, module file extensions, and conventional module directories.

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

# Filesystems that must never be recursively scanned. Temporary files,
# removable mounts, and service data are not package installation locations
# for this discovery mode.
EXCLUDED_TOPLEVEL = {"home", "root", "proc", "run", "sys", "dev", "tmp", "mnt", "srv"}

MODULE_EXTENSIONS = {
    ".py", ".pyc", ".pyo", ".so", ".pyd",
    ".rs", ".rlib", ".rmeta", ".crate",
    ".rb", ".rake", ".gem",
    ".pm", ".pod",
    ".js", ".mjs", ".cjs", ".node",
    ".go", ".a",
    ".jar", ".class", ".war", ".ear",
    ".php", ".phar",
    ".lua", ".luac",
    ".tcl", ".tm",
    ".wasm",
    ".dll", ".dylib", ".so",
}

MODULE_METADATA = {
    "pyproject.toml": "python", "setup.py": "python", "setup.cfg": "python",
    "PKG-INFO": "python", "METADATA": "python", "Cargo.toml": "rust-cargo",
    "Cargo.lock": "rust-cargo", "gemfile": "ruby", "gemspec": "ruby",
    ".bundle": "ruby", "package.json": "nodejs", "package-lock.json": "nodejs",
    "yarn.lock": "nodejs", "pnpm-lock.yaml": "nodejs", "go.mod": "go",
    "go.sum": "go", "pom.xml": "java", "build.gradle": "java",
    "build.gradle.kts": "java", "composer.json": "php", "DESCRIPTION": "r",
    "NAMESPACE": "r",
}

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


def excluded_dir(path, root):
    try:
        relative = path.relative_to(root)
    except ValueError:
        return True
    return bool(relative.parts and relative.parts[0] in EXCLUDED_TOPLEVEL)


def scan_tree(root, predicate=lambda p: True):
    """Scan every directory below root except explicit non-package trees."""
    result = []
    for current, dirnames, filenames in os.walk(root, topdown=True, followlinks=False):
        current_path = Path(current)
        if excluded_dir(current_path, root):
            dirnames[:] = []
            continue
        dirnames[:] = [d for d in dirnames if d not in EXCLUDED_TOPLEVEL and not (current_path / d).is_symlink()]
        for filename in filenames:
            p = current_path / filename
            if (p.is_file() or p.is_symlink()) and predicate(p):
                result.append(p)
    return result


def scan_files(root, dirs, predicate):
    result = []
    for directory, prefix in dirs:
        base = root_path(str(root), directory)
        if not base.exists() or excluded_dir(base, root):
            continue
        for current, dirnames, filenames in os.walk(base, followlinks=False):
            current_path = Path(current)
            dirnames[:] = [d for d in dirnames if not (current_path / d).is_symlink()]
            for filename in filenames:
                p = current_path / filename
                if (p.is_file() or p.is_symlink()) and predicate(p):
                    result.append((p, prefix))
    return result


def scan_include_dirs(root):
    base = root_path(str(root), "/usr/include")
    if not base.exists() or excluded_dir(base, root):
        return []
    return [p for p in base.iterdir() if p.is_dir() and not p.is_symlink()]


def scan_firefox(root):
    result = []
    for directory in FIREFOX_DIRS:
        base = root_path(str(root), directory)
        if not base.is_dir() or excluded_dir(base, root):
            continue
        for current, dirnames, filenames in os.walk(base, followlinks=False):
            current_path = Path(current)
            dirnames[:] = [d for d in dirnames if not (current_path / d).is_symlink()]
            result.extend(current_path / f for f in filenames)
    return result


def package_guess(path):
    name = path.name
    lower = name.lower()
    if name.endswith(".pc"):
        return name[:-3]
    for parent in (path.parent, *path.parents):
        p = parent.name
        if p.lower() in {"site-packages", "dist-packages", "vendor", "vendor_ruby", "gems", "node_modules", "vendor_modules", "lib", "modules"}:
            break
        if p and p not in {"python", "python3", "ruby", "perl", "node", "nodejs"}:
            if lower.endswith((".py", ".rb", ".pm", ".lua", ".tcl", ".php")):
                return p
    so = re.sub(r"\.so(?:\.[0-9A-Za-z._-]+)*$", "", name)
    if so != name:
        return so[3:] if so.startswith("lib") and len(so) > 3 else so
    if "." in name and name.startswith("lib"):
        return name.split(".", 1)[0][3:]
    for suffix in (".rlib", ".rmeta", ".crate", ".gem", ".node", ".jar", ".wasm"):
        if name.endswith(suffix):
            return name[:-len(suffix)]
    return name


def module_kind(path):
    lower_parts = {p.lower() for p in path.parts}
    suffix = path.suffix.lower()
    name = path.name
    if name in MODULE_METADATA:
        return MODULE_METADATA[name]
    if "site-packages" in lower_parts or "dist-packages" in lower_parts or suffix in {".py", ".pyc", ".pyo", ".pyd"}:
        return "python"
    if "cargo" in lower_parts or "rustlib" in lower_parts or suffix in {".rlib", ".rmeta", ".crate"}:
        return "rust-cargo"
    if "gems" in lower_parts or "vendor_ruby" in lower_parts or suffix in {".rb", ".gem"}:
        return "ruby"
    if "node_modules" in lower_parts or suffix in {".js", ".mjs", ".cjs", ".node"}:
        return "nodejs"
    if suffix in {".pm", ".pod"} or "perl" in lower_parts:
        return "perl"
    if suffix == ".go" or "gopath" in lower_parts:
        return "go"
    if suffix in {".jar", ".class", ".war", ".ear"} or "java" in lower_parts:
        return "java-jvm"
    if suffix in {".php", ".phar"} or "composer" in lower_parts:
        return "php"
    if suffix in {".lua", ".luac"} or "lua" in lower_parts:
        return "lua"
    if suffix in {".tcl", ".tm"} or "tcl" in lower_parts:
        return "tcl"
    if suffix == ".wasm":
        return "wasm"
    if suffix in MODULE_EXTENSIONS:
        return "generic-module"
    return None


def scan_modules(root):
    return [(p, module_kind(p)) for p in scan_tree(root, lambda p: module_kind(p) is not None)]


def version_from_text(text):
    for line in text.splitlines()[:80]:
        m = VERSION_RE.search(line)
        if m:
            return m.group(1)
    return None


def probe_version(path):
    if not path.is_file() or not os.access(path, os.X_OK):
        return None
    for option in (("--version",), ("-version",)):
        try:
            p = subprocess.run([str(path), *option], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=3, check=False)
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
        p = Path(p)
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
        p = Path(p)
        for parent in (p.parent, *p.parents):
            for metadata in ("VERSION", "version", "VERSION.txt", "PKG-INFO", "METADATA", "pyproject.toml", "Cargo.toml", "gemspec", "package.json"):
                m = parent / metadata
                try:
                    if m.is_file():
                        text = m.read_text(errors="replace")
                        if metadata == "package.json":
                            try:
                                value = json.loads(text).get("version")
                                if value:
                                    return str(value), "package.json"
                            except json.JSONDecodeError:
                                pass
                        v = version_from_text(text)
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
    return (arch, "uname") if arch else ("unknown", "unknown")


def discover(root, minimum):
    root = Path(root).resolve()
    packages, owned = load_local_db(str(root))
    candidates = defaultdict(lambda: {"count": 0, "types": set(), "evidence": []})
    artifacts = []
    artifacts += [(p, "library", pre) for p, pre in scan_files(root, LIB_DIRS, lambda p: ".so" in p.name)]
    artifacts += [(p, "binary", "") for p in scan_files(root, tuple((d, "") for d in BIN_DIRS), lambda p: os.access(p, os.X_OK) and "." not in p.name)]
    artifacts += [(p, "pkgconfig", pre) for p, pre in scan_files(root, PC_DIRS, lambda p: p.name.endswith(".pc"))]
    artifacts += [(p, "include", "") for p in scan_include_dirs(root)]
    artifacts += [(p, "firefox", "") for p in scan_firefox(root)]
    artifacts += [(p, kind, "") for p, kind in scan_modules(root)]
    generic_predicate = lambda p: os.access(p, os.X_OK) or p.suffix.lower() in {".dll", ".dylib", ".so", ".a", ".jar", ".wasm"}
    artifacts += [(p, "generic", "") for p in scan_tree(root, generic_predicate)]

    seen = set()
    for path, kind, prefix in artifacts:
        try:
            key = rel(path, root)
        except ValueError:
            continue
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
    return sorted(((n, x["count"], x["types"], x["evidence"]) for n, x in candidates.items() if x["count"] >= minimum), key=lambda x: (-x[1], x[0]))


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
    metadata = {"format": "kuzpkg-detected-v3", "name": name, "version": version, "version_verified": source != "unverified", "version_source": source, "pkgrel": pkgrel, "arch": arch, "artifact_types": sorted(types), "artifact_count": count, "origin": "filesystem-discovery"}
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
                output, _, _, created = make_archive(name, count, types, evidence, root, Path(args.output_dir).resolve(), args.pkgrel, arch, args.overwrite)
                print(f"      package: {output} ({'created' if created else 'exists; skipped'})")
            except (OSError, subprocess.SubprocessError, RuntimeError) as exc:
                print(f"      package: FAILED: {exc}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
