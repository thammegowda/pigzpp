#!/usr/bin/env bash
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

echo "==> Initializing git submodules..."
git submodule update --init --recursive

echo ""
echo "==> Installing build dependencies..."
if command -v apt-get >/dev/null 2>&1; then
    sudo apt-get update -qq && sudo apt-get install -y -qq nasm cmake build-essential python3-dev
elif command -v dnf >/dev/null 2>&1; then
    sudo dnf install -y nasm cmake gcc gcc-c++ python3-devel
elif command -v brew >/dev/null 2>&1; then
    brew install nasm cmake
else
    echo "Warning: unknown package manager — please install nasm, cmake, and a C/C++ compiler manually"
fi

echo ""
echo "==> Installing Python packages..."
pip install --quiet pytest nanobind scikit-build-core 2>/dev/null || \
    echo "Warning: some Python packages failed to install"

echo ""
echo "✓ Setup complete. Run 'make build' to compile."
