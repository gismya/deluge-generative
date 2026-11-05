#!/usr/bin/env bash
# pre-commit hook: format staged website files with the website's own Prettier,
# so local formatting matches the CI Formatting Check.
#
# pre-commit passes paths relative to the repo root (website/...). Prettier has to
# run from website/ to load the plugins in .prettierrc and to read .prettierignore,
# so cd there and strip the prefix.
set -euo pipefail

[ $# -eq 0 ] && exit 0

cd "$(dirname "$0")/.."

if [ ! -x node_modules/.bin/prettier ]; then
  echo "website/node_modules is missing, so Prettier can't run." >&2
  echo "Run 'pnpm install' in website/ once, then commit again." >&2
  exit 1
fi

files=()
for f in "$@"; do
  files+=("${f#website/}")
done

exec node_modules/.bin/prettier --write --ignore-unknown --log-level warn "${files[@]}"
