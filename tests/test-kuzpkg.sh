#!/usr/bin/env bash
set -euo pipefail

kuzpkg="$1"
kuzpkg_name="$(basename "$kuzpkg")"
vercmp="$2"
pass=0
fail=0
n=0

run_test() {
  local name="$1"
  shift
  n=$((n + 1))
  if "$@"; then
    printf 'ok %d - %s\n' "$n" "$name"
    pass=$((pass + 1))
  else
    printf 'not ok %d - %s\n' "$n" "$name"
    fail=$((fail + 1))
  fi
}

contains() {
  local needle="$1"
  shift
  grep -Fq -- "$needle" <<<"$("$@" 2>&1)"
}

status_is() {
  local expected="$1"
  shift
  set +e
  "$@" >/dev/null 2>&1
  local got=$?
  set -e
  test "$got" -eq "$expected"
}

vercmp_is() {
  local expected="$1" a="$2" b="$3"
  test "$("$vercmp" "$a" "$b")" -eq "$expected"
}

printf 'TAP version 13\n'

run_test "install maps to sync" contains "usage:  $kuzpkg_name {-S --sync}" "$kuzpkg" install --help
run_test "remove maps to remove" contains "usage:  $kuzpkg_name {-R --remove}" "$kuzpkg" remove --help
run_test "update maps to sync" contains "usage:  $kuzpkg_name {-S --sync}" "$kuzpkg" update --help
run_test "upgrade maps to sync" contains "usage:  $kuzpkg_name {-S --sync}" "$kuzpkg" upgrade --help
run_test "search maps to sync" contains "usage:  $kuzpkg_name {-S --sync}" "$kuzpkg" search --help
run_test "info maps to query" contains "usage:  $kuzpkg_name {-Q --query}" "$kuzpkg" info --help
run_test "files maps to files" contains "usage:  $kuzpkg_name {-F --files}" "$kuzpkg" files --help
run_test "sync maps to sync" contains "usage:  $kuzpkg_name {-S --sync}" "$kuzpkg" sync --help
run_test "query maps to query" contains "usage:  $kuzpkg_name {-Q --query}" "$kuzpkg" query --help
run_test "database maps to database" contains "usage:  $kuzpkg_name {-D --database}" "$kuzpkg" database --help
run_test "deptest maps to deptest" contains "usage:  $kuzpkg_name {-T --deptest}" "$kuzpkg" deptest --help
run_test "depcheck aliases deptest" contains "usage:  $kuzpkg_name {-T --deptest}" "$kuzpkg" depcheck --help
run_test "clean maps to sync" contains "usage:  $kuzpkg_name {-S --sync}" "$kuzpkg" clean --help
run_test "groups maps to sync" contains "usage:  $kuzpkg_name {-S --sync}" "$kuzpkg" groups --help
run_test "check maps to query" contains "usage:  $kuzpkg_name {-Q --query}" "$kuzpkg" check --help

run_test "remove exposes cascade" contains "--cascade" "$kuzpkg" remove --help
run_test "remove exposes recursive" contains "--recursive" "$kuzpkg" remove --help
run_test "query exposes info" contains "--info" "$kuzpkg" query --help
run_test "query exposes check" contains "--check" "$kuzpkg" query --help
run_test "query exposes search" contains "--search" "$kuzpkg" query --help
run_test "sync exposes refresh" contains "--refresh" "$kuzpkg" sync --help
run_test "sync exposes sysupgrade" contains "--sysupgrade" "$kuzpkg" sync --help
run_test "sync exposes downloadonly" contains "--downloadonly" "$kuzpkg" sync --help
run_test "files exposes regex" contains "--regex" "$kuzpkg" files --help
run_test "files exposes machinereadable" contains "--machinereadable" "$kuzpkg" files --help
run_test "database exposes asdeps" contains "--asdeps" "$kuzpkg" database --help
run_test "database exposes asexplicit" contains "--asexplicit" "$kuzpkg" database --help

run_test "unknown option fails" status_is 1 "$kuzpkg" --kuzpkg-test-invalid-option
run_test "unknown command fails" status_is 1 "$kuzpkg" kuzpkg-test-invalid-command
run_test "empty arguments fail" status_is 1 "$kuzpkg"
run_test "duplicate operations fail" status_is 1 "$kuzpkg" -S -R
run_test "invalid color fails" status_is 1 "$kuzpkg" -S --color invalid
run_test "invalid debug level fails" status_is 1 "$kuzpkg" -S --debug invalid

run_test "vercmp equal" vercmp_is 0 1.0 1.0
run_test "vercmp lower" vercmp_is -1 1.0 2.0
run_test "vercmp higher" vercmp_is 1 2.0 1.0
run_test "vercmp patch lower" vercmp_is -1 1.0.1 1.0.2
run_test "vercmp patch higher" vercmp_is 1 1.0.2 1.0.1
run_test "vercmp trailing zero" vercmp_is -1 1.0 1.0.0
run_test "vercmp epoch" vercmp_is 1 2:1.0 1:9.9
run_test "vercmp prerelease" vercmp_is -1 1.0alpha 1.0
run_test "vercmp help succeeds" status_is 0 "$vercmp" --help
run_test "vercmp missing arguments returns 2" status_is 2 "$vercmp"
run_test "vercmp too many arguments fails" status_is 1 "$vercmp" 1 2 3

printf '1..%d\n' "$n"
printf '# passed: %d, failed: %d\n' "$pass" "$fail"
test "$fail" -eq 0
