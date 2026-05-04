# Huawei MatePad 11 2021 (DBY-W09) — Kernel 4.19 Build Fix

> **Status: ✅ BUILD SUCCESSFUL**
> Kernel 4.19.157 for Snapdragon 865 (Kona) — compiled with Clang + ld.lld on Ubuntu

This repository contains the HarmonyOS 3.0 open-source kernel for the **Huawei MatePad 11 2021 (DBY-W09)**, patched to compile successfully with a modern Clang/LLVM toolchain on a contemporary Linux host.

---

## Device Specifications

| Field | Value |
|---|---|
| Device | Huawei MatePad 11 2021 |
| Model | DBY-W09 |
| SoC | Qualcomm Snapdragon 865 (SM8250 / Kona) |
| Kernel | 4.19.157 |
| Architecture | AArch64 (ARM64) |
| Original OS | HarmonyOS 3.0.0 |
| Board variants | DBY_W09_VA, DBY_W09_VB |

---

## Build Instructions

### Prerequisites

```bash
sudo apt install clang lld llvm gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu
```

### Build

```bash
git clone https://github.com/RobertoSiragusa/DBY-W09.git
cd DBY-W09

# Generate .config
yes "" | make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- CC=clang \
  CLANG_TRIPLE=aarch64-linux-gnu- LD=ld.lld HOSTCFLAGS=-fcommon \
  merge_kona_defconfig

# Disable two incompatible options
sed -i 's/CONFIG_HUAWEI_PAGECACHE_HELPER=y/# CONFIG_HUAWEI_PAGECACHE_HELPER is not set/' .config
sed -i 's/CONFIG_IKHEADERS=y/# CONFIG_IKHEADERS is not set/' .config

# Build
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- CC=clang \
  CLANG_TRIPLE=aarch64-linux-gnu- LD=ld.lld AR=llvm-ar NM=llvm-nm \
  OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip \
  HOSTCFLAGS=-fcommon -j$(nproc) Image.gz 2>&1 | tee /tmp/build.log
```

Output: `arch/arm64/boot/Image.gz`

---

## Patches Applied — Problems & Solutions

This section documents every build failure encountered and exactly how it was resolved. The original source tree required extensive patching to compile with modern Clang/LLD.

---

### PATCH 1 — Global `-Werror` suppression for common clang warnings

**Commit:** `dadededff`

**Problem:**
The root `Makefile` passed `-Werror=strict-prototypes` and `-Werror=incompatible-pointer-types` to Clang. Modern Clang (18+) is stricter than older GCC and treats hundreds of vendor driver patterns as errors:
- Functions declared without parameter lists: `void foo()` instead of `void foo(void)`
- Implicit pointer-type conversions between incompatible types
- Unused-but-set variables in vendor code

These caused hundreds of compilation failures across `drivers/devkit/`, `drivers/hwsensor/`, `techpack/audio/`, and many other subsystems.

**Fix:**
Replaced the `-Werror=` flags with `-Wno-` equivalents in the root `Makefile` (`KBUILD_CFLAGS`):

```makefile
# Before
KBUILD_CFLAGS += -Werror=strict-prototypes
KBUILD_CFLAGS += -Werror=incompatible-pointer-types

# After
KBUILD_CFLAGS += $(call cc-option,-Wno-strict-prototypes)
KBUILD_CFLAGS += $(call cc-option,-Wno-incompatible-pointer-types)
KBUILD_CFLAGS += $(call cc-option,-Wno-unused-but-set-variable)
```

---

### PATCH 2 — Individual source file fixes (void prototypes, CRLF, includes)

**Commits:** `3b7b4ba59`, `39ac51834`, `51b5a2ffb`, `a47f9b781`, `c65bdd4a0`

**Problem:**
Several vendor driver files had syntax issues that Clang rejected:
- Missing `void` in function parameter lists (required by C89/C99 standards)
- Windows-style CRLF line endings (`\r\n`) in `.c` files causing parse errors
- Angled includes (`#include <relative/path.h>`) for files that are not in system include paths

