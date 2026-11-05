#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# What is new upstream since Alice68 last looked: tools/upstreams.tsv lists each upstream
# and the version reviewed. Needs git and network access. Changes nothing.
#   tools/check-upstreams.sh
D=$(cd "$(dirname "$0")" && pwd)
new=0
grep -v '^#' "$D/upstreams.tsv" | while IFS='	' read -r name kind repo ref reviewed what; do
    [ -n "$name" ] || continue
    case $kind in
    branch)
        head=$(git ls-remote "$repo" "$ref" 2>/dev/null | cut -f1)
        if [ -z "$head" ]; then printf '%-16s ?  could not reach %s\n' "$name" "$repo"
        elif [ "$head" = "$reviewed" ]; then printf '%-16s ok  %s\n' "$name" "$(echo "$reviewed" | cut -c1-7)"
        else printf '%-16s NEW %s -> %s   %s/compare/%s...%s\n' "$name" "$(echo "$reviewed" | cut -c1-7)" "$(echo "$head" | cut -c1-7)" "$repo" "$(echo "$reviewed" | cut -c1-12)" "$(echo "$head" | cut -c1-12)"
        fi ;;
    tags)
        tags=$(git ls-remote --tags --refs "$repo" 2>/dev/null | sed 's#.*refs/tags/##' | sort -V)
        if [ -z "$tags" ]; then printf '%-16s ?  could not reach %s\n' "$name" "$repo"; continue; fi
        newer=$(printf '%s\n%s\n' "$tags" "$reviewed" | sort -V -u | sed -n "/^$(printf '%s' "$reviewed" | sed 's/[.[\*^$/]/\\&/g')\$/,\$p" | grep -v -x -F "$reviewed")
        if [ -z "$newer" ]; then printf '%-16s ok  %s\n' "$name" "$reviewed"
        else printf '%-16s NEW after %s: %s   %s/releases\n' "$name" "$reviewed" "$(echo $newer)" "$repo"
        fi ;;
    manual)
        printf '%-16s ?? check by hand (reviewed %s): %s\n' "$name" "$reviewed" "$repo" ;;
    esac
done
