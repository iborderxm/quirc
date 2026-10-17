#!/usr/bin/env bash
#==============================================================================
# quirc -- Linux x86_64 编译脚本
#
# 用途：
#   在 Linux x86_64 平台上编译 quirc 二维码识别库及相关工具。
#
# 默认构建目标：
#   1. libquirc.a       静态库
#   2. libquirc.so.1.2  动态库（含 libquirc.so 软链接）
#   3. qrtest           测试/调试工具（依赖 libjpeg、libpng，缺依赖时自动跳过）
#   4. quirc-scan       命令行二维码扫描工具（仅依赖 libm）
#
# 用法：
#   chmod +x build_linux_x86_64.sh
#   ./build_linux_x86_64.sh              # 构建默认目标
#   ./build_linux_x86_64.sh <make目标...> # 构建 Makefile 中的其他目标，例如：
#     ./build_linux_x86_64.sh v4l        # quirc-scanner（需 V4L2 + libjpeg）
#     ./build_linux_x86_64.sh sdl        # inspect、quirc-demo（需 SDL + SDL_gfx）
#     ./build_linux_x86_64.sh opencv     # OpenCV 版本 demo（需 OpenCV4）
#
# 可选环境变量：
#   CC      编译器（默认 gcc；交叉编译时设为如 x86_64-linux-gnu-gcc）
#   CFLAGS  编译参数（Makefile 默认 -O3 -Wall -fPIC）
#   JOBS    并行编译任务数（默认取 nproc）
#==============================================================================

set -euo pipefail

# 切换到脚本所在目录（即项目根目录），保证在任意目录下调用脚本均可工作
cd "$(dirname "$0")"

#------------------------------------------------------------------------------
# 1. 平台检查：本脚本仅面向 Linux x86_64
#------------------------------------------------------------------------------
OS_NAME="$(uname -s)"
ARCH="$(uname -m)"

if [ "$OS_NAME" != "Linux" ]; then
    echo "错误：本脚本仅适用于 Linux，当前系统为 $OS_NAME" >&2
    exit 1
fi

if [ "$ARCH" != "x86_64" ]; then
    echo "警告：目标架构为 x86_64，但当前机器架构为 $ARCH" >&2
    echo "      如需交叉编译，请设置 CC 为对应的交叉编译器后重试。" >&2
fi

#------------------------------------------------------------------------------
# 2. 工具链检查：需要 C 编译器与 make
#------------------------------------------------------------------------------
CC="${CC:-gcc}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 1)}"

for tool in "$CC" make; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "错误：未找到编译工具 '$tool'" >&2
        echo "      Debian/Ubuntu: sudo apt install build-essential" >&2
        echo "      Fedora/RHEL  : sudo dnf groupinstall 'Development Tools'" >&2
        exit 1
    fi
done

echo "==> 平台   : $OS_NAME $ARCH"
echo "==> 编译器 : $CC"
echo "==> 并行数 : $JOBS"

#------------------------------------------------------------------------------
# 3. 依赖库检查：qrtest 需要 libjpeg / libpng 的开发文件
#    通过实际编译一小段测试代码来判断依赖是否可用
#------------------------------------------------------------------------------
can_compile() {
    # $1: C 测试源码；编译通过则返回 0
    printf '%s\n' "$1" | "$CC" -x c - -o /dev/null -c >/dev/null 2>&1
}

MISSING=""
can_compile '#include <stdio.h>
#include <stddef.h>
#include <jpeglib.h>
int main(void) { return 0; }' || MISSING="$MISSING libjpeg"
can_compile '#include <png.h>
int main(void) { return 0; }' || MISSING="$MISSING libpng"

if [ -n "$MISSING" ]; then
    echo "警告：缺少开发库:$MISSING" >&2
    echo "      Debian/Ubuntu: sudo apt install libjpeg-dev libpng-dev" >&2
    echo "      Fedora/RHEL  : sudo dnf install libjpeg-turbo-devel libpng-devel" >&2
fi

#------------------------------------------------------------------------------
# 4. 清理旧构建产物
#    注意：Makefile 的 clean 目标使用 bash 花括号展开（libquirc.{so,so.1.2}），
#    在 /bin/sh 为 dash 的系统（Debian/Ubuntu 默认）上不生效，残留的
#    libquirc.so 会导致后续 ln -s 失败，因此这里显式删除库文件兜底。
#------------------------------------------------------------------------------
echo "==> 清理旧构建产物..."
make clean >/dev/null 2>&1 || true
rm -f libquirc.so libquirc.so.1.2 libquirc.a

#------------------------------------------------------------------------------
# 5. 编译
#    - 指定了参数则透传给 make 作为构建目标
#    - 未指定参数则构建默认目标；缺少 libjpeg/libpng 时自动跳过 qrtest
#------------------------------------------------------------------------------
if [ "$#" -gt 0 ]; then
    TARGETS="$*"
elif [ -n "$MISSING" ]; then
    TARGETS="libquirc.a libquirc.so quirc-scan"
    echo "==> 因缺少依赖，跳过 qrtest，仅构建核心库与 quirc-scan"
else
    TARGETS="all quirc-scan"
fi

echo "==> 构建目标: $TARGETS"
make -j"$JOBS" $TARGETS

#------------------------------------------------------------------------------
# 6. 汇总构建产物
#------------------------------------------------------------------------------
echo
echo "==> 构建完成，产物列表："
for f in libquirc.a libquirc.so.1.2 qrtest quirc-scan quirc-scanner quirc-demo inspect; do
    if [ -f "$f" ]; then
        printf '    %-20s %s\n' "$f" "$(du -h "$f" | cut -f1)"
    fi
done
# libquirc.so 是指向带版本号动态库的软链接，单独展示其指向
if [ -L libquirc.so ]; then
    printf '    %-20s -> %s\n' libquirc.so "$(readlink libquirc.so)"
fi
