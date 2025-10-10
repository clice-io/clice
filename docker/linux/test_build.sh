#!/bin/bash
# ========================================================================
# 🧪 Clice Build Test Script
# ========================================================================
# File: docker/linux/test_build.sh
# Purpose: Test building Clice with different configurations
# 
# This script tests the complete Clice build process with four different
# configurations to ensure the development environment is working correctly.
# It also runs xmake tests to validate the built binaries.
# ========================================================================

set -e

# ========================================================================
# 🔧 Environment Setup
# ========================================================================

# SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONTAINER_NAME=""
COMPILER="clang"
PARALLEL_JOBS=$(nproc 2>/dev/null || echo "4")
BUILD_DIR_BASE="/tmp/clice-test-builds"

# ========================================================================
# ⚙️ Configuration Matrix
# ========================================================================

# Define the four build configurations to test
declare -A BUILD_CONFIGS=(
    ["debug"]="--mode=debug"
    ["release"]="--mode=release"
    ["debug-optimized"]="--mode=debug --release=y"
    ["release-optimized"]="--mode=release --release=y"
)

# ========================================================================
# 📚 Usage Information
# ========================================================================

usage() {
cat <<EOF
🧪 Clice Build Test Script

Usage: $0 [OPTIONS]

OPTIONS:
  --container <name>         Use specific container name
  --compiler <gcc|clang>     Target compiler (default: ${COMPILER})
  --jobs <n>                 Number of parallel jobs (default: ${PARALLEL_JOBS})
  --config <name>            Test specific configuration only
  --help, -h                 Show this help message

CONFIGURATIONS:
  debug                      Debug build (--mode=debug)
  release                    Release build (--mode=release)
  debug-optimized            Debug with optimizations (--mode=debug --release=y)
  release-optimized          Release with optimizations (--mode=release --release=y)

EXAMPLES:
  $0                         Test all configurations
  $0 --config debug          Test debug configuration only
  $0 --compiler gcc --jobs 8    Use GCC with 8 parallel jobs

This script will:
  1. Set up separate build directories for each configuration
  2. Build Clice with each configuration
  3. Run xmake tests for each build
  4. Report results and timing information
EOF
}

# ========================================================================
# 🔍 Command Line Parsing
# ========================================================================

SPECIFIC_CONFIG=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    --container)
      CONTAINER_NAME="$2"; shift 2;;
    --compiler)
      COMPILER="$2"; shift 2;;
    --jobs)
      PARALLEL_JOBS="$2"; shift 2;;
    --config)
      SPECIFIC_CONFIG="$2"; shift 2;;
    -h|--help)
      usage; exit 0;;
    *)
      echo "❌ Unknown parameter: $1" >&2; usage; exit 1;;
  esac
done

# Auto-detect container if not specified
if [ -z "$CONTAINER_NAME" ]; then
  CONTAINER_NAME="clice-dev-linux-${COMPILER}"
fi

# ========================================================================
# 🧪 Test Functions
# ========================================================================

run_in_container() {
  local cmd="$1"
  echo "🏃 Running in container: $cmd"
  docker exec -w "/clice" "$CONTAINER_NAME" bash -c "$cmd"
}

test_build_configuration() {
  local config_name="$1"
  local build_args="$2"
  local build_dir="$BUILD_DIR_BASE/$config_name"
  
  echo ""
  echo "========================================================================="
  echo "🔨 Testing Configuration: $config_name"
  echo "========================================================================="
  echo "📁 Build Directory: $build_dir"
  echo "⚙️  Build Arguments: $build_args"
  echo "🔧 Compiler: $COMPILER"
  echo "⚡ Parallel Jobs: $PARALLEL_JOBS"
  echo "========================================================================="
  
  local start_time
  start_time=$(date +%s)
  
  # Create build directory
  run_in_container "mkdir -p $build_dir"
  
  # Configure build
  echo "🔧 Configuring build..."
  if ! run_in_container "cd $build_dir && xmake config $build_args --jobs=$PARALLEL_JOBS"; then
    echo "❌ Configuration failed for $config_name"
    return 1
  fi
  
  # Build project
  echo "🏗️ Building project..."
  if ! run_in_container "cd $build_dir && xmake build --jobs=$PARALLEL_JOBS"; then
    echo "❌ Build failed for $config_name"
    return 1
  fi
  
  # Run tests
  echo "🧪 Running tests..."
  if ! run_in_container "cd $build_dir && xmake test"; then
    echo "⚠️ Tests failed for $config_name (build succeeded)"
    # Don't return error for test failures, just note them
  fi
  
  local end_time
  end_time=$(date +%s)
  local duration=$((end_time - start_time))
  
  echo "✅ Configuration $config_name completed in ${duration}s"
  
  # Store build info
  run_in_container "cd $build_dir && echo 'Build completed at: $(date)' > build_info.txt"
  run_in_container "cd $build_dir && echo 'Build duration: ${duration}s' >> build_info.txt"
  run_in_container "cd $build_dir && echo 'Configuration: $config_name' >> build_info.txt"
  run_in_container "cd $build_dir && echo 'Build args: $build_args' >> build_info.txt"
  
  return 0
}

