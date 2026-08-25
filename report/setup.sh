#!/usr/bin/env bash
set -euo pipefail

log() {
    echo "[$(date +'%Y-%m-%d %H:%M:%S')] $1" >&2
}

# A "working" Tectonic is one that actually runs on this host. Some prebuilt
# Linux binaries are compiled against a newer glibc than older distros ship
# (e.g. the GitHub gnu build needs glibc >= 2.36, which Ubuntu 22.04 lacks), so
# we must verify execution, not just presence.
tectonic_works() { command -v "$1" >/dev/null 2>&1 && "$1" --version >/dev/null 2>&1; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ACL natbib bibliography style (used by \bibliographystyle{acl_natbib}); not
# committed to git, fetched here if missing.
ACL_BST="${SCRIPT_DIR}/acl_natbib.bst"
if [ ! -f "${ACL_BST}" ]; then
    log "Downloading acl_natbib.bst ..."
    curl -fsSL -o "${ACL_BST}" \
        "https://raw.githubusercontent.com/acl-org/acl-style-files/master/acl_natbib.bst" \
        || { log "ERROR: failed to download acl_natbib.bst"; exit 1; }
    log "acl_natbib.bst ready."
fi

# 0) Already have a working Tectonic on PATH? Done.
if tectonic_works tectonic; then
    log "Tectonic already installed and working: $(command -v tectonic)"
    exit 0
fi

# 1) Preferred shortcut on Linux (esp. older glibc): install from conda-forge.
#    The conda-forge build targets glibc 2.16 and runs on Ubuntu 20.04/22.04+
#    without any OS upgrade. This is the recommended path when conda/mamba exist.
if command -v mamba >/dev/null 2>&1; then
    log "Installing Tectonic from conda-forge via mamba..."
    if mamba install -y -c conda-forge tectonic && tectonic_works tectonic; then
        log "Installed conda-forge Tectonic."; exit 0
    fi
elif command -v conda >/dev/null 2>&1; then
    log "Installing Tectonic from conda-forge via conda..."
    if conda install -y -c conda-forge tectonic && tectonic_works tectonic; then
        log "Installed conda-forge Tectonic."; exit 0
    fi
fi

# 2) Fallback: download the official prebuilt release into ~/.local/bin.
#    NOTE: on Linux this is a glibc build; if it fails to run on an older distro,
#    install conda/mamba and re-run, or upgrade the OS.
TECTONIC_VERSION="0.16.9"
BASE_URL="https://github.com/tectonic-typesetting/tectonic/releases/download/tectonic%40${TECTONIC_VERSION}"

OS="$(uname -s)"
ARCH="$(uname -m)"
case "${OS}" in
    Linux)
        TARGET="x86_64-unknown-linux-gnu" ;;
    Darwin)
        case "${ARCH}" in
            arm64|aarch64) TARGET="aarch64-apple-darwin" ;;
            x86_64)        TARGET="x86_64-apple-darwin" ;;
            *) log "Unsupported macOS architecture: ${ARCH}"; exit 1 ;;
        esac ;;
    *)
        log "Unsupported OS: ${OS}"; exit 1 ;;
esac
URL="${BASE_URL}/tectonic-${TECTONIC_VERSION}-${TARGET}.tar.gz"

if [ ! -f "$HOME/.local/bin/tectonic" ]; then
    log "Downloading ${TARGET} build into ~/.local/bin ..."
    mkdir -p ~/.local/bin
    curl -L "$URL" | tar -xz -C ~/.local/bin
fi
# if .local/bin/tectonic exists but is not in PATH, add it to PATH
if [ -f "$HOME/.local/bin/tectonic" ] && [[ ":${PATH}:" != *":$HOME/.local/bin:"* ]]; then
    log "Adding ~/.local/bin to PATH for this session..."
    export PATH="$HOME/.local/bin:$PATH"
fi

if ! tectonic_works "$HOME/.local/bin/tectonic"; then
    log "WARNING: the downloaded Tectonic does not run on this host (likely a"
    log "glibc mismatch). Install conda/mamba and re-run ./setup.sh to get the"
    log "conda-forge build, which works on older distros without an OS upgrade."
fi