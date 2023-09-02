#!/bin/sh

set -eu
export LC_ALL=C

echo() { printf %s\\n "$*"; }
err()  { >&2 echo "$self: $*"; false; }
self=${0##*[\\/]}

# default libpath or CC if unset by the user
: ${libpath:=} ${CC:=cc}

HELP="\
Usage: $self   -e      LIBNAME    List exported symbols.
       $self  [-a|-A]  WINEXE     List imported symbols.

  -a   Also mark [A]NSI symbols with the matching wide variant.
  -A   Plain list of only the imported ANSI symbols.

  -a/-A   look for symbols which are potentially limiting (vs wide).
  -ax/-Ax add also general ANSI symbols with a matching wide symbol.

objdump is required. -e/-a/-A expect \$libpath, or mingw \$CC to infer it.
WINEXE is a binary, LIBNAME is msvcrt/user32/etc (-> \$libpath/lib*.a)."


# this libpath inference from $CC works on debian mingw-w64, w64devkit, MXE
ensure_libpath() {
    [ "${libpath-}" ] \
    || { _1=$(which -- "$CC") \
         && libpath=$(dirname -- "$_1")/../$("$CC" -dumpmachine)/lib \
         && [ -d "$libpath" ]; } \
    || err "can't resolve \$libpath from mingw \$CC - $CC"
}

# set $R to the lib path for dll $1, e.g. USER32.DLL -> /path/to/libuser32.a
dll_lib() {
    case $1 in *[A-Z]*) set -- "$(echo "$1" | tr \[A-Z] \[a-z])"; esac
    ensure_libpath && R=$libpath/lib${1%.*}.a \
    && { [ -e "$R" ] || err "missing lib file -- $R"; }
}

# list the exported symbols of lib (path) $1, one per line
# if $2 is not empty, wrap names as ":NAME:" (helps avoiding substring match)
list_lib_exports() {
    [ "${2-}" ] && set -- "$1" ":\1:" || set -- "$1" "\1"
    objdump -t "$1" | sed -n "s/.*__imp_\(.*\)/$2/p"  # X if ...__imp_X line
}

# list the exports for dll name (not path) $1 (upper/lower, maybe with ext)
# requires libpath or CC for dll_lib. extra arg forwards to list_lib_exports
list_dll_exports() {
    [ "${1-}" = -- ] && shift
    [ "${1-}" ] || { err "missing LIBNAME"; return; }

    dll_lib "$1" && list_lib_exports "$R" "${2-}"
}


# test whether the symbol $1 is an ansi API, in a list of exports $2.
# the exports list should have each name as ":NAME:".
# by default it tests whether ANSI is a limitation, like fopen or CreateFileA.
# if $3 is not empty, test also general matching wide APIs like {str,wcs}len.
# if ansi, then $R will hold the reason - the existing matching wide symbol.
is_ansi() {
    case $1 in [A-Z]*)
        for R in "${1%A}W" "$1W"; do  # CreateFile{A,W}, Process32First[W]
            case $2 in *":$R:"*) return 0; esac
        done
        return 1  # nothing else for APIs beginning with upper case
    esac

    for R in "_w${1#_}" "__w${1#__}"; do
        case $2 in *":$R:"*) return 0; esac
    done

    if [ "${3}" ]; then
        case $1 in str*|_str*)
            for R in "wcs${1#str}" "_wcs${1#*str}"; do
                case $2 in *":$R:"*) return 0; esac
            done
        esac

        # insert 'w' before each char, like for get[w]ch or vsn[w]printf
        tail=$1
        while [ "$tail" ]; do
            R=${1%"$tail"}w$tail
            case $2 in *":$R:"*) return 0; esac
            tail=${tail#?}
        done
    fi

    false
}

# lists all imported symbols at a win32 executable $1, grouped by DLL.
# -a before the exe also marks [A]NSI symbols (and their matching wide symbol).
# -A (instead of -a) prints a plain list of only the ANSI symbols.
list_imports() {
    ansi= plain=
    case $1 in -a*) ansi=yes plain=    extra=${1#??}; shift; esac
    case $1 in -A*) ansi=yes plain=yes extra=${1#??}; shift; esac
    [ "${1-}" = -- ] && shift
    [ "${1-}" ] || { err "missing WINEXE"; return; }
    [ "$ansi" ] && { ensure_libpath || return; }

    first=yes got=
    objdump --private-headers -- "$1" | {
        while read _1 _2 _3; do
            case "$_1 $_2 $_3" in
            "The Function Table"*)
                got=yes  # got useful input (else we should fail)
                break  # not interesting beyond this point (optimization)
                ;;

            "DLL Name:"*)
                [ "$plain" ] || { [ "$first" ] && first= || echo; }

                dll=$_3  # e.g. FOO.DLL
                [ "$plain" ] || echo "$dll:"

                if [ "$ansi" ]; then
                    exports=$(list_dll_exports "$dll" x) && [ "$exports" ] \
                    || err "can't resolve exports for '$dll'"
                fi

                read -r dummy  # fields header
                while read _vma _ord sym && [ "$sym" ]; do
                    if [ "$ansi" ] && is_ansi "$sym" "$exports" "$extra"; then
                        [ "$plain" ] && echo "$sym" || echo "[A] $sym ($R)"
                    else
                        [ "$plain" ]                || echo "    $sym"
                    fi
                done
            esac
        done && [ "$got" ]
    }
}


# main
case ${1-} in
    ""|-h) echo "$HELP";;
       -e) shift; list_dll_exports "$@";;
        *) list_imports "$@"
esac
