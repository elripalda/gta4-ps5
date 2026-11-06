#!/usr/bin/env bash
# [ps5 port] With ThinLTO, CMake's own link of gta4 would run the whole
# program's code generation, only for ps5_package.sh to link (and generate)
# it again: this stands in for that link and only writes the output's name.
out=
while [[ $# -gt 0 ]]; do
    if [[ $1 == -o ]]; then out=$2; shift; fi
    shift
done
[[ -n $out ]] && : > "$out"
exit 0
