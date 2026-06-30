#!/bin/bash

source "$(dirname "$0")"/../tap.sh || exit 1

if [[ -z $PMTEST_LIBMAKEPKG_DIR || ! -d $PMTEST_LIBMAKEPKG_DIR ]]; then
	tap_bail "libmakepkg directory (%s) could not be located" "$PMTEST_LIBMAKEPKG_DIR"
	exit 1
fi

MAKEPKG_LIBRARY=${PMTEST_LIBMAKEPKG_DIR%/}
source "$MAKEPKG_LIBRARY/util/source.sh" || exit 1
source "$MAKEPKG_LIBRARY/source/file.sh" || exit 1
source "$MAKEPKG_LIBRARY/source/local.sh" || exit 1

TMPDIR="$(mktemp -d "/tmp/${0##*/}.XXXXXX")"
trap "rm -rf '${TMPDIR}'" EXIT TERM

startdir="$TMPDIR/start"
SRCDEST="$TMPDIR/srcdest"
mkdir "$startdir" "$SRCDEST"

downloader="$TMPDIR/downloader"
cat > "$downloader" <<'EOF'
#!/bin/bash
outfile=
url=
while (( $# )); do
	case $1 in
		-o) shift; outfile=$1 ;;
		*) url=$1 ;;
	esac
	shift
done
printf '%s\n' "$url" > "$outfile"
printf '%s\n' "$url" >> "$DOWNLOAD_LOG"
EOF
chmod +x "$downloader"

DOWNLOAD_LOG="$TMPDIR/download.log"
export DOWNLOAD_LOG
QUIET=1

tap_plan 9

source_url='testdl://example.invalid/source.txt'

download_test_file() {
	pushd "$SRCDEST" &>/dev/null
	download_file "$1"
	popd &>/dev/null
}

printf 'cached\n' > "$SRCDEST/source.txt"
MAKEPKG_FRESHSOURCE=n
DLAGENTS=()
download_test_file "$source_url"
tap_diff "$SRCDEST/source.txt" <(printf 'cached\n') "cached source is reused without a downloader by default"

DLAGENTS=("testdl::$downloader -o %o %u")
download_test_file "$source_url"
tap_diff "$SRCDEST/source.txt" <(printf 'cached\n') "cached source is reused by default"
[[ ! -e $DOWNLOAD_LOG ]]
tap_ok $? "default source reuse does not run downloader"

printf 'cached\n' > "$SRCDEST/source.txt"
printf 'partial\n' > "$SRCDEST/source.txt.part"
MAKEPKG_FRESHSOURCE=y
download_test_file "$source_url"
tap_diff "$SRCDEST/source.txt" <(printf '%s\n' "$source_url") "freshsource redownloads cached source"
[[ ! -e $SRCDEST/source.txt.part ]]
tap_ok $? "freshsource removes partial download"
tap_diff "$DOWNLOAD_LOG" <(printf '%s\n' "$source_url") "freshsource runs downloader once"

rm -f "$DOWNLOAD_LOG"
printf 'local\n' > "$startdir/local.txt"
download_local "local.txt"
tap_diff "$startdir/local.txt" <(printf 'local\n') "freshsource does not remove local sources"
[[ ! -e $DOWNLOAD_LOG ]]
tap_ok $? "local source does not run downloader"

rm -f "$SRCDEST/source.txt"
download_test_file "$source_url"
tap_diff "$SRCDEST/source.txt" <(printf '%s\n' "$source_url") "freshsource downloads missing source"

tap_finish
