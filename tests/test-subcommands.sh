#!/usr/bin/env bash
set -euo pipefail

kuzpkg="$1"

assert_help_maps() {
  local command="$1"
  local expected="$2"
  local output

  output="$("$kuzpkg" "$command" --help 2>&1)"
  grep -Fq "$expected" <<<"$output"
}

assert_help_maps install "kuzpkg {-S --sync}"
assert_help_maps remove "kuzpkg {-R --remove}"
assert_help_maps update "kuzpkg {-S --sync}"
assert_help_maps upgrade "kuzpkg {-S --sync}"
assert_help_maps search "kuzpkg {-S --sync}"
assert_help_maps info "kuzpkg {-Q --query}"
assert_help_maps files "kuzpkg {-F --files}"
assert_help_maps sync "kuzpkg {-S --sync}"
assert_help_maps query "kuzpkg {-Q --query}"
assert_help_maps database "kuzpkg {-D --database}"
assert_help_maps deptest "kuzpkg {-T --deptest}"
assert_help_maps depcheck "kuzpkg {-T --deptest}"
assert_help_maps clean "kuzpkg {-S --sync}"
assert_help_maps groups "kuzpkg {-S --sync}"
assert_help_maps check "kuzpkg {-Q --query}"

help="$("$kuzpkg" --help 2>&1)"
grep -Fq "kuzpkg install <pkg>..." <<<"$help"
grep -Fq "kuzpkg build [options]" <<<"$help"

echo "kuzpkg subcommand tests: PASS"
