#!/usr/bin/env bash
# Builds the Linux x64 server packages into dist/:
#   CS2SPY-<version>-linux-x64.zip             Metamod:Source 2.0 with KHook (plugin API 18)
#   CS2SPY-<version>-linux-x64-sourcehook.zip  Metamod:Source with SourceHook (plugin API 17)
#
# Needs git, python3 with AMBuild (pip install git+https://github.com/alliedmodders/ambuild),
# clang and zip. Build on glibc 2.31 or older (Ubuntu 20.04), otherwise the plugin
# does not load in the Steam Runtime CS2 servers run in.
#
# usage: ./build-linux.sh [khook|sourcehook]   (both by default)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
EXTERNAL="$ROOT/external"
VERSION="$(tr -d '[:space:]' < "$ROOT/VERSION")"

# hl2sdk (cs2 branch) the plugin was tested with
HL2SDK_REF=22087f532ec572d0965c11cdf0f9c245e65ac71b
# Metamod:Source 2.0 with KHook (plugin API 18)
MMS_KHOOK_REF=9c49d4c9733d94605901fb80148ea8c553f059d3
# Last Metamod:Source commit with SourceHook (plugin API 17)
MMS_SOURCEHOOK_REF=7ec0f16948ab3a0910a98b4ac10e1c0e360d5339

fetch() { # url dir ref [submodule]
	if [ ! -d "$2/.git" ]; then
		git init -q "$2"
		git -C "$2" remote add origin "$1"
	fi
	if [ "$(git -C "$2" rev-parse -q --verify HEAD || true)" != "$3" ]; then
		git -C "$2" fetch -q --depth 1 origin "$3"
		git -C "$2" checkout -q FETCH_HEAD
	fi
	if [ -n "${4:-}" ]; then
		git -C "$2" submodule update -q --init --depth 1 "$4"
	fi
}

build() { # name metamod-dir zip
	local out="$ROOT/build-$1"
	rm -rf "$out"
	mkdir -p "$out" "$ROOT/dist"
	(
		cd "$out"
		CC="${CC:-clang}" CXX="${CXX:-clang++}" python3 "$ROOT/configure.py" -s cs2 --targets x86_64 --enable-optimize \
			--hl2sdk-manifests=./hl2sdk-manifests --mms_path="$2" --hl2sdk-root="$EXTERNAL"
		ambuild
	)
	rm -f "$ROOT/dist/$3"
	(cd "$out/package" && zip -qr "$ROOT/dist/$3" addons)
	echo "dist/$3"
}

VARIANTS="${1:-khook sourcehook}"
mkdir -p "$EXTERNAL"
fetch https://github.com/alliedmodders/hl2sdk.git "$EXTERNAL/hl2sdk-cs2" "$HL2SDK_REF"

for variant in $VARIANTS; do
	case "$variant" in
		khook)
			fetch https://github.com/alliedmodders/metamod-source.git "$EXTERNAL/metamod-khook" "$MMS_KHOOK_REF" third_party/khook
			build khook "$EXTERNAL/metamod-khook" "CS2SPY-$VERSION-linux-x64.zip"
			;;
		sourcehook)
			fetch https://github.com/alliedmodders/metamod-source.git "$EXTERNAL/metamod-sourcehook" "$MMS_SOURCEHOOK_REF"
			build sourcehook "$EXTERNAL/metamod-sourcehook" "CS2SPY-$VERSION-linux-x64-sourcehook.zip"
			;;
		*)
			echo "unknown variant: $variant (khook, sourcehook)" >&2
			exit 1
			;;
	esac
done
