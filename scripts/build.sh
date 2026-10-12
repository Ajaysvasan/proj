#!/bin/sh
# Builds proj: the native engine (into bin/proj-engine) and the CLI (cli/dist).
# Usage: scripts/build.sh            (from any directory)
# Nothing is installed system-wide: third-party sources are downloaded into
# build/ (pinned, SHA-256 verified) and Node packages into cli/node_modules.
set -e

root=$(cd "$(dirname "$0")/.." && pwd -P)
build_dir=${PROJ_BUILD_DIR:-"$root/build/release"}

need() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "build.sh: '$1' is required but was not found on PATH ($2)" >&2
    exit 1
  fi
}
need cmake "CMake 3.24 or newer"
need node "Node.js 20 or newer"
need npm "npm, usually installed with Node.js"
if ! node -e 'process.exit(Number(process.versions.node.split(".")[0]) >= 20 ? 0 : 1)'; then
  echo "build.sh: Node.js 20 or newer is required (found $(node --version))" >&2
  exit 1
fi

echo "==> Building the engine (release) in $build_dir"
cmake -S "$root/engine" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DPROJ_BUILD_TESTS=OFF
cmake --build "$build_dir" --parallel --target proj-engine

echo "==> Installing the engine into bin/"
mkdir -p "$root/bin"
cp "$build_dir/proj-engine" "$root/bin/proj-engine.tmp"
if command -v strip >/dev/null 2>&1; then strip "$root/bin/proj-engine.tmp"; fi
mv "$root/bin/proj-engine.tmp" "$root/bin/proj-engine"
"$root/bin/proj-engine" --version

echo "==> Building the CLI"
(cd "$root/cli" && npm ci && npm run build)

echo
echo "Done. Run proj with:"
echo "  $root/bin/proj --help"
echo "To use it from any directory, add bin/ to your PATH (e.g. in ~/.bashrc or ~/.zshrc):"
echo "  export PATH=\"$root/bin:\$PATH\""