Affected files included:
- `drivers/hwsensor/sensors_class.c`, `sensors_sysfs_als.c`, `sensors_sysfs_ps.c`
- `drivers/lbs/airoha_gps_driver.c` (CRLF issue)
- `drivers/nfc/pn547/pn547.c`
- `drivers/input/touchscreen/st/fts_lib/ftsIO.c`, `ftsTime.c`
- 15+ files in `drivers/devkit/tpkit/`

**Fix:**
- Added `void` to all zero-argument function definitions
- Converted CRLF to LF in `airoha_gps_driver.c`
- Converted angled relative includes to quoted includes where appropriate

---

### PATCH 3 — TRACE_INCLUDE_PATH fixes for trace headers

**Commits:** `8f72b3311`, `a2987b84f`

**Problem:**
The Linux kernel trace subsystem uses a macro `TRACE_INCLUDE_PATH` to locate trace header files at compile time. Files using `CREATE_TRACE_POINTS` need `-I$(srctree)` in their compiler flags so the trace framework can find `TRACE_INCLUDE_PATH/TRACE_INCLUDE_FILE.h`. Multiple trace files had incorrect or missing paths:
- `techpack/display/rotator/sde_rotator_trace.h`
- `techpack/camera/drivers/cam_utils/cam_trace.h`
- `fs/hmdfs/hmdfs_trace.h`

**Fix:**
- Set correct absolute `TRACE_INCLUDE_PATH` in each trace header
- Added `-I$(srctree)` via `CFLAGS_<file>.o` only for files that use `CREATE_TRACE_POINTS`, avoiding the flag leaking to files that don't need it

---

### PATCH 4 — techpack Makefile ccflags ordering bug

**Commits:** `75a0e93ef`, `98286e884`

**Problem:**
In kbuild, `ccflags-y` applies to files compiled **in the same directory** as the Makefile. For files in **subdirectories**, you must use `subdir-ccflags-y`. Additionally, `ccflags-y` placed **after** `obj-y` declarations is ignored by kbuild for subdirectory targets.

`techpack/audio/Makefile` had include paths declared as `ccflags-y` after `obj-y += asoc/`, meaning files in `asoc/` (like `kona.c`) never received the `-I$(srctree)/techpack/audio/include` flag — causing them to fail to find `<soc/snd_event.h>`, `<dsp/q6afe-v2.h>`, and similar headers.

**Fix:**
```makefile
# Before (broken)
obj-y += soc/
obj-y += asoc/
ccflags-y += -I$(srctree)/techpack/audio/include

# After (correct)
subdir-ccflags-y += -I$(srctree)/techpack/audio/include
subdir-ccflags-y += -I$(srctree)/techpack/audio/include/uapi
subdir-ccflags-y += -I$(srctree)/drivers/devkit/audiokit
obj-y += soc/
obj-y += asoc/
```

Same fix applied to `drivers/devkit/audiokit/smartpakit/Makefile` and `drivers/devkit/lcdkit/lcdkit3.0/Makefile`.

---

### PATCH 5 — ROOT CAUSE: 109 auto-generated stub headers shadowing real headers

**Commit:** `270e49714`

**Problem — The root cause of most build failures:**

An early attempted fix (commit `70ebef47e`) created **109 empty stub header files** inside `include/` subdirectories:

```
include/comm/device_node.h        ← empty stub
include/authority/authentication.h ← empty stub
include/codecs/bolero/*.h         ← empty stubs
include/dsm_audio/dsm_audio.h    ← empty stub
include/boxid/boxid.h             ← empty stub
... (109 total)
```

Each stub looked like:
```c
/* Auto-generated stub header */
#ifndef _COMM_DEVICE_NODE_H_
#define _COMM_DEVICE_NODE_H_
#endif
```

Because `include/` appears early in the compiler's include search path (via `-I./include`), these empty stubs were found **before** the real headers in:
- `fs/hmdfs/comm/device_node.h` (real hmdfs header)
- `fs/hmdfs/authority/authentication.h` (real auth header with hmdfs_check_cred etc.)
- `drivers/devkit/audiokit/dsm_audio/dsm_audio.h` (real DSM header)

