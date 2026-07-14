#!/usr/bin/env bash
# Install dependencies for the Docker gzip benchmark (benchmarks/go-docker).
#
# Installs the Go toolchain (needed to build dockergzbench) and fetches the
# Go module dependencies. Run with sudo so Go can be installed to /usr/local.
#
#   sudo ./setup.sh
#
# Optional: install docker/podman separately if you want gen_layer.sh to build
# a real image layer as benchmark input.
set -euo pipefail

GO_VERSION="${GO_VERSION:-1.22.5}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# --- Detect OS/arch for the Go tarball ---
case "$(uname -s)" in
    Linux)  GOOS="linux" ;;
    Darwin) GOOS="darwin" ;;
    *) echo "Unsupported OS: $(uname -s)" >&2; exit 1 ;;
esac
case "$(uname -m)" in
    x86_64|amd64) GOARCH="amd64" ;;
    aarch64|arm64) GOARCH="arm64" ;;
    *) echo "Unsupported arch: $(uname -m)" >&2; exit 1 ;;
esac

# --- Install Go if missing or too old ---
need_go=1
if command -v go >/dev/null 2>&1; then
    echo "==> Found existing Go: $(go version)"
    need_go=0
elif [ -x /usr/local/go/bin/go ]; then
    echo "==> Found Go in /usr/local/go: $(/usr/local/go/bin/go version)"
    export PATH="/usr/local/go/bin:$PATH"
    need_go=0
fi

if [ "$need_go" -eq 1 ]; then
    echo "==> Installing Go ${GO_VERSION} for ${GOOS}-${GOARCH} ..."
    if command -v apt-get >/dev/null 2>&1 && [ "$GOOS" = "linux" ]; then
        # Prefer the official tarball for an up-to-date toolchain.
        tarball="go${GO_VERSION}.${GOOS}-${GOARCH}.tar.gz"
        tmp="$(mktemp -d)"
        trap 'rm -rf "$tmp"' EXIT
        echo "    downloading https://go.dev/dl/${tarball}"
        curl -fsSL "https://go.dev/dl/${tarball}" -o "$tmp/go.tar.gz"
        rm -rf /usr/local/go
        tar -C /usr/local -xzf "$tmp/go.tar.gz"
        export PATH="/usr/local/go/bin:$PATH"
    elif command -v brew >/dev/null 2>&1; then
        brew install go
    else
        echo "error: could not install Go automatically; install Go >= 1.22 manually" >&2
        exit 1
    fi
    echo "==> Installed: $(go version)"
fi

# --- Fetch Go module deps ---
echo ""
echo "==> Fetching Go module dependencies ..."
cd "$SCRIPT_DIR"
go mod tidy

# --- Build the benchmark ---
echo ""
echo "==> Building dockergzbench ..."
go build -o dockergzbench .

echo ""
echo "✓ Setup complete."
echo "  If Go was just installed, add it to your PATH:"
echo "      export PATH=/usr/local/go/bin:\$PATH"
echo ""
echo "  Run the benchmark, e.g.:"
echo "      ./dockergzbench -size 64 -iters 5 -threads \$(nproc) -pigzpp ../../build/pigzpp"
echo "  Or on a real layer:"
echo "      ./gen_layer.sh && ./dockergzbench -input layer.tar -threads \$(nproc) -pigzpp ../../build/pigzpp"
