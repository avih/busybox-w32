#!/bin/sh
set -eu
cd -- "$(dirname -- "$0")"

toolchain64=d:/run/w64devkit/latest-x86_64/bin
toolchain32=d:/run/w64devkit/latest-i686/bin

# getopt-like, we only support [-k] [--]
[ "${1-}" = -k ] && keep=x && shift || keep=  # the tmp work dir
[ "${1-}" != -- ] || shift

id_date=$(date +%Y-%m-%d)
merge_base=$(git merge-base master HEAD)
# id_git=$(git describe --tags $merge_base)+$(git rev-list --count $merge_base..HEAD)@avih
# id_git=$(git rev-parse --short $merge_base)+$(git rev-list --count $merge_base..HEAD)@avih
id_git=$(git describe --match FRP $merge_base)+$(git rev-list --count $merge_base..HEAD)@avih
id=busybox-w32.$id_git.$id_date${1+.$*}

rm -rf -- "$id"
mkdir -- "$id"


# toolchain path, target dir inside $id, optional -u for unicode build
build_one() (
    git clean -xf
    PATH=$1\;$PATH
    target=$id/$2
    shift 2

    ./avih-build "$@"

    mkdir "$target"
    cp -a -- ./busybox.exe "$target"
)


# copy the patches

p=$id/patches-avih
mkdir "$p"
git format-patch --quiet -o "$p" $merge_base


# 4 builds: 64 (normal, unicode), 32 (normal, unicode)

build_one "$toolchain64" x86_64
build_one "$toolchain64" x86_64-unicode -u
build_one "$toolchain32" i686
build_one "$toolchain32" i686-unicode -u


# create the archive

out=$id.tar.gz
tar c -- "$id" | gzip > "$out"

echo "created: $out from:"
ls -tl -- "$id"
[ "$keep" ] && echo keeping "$id"/ || rm -rf -- "$id"