This caused cascading failures where functions like `hmdfs_check_cred()`, `hmdfs_override_creds()`, `CAPABILITY_P2P`, `LINK_TYPE_P2P`, `DSM_SMARTPA_BUF_SIZE`, and `AUDIO_CODEC` appeared as undeclared identifiers — even though their real definitions existed and were correct.

**Fix:**
Deleted all 109 auto-generated stub headers:
```bash
grep -rl "Auto-generated stub" include/ | xargs rm -f
```

Then fixed the hmdfs Makefile include paths to use absolute `$(srctree)` paths so headers are found correctly regardless of compilation directory:
```makefile
ccflags-y += -I$(srctree)/fs/hmdfs -I$(srctree)/fs/hmdfs/comm -I$(src) -Wall
```

And added per-file `CFLAGS_<file>.o` with `-Wno-implicit-function-declaration -Wno-int-conversion` for files in `comm/` and `DFS_1_0/` that depend on the hmdfs include chain.

---

### PATCH 6 — hmdfs authentication.h stubs for non-Android builds

**Commit:** `270e49714`, `c1bdfdffc`

**Problem:**
`fs/hmdfs/authority/authentication.h` defines `hmdfs_check_cred()`, `hmdfs_override_creds()`, `hmdfs_revert_creds()`, `hmdfs_override_fsids()`, and `hmdfs_revert_fsids()` **only** inside `#ifdef CONFIG_HMDFS_ANDROID`. Files that call these functions without the ifdef guard would fail if the config wasn't active.

**Fix:**
Added a `#ifndef CONFIG_HMDFS_ANDROID` block at the end of the header with safe no-op stubs:
```c
#ifndef CONFIG_HMDFS_ANDROID
static inline void hmdfs_check_cred(unsigned int user_id, const struct cred *cred) {}
static inline const struct cred *hmdfs_override_creds(const struct cred *new)
{
    return override_creds(new);
}
static inline void hmdfs_revert_creds(const struct cred *old)
{
    if (old) revert_creds(old);
}
static inline const struct cred *hmdfs_override_fsids(struct hmdfs_sb_info *sbi, bool b)
{
    return NULL;
}
static inline void hmdfs_revert_fsids(const struct cred *old) {}
#endif /* !CONFIG_HMDFS_ANDROID */
```

---

### PATCH 7 — Conflicting static inline stubs removed after real headers restored

**Commit:** `434fa4654`

**Problem:**
After PATCH 5 removed the stub headers, the real `dsm_audio/dsm_audio.h` and `sde/sde_connector.h` became visible again. But intermediate patches had added `static inline` fallback stubs directly into source files:
- `techpack/audio/asoc/kona.c` — stub for `audio_dsm_report_info()`
- `drivers/devkit/audiokit/smartpakit/smartpakit_i2c_ops.c` — stubs for `DSM_SMARTPA_BUF_SIZE`, `audio_dsm_report_info()`, `boxid_read()`
- `drivers/devkit/lcdkit/.../lcd_kit_utils.c` — stubs for `to_sde_connector()`, `_sde_connector_report_panel_dead()`

The real headers declare these as non-static functions. Clang error:
```
error: static declaration of 'audio_dsm_report_info' follows non-static declaration
```

**Fix:**
Removed all conflicting `static inline` stubs from the three source files. The real implementations in `dsm_audio.h` and `sde_connector.h` are now found correctly via the fixed include paths.

---

### PATCH 8 — techpack_autoconf.h missing after git reset

**Commit:** `149044bc4`

**Problem:**
The root `Makefile` passes `techpack_autoconf.h` as a global forced-include:
```makefile
KBUILD_CPPFLAGS := -D__KERNEL__ -include $(srctree)/include/generated/techpack_autoconf.h
```

This file consolidates `CONFIG_` values from `techpack/*/config/*.conf` files (e.g. `konacamera.conf`, `konaauto.conf`, `konadisp.conf`) that set configs like `CONFIG_SPECTRA_CAMERA=y`, `CONFIG_SND_SOC_KONA=y`, etc. These `.conf` values bypass the `.config` system.

