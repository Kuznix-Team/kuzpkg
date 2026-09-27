#!/bin/bash
#
# kuzpkg-lfs.sh - Build a Kuzpkg package from an LFS/BLFS source tree.
#
# Usage:
#   kuzpkg-lfs.sh <source-dir> <dest-dir> [name] [version]
#
# This helper builds common LFS/BLFS source trees into DESTDIR, creates a
# pacman-compatible Kuzpkg package with makepkg, and prints the package path.
#
# Environment:
#   KUZPKG                   kuzpkg executable (default: kuzpkg)
#   MAKEPKG                  makepkg executable (default: makepkg)
#   PKGREL                   package release (default: 1)
#   PKGARCH                  package architecture (auto-detected)
#   PKGOUT                   package output directory (default: /var/lib/packages)
#   BUILDER                  unprivileged build user (default: builder)
#   PREFIX                   install prefix (default: /usr)
#   JOBS                     parallel jobs (auto-detected)
#   KUZPKG_LFS_INSTALL_CMD   explicit install command
#   KUZPKG_LFS_SKIP_BUILD    set to 1 to skip the build step
#   KUZPKG_LFS_SKIP_INSTALL  set to 1 if DESTDIR is already populated
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -eo pipefail

die() {
    printf 'kuzpkg-lfs: error: %s\n' "$*" >&2
    exit 1
}

msg() {
    printf 'kuzpkg-lfs: %s\n' "$*" >&2
}

if [ "$#" -lt 2 ] || [ "$#" -gt 4 ]; then
    printf 'Usage: %s <source-dir> <dest-dir> [name] [version]\n' "$0" >&2
    exit 2
fi

SOURCE_DIR=$(readlink -f "$1") || die "cannot resolve source directory"
DEST_DIR=$(readlink -f "$2") || die "cannot resolve destination directory"
PACKAGE_NAME=$3
VERSION=$4

[ -d "$SOURCE_DIR" ] || die "source directory does not exist: $SOURCE_DIR"
[ "$DEST_DIR" != "/" ] || die "refusing to use / as DESTDIR"

