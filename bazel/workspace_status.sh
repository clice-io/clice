#!/usr/bin/env bash
# Bazel's --workspace_status_command: the version cmake/generate_version.cmake
# derives from git, as STABLE_CLICE_VERSION (a change rebuilds the stamped
# version.h and its includers).
fallback=0.1.0
cd "$(dirname "$0")/.." || exit 1
version=$fallback
if [ "$(git rev-parse --show-toplevel 2>/dev/null)" = "$(pwd -P)" ]; then
    tag=$(git tag --points-at HEAD --sort=-v:refname 2>/dev/null | head -1)
    if [ -n "$tag" ]; then
        describe=$tag
        git diff-index --quiet HEAD -- 2>/dev/null || describe=$describe-dirty
    else
        describe=$(git describe --tags --always --dirty 2>/dev/null)
    fi
    case "$describe" in
        "") ;;
        v*) version=${describe#v} ;;
        *)
            if printf '%s' "$describe" | grep -Eq '^[0-9a-f]+(-dirty)?$'; then
                version=$fallback+g$describe
            else
                version=$describe
            fi
            ;;
    esac
fi
echo "STABLE_CLICE_VERSION $version"
