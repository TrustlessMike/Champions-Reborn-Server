#!/bin/sh
# Build the server on macOS or Linux:  sh build.sh [debug]
# "debug" defines _DEBUG, which turns on Log::Debug output and packet hex dumps.
# The binary lands in bin/ next to config.ini; run it from that directory.
set -eu

cd "$(dirname "$0")"

CXX="${CXX:-c++}"
CC="${CC:-cc}"
OUT=build
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

CXXFLAGS="-std=c++20 -O2 -g -pthread -IInclude -IDependency/sqlite -Wno-deprecated-declarations"
if [ "${1:-}" = "debug" ]; then
	CXXFLAGS="$CXXFLAGS -D_DEBUG"
	OUT=build-debug
fi

mkdir -p "$OUT" bin

# sqlite is plain C and slow to compile, so only rebuild it when missing.
if [ ! -f "$OUT/sqlite3.o" ]; then
	echo "CC  Dependency/sqlite/sqlite3.c"
	"$CC" -O2 -DSQLITE_THREADSAFE=1 -c Dependency/sqlite/sqlite3.c -o "$OUT/sqlite3.o"
fi

# One object per source file; the name is the path with separators flattened.
# The excluded files are leftovers that the Visual Studio project does not build either.
sources() {
	find Source -name '*.cpp' ! -name 'stdafx.cpp' \
		! -name 'NotifyClientReqConnect.cpp' ! -name 'NotifyReserveUserSlot_RTA.cpp' \
		! -name 'Notify_4C.cpp' ! -name 'Notify_65.cpp' ! -name 'RequestGetFriendList.cpp' \
		! -name 'Request_5F.cpp' ! -name 'Request_61.cpp' "$@"
}

sources -print0 |
	xargs -0 -P "$JOBS" -I{} sh -c '
		obj="$1/$(printf "%s" "$2" | tr "/ " "__").o"
		if [ ! -f "$obj" ] || [ "$2" -nt "$obj" ] || [ -n "$(find Include -newer "$obj" -print -quit)" ]; then
			echo "CXX $2"
			'"$CXX"' '"$CXXFLAGS"' -c "$2" -o "$obj"
		fi
	' sh "$OUT" {}

# Link exactly the objects of the sources above, not whatever *.o is lying in the
# directory: an object of a source that no longer exists, or a duplicate left by a
# file-sync client ("name 2.o"), would otherwise be linked in as well.
OBJECTS=$(sources -print | while IFS= read -r src; do
	printf '%s ' "$OUT/$(printf "%s" "$src" | tr "/ " "__").o"
done)

echo "LD  bin/champions-server"
# The object names contain no spaces (see the tr above), so the list can be split on them.
# shellcheck disable=SC2086
"$CXX" -pthread "$OUT/sqlite3.o" $OBJECTS -o bin/champions-server

echo "Built bin/champions-server"