# Detect package metadata from common build-system files. Explicit arguments
# always take precedence.
detect_metadata() {
    local f value

    if [ -z "$PACKAGE_NAME" ] && [ -f Cargo.toml ]; then
        value=$(awk '
            /^\[package\]/{in_package=1; next}
            /^\[/{in_package=0}
            in_package && /^[[:space:]]*name[[:space:]]*=/ {
                gsub(/[[:space:]]/, "", $0); sub(/^name="/, "", $0); sub(/"$/, "", $0); print; exit
            }' Cargo.toml)
        [ -n "$value" ] && PACKAGE_NAME=$value
    fi
    if [ -z "$VERSION" ] && [ -f Cargo.toml ]; then
        value=$(awk '
            /^\[package\]/{in_package=1; next}
            /^\[/{in_package=0}
            in_package && /^[[:space:]]*version[[:space:]]*=/ {
                gsub(/[[:space:]]/, "", $0); sub(/^version="/, "", $0); sub(/"$/, "", $0); print; exit
            }' Cargo.toml)
        [ -n "$value" ] && VERSION=$value
    fi

    if [ -f pyproject.toml ]; then
        if [ -z "$PACKAGE_NAME" ]; then
            value=$(sed -n '/^\[project\]/,/^\[/{s/^[[:space:]]*name[[:space:]]*=[[:space:]]*["'\''"]\([^"'\''"]*\)["'\''"].*/\1/p;}' pyproject.toml | head -n1)
            [ -n "$value" ] && PACKAGE_NAME=$value
        fi
        if [ -z "$VERSION" ]; then
            value=$(sed -n '/^\[project\]/,/^\[/{s/^[[:space:]]*version[[:space:]]*=[[:space:]]*["'\''"]\([^"'\''"]*\)["'\''"].*/\1/p;}' pyproject.toml | head -n1)
            [ -n "$value" ] && VERSION=$value
        fi
    fi

    if [ -z "$PACKAGE_NAME" ] && [ -f configure.ac ]; then
        value=$(sed -n 's/^[[:space:]]*AC_INIT[[:space:]]*(\[\([^]]*\)\].*/\1/p' configure.ac | head -n1)
        [ -n "$value" ] && PACKAGE_NAME=$value
    fi
    if [ -z "$VERSION" ] && [ -f configure.ac ]; then
        value=$(sed -n 's/^[[:space:]]*AC_INIT[[:space:]]*(\[[^]]*\][[:space:]]*,[[:space:]]*\[\([^]]*\)\].*/\1/p' configure.ac | head -n1)
        [ -n "$value" ] && VERSION=$value
    fi

    if [ -z "$PACKAGE_NAME" ] && [ -f meson.build ]; then
        value=$(sed -n "s/^[[:space:]]*project[[:space:]]*(\(['\"]\)\([^'\"]*\)\1.*/\2/p" meson.build | head -n1)
        [ -n "$value" ] && PACKAGE_NAME=$value
    fi
    if [ -z "$VERSION" ] && [ -f meson.build ]; then
        value=$(sed -n "s/^[[:space:]]*project[[:space:]]*(.*version:[[:space:]]*['\"]\([^'\"]*\)['\"].*/\1/p" meson.build | head -n1)
        [ -n "$value" ] && VERSION=$value
    fi

    if [ -z "$PACKAGE_NAME" ] && [ -f CMakeLists.txt ]; then
        value=$(sed -n 's/^[[:space:]]*project[[:space:]]*(\([A-Za-z0-9_.+-][A-Za-z0-9_.+-]*\).*/\1/p' CMakeLists.txt | head -n1)
        [ -n "$value" ] && PACKAGE_NAME=$value
    fi
    if [ -z "$VERSION" ] && [ -f CMakeLists.txt ]; then
        value=$(sed -n 's/^[[:space:]]*project[[:space:]]*(.*VERSION[[:space:]]*\([0-9][0-9A-Za-z._+-]*\).*/\1/p' CMakeLists.txt | head -n1)
        [ -n "$value" ] && VERSION=$value
    fi

    # KDE projects commonly keep PROJECT_VERSION in a separate set() call.
    if [ -z "$VERSION" ] && [ -f CMakeLists.txt ]; then
        value=$(sed -n 's/^[[:space:]]*set[[:space:]]*(PROJECT_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' CMakeLists.txt | head -n1)
        [ -n "$value" ] && VERSION=$value
    fi

    if [ -z "$PACKAGE_NAME" ] && compgen -G '*.gemspec' >/dev/null; then
        f=$(printf '%s\n' *.gemspec | head -n1)
        value=$(sed -n 's/.*\.name[[:space:]]*=[[:space:]]*["'\''"]\([^"'\''"]*\)["'\''"].*/\1/p' "$f" | head -n1)
        [ -n "$value" ] && PACKAGE_NAME=$value
    fi
    if [ -z "$VERSION" ] && compgen -G '*.gemspec' >/dev/null; then
        f=$(printf '%s\n' *.gemspec | head -n1)
        value=$(sed -n 's/.*\.version[[:space:]]*=[[:space:]]*["'\''"]\([^"'\''"]*\)["'\''"].*/\1/p' "$f" | head -n1)
        [ -n "$value" ] && VERSION=$value
    fi

    if [ -z "$PACKAGE_NAME" ]; then
        PACKAGE_NAME=$(basename "$SOURCE_DIR")
        PACKAGE_NAME=$(printf '%s\n' "$PACKAGE_NAME" | sed -E 's/-[0-9][0-9A-Za-z._+~-]*$//')
    fi
    if [ -z "$VERSION" ]; then
        VERSION=$(basename "$SOURCE_DIR" | sed -E 's/^.*-([0-9][0-9A-Za-z._+~-]*)$/\1/')
    fi
}

cd "$SOURCE_DIR"
detect_metadata
[ -n "$PACKAGE_NAME" ] || die "package name could not be determined; pass it explicitly"
[ -n "$VERSION" ] || die "version could not be determined; pass it explicitly"

WORK_DIR=$(mktemp -d "/tmp/kuzpkg-lfs.XXXXXX")
STAGE_DIR=$WORK_DIR/stage
PKGROOT=$WORK_DIR/package
mkdir -p "$STAGE_DIR" "$PKGROOT" "$PKGOUT"
trap 'rm -rf "$WORK_DIR"' EXIT

run_builder() {
    if [ "$(id -u)" -eq 0 ] && id "$BUILDER" >/dev/null 2>&1; then
        su "$BUILDER" -s /bin/bash -c "$*"
    else
        bash -c "$*"
    fi
}

cd "$SOURCE_DIR"

# Build-system detection.
if [ "$KUZPKG_LFS_SKIP_BUILD" != 1 ]; then
    if [ -f x.py ] && [ -f src/bootstrap/Cargo.toml ]; then
        msg "detected Rust compiler source"
        run_builder './x.py build'
    elif [ -f meson.build ]; then
        msg "detected Meson project"
        [ -d build ] || run_builder "meson setup build --prefix=$PREFIX"
        run_builder "meson compile -C build"
    elif [ -f CMakeLists.txt ]; then
        msg "detected CMake project"
        [ -d build ] || run_builder "cmake -S . -B build -G Ninja -DCMAKE_INSTALL_PREFIX=$PREFIX"
        if [ -f build/build.ninja ]; then
            run_builder "ninja -C build -j$JOBS"
        else
            run_builder "cmake --build build --parallel $JOBS"
        fi
    elif [ -f Cargo.toml ]; then
        msg "detected Cargo project"
        run_builder 'cargo build --release'
    elif [ -f pyproject.toml ] || [ -f setup.py ] || [ -f setup.cfg ]; then
        msg "detected Python project"
        run_builder 'python3 -m build --wheel --no-isolation'
    elif compgen -G '*.gemspec' >/dev/null; then
        msg "detected Ruby gem project"
        run_builder 'gem build *.gemspec'
    elif [ -f configure ]; then
        msg "detected Autotools project"
        [ -f Makefile ] || run_builder "./configure --prefix=$PREFIX"
        run_builder "make -j$JOBS"
    elif [ -f Makefile ]; then
        msg "detected Makefile project"
        run_builder "make -j$JOBS"
    else
        die "could not detect a supported build system"
    fi
fi

# Install into a staging tree rather than the live LFS filesystem.
if [ "$KUZPKG_LFS_SKIP_INSTALL" != 1 ]; then
    if [ -n "$INSTALL_CMD" ]; then
        msg "using explicit install command"
        run_builder "DESTDIR=$STAGE_DIR PREFIX=$PREFIX $INSTALL_CMD"
    elif [ -f x.py ] && [ -f src/bootstrap/Cargo.toml ]; then
        msg "installing with x.py"
        run_builder "DESTDIR=$STAGE_DIR ./x.py install"
    elif [ -f meson.build ] && [ -f build/meson-private/coredata.dat ]; then
        msg "installing with Meson"
        run_builder "DESTDIR=$STAGE_DIR meson install -C build --no-rebuild"
    elif [ -f CMakeLists.txt ] && [ -d build ]; then
        msg "installing with CMake"
        run_builder "DESTDIR=$STAGE_DIR cmake --install build"
    elif [ -f build/build.ninja ]; then
        msg "installing with Ninja"
        run_builder "DESTDIR=$STAGE_DIR ninja -C build install"
    elif [ -f build.ninja ]; then
        msg "installing with Ninja"
        run_builder "DESTDIR=$STAGE_DIR ninja install"
    elif [ -f Makefile ]; then
        msg "installing with make"
        run_builder "DESTDIR=$STAGE_DIR make install"
    elif compgen -G '*.whl' >/dev/null; then
        WHEEL=$(printf '%s\n' *.whl | head -n1)
        msg "installing Python wheel: $WHEEL"
        run_builder "python3 -m pip install --no-deps --root=$STAGE_DIR --prefix=$PREFIX $WHEEL"
    elif compgen -G '*.gem' >/dev/null; then
        GEM=$(printf '%s\n' *.gem | head -n1)
        msg "installing Ruby gem: $GEM"
        run_builder "gem install --local --ignore-dependencies --install-dir=$STAGE_DIR$PREFIX/lib/ruby/gems $GEM"
    elif [ -f Cargo.toml ]; then
        msg "installing Rust package with cargo"
        run_builder "cargo install --path . --root=$STAGE_DIR$PREFIX --locked"
    else
        die "could not detect an install method; set KUZPKG_LFS_INSTALL_CMD"
    fi
fi

# LFS already owns the shared Info directory index.
rm -f "$STAGE_DIR/usr/share/info/dir"

rm -rf "$DEST_DIR"
mkdir -p "$DEST_DIR"
cp -a "$STAGE_DIR"/. "$DEST_DIR"/

# Create a temporary PKGBUILD around the staged tree. This lets Kuzpkg use the
# same archive format and package metadata path as pacman/makepkg.
mkdir -p "$PKGROOT/payload"
cp -a "$DEST_DIR"/. "$PKGROOT/payload/"

cat > "$PKGROOT/PKGBUILD" <<EOF
pkgname='$PACKAGE_NAME'
pkgver='$VERSION'
pkgrel='$PKGREL'
pkgdesc='$PACKAGE_NAME built by kuzpkg-lfs'
arch=('$PKGARCH')

package() {
    cp -a "\$srcdir/payload/." "\$pkgdir/"
}
EOF

cd "$PKGROOT"
msg "creating $PACKAGE_NAME-$VERSION-$PKGREL-$PKGARCH"
"$MAKEPKG" -f --noconfirm

GENERATED=$(find "$PKGROOT" -maxdepth 1 -type f \
    -name "$PACKAGE_NAME-$VERSION-$PKGREL-$PKGARCH.pkg.tar.*" \
    -print -quit)

[ -n "$GENERATED" ] || die "makepkg did not create the expected package"

PKGFILE=$PKGOUT/$(basename "$GENERATED")
mv -f "$GENERATED" "$PKGFILE"

msg "package created: $PKGFILE"
msg "install with: $KUZPKG -U '$PKGFILE'"
printf '%s\n' "$PKGFILE"
