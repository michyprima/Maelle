#!/bin/bash
# Builds the Mac (or Linux) stand-in for Maelle's save sync server, and the core's tests
set -e
here=$(cd "$(dirname "$0")"; pwd)
c++ -std=c++17 -O2 -Wall -o "$here/maelle-sync-host" "$here/host.cpp" "$here/../common/sync_server.cpp" \
    "$here/../common/sync_core.cpp"
c++ -std=c++17 -O2 -Wall -o "$here/core-test" "$here/core_test.cpp" "$here/../common/sync_core.cpp"
