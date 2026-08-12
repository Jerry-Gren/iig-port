#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
CXX=${CXX:-c++}
LLVM_CONFIG=${LLVM_CONFIG:-llvm-config}
TMP_BASE=${TMPDIR:-/tmp}
WORK=$(mktemp -d "$TMP_BASE/iig-port-tests.XXXXXX")

cleanup()
{
	rm -rf "$WORK"
}
trap cleanup EXIT HUP INT TERM

TOOL=${IIG_TOOL:-"$WORK/iig"}

build_tool()
{
	if [ -n "${IIG_TOOL:-}" ]; then
		if [ ! -x "$IIG_TOOL" ]; then
			printf 'IIG_TOOL is not executable: %s\n' "$IIG_TOOL" >&2
			exit 1
		fi
		return
	fi

	(
		cd "$ROOT"
		"$CXX" -std=c++17 $("$LLVM_CONFIG" --cxxflags --ldflags) \
			iig.cpp -lclang -o "$TOOL"
	)
}

normalize_iig_cpp()
{
	sed '1s|^/\* iig([^)]*) generated from |/* iig(<normalized>) generated from |' "$1"
}

compare_outputs()
{
	manifest=$1
	expected_dir=$2
	actual_dir=$3
	diff_dir=$4

	mkdir -p "$diff_dir"
	checked=0
	failed=0

	while IFS= read -r case_name; do
		for ext in h iig.cpp edits; do
			checked=$((checked + 1))
			expected="$expected_dir/$case_name.$ext"
			actual="$actual_dir/$case_name.$ext"
			diff_file="$diff_dir/$case_name.$ext.diff"

			if [ "$ext" = "iig.cpp" ]; then
				expected_norm="$WORK/$case_name.$ext.expected"
				actual_norm="$WORK/$case_name.$ext.actual"
				normalize_iig_cpp "$expected" > "$expected_norm"
				normalize_iig_cpp "$actual" > "$actual_norm"
				if diff -u "$expected_norm" "$actual_norm" > "$diff_file"; then
					rm -f "$diff_file"
				else
					failed=$((failed + 1))
					printf 'DIFF %s.%s\n' "$case_name" "$ext"
				fi
			else
				if diff -u "$expected" "$actual" > "$diff_file"; then
					rm -f "$diff_file"
				else
					failed=$((failed + 1))
					printf 'DIFF %s.%s\n' "$case_name" "$ext"
				fi
			fi
		done
	done < "$manifest"

	printf 'CHECKED=%s FAIL=%s\n' "$checked" "$failed"
	test "$failed" -eq 0
}

run_generator()
{
	script=$1
	out_dir=$2

	if [ -n "${XNU_SRC:-}" ]; then
		IIG_TOOL="$TOOL" OUT_DIR="$out_dir" XNU_SRC="$XNU_SRC" "$script"
	else
		IIG_TOOL="$TOOL" OUT_DIR="$out_dir" "$script"
	fi
}

build_tool

archive="$ROOT/tests/archive/test-archive.tar.gz"
if [ ! -f "$archive" ]; then
	printf 'missing test archive: %s\n' "$archive" >&2
	exit 1
fi

tar -xzf "$archive" -C "$WORK"
bundle_name=$(tar -tzf "$archive" | sed -n '1s|/.*||p')
bundle="$WORK/$bundle_name"
if [ -z "$bundle_name" ] || [ ! -d "$bundle/scripts" ]; then
	printf 'test archive has unexpected layout: %s\n' "$archive" >&2
	exit 1
fi
generated="$WORK/generated"
diffs="$WORK/diffs"

mkdir -p "$generated" "$diffs"

behavior_out="$generated/behavior/DriverKit"
run_generator "$bundle/scripts/generate-behavior-local.sh" "$behavior_out"

xnu_out="$generated/xnu-driverkit/DriverKit"
run_generator "$bundle/scripts/generate-xnu-driverkit-local.sh" "$xnu_out"

cat \
	"$bundle/behavior/manifests/cases.list" \
	"$bundle/xnu-driverkit/manifests/cases.list" \
	> "$WORK/cases.list"

combined_expected="$WORK/expected/DriverKit"
combined_actual="$WORK/actual/DriverKit"
mkdir -p "$combined_expected" "$combined_actual"

cp "$bundle/behavior/xcode-output/DriverKit/"* "$combined_expected/"
cp "$bundle/xnu-driverkit/xcode-output/DriverKit/"* "$combined_expected/"
cp "$behavior_out/"* "$combined_actual/"
cp "$xnu_out/"* "$combined_actual/"

compare_outputs \
	"$WORK/cases.list" \
	"$combined_expected" \
	"$combined_actual" \
	"$diffs"

printf 'PASS full test archive\n'
