#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
DEMO_BIN="${BUILD_DIR}/memory_pool_demo"

if [[ ! -x "${DEMO_BIN}" ]]; then
    echo "程序未编译或不可执行：${DEMO_BIN}"
    echo "请先运行：./build.sh 或 make build"
    exit 1
fi

if [[ "${1:-}" == "test" ]]; then
    mkdir -p log
    echo "自动压测启动，日志将写入：log/test_log.log"
fi

exec "./${DEMO_BIN}" "$@"