The file is generated at build time and is **not tracked by git**. After `git reset --hard`, it was deleted, causing the very first step of the build to fail:
```
<built-in>:3:10: fatal error: './include/generated/techpack_autoconf.h' file not found
```

**Fix:**
- Committed `include/generated/techpack_autoconf.h` directly to the repository
- Added a Makefile rule to regenerate it automatically if missing:
```makefile
$(srctree)/include/generated/techpack_autoconf.h:
    @mkdir -p $(srctree)/include/generated
    @echo "/* Auto-generated from techpack .conf files */" > $@
    # ... reads all .conf files and generates #define statements
```

---

### PATCH 9 — qcacld WiFi driver: `-Warray-parameter` error

**Commit:** `c5807cd43`

**Problem:**
Clang 18 added `-Warray-parameter` as a new warning that catches array parameter type mismatches. The qcacld WiFi driver (`drivers/qcacld/`) had functions like:
```c
void populate_mdie(struct mac_context *mac,
                   tDot11fIEMobilityDomain *pDot11f,
                   uint8_t mdie[SIR_MDIE_SIZE])  // ← mismatched bound
```
With `-Werror` active in the driver's build flags, this became a fatal error.

**Fix:**
Added `-Wno-array-parameter` to global `KBUILD_CFLAGS` in root `Makefile`:
```makefile
KBUILD_CFLAGS += $(call cc-option,-Wno-array-parameter)
```

---

### PATCH 10 — Linker error: `R_AARCH64_ABS32` relocation with ld.lld

**Commit:** `9e052bf7d`

**Problem:**
`ld.lld` rejected the build with:
```
ld.lld: error: relocation R_AARCH64_ABS32 cannot be used against symbol
'__crc_gsi_write_channel_scratch'; recompile with -fPIC
```

`CONFIG_MODVERSIONS=y` causes the kernel to embed CRC checksums for all exported symbols. On AArch64, these CRCs are stored using `R_AARCH64_ABS32` (32-bit absolute) relocations. This is incompatible with `ld.lld`, which requires position-independent addressing for non-relocatable sections.

This is a known upstream issue with `ld.lld` + `CONFIG_MODVERSIONS` on AArch64.

**Fix:**
Disabled `CONFIG_MODVERSIONS` in `arch/arm64/configs/merge_kona_defconfig`:
```
# CONFIG_MODVERSIONS is not set
```
Safe to disable: MODVERSIONS is only needed when loading out-of-tree kernel modules that were compiled against a different kernel. For an integrated build with all drivers in-tree, it provides no benefit.

---

### PATCH 11 — Linker error: `undefined symbol: __read_overflow2`

**Commit:** `9e052bf7d`

**Problem:**
`CONFIG_FORTIFY_SOURCE=y` enables compile-time buffer overflow detection in string functions (`memcpy`, `strcpy`, etc.). When the compiler cannot statically determine that a copy is safe, it emits a call to `__read_overflow2()` which is declared as a `__compiletime_error` — it should never actually appear in the final binary because the optimizer eliminates the dead code branch.

