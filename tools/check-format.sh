#!/bin/sh
# Check C source against the repository's pinned formatter.
#
# Every tracked C/H file under wyrebox/ and tests/ must be an exact fixed point
# of tools/format-c, except tests/*/fixtures/, which mirror external sources
# verbatim.
#
#   --staged          check staged C/H files in the prospective index
#   --changed <base>  check C/H files changed since <base>; re-check every file
#                     when the formatter or its configuration changed
#   --all             check every formatted C/H file

set -eu

usage()
{
  echo "usage: tools/check-format.sh --staged|--changed <base-ref>|--all" >&2
  exit 2
}

BASE_REF=
case "${1-}" in
  --staged | --all)
    [ "$#" -eq 1 ] || usage
    MODE=$1
    ;;
  --changed)
    [ "$#" -eq 2 ] || usage
    MODE=$1
    BASE_REF=$2
    ;;
  *)
    usage
    ;;
esac

REPO_ROOT=$(git rev-parse --show-toplevel 2>/dev/null) || {
  echo "format: not inside a Git worktree" >&2
  exit 1
}
WORK_DIR=$(mktemp -d)
FILES="$WORK_DIR/files"
FAILURES="$WORK_DIR/failures"
INDEX_FORMATTER="$WORK_DIR/format-c"
trap 'rm -rf "$WORK_DIR"' EXIT HUP INT TERM

SOURCE=tree
FORMATTER="$REPO_ROOT/tools/format-c"

set -- 'wyrebox/*.c' 'wyrebox/*.h' 'tests/*.c' 'tests/*.h' \
  ':!:tests/*/fixtures/*'

for executable_path in \
  tools/format-c \
  tools/check-format.sh \
  tools/install-uncrustify.sh \
  tools/setup-git-hooks.sh \
  hooks/pre-commit
do
  index_mode=$(git -C "$REPO_ROOT" ls-files -s -- "$executable_path" |
    awk 'NR == 1 { print $1 }')
  if [ "$index_mode" != 100755 ]; then
    echo "format: index requires executable $executable_path" >&2
    exit 1
  fi
done

if [ "$MODE" = "--staged" ]; then
  SOURCE=index

  # The staged check must not execute an unstaged formatter. Materialize the
  # formatter blob from the prospective index before inspecting any source.
  if ! git -C "$REPO_ROOT" cat-file -e :tools/format-c 2>/dev/null; then
    echo "format: prospective index is missing tools/format-c" >&2
    exit 1
  fi
  git -C "$REPO_ROOT" show :tools/format-c > "$INDEX_FORMATTER"
  chmod +x "$INDEX_FORMATTER"
  FORMATTER=$INDEX_FORMATTER
fi

if [ ! -x "$FORMATTER" ]; then
  echo "format: formatter is missing or not executable: tools/format-c" >&2
  exit 1
fi
"$FORMATTER" --check-version > /dev/null

case "$MODE" in
  --staged)
    git -C "$REPO_ROOT" diff --cached --name-only --diff-filter=ACMR -z -- \
      "$@" > "$FILES"
    ;;
  --changed)
    if ! git -C "$REPO_ROOT" rev-parse --verify --quiet "$BASE_REF" \
      > /dev/null; then
      echo "format: unknown base ref: $BASE_REF" >&2
      exit 1
    fi
    if git -C "$REPO_ROOT" diff --quiet "$BASE_REF...HEAD" -- \
      uncrustify.cfg tools/format-c; then
      git -C "$REPO_ROOT" diff --name-only --diff-filter=ACMR -z \
        "$BASE_REF...HEAD" -- "$@" > "$FILES"
    else
      git -C "$REPO_ROOT" ls-files -z -- "$@" > "$FILES"
    fi
    ;;
  --all)
    git -C "$REPO_ROOT" ls-files -z -- "$@" > "$FILES"
    ;;
esac

if [ ! -s "$FILES" ]; then
  exit 0
fi

# POSIX sh has no portable NUL-delimited read. Linux xargs -0 passes each
# child one exact pathname as an argv element.
# shellcheck disable=SC2016 # The single-quoted child script expands in sh -c.
if ! xargs -0 -n 1 sh -c '
  repo_root=$1
  source_kind=$2
  formatter=$3
  failures=$4
  path=$5
  input=$(mktemp)
  formatted=$(mktemp)
  trap '"'"'rm -f "$input" "$formatted"'"'"' EXIT HUP INT TERM

  if [ "$source_kind" = index ]; then
    if ! git -C "$repo_root" show ":$path" > "$input"; then
      echo "format: unable to read staged content: $path" >&2
      exit 1
    fi
  elif ! cp "$repo_root/$path" "$input"; then
    echo "format: tracked file is missing from the worktree: $path" >&2
    exit 1
  fi

  cp "$input" "$formatted"
  if ! "$formatter" "$formatted"; then
    echo "format: formatter failed while checking: $path" >&2
    exit 1
  fi
  if ! cmp -s "$input" "$formatted"; then
    {
      printf "  "
      printf "%s" "$path" | sed -n l
    } >> "$failures"
    exit 1
  fi
' sh "$REPO_ROOT" "$SOURCE" "$FORMATTER" "$FAILURES" < "$FILES"
then
  if [ -s "$FAILURES" ]; then
    if [ "$SOURCE" = index ]; then
      echo "format: staged content is not an Uncrustify 0.83.0 fixed point:" >&2
    else
      echo "format: files are not an Uncrustify 0.83.0 fixed point:" >&2
    fi
    cat "$FAILURES" >&2
    echo "format: fix with ./tools/format-c <file>, then stage the result" >&2
  else
    echo "format: formatter failed before completing the fixed-point check" >&2
  fi
  exit 1
fi
