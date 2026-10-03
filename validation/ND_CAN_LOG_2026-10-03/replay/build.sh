#!/bin/sh
# Host build with the repo's flags (Makefile: C_WARN / CXX_WARN, test_navigation link set).
set -e
W=$(cd "$(dirname "$0")" && pwd); R=${R:-$(cd "$W/../../.." && pwd)}
cc -std=c99 -O2 -Wall -Wextra -Werror -pedantic -c $R/src/core/dr_core.c -o $W/core_host.o
c++ -std=c++11 -O2 -Wall -Wextra -Werror -I$R/src $W/replay.cpp $R/src/navigation/pipeline.cpp $R/src/navigation/channel.cpp $R/src/navigation/holdout.cpp $R/src/runtime/core_bridge.cpp \
  $R/src/adapter/adapter.cpp $R/src/adapter/arm_entry.cpp $R/src/adapter/v74_install.cpp $R/src/adapter/bus_hooks.cpp $R/src/adapter/session_hooks.cpp \
  $R/src/adapter/request_hooks.cpp $R/src/runtime/request_observer.cpp $R/src/runtime/request_trace.cpp \
  $W/core_host.o -lm -ldl -pthread -o $W/replay