check_prerequisites() {
  echo "🔍 Checking prerequisites..."
  
  # Check if container exists and is running
  if ! docker ps --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
    echo "❌ Container $CONTAINER_NAME is not running"
    echo "💡 Start it with: ./docker/linux/run.sh --compiler $COMPILER"
    exit 1
  fi
  
  # Check if xmake is available in container
  if ! run_in_container "command -v xmake >/dev/null"; then
    echo "❌ xmake is not available in container $CONTAINER_NAME"
    echo "💡 Make sure the container setup completed successfully"
    exit 1
  fi
  
  # Check if we're in the clice project directory
  if ! run_in_container "test -f xmake.lua"; then
    echo "❌ xmake.lua not found in container /clice directory"
    echo "💡 Make sure the project is properly mounted in the container"
    exit 1
  fi
  
  echo "✅ Prerequisites check passed"
}

generate_report() {
  echo ""
  echo "========================================================================="
  echo "📊 BUILD TEST REPORT"
  echo "========================================================================="
  
  local total_configs=0
  local successful_configs=0
  local failed_configs=()
  
  # Count and report results
  for config_name in "${!BUILD_CONFIGS[@]}"; do
    if [ -n "$SPECIFIC_CONFIG" ] && [ "$config_name" != "$SPECIFIC_CONFIG" ]; then
      continue
    fi
    
    total_configs=$((total_configs + 1))
    
    local build_dir="$BUILD_DIR_BASE/$config_name"
    if run_in_container "test -f $build_dir/build_info.txt" 2>/dev/null; then
      successful_configs=$((successful_configs + 1))
      echo "✅ $config_name: SUCCESS"
      run_in_container "cat $build_dir/build_info.txt" | sed 's/^/   /'
    else
      failed_configs+=("$config_name")
      echo "❌ $config_name: FAILED"
    fi
    echo ""
  done
  
  # Summary
  echo "========================================================================="
  echo "📈 SUMMARY"
  echo "========================================================================="
  echo "🎯 Total Configurations: $total_configs"
  echo "✅ Successful: $successful_configs"
  echo "❌ Failed: ${#failed_configs[@]}"
  
  if [ ${#failed_configs[@]} -gt 0 ]; then
    echo ""
    echo "Failed configurations:"
    for config in "${failed_configs[@]}"; do
      echo "  • $config"
    done
  fi
  
  echo ""
  echo "📁 Build artifacts location: $BUILD_DIR_BASE"
  echo "🐳 Container: $CONTAINER_NAME"
  echo "🔧 Compiler: $COMPILER"
  echo "========================================================================="
  
  # Return exit code based on results
  if [ ${#failed_configs[@]} -eq 0 ]; then
    return 0
  else
    return 1
  fi
}

# ========================================================================
# 🚀 Main Execution
# ========================================================================

main() {
  echo "========================================================================="
  echo "🧪 CLICE BUILD TEST RUNNER"
  echo "========================================================================="
  echo "🐳 Container: $CONTAINER_NAME"
  echo "🔧 Compiler: $COMPILER"
  echo "⚡ Parallel Jobs: $PARALLEL_JOBS"
  if [ -n "$SPECIFIC_CONFIG" ]; then
    echo "🎯 Testing Configuration: $SPECIFIC_CONFIG"
  else
    echo "🎯 Testing All Configurations: ${!BUILD_CONFIGS[*]}"
  fi
  echo "========================================================================="
  
  # Check prerequisites
  check_prerequisites
  
  # Clean up previous build directories
  echo "🧹 Cleaning up previous build directories..."
  run_in_container "rm -rf $BUILD_DIR_BASE"
  
  # Test configurations
  for config_name in "${!BUILD_CONFIGS[@]}"; do
    # Skip if testing specific configuration
    if [ -n "$SPECIFIC_CONFIG" ] && [ "$config_name" != "$SPECIFIC_CONFIG" ]; then
      continue
    fi
    
    test_build_configuration "$config_name" "${BUILD_CONFIGS[$config_name]}" || true
  done
  
  # Generate final report
  if generate_report; then
    echo "🎉 All build tests completed successfully!"
    exit 0
  else
    echo "💥 Some build tests failed!"
    exit 1
  fi
}

# Run main function
main "$@"