#!/bin/bash
# Runs the core tests (host g++) and cross-compiles Telegraph.asi (x64 DLL) in a
# throwaway mingw-w64 container.
set -euo pipefail
cd "$(dirname "$0")"
docker build -q -t telegraph-build . >/dev/null
mkdir -p out
docker run --rm -v "$PWD":/src -w /src telegraph-build sh -c '
  set -e
  g++ -std=c++17 -O1 -Wall -Wextra -Werror -o /tmp/test_core tests/test_core.cpp && /tmp/test_core
  x86_64-w64-mingw32-g++-posix -std=c++17 -O2 -Wall -Wextra -Wno-cast-function-type \
    -shared -static -static-libgcc -static-libstdc++ -s \
    -o out/Telegraph.asi src/telegraph.cpp -lpsapi -lwinhttp'
ls -l out/Telegraph.asi
