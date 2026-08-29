#!/bin/sh
set -eu

repository_path=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repository_path"
exec docker compose -f compose.yml -f compose.demo.yml up --build
