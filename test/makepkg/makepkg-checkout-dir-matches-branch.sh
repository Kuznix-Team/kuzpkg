#!/usr/bin/bash

set -e

tempdir=$(mktemp -d)
cleanup() { rm -rf "$tempdir"; }
trap cleanup EXIT

export MAKEPKG_LIBRARY="${PMTEST_LIBMAKEPKG_DIR}"
export MAKEPKG_CONF="${PMTEST_UTIL_DIR}/makepkg.conf"
export PACMAN="${PMTEST_UTIL_DIR}/pacman"
MAKEPKG="${PMTEST_SCRIPT_DIR}/makepkg"

# https://gitlab.archlinux.org/pacman/pacman/-/issues/143
# Always check out the default branch, and not a branch matching
# the directory name.
test() {
	# Dummy git repo
	export GIT_COMMITTER_NAME="Test User"
	export GIT_COMMITTER_EMAIL="test@example.com"
	export GIT_AUTHOR_NAME="$GIT_COMMITTER_NAME"
	export GIT_AUTHOR_EMAIL="$GIT_COMMITTER_EMAIL"

	local gitrepo="$tempdir/gitrepo"
	mkdir -p $gitrepo && cd $gitrepo
	git init --initial-branch=main
	git checkout -b main
	touch good
	git add good
	git commit -m "good"
	git checkout -b somebranch
	git rm good
	git commit -m "bad"
	git checkout main

	# Dummy PKGBUILD
	local pkgbuild_content=$(cat <<EOF
pkgname=test
pkgver=1
pkgrel=1
arch=('any')
source=(somebranch::git+file://$gitrepo)
sha256sums=('SKIP')
package() {
  [[ -e somebranch/good ]]
}
EOF
)

	local pkgdir="$tempdir/pkgdir"
	mkdir -p "$pkgdir" && cd "$pkgdir"
	echo "$pkgbuild_content" > "PKGBUILD"

	"$MAKEPKG"
}

test;
