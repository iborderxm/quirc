---
name: quirc-build-verify
description: Remote Linux x86_64 build and smoke-test for the quirc QR library. Use after C source, header, or Makefile edits to scp changes, build with zero warnings, and run quirc-scan / qrtest smoke checks. Do not use for editing code.
---

# quirc 远端构建与验证

quirc 只在 Linux 构建（本机 Windows 不构建）。所有编译、测试都在远端机进行，
本地 Windows 只负责改代码和 `scp` 同步。

## 固定环境（不要重新探测，除非用户说明已变更）

- 远端机：`iborder@192.168.201.130`，ssh/scp 免密，shell 为 bash
- 远端仓库：`~/quirc`（与本地 `d:\git\quirc` 保持同名文件镜像，**没有 git 自动同步**）
- 构建方式：纯 Makefile，在 `~/quirc` 下原地构建（无独立 build 目录、无 cmake、无 ctest）
- 编译要求：`-Wall` **零告警零错误**（Makefile 默认 `CFLAGS = -O3 -Wall -fPIC`）

## 项目结构与构建产物

- `lib/*.c` → `libquirc.a`、`libquirc.so.1.2`（核心库）
- `tests/qrtest.c` → `qrtest`（测试/调试工具，依赖 libjpeg、libpng；缺依赖时脚本自动跳过）
- `qr-scan/scanner.c` → `quirc-scan`（命令行扫描工具，仅依赖 libm）
- `demo/` → 可选目标：`quirc-scanner`（v4l）、`inspect`/`quirc-demo`（sdl）、opencv 版 demo

## 标准流程

### 1. 本地改完代码后，逐文件 scp 同步

只传本次改动的文件，保持相对路径一致：

```powershell
scp d:\git\quirc\lib\identify.c iborder@192.168.201.130:~/quirc/lib/identify.c
scp d:\git\quirc\qr-scan\scanner.c iborder@192.168.201.130:~/quirc/qr-scan/scanner.c
scp d:\git\quirc\tests\qrtest.c iborder@192.168.201.130:~/quirc/tests/qrtest.c
scp d:\git\quirc\Makefile iborder@192.168.201.130:~/quirc/Makefile
```

常见同步面：`lib/*.{c,h}`、`qr-scan/*.c`、`tests/*.{c,h,cxx}`、`demo/*.{c,h,cxx}`、
`Makefile`、`build_linux_x86_64.sh`。冒烟用的测试图（`qr-code.jpg`、`qr-code1.jpg`）
若远端缺失也需一并同步。

### 2. 一键远端验证（首选）

先同步脚本，再用单引号整体包裹远端命令执行：

```powershell
scp d:\git\quirc\build_linux_x86_64.sh iborder@192.168.201.130:~/quirc/build_linux_x86_64.sh
ssh iborder@192.168.201.130 'bash ~/quirc/build_linux_x86_64.sh'
```

### 3. 验收门禁

| 检查 | x86_64 |
| --- | --- |
| 编译零告警 | 必须 |
| 默认目标全部产出 | `libquirc.a`、`libquirc.so.1.2`、`qrtest`、`quirc-scan` |
| 冒烟 | `./quirc-scan qr-code.jpg` 能正确解出二维码内容 |

注意：远端缺 libjpeg/libpng 时脚本会跳过 `qrtest`，此时产物只有
`libquirc.a`、`libquirc.so.1.2`、`quirc-scan`，属于预期行为而非构建失败。

### 4. 需要手工排查时的命令

```bash
# 清理 + 全量构建
cd ~/quirc
bash build_linux_x86_64.sh

# 冒烟：扫描仓库根目录自带测试图
./quirc-scan qr-code.jpg

# 调试单张图片的识别流程（依赖 libjpeg/libpng）
./qrtest qr-code.jpg
```

## Windows / PowerShell 陷阱（都实际踩过）

- 远端命令一律用**单引号**整体包裹：`ssh host 'cd ~/quirc && ...'`。双引号会让 `$?`、
  `$(nproc)`、`$HOME` 被本地 PowerShell 展开；裸写的 `|`、`&&` 可能被本地拆词
  （曾出现 `grep -E "A|B"` 里的 `FAIL: command not found`）。
- 复杂逻辑不要塞在 ssh 一行里：落成仓库脚本（如 `build_linux_x86_64.sh`），scp 上去执行。
- 不要执行 `.ps1`（本机执行策略禁止），不要改执行策略。
- 本地终端是 PowerShell 5：没有 `&&`/`||`，但这些语法只出现在远端单引号字符串内，不受影响。

## 完成后汇报内容

构建告警数、构建产物清单、冒烟识别结果，以及修复过程中发现的问题。
失败时给出远端完整诊断输出，不要凭猜测宣布成功。