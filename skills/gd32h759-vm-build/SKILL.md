---
name: gd32h759-vm-build
description: "Drives the GD32H759 openvela contest build loop: connect to the Ubuntu build VM, upload sources, run make -j4, and pull the firmware back to Windows. Use when asked to 连VM / 上VM编译 / 编译固件 / 出新的nuttx.bin / 传文件到VM / 下载固件 / build the GD32 firmware / sync sources to the VM, or whenever GD32H759 firmware has to be produced or refreshed."
when_to_use: "连VM, 连接VM, 上VM, VM上编译, 编译固件, 编译新固件, 出新固件, 烧录固件, nuttx.bin, 传文件到VM, 上传到VM, 下载固件, 同步源码, build firmware, build nuttx, cross-compile, arm-none-eabi, GD32H759"
---

# GD32H759 openvela: build on the VM, flash from Windows

Firmware is **built on an Ubuntu VM** and **flashed from Windows**. Nothing in
this loop runs on the Windows side except ssh/scp and the GUI flasher. The local
`C:\Users\QQQ\Desktop\openvela` checkout is *not* where the firmware is built.

Use `scripts/vm.sh` for every VM interaction. It encodes the ssh options and
the two scp habits that are easy to get wrong; see Gotchas for why.

## The loop

### 1. Connect

```bash
"$HOME/.claude/skills/gd32h759-vm-build/scripts/vm.sh" shell
```

Key auth only. The key is `~/.ssh/openvela_vm`; its public half is already
installed on the VM. **Password auth cannot work from a tool-driven shell** —
`ssh` prompts on a TTY and there is none — so if the key is missing, stop and
have the user install it rather than trying to script a password.

Confirm you are on the right machine before touching anything:

```bash
vm.sh run 'hostname; ls -d /home/topeet/Desktop/openvela/{nuttx,apps,vendor}'
```

### 2. Upload

```bash
vm.sh put  <local-file> <remote-path>     # one file per call
vm.sh script <local.sh> [args]            # upload + run, for multi-line work
```

### 3. Build

```bash
vm.sh script scripts/build_fw.sh
```

That script sets `PATH` to the bundled `arm-none-eabi` toolchain, runs
`make -j4` in `nuttx/`, and reports size and md5. Equivalent by hand:

```bash
export PATH=/home/topeet/Desktop/openvela/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH:/usr/bin:/bin
cd /home/topeet/Desktop/openvela/nuttx && make -j4
```

A no-op build is **~4 seconds**. If make does real work, something changed —
check `git status` in the delivery repo and the source mtimes before believing
a "new" firmware is actually new.

Product: `/home/topeet/Desktop/openvela/nuttx/nuttx.bin` (and `nuttx.hex`).

### 4. Download

```bash
vm.sh get /home/topeet/Desktop/openvela/nuttx/nuttx.bin ./nuttx.bin
```

Then verify the transfer and the link address:

```bash
ls -l nuttx.bin
xxd -l 16 nuttx.bin      # SP must be 0x2400xxxx, reset vector 0x0800xxxx
```

Expected vector table for this board (little-endian): initial SP in
`0x24000000`+ (SRAM), reset vector in `0x08000000`+ (flash).

### 5. Flash (user does this — see below)

## Where the sources actually live

The build tree is the source of truth. The delivery repo's `board/` copies are
**archives that drift silently** — never trust them, refresh whole files.

| Delivery repo path | Real source on the VM |
|---|---|
| `board/.../gd32h7xx_tli.c` / `.h` | `nuttx/arch/arm/src/gd32h7xx/` |
| `board/.../gd32h7xx_sdram.c` | `nuttx/arch/arm/src/gd32h7xx/` |
| `board/.../gd32h7xx_gpio.h` | `nuttx/arch/arm/src/gd32h7xx/` |
| `board/contest_board/src/gd32h7xx_bringup.c` | `vendor/gigadevice/boards/gd32h7/gd32h759imt6/src/` |
| `board/contest_board/src/gd32h7xx_gt911.c` | same |
| `app/gui/` | `apps/examples/gui/` |

Project paths on the VM:

```
/home/topeet/Desktop/openvela/
  nuttx/                                       kernel sources (NOT a git repo)
  apps/  vendor/  prebuilts/                   components
  contest2026_471_sibakenailongshiyanshi/      delivery repo (git)
```

## Flashing

The flasher (`GD32AllInOneProgrammer.exe`) is **GUI-only — no CLI**. Verified by
reading its manual: it documents three tabs (Single Serial Port, SuperBatch,
CMDTest) and no command-line options; the `CMDTest` tab is a bootloader-command
debug panel, not a scriptable interface. So **an agent cannot flash**; prepare
everything and hand it to the user to click.

Known-good settings from `ConfigInfo.ini` next to the exe:

| Setting | Value |
|---|---|
| Interface | COM / UART bootloader |
| Port | COM12 @ 57600, 8E1 |
| Part | GD32H759IMT6 |
| Start address | `0x08000000` (matches `ld.script` flash ORIGIN) |
| Verify | **off** — turn it on, or check the boot log over serial |

Hardware prerequisite: the board must be in **BOOT mode (`BOOT0=1, BOOT1=0`)**
or the ISP handshake fails.

## Gotchas

- **scp takes one file at a time.** A space-separated list inside one quoted
  string is treated as a single filename and fails with "No such file". Loop.
- **Never pass a multi-line command through `ssh host '...'`.** Local bash →
  ssh → remote bash mangles it, silently truncating or eating quotes and
  apostrophes. Write a script to a file, `scp` it, run it — that is what
  `vm.sh script` does.
- **md5 is not a firmware identity here.** `nuttx/libs/libc/misc/lib_utsname.c`
  embeds `__DATE__ " " __TIME__` into `g_version`, so every build differs.
  Compare with `cmp` and expect only the timestamp bytes to differ — a rebuild
  of identical sources differs by ~3 bytes, all inside the version string.
- **`.repo/` is missing on the VM workspace**, which disables the contest log
  collector's privacy gate. Unrelated to building; mentioned so it is not
  mistaken for a broken toolchain.
- Builds need the bundled toolchain on `PATH` *first*; the system may have an
  older `arm-none-eabi-gcc` that fails in confusing ways.

## Done criteria

Report all of these, not just "it built":

1. `make -j4` exit code, and the error count
2. `nuttx.bin` size and md5
3. Whether the build was incremental (fast) or did real work
4. The `cmp`-based comparison against the previously shipped firmware if one exists
