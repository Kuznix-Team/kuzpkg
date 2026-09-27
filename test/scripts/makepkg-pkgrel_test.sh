#!/bin/bash

source "$(dirname "$0")"/../tap.sh || exit 1

pkgrel_lib=${1:-${PMTEST_LIBMAKEPKG_DIR}lint_pkgbuild/pkgrel.sh}
fullpkgver_lib=${PMTEST_LIBMAKEPKG_DIR}lint_pkgbuild/fullpkgver.sh
if [[ -z $pkgrel_lib || ! -f $pkgrel_lib ]]; then
	tap_bail "pkgrel library ($pkgrel_lib) could not be located"
	exit 1
fi
if [[ ! -f $fullpkgver_lib ]]; then
	tap_bail "fullpkgver library ($fullpkgver_lib) could not be located"
	exit 1
fi

. "$pkgrel_lib"
. "$fullpkgver_lib"

tap_plan 10

# pkgrel values with Kuznix suffixes, including multiple hyphen-separated
# components, must be accepted.
tap_is_int "$(check_pkgrel 1-kuznix-security >/dev/null 2>&1; echo $?)" 0 \
	"hyphenated pkgrel is accepted"
tap_is_int "$(check_pkgrel 1-kuznix-security~16.1 >/dev/null 2>&1; echo $?)" 0 \
	"hyphenated pkgrel with tilde suffix is accepted"
tap_is_int "$(check_pkgrel 1+kuznix1~lts16.1 >/dev/null 2>&1; echo $?)" 0 \
	"extended pkgrel is accepted"
tap_is_int "$(check_pkgrel 1-kuznix-security-2 >/dev/null 2>&1; echo $?)" 0 \
	"multiple hyphenated pkgrel components are accepted"
tap_is_int "$(check_pkgrel kuz >/dev/null 2>&1; echo $?)" 1 \
	"pkgrel without numeric prefix is rejected"

# Full versions must split pkgver from pkgrel at the first hyphen, allowing
# additional hyphens in pkgrel.
tap_is_int "$(check_fullpkgver 1.0-1-kuznix-security >/dev/null 2>&1; echo $?)" 0 \
	"full version with hyphenated pkgrel is accepted"
tap_is_int "$(check_fullpkgver 1.0-1+kuznix1~lts16.1 >/dev/null 2>&1; echo $?)" 0 \
	"full version with extended pkgrel is accepted"
tap_is_int "$(check_fullpkgver 1.0-1-kuznix-security-2 >/dev/null 2>&1; echo $?)" 0 \
	"full version with multiple pkgrel hyphens is accepted"
tap_is_int "$(check_fullpkgver 1.0-1-kuznix-security-extra >/dev/null 2>&1; echo $?)" 0 \
	"full version with several pkgrel suffixes is accepted"
tap_is_int "$(check_fullpkgver 1.0--bad >/dev/null 2>&1; echo $?)" 1 \
	"empty pkgver is rejected"

tap_finish
