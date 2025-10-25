# ========================================================================
# 🚀 Clice Dev Container Shell Initialization
# ========================================================================
# File: docker/linux/container-entrypoint.sh
# Purpose: Bash initialization script for Clice dev container
# 
# This script is sourced by .bashrc and performs:
# 1. Runs uv sync to create/update virtual environment if needed
# 2. Auto-activates virtual environment for interactive shells
#
# Usage: This file will be appended to /root/.bashrc during image build
# ========================================================================

# Only run in interactive shells to avoid breaking non-interactive scripts
if [[ $- == *i* ]]; then
    UV_PACKAGE_ROOT=$(find "${RELEASE_PACKAGE_DIR}" -maxdepth 1 -type d -name "uv-*" | head -n 1)
    UV_PACKAGE_CACHE_DIR="${UV_PACKAGE_ROOT}/${UV_PACKAGE_DIR_NAME}"

    echo "📦 Running uv sync..."
    
    if UV_CACHE_DIR="${UV_PACKAGE_CACHE_DIR}" uv sync --project "${CLICE_WORKDIR}/pyproject.toml"; then
        echo "✅ Python environment ready at ${CLICE_WORKDIR}/.venv"
    else
        echo "⚠️  Failed to sync Python environment (pyproject.toml might not exist)"
    fi
    
    # Auto-activate virtual environment if it exists
    if [ -f "${CLICE_WORKDIR}/.venv/bin/activate" ]; then
        source "${CLICE_WORKDIR}/.venv/bin/activate"
    fi
fi

alias ll='ls -alF --color=auto'