However, some vendor driver code compiled with certain optimization patterns left a reference to `__read_overflow2` in the object file, and the symbol has no definition anywhere in the kernel (by design — it's only meant as a compile-time trap).

```
ld.lld: error: undefined symbol: __read_overflow2
```

**Fix:**
Added weak definitions of all `__read_overflow*` and `__write_overflow` symbols to `lib/string.c`:
```c
void __read_overflow(void) {}
EXPORT_SYMBOL(__read_overflow);
void __read_overflow2(void) {}
EXPORT_SYMBOL(__read_overflow2);
void __read_overflow3(void) {}
EXPORT_SYMBOL(__read_overflow3);
void __write_overflow(void) {}
EXPORT_SYMBOL(__write_overflow);
```
These are no-ops that satisfy the linker. In practice they should never be called at runtime because the fortify checks that trigger them are always paired with bounds checks that would panic the kernel first.

---

### PATCH 12 — Linker error: `undefined symbol: get_bootdevice_type`

**Commits:** `9e052bf7d`, `49512ac64`

**Problem:**
Multiple drivers (`drivers/scsi/ufs/ufshcd_mas_extend.c`, `drivers/mmc/core/mmc.c`, `drivers/mmc/host/sdhci-msm.c`) call `get_bootdevice_type()` to detect whether the boot device is UFS or eMMC.

The function exists in two parallel directories:
- `drivers/bootdevice/proc_boot.c` — legacy version, defines `get_bootdevice_type()`
- `drivers/bootdevices/proc_boot.c` — extended UFS/MMC version, calls but does not define `get_bootdevice_type()`

`drivers/bootdevices/` is compiled when `CONFIG_HUAWEI_QCOM_UFS=y` (which is set). `drivers/bootdevice/` was not referenced anywhere in `drivers/Makefile`. The function therefore had no definition at link time.

An initial fix incorrectly added `obj-y += bootdevice/` to `drivers/Makefile`, which caused the opposite problem — both directories were compiled and all `set_bootdevice_*` functions appeared as duplicate symbols:
```
ld.lld: error: duplicate symbol: set_bootdevice_type
ld.lld: error: duplicate symbol: set_bootdevice_name
... (13 duplicate symbols total)
```

**Fix:**
Kept only `bootdevices/` in the build (the correct directory for UFS), and added the missing `get_bootdevice_type()` definition directly into `drivers/bootdevices/proc_boot.c`:
```c
enum bootdevice_type get_bootdevice_type(void)
{
    return bootdevice.type;
}
EXPORT_SYMBOL(get_bootdevice_type);
```

---

## Summary Table

| # | Commit | Problem | Fix |
|---|--------|---------|-----|
| 1 | `dadededff` | `-Werror=strict-prototypes` rejects hundreds of vendor files | Replace with `-Wno-strict-prototypes` globally |
| 2 | `51b5a2ffb` | Missing `void`, CRLF, bad includes in vendor drivers | Fix prototypes, line endings, quoted includes |
| 3 | `8f72b3311` | Wrong `TRACE_INCLUDE_PATH` in trace headers | Fix paths, add `-I$(srctree)` only where needed |
| 4 | `75a0e93ef` | `ccflags-y` after `obj-y` ignored for subdirs | Change to `subdir-ccflags-y`, move before `obj-y` |
| 5 | `270e49714` | **ROOT CAUSE**: 109 empty stub headers shadow real headers | Delete all stubs from `include/`, fix hmdfs paths |
| 6 | `270e49714` | hmdfs functions missing outside `CONFIG_HMDFS_ANDROID` | Add `#ifndef` stubs in `authentication.h` |
| 7 | `434fa4654` | Static inline stubs conflict with non-static real declarations | Remove stubs now that real headers are visible |
| 8 | `149044bc4` | `techpack_autoconf.h` missing after `git reset --hard` | Commit file to repo + add generation rule |
| 9 | `c5807cd43` | qcacld: `-Warray-parameter` treated as error | Add `-Wno-array-parameter` to `KBUILD_CFLAGS` |
| 10 | `9e052bf7d` | `MODVERSIONS` + `ld.lld` = `R_AARCH64_ABS32` relocation error | Disable `CONFIG_MODVERSIONS` in defconfig |
| 11 | `9e052bf7d` | `__read_overflow2` undefined at link time | Add weak no-op definitions in `lib/string.c` |
| 12 | `49512ac64` | `get_bootdevice_type` undefined; then duplicate symbols | Add definition to `bootdevices/proc_boot.c` only |

---

## Notes

- `CONFIG_HUAWEI_PAGECACHE_HELPER` and `CONFIG_IKHEADERS` must be disabled post-defconfig via `sed` as shown in the build instructions — they cause compilation failures with modern toolchains.
- The `.conf` files in `techpack/*/config/kona*.conf` force certain `CONFIG_` values regardless of `.config`. This is why `techpack_autoconf.h` must exist before compilation starts.
- `CONFIG_HMDFS_ANDROID=y` is required and already set in `merge_kona_defconfig`. The hmdfs filesystem uses Android credential management that must be enabled.
