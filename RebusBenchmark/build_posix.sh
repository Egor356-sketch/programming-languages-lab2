#!/bin/sh
set -eu
cd "$(dirname "$0")"
mkdir -p build
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Wpedantic benchmark.c wrappers/baseline.c wrappers/opt1.c wrappers/opt2.c wrappers/opt3.c wrappers/opt4.c -o build/RebusBenchmark
printf 'Run: ./build/RebusBenchmark\n'
