#!/bin/sh
# Build the libfprint-cs9711 source RPM, and with --binary also the binary
# RPMs, from the current working tree (uncommitted changes included).
#
# Output goes to packaging/rpm/out/ (ignored by git). Nothing is installed.
#
#   sh packaging/rpm/make-srpm.sh            # tarball + SRPM
#   sh packaging/rpm/make-srpm.sh --binary   # tarball + SRPM + RPMs (-ba)
#
# RPMBUILD may point to an rpmbuild outside PATH; RPMBUILD_FLAGS adds
# options (for example --nodeps when a BuildRequires is provided through
# RPM_CONFIGDIR instead of an installed package).
set -eu

name=libfprint-cs9711
here=$(cd "$(dirname "$0")" && pwd)
top=$(git -C "$here" rev-parse --show-toplevel)
spec="$here/$name.spec"
out="$here/out"
rpmbuild=${RPMBUILD:-rpmbuild}
mode=-bs
[ "${1:-}" = "--binary" ] && mode=-ba

version=$(sed -n 's/^Version:[[:space:]]*//p' "$spec")
commit=$(git -C "$top" rev-parse HEAD)
snapdate=$(git -C "$top" log -1 --format=%cd --date=format:%Y%m%d)
if [ -n "$(git -C "$top" status --porcelain --untracked-files=normal)" ]; then
    dirtytag=.dirty
else
    dirtytag='%{nil}'
fi

mkdir -p "$out/SOURCES" "$out/SPECS"
tarball="$out/SOURCES/$name-$version.tar.gz"

# Tracked and untracked-but-not-ignored files as they are on disk. Build
# directories and packaging output are ignored and so never included.
# The S transform flag prefixes member names but leaves symlink targets
# alone, so relative links such as AGENTS.md -> CLAUDE.md stay valid.
(
    cd "$top"
    git ls-files -z --cached --others --exclude-standard |
        xargs -0 sh -c 'for f; do [ -e "$f" ] && printf "%s\0" "$f"; done' sh |
        tar --null --files-from=- --owner=0 --group=0 --numeric-owner \
            --sort=name --mtime="@$(git log -1 --format=%ct)" \
            --transform "s,^,$name-$version/,S" -czf "$tarball"
)

cp -p "$spec" "$out/SPECS/"
cp -p "$here/50-libfprint-cs9711.conf" "$here/README.md" "$out/SOURCES/"

# shellcheck disable=SC2086
"$rpmbuild" "$mode" ${RPMBUILD_FLAGS:-} \
    --define "_topdir $out" \
    --define "commit $commit" \
    --define "snapdate $snapdate" \
    --define "dirtytag $dirtytag" \
    "$out/SPECS/$name.spec"

echo "Output in $out/SRPMS and $out/RPMS"
