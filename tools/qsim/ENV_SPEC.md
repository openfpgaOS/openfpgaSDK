# qsim ENV_SPEC — the openfpgaOS (os25) environment qsim provides

This is the specification of the emulated environment `qsim` puts around the
CPU model: the ELF loader and boot contract, the ECALL interface, the OS
services table, the capability descriptor, the MMIO map, the data-slot file
system, the virtual clock, and where the CPU (rather than the GPU) writes
pixels. It is what an openfpgaOS app ELF sees when it boots under qsim, and
it doubles as a precise description of what the real os25 kernel + bitstream
give an app on an Analogue Pocket.

**Scope note.** Everything here is app-agnostic — it is the *platform*
contract. It was written and verified against one app, the openfpgaOS Quake
port (`src/quake/engine`, baseline ELF sha256 `0d7bd13e…b546`), because a
real, complete app is the only honest way to pin down which parts of the
kernel an app actually depends on. So the worked examples, the §7 workload
recipe and the §8 CPU-written-pixel list are Quake's. Read them as "here is
one app exercising this contract", not as "the contract is about Quake".
Another app that boots under qsim uses the same loader, the same 106-entry
service table, the same ecalls and the same GPU MMIO; what it supplies of its
own is an **app profile** (`app.h`, `tools/qsim/apps/<app>/<app>.app`): its
data slots, its `.ini`, its `argv[0]`, its frame-counter/z-buffer symbols and
its console conventions. See README.md, "Onboarding a new app".

Every fact is tagged:

* **[ELF]** verified by disassembling the baseline app ELF
* **[SRC]** read from source (path:line given where it matters)
* **[DER]** derived arithmetic from [SRC]/[ELF] facts, not observed on hardware
* **[CHOICE]** an emulator design decision (device behaviour is timing-dependent
  or unobservable); pick it once and keep it fixed for every A/B run

Source trees used (read-only) when this spec was derived:

| what | path |
|---|---|
| app source | the Quake port's `src/quake/engine` (`main.c sys_of.c vid_of.c snd_of.c cd_of.c in_of.c`) |
| SDK headers the ELF was built with | that checkout's `src/sdk/include` (working tree; `of_gpu.h` was locally modified, and that is what the ELF contains) |
| musl | prebuilt `src/sdk/musl/lib/{crt1.o,libc.a}` |
| kernel | openfpgaOS `src/firmware/os` |
| RTL | openfpgaOS `src/fpga/common/{gpu_core.v,axi_periph_slave.v}`, `targets/pocket/{core_top.v,variants/os25.mk}` |

Shipped runtime = the port's `runtime/pocket/os.bin` + `os25.rbf_r` (MANIFEST:
`openfpgaos-mister-v0.9.5-dirty`). That `os.bin` is not byte-identical to a
local rebuild, but every string in it matches, so the feature set and messages
are the same. `gpu_core.v` was modified *after* `os25.rbf_r` was built
(07:57 Sep 15 vs 19:18 Sep 14), so RTL register details below come from source
that may be slightly newer than the bitstream.

---------------------------------------------------------------------------

## 1. Load / boot contract

### 1.1 The ELF [ELF]

```
ELF32 RISC-V EXEC, flags 0x3 (RVC, single-float ABI), entry 0x10400000
PT_LOAD[0]  off 0x001000  vaddr 0x10400000  filesz 0x84848  memsz 0x84848  RWE
            sections: .text(+.rodata, linked into .text) .eh_frame .init_array .data .fastdata
PT_LOAD[1]  off 0         vaddr 0x10488000  filesz 0        memsz 0x618484 RW   (.bss)
GNU_STACK
```

* **No APP_BRAM segment.** `.app_fasttext/.app_fastdata` are empty, so nothing
  is copied to 0x4000–0x7800. `.fastdata` (0x10481c44, 0x2c04 bytes; holds e.g.
  `paintedtime`) is an orphan section that the linker placed in SDRAM inside
  PT_LOAD[0]. (Later ELFs could add BRAM PT_LOADs; §1.2 describes how the
  loader handles them.)
* Gap 0x10484848–0x10487fff belongs to no segment. The loader does not touch it.
  Zero it.
* `__bss_start`=0x10488000, `__bss_end`=0x10aa0484, `__global_pointer$`=0x104817d8.
* `.rodata` is linked into `.text`, so objdump decodes data as code in places
  (e.g. `.insn 2, 0xb89c` after `__fixunstfsi`). Decode lazily, only what
  actually executes.

### 1.2 Kernel loader behaviour (`kernel/loader.c`, `kernel/main.c`) [SRC]

1. The app ELF is read from a data slot (`[os] ELF=quake.elf` resolves to slot 3).
   ET_EXEC at a nonzero base means `load_base = 0`: vaddrs are absolute.
2. Each PT_LOAD must satisfy the app vmap v1: `[0x4000,0x7800)` or
   `[0x10400000, 0x13A00000)` (`OF_TARGET_APP_STATIC_END`). Otherwise error -7.
3. BRAM-range PT_LOAD: file bytes are copied to the vaddr and `memsz>filesz` is
   zeroed. The kernel does this; there is no LMA→VMA copy in crt code.
4. SDRAM PT_LOAD: file bytes are read through the uncached alias
   (`vaddr + 0x40000000`), `memsz-filesz` is zeroed, then the cached lines are
   invalidated. Functionally this is "copy + zero".
5. There is no `PT_DYNAMIC`, so no relocations.
6. `result.bss_end = (max(vaddr+memsz) + 15) & ~15` = **0x10AA0490** [DER].
7. `syscall_init(bss_end)` sets `brk_base = current_brk = 0x10AA0490` and
   `mmap_bottom = APP_STACK_TOP - APP_STACK_SIZE`.
8. `of_mixer_memory_set_floor(align_up(bss_end + 0x80000, 0x1000))`,
   `filesystem_init()` (slot probe, §6), then `bank_preload()`. For Quake, no
   slot is named `*.ofsf` and slot 7 is empty, so no SoundFont memory is
   reserved [DER].
9. `app.stack_top = of_mixer_app_memory_top()` = the lowest audio reservation
   = `align_down(0x13B00000 - 0x80000, 0x1000)` = **0x13A80000** [DER]
   (only the 512 KB audio stream ring is reserved).
   `syscall_set_app_stack_top(0x13A80000)` sets `mmap_bottom` =
   **0x13A00000** and clears the mmap slot table.
10. `caps_table_init(bss_end)` fills the caps (§4). `services_table_init()`
    runs before that.
11. `elf_exec` builds the SysV stack (§1.4) and runs
    `switch_to_runtime_stack_and_call(entry, sp)`, which is `mv sp,a1; jr a0`.

### 1.3 Physical/virtual memory map the emulator must provide [SRC]

All of these are CPU addresses. There is no MMU, and the app runs in M-mode.

| range | size | what | app access |
|---|---|---|---|
| 0x00000000–0x00007FFF | 32 KB | BRAM: kernel boot, trap frame, OS fasttext; app window 0x4000–0x77FF | none for this ELF. A NULL+small deref lands here; flag it |
| 0x10000000–0x13FFFFFF | 64 MB | SDRAM, **cached alias** (PMA main=1, exe=1) | yes |
| 0x20000000–0x2FFFFFFF | | APF bridge space (main=0, exe=0) | no |
| 0x30000000–0x30FFFFFF | 16 MB | CRAM0 (uncached, non-exec) | only the pointer returned by `FILE/DMA_STAGE_ALLOC` (0x30500000); with no CD tracks it is never dereferenced |
| 0x40000000 | | sysreg (kernel only) | no |
| 0x48000000 | | HW audio mixer (kernel only) | no |
| **0x4A000000–0x4A00003F** | | **GPU MMIO** (`caps->gpu_base`) | **yes**, the only MMIO the app touches (§5) |
| 0x4C000000, 0x4D000000, 0x4E000000, 0x4F000000 | | audio FIFO, link, CRAM0_MODE, UART (kernel only) | no |
| **0x50000000–0x53FFFFFF** | 64 MB | **SDRAM uncached alias**: same physical bytes as 0x10000000+ | **yes, heavily** (FB and z-buffer writes, colormap upload, uncached `volatile` accesses) |

**The 0x5xxxxxxx alias must map onto the same host array as 0x1xxxxxxx.**
If it goes to the MMIO callback, VID_Init and every pixel write break.
For a cache model, 0x5xxxxxxx accesses bypass the D-cache (p_axi).

SDRAM sub-layout seen by the app [SRC target_platform.h, DER]:

| CPU addr (cached) | content |
|---|---|
| 0x10000000, 0x10100000, 0x10200000 | FB0/FB1/FB2 (1 MB slots; Quake uses 320×200 stride 320) |
| 0x10300000 | terminal FB (kernel) |
| 0x10320000–~0x1038E280 | kernel .osdata/.bss (local-build `firmware.elf`: `g_svc`@0x10343d14, `g_caps`@0x10343ec8, `app_elf_name`@0x10343cd4, `app_args_buf`@0x1035fab0) |
| 0x103FE000 | interact vars (unused) |
| 0x10400000–0x10AA0484 | app image + .bss |
| 0x10AA0490 → up | brk heap (musl mallocng meta) |
| 0x13A00000 → down | mmap arena (musl large allocs: Quake 35 MB heap, 5 MB surface cache, …) |
| 0x13A00000–0x13A7FFFF | app stack (512 KB, grows down from 0x13A80000) |
| 0x13A80000–0x13AFFFFF | audio stream ring (kernel) |
| 0x13B00000–0x13EFFFFF | kernel file read cache (4 MB) |
| 0x13F00000–0x13F7FFFF | kernel runtime stack |
| 0x13FC0000–0x13FFFFFF | legacy fixed GPU palookup (not used: the SDK programs its own) |

### 1.4 Initial stack, auxv and registers [SRC loader.c `elf_exec`, DER]

`T = 0x13A80000`. Words are pushed downward:

```
T-0x04  0            AT_NULL value
T-0x08  0            AT_NULL (0)
T-0x0C  svc_ptr      AT_OF_SVC value
T-0x10  0xC0DE0002   AT_OF_SVC
T-0x14  caps_ptr     AT_OF_CAPS value
T-0x18  0xC0DE0001   AT_OF_CAPS
T-0x1C  0            AT_RANDOM value (NULL is allowed)
T-0x20  25           AT_RANDOM
T-0x24  0            AT_HWCAP value
T-0x28  16           AT_HWCAP
T-0x2C  4096         AT_PAGESZ value   <- mallocng needs this
T-0x30  6            AT_PAGESZ
T-0x34  0            envp terminator (no envp)
T-0x38  0            argv terminator
T-0x3C-4(argc-1) … argv[argc-1] … argv[0]   (argv[0] is at 0x13A7FFC8 - 4*argc)
sp      argc                                  sp = 0x13A7FFC4 - 4*argc
```

* argv strings live in kernel .bss on the device (`app_elf_name`,
  `app_args_buf`). Put them in any readable RAM that does not overlap the app.
  0x10343cd4/0x1035fab0 are realistic; above T also works. Quake only reads
  them (it stores `com_argv = argv`).
* `svc_ptr`/`caps_ptr` also point into kernel .bss. Place the 436-byte services
  table and the 100-byte caps block in the 0x10320000–0x103FFFFF hole
  [CHOICE; suggested 0x10343d14 / 0x10343ec8].
* Registers at entry: `pc=0x10400000`, `sp` as above, `a0=entry`, `a1=sp`.
  All others are don't-care (`_start` sets `gp`; `__init_tp` sets `tp`).
  Use 0. `frm=0` (round-to-nearest), `fflags=0`. M-mode, MIE=1 on the device.
  The emulator does not need interrupts (§7.6).
* `_start` [ELF]: `auipc/addi gp → 0x104817d8; mv a0,sp; li a1,0; andi sp,sp,-16; j _start_c`.
  `_start_c` then calls `__libc_start_main(main, argc, argv, _init, …)`.

### 1.5 Init chain, and how `_of_svc_ptr`/`_of_caps_ptr` are set

`__libc_start_main` → `__init_libc(envp, argv[0])` parses auxv into
`libc.auxv` (tags ≥ AUX_CNT are skipped there but remain in the vector) →
`__init_tls` (no PT_TLS, builtin TLS, **no mmap**) → `__init_tp`
(`set_tid_address`, syscall 96) → `__init_ssp(NULL)` → secure-mode check
(aux UID/GID are all 0, so no `ppoll`/`open("/dev/null")`) →
`__libc_start_init` → `_init` → `.init_array` (1 entry, `of_runtime_init`,
constructor priority 101) → `main`.

`of_runtime_init` (`sdk/of_init.c`) [SRC+ELF 0x1045f800]:

```
_of_caps_ptr = getauxval(0xC0DE0001);   // stored at 0x10481204 (gp-0x5d4)
_of_svc_ptr  = getauxval(0xC0DE0002);   // stored at 0x104813c4 (gp-0x414)
setbuf(stdout, NULL);                    // stdout unbuffered: every printf is a writev(1,…)
```

**Mechanism: auxv only.** There is no fixed address, no ecall, and the loader
does not write into app memory.

---------------------------------------------------------------------------

## 2. ECALL interface

### 2.1 Trap ABI (`targets/pocket/boot/start.S` `_ecall_handler`) [SRC]

* `ecall` from M-mode (mcause 11). The kernel saves all 32 x-regs and 32
  f-regs, runs `syscall_dispatch(a0..a5, fid=a6, eid=a7)` on its own stack,
  then restores **every register** except:
  * **Linux path** (`a7 < 0xC0DE0000`): `a0` = return value (negative errno on
    failure). **`a1` is restored to its pre-ecall value** (musl relies on this).
  * **Vendor path** (`a7 ≥ 0xC0DE0000`): `a0 = sbiret.error`,
    `a1 = sbiret.value`. Convention: when the handler result `v < 0`, the
    kernel returns `{error=v, value=0}`; otherwise `{0, v}`.
    **Exception:** `TIMER/GET_US` and `TIMER/GET_MS` always return `{0, raw}`,
    even when bit 31 is set.
* `mepc += 4`. All FP registers are preserved.

### 2.2 Complete set of ecall sites in the ELF [ELF `specwork/ecall_cp.py`]

Linux-compat (a7 = number):

| a7 | name (kernel `SYS_*`) | musl caller(s) in ELF | reached in timedemo? | kernel semantics / emulation |
|---|---|---|---|---|
| 25 | fcntl | `open`, `fopen`, `__fdopen` | yes | **return -38 (ENOSYS)**; musl ignores the result |
| 29 | ioctl | `__stdout_write`, `__fdopen` | yes | return 0 (the "tty" answer; stdout is unbuffered anyway) |
| 35 | unlinkat | `unlink` (from `Con_Init` only with `-condebug`) | no | -38 |
| 56 | openat | `open` (opendir), `fopen`, `__init_libc`(unreached) | **yes** | §6.3. `a0=dirfd` (ignored), `a1=path`, `a2=flags`, `a3=mode`. Returns fd ≥ 3 or -errno |
| 57 | close | `close`, `__stdio_close`, `fopen` error path | yes | fd<3 → -9; clear; NV files publish their size (§6.4) |
| 61 | getdents64 | `readdir` | yes (CD track lookup, §6.6) | §6.6 |
| 62 | _llseek | `__lseek` | yes | `a0=fd, a1=off_hi, a2=off_lo, a3=&result(int64), a4=whence(0/1/2)`; writes the 64-bit result; returns 0; -22 if out of [0, 2^32) |
| 63 | read | `__stdio_read` | yes | §6.3 |
| 65 | readv | `__stdio_read` | **yes** (hot during loads) | loop over iov: `sys_read`; stop on rc<0 (return total or rc), rc==0, or a short read |
| 66 | writev | `__stdio_write` | **yes** (all stdout) | fd 1/2 → terminal, returns count; NV fd → §6.4; else -13 |
| 93 | exit | `_Exit`, `Host_Quit_f` (inline `of_exit`) | no | device halts (console reveal + spin). Stop emulation |
| 94 | exit_group | `_Exit` | no | same |
| 96 | set_tid_address | `__init_tp` | once | return 1 |
| 214 | brk | `__malloc_alloc_meta` | yes | `a0==0` → current brk; `a0<brk_base` → current; `a0>mmap_bottom` → current (failure); else **zero [old,new)** and set brk |
| 215 | munmap | `__munmap` | yes | §2.4 |
| 216 | mremap | `__mremap` (realloc) | maybe | **-38**; musl falls back to malloc+copy |
| 222 | mmap2 | `__mmap`, `__init_tls`(unreached) | yes | §2.4 |
| 226 | mprotect | `__mprotect` (mallocng guard) | maybe | return 0 |
| 291 | statx | `__fstatat` (`stat()`) | yes (CD code) | path mode (`a0=AT_FDCWD=-100`): open by path → size → free the fd **without** close side effects; `memset(a4,0,256)`, `u32[0]=0x7FF`, `u32[10..11]=size`; -2 if the open fails. fd mode (`a0` = valid fd): size of the fd |
| 414 | ppoll_time64 | `__init_libc` | no (secure mode off) | -38 |
| 422 | futex | `__lock/__unlock/__lockfile/locking_getc/putc` | no (single thread; `libc.need_locks==0`) | return 0 |
| ? | computed | `__syscall_cp_c` (generic: a7=a0, args shifted) | yes | used by `open`(56), `close`(57), `write`(64) wrappers. Dispatch on the runtime a7 |

The kernel also implements 17, 48, 49, 64, 79, 80, 103, 113–115, 130, 134, 135,
172, 178, 233, 259 and 403/406/407. None of them is reachable from the Quake
ELF's static call sites; still answer them per `kernel/syscall.c:2512-2791`.
Unknown numbers → -38.

Vendor SBI (a7 = EID, a6 = FID):

| EID/FID | SDK inline | call sites [ELF] | semantics | emulation |
|---|---|---|---|---|
| 0xC0DE0006 / 2 `TIMER_GET_US` | `of_time_us()` | **`main` (2 sites: `last_us`, `now_us`)**, **`Sys_FloatTime`**, plus host_speeds-gated code in `_Host_Frame`, `D_DrawSurfaces`, `R_ScanEdges`, and net/menu/save code | µs since boot = `cycles / (CPU_FREQ_HZ/1e6)`, 32-bit wrap. Returns `{0, value}` | **virtual clock, §7.4** |
| 0xC0DE0006 / 3 `TIMER_GET_MS` | `of_time_ms()` | `CDAudio_*` (async waits, quiet deadline), `Host_Savegame/Loadgame` | ms since boot | virtual clock, µs/1000 |
| 0xC0DE000E / 8 `FILE_ASYNC_MAX_READ` | `of_file_async_max_read` | `main` (inlined `CDAudio_Init`) | returns `OF_TARGET_CRAM0_DMA_CHUNK_SIZE` = **65536** | `{0,65536}` |
| 0xC0DE000E / 6 `FILE_DMA_STAGE_ALLOC(size,align)` | `of_file_dma_stage_alloc` | `main` (CDAudio_Init: size 65536, align 2048) | bump allocator in the CRAM0 app pool: `0x30000000+0x500000+align_up(next,align)`; NULL (`error=OF_ERR_NO_SHMEM`) if it overflows 1 MB | `{0,0x30500000}` on the first call. Map CRAM0 as RAM so a later deref works |
| 0xC0DE000E / 0 `FILE_READ_ASYNC(slot,off,dest,len,cb)` | `of_file_read_async` | `CDAudio_Update` | fire-and-return DMA; callback `cb(token,result)` called later from kernel (IRQ or poll) | only reached if a CD track opens. With no tracks, unreachable. If implemented: copy synchronously, set a completion flag, call `cb` on the next `ASYNC_POLL` (run a guest call into app code) |
| 0xC0DE000E / 1 `FILE_ASYNC_POLL` | `of_file_async_poll` | `CDAudio_Stop`, `CDAudio_DrainAsync` | only reached when `cd_async_pending` | return `{0,0}` if nothing is pending |
| 0xC0DE000E / 2 `FILE_ASYNC_BUSY` | `of_file_async_busy` | `CDAudio_Stop` | `async_state.active` | `{0,0}` |
| 0xC0DE0004 / 1 `ANALOGIZER_IS_ENABLED` | `of_analogizer_enabled` | `CL_SendCmd` (every frame via `in_of.c`) | 0 (no adapter on the cart port) | `{0,0}` |
| 0xC0DE0004 / 0 `ANALOGIZER_GET_STATE(out)` | `of_analogizer_state` | `CL_SendCmd`, only if enabled | copies 6 bytes | never reached when IS_ENABLED returns 0 |

No other vendor EIDs appear. The ELF also contains the constants `0xC0DE0001`
and `0xC0DE0002`, but those are the getauxval tags, not ecalls.

### 2.3 Instructions and CSRs the environment must accept [ELF]

* `fence` (0x0FF0000F, 74 sites) → NOP. (`fence.i` 0x0000100F does not appear in the app.)
* Zicbom **`cbo.flush`**, `.insn i 0x0F,2,x0,rs1,2` (e.g. 0x0027A00F rs1=a5,
  0x0026200F rs1=a2, 0x0026A00F rs1=a3) → functionally a NOP. For a cache
  model: write back and invalidate the 64 B D-cache line containing `rs1`.
  Legal only for 0x10000000–0x13FFFFFF.
* CSR `fflags` (csrrs/csrrsi from libgcc soft double/quad; `fmt_fp` uses long
  double = quad) and `frrm` (read `frm`). No `mcycle/rdcycle` (the SDK comments
  say those trap on this CPU).
* `lr.w/sc.w` exist in musl (A extension). `ebreak` (65 sites) =
  `__builtin_trap()` in GPU/SDK wait timeouts; treat it as fatal.
* ISA as built: rv32imafc + Zicsr + Zicbom (`generate_vexii.sh --with-rvm
  --with-rva --with-rvf --with-rvc --with-rvZcbm`), FPU built with
  `--fpu-ignore-subnormal` (see §9).

### 2.4 mmap2 / munmap / brk model (port `kernel/syscall.c:1921-2230` verbatim) [SRC]

Port these exactly, so pointer values and zero-fill match the device:

* `mmap2(addr,len,prot,flags,fd,pgoff)`: `len≤0` → -22. Align up to 4096
  (overflow → -12). First `mmap_coalesce_bottom()` (free slots whose base ==
  `mmap_bottom` raise `mmap_bottom`, to a fixed point). Then best-fit reuse of a
  free slot; split the tail into a new free slot if excess ≥ 4096 and a slot
  is spare; memset the whole region to 0. Otherwise carve
  `base = mmap_bottom - len`: -12 if `len > mmap_bottom` or `base < current_brk`
  or base outside [0x10000000,0x40000000). Needs a free tracking slot
  (4096 slots); if none, `mmap_merge_free()` and retry reuse. memset 0 and
  lower `mmap_bottom`.
* `munmap(addr,len)`: `!addr||len≤0` → 0. Match on **base only**. If base ==
  `mmap_bottom`: raise by the recorded len, free the slot, coalesce. Else mark
  it free. No match → 0.
* Initial state for the app: `brk_base=current_brk=0x10AA0490`,
  `mmap_bottom=0x13A00000`, slot table empty.

---------------------------------------------------------------------------

## 3. OS services table (`of_services.h`, `kernel/services_table.c`)

### 3.1 Layout and mechanism

* `struct of_services_table` = 109 × 4-byte fields = **436 bytes**.
  `magic=0x4F535643` ('OSVC') @0, `version=2` @4,
  **`count=106`** @8 (`(436-12)/4`), then function pointers starting at @12
  (index `i` → offset `12+4*i`).
* The app calls entries with plain `jalr` (no ecall). Services run in the app's
  context on the app's stack under the normal ilp32f ABI: args in a0..a7, a
  64-bit argument takes two registers (lo,hi), return in a0 (a0:a1 for
  `uint64`), caller-saved regs may be clobbered.
* **Emulation:** fill each pointer with a distinct trap address in a range the
  CPU core intercepts before fetch. Suggested [CHOICE]: `0x103F0000 + 4*i`
  (inside the kernel hole, cached SDRAM, executable, never used by the app).
  On fetch at such a PC, run the C service, set a0/a1 (and fa0 if ever
  needed), then `pc = ra`. Unused entries also get unique non-NULL values: the
  SDK tests `OF_SVC->X != NULL` and `count > index` before many calls. Count
  these as "kernel instructions" in the profile if desired. Their device cost
  is not modelled.

### 3.2 Entries the ELF calls [ELF `specwork/ptr_cp2.py`, source-cross-checked]

Also read: `magic` (VID_Init), `count` (many callers). Entries marked † are
gated on `count`/NULL checks.

| off | entry | app caller(s) | signature | kernel behaviour (Pocket) | minimal faithful emulation |
|---|---|---|---|---|---|
| 12 | video_init | main | `void()` | wait out pending swap, mode ← 320×240/8-bit, `TERM_FB_CTRL=0`, reset buffer roles from HW, vblank IRQ on, **memset 0 the 3 FBs** (76 800 B each, cached alias) | role model §3.3 `init`; zero FB0–2 (320×240) |
| 16 | video_get_surface | VID_Init, VID_Update (legacy path only) | `uint8_t*()` | `fb_addr[buf_draw]` (0x10000000/0x10100000/0x10200000) | same |
| 20 | video_flip | VID_Init (3×), VID_Update (legacy) | `uint8_t*()` | §3.3 `flip`; cleans cache over the draw FB; writes `FB_SWAP_CTRL=(draw<<1)|1`; returns the new draw surface | §3.3. **Frame event for kernel flips** |
| 24 | video_wait_flip | VID_Init (3×), VID_Update (when a present is pending) | `void()` | spins while `FB_SWAP_CTRL&1` (≤1/30 s), then `sync_swap_state` | instant: `sync` |
| 36 | video_set_palette_bulk | VID_SetPalette (init + every V_UpdatePalette change) | `(const uint32_t *pal, int count)` | shadow ← pal[0..min(count,256)) as 0x00RRGGBB; staged HW upload | **store 256 entries; part of the frame oracle** |
| 52 | video_set_display_mode | main (FRAMEBUFFER=1), Sys_Error (TERMINAL=0) | `(int)` | reprogram scanout; overlay palette only for mode 2 | record; Sys_Error → stop |
| 56 | video_set_color_mode | main (8BIT=0) | `(int)` | normalize {cur w,h, stride 0, mode}; reset roles; **clear FBs (max(old,new,76800) B)**; force-present a clean buffer | §3.3 `set_color_mode` |
| 60 | video_acquire_next | VID_Init (`-1,0`), SCR_UpdateScreen (VID_WaitSync, every rendered frame) | `int(int just_flipped_idx, uint32_t fence_token)` | §3.3 `acquire` (waits ≤500 000 spins for `GPU_FENCE_REACHED ≥ token`; on timeout re-kicks via FB_SWAP_CTRL) | §3.3; with the instant GPU the fence is always reached |
| 64 | video_buffer_addr | VID_Init (0,1,2) | `uint8_t*(int idx)` | `fb_addr[idx]`; NULL if idx≥3 | same |
| 68 | input_poll | IN_SendKeyEvents (inline `of_input_poll`) | `void()` | latch pads | no-op |
| 72 | input_get_state | IN_SendKeyEvents (players 0,1, then 0 again), CL_SendCmd | `(int player, of_input_state_t *out)` | memcpy 24 bytes | **write 24 zero bytes** (no buttons) |
| 84 | mixer_init | main (`S_Startup`: 32, 48000) | `(int,int)` | HW mixer init | no-op |
| 96 | mixer_stop_all | main | `void()` | stop voices | invalidate all handles |
| 172 | audio_init | main (SNDDMA_Init), S_ClearBuffer | `void()` | deactivate stream voice; `audio_write_idx=0` | reset ring model (§3.4) |
| 176 | audio_write | R_RenderView_ (SNDDMA_FillRing ×2/frame), SNDDMA_Submit | `int(const int16_t*, int pairs)` | copy ≤room pairs into the 131 072-pair ring; activate the voice | §3.4 |
| 180 | audio_get_free | same | `int()` | room = `131072` if the voice is inactive, else `131071 - ((wr - rd) & 0x1FFFF)` | §3.4 |
| 204 | cache_flush † | CDAudio_*, S_LoadSound (fallback only) | `void()` | full D-cache writeback + I-cache invalidate | no-op (cache model: flush all) |
| 240 | mixer_set_group_volume | main (SFX, AUX =255), CDAudio_* | `(int g,int v)` | HW reg | no-op |
| 244 | mixer_set_master_volume | main (255) | `(int)` | HW reg | no-op |
| 284 | cache_flush_range † | S_LoadSound (every loaded sfx), CDAudio_* | `(void*,uint32_t)` | cbo.flush per line | no-op (cache model: flush range) |
| 288 | input_get_keyboard_state † | IN_SendKeyEvents | `(of_keyboard_state_t *out)` | memcpy 112 B | **112 zero bytes** (`present=0`) |
| 292 | input_read_mouse_state † | IN_SendKeyEvents | `(of_mouse_state_t *out)` | fill 20 B, consume deltas | **20 zero bytes** (`present=0`) |
| 304 | mixer_alloc_for_group_h † | S_MixerStartChannel (every SFX start), CDAudio_Play | `uint64_t(int group, const uint8_t *pcm, uint32_t n, uint32_t rate, int prio, int vol)` | validates pcm ∈ SDRAM; picks a voice; handle = `(generation<<8)|voice`; 0 = invalid | return a unique nonzero 64-bit handle in a0:a1 (e.g. `(seq<<8)|(seq%32)`); keep a table of live handles |
| 312 | mixer_stop_h † | S_StartSound, S_Update, S_ClearBuffer, CDAudio, menus, CL_Disconnect | `(uint64_t h)` (a0:a1) | stop if the handle is valid | mark stopped |
| 336 | mixer_set_loop_h † | S_MixerStartChannel, CDAudio_Play | `(uint64_t h, int start, int end)` a0:a1,a2,a3 | | no-op |
| 340 | mixer_set_rate_h † | CDAudio_Update | `(uint64_t h, int hz)` | | no-op |
| 348 | mixer_set_vol_lr_h † | S_StartSound, S_Update (spatialization, every frame per channel), CDAudio_Play | `(uint64_t h, int l, int r)` | | no-op |
| 356 | mixer_get_position_h † | CDAudio_Update | `int(uint64_t h)` | voice `POS & 0x3FFFFF`, -1 if the handle is invalid | only with a CD track: return a position advancing at `rate × virtual time` |
| 360 | mixer_set_position_h † | S_MixerStartChannel | `(uint64_t h, int pos)` | | no-op |
| 388 | video_set_mode † | VID_Init | `int(const of_video_mode_t*)` | `video_normalize_mode` (w,h ≠0, ≤800×600, stride even ≥ line, frame ≤1 MB); set mode; reset roles; `TERM_FB_CTRL=0`; clear FBs max(old,new,76800); force-present | §3.3 `set_mode`; return 0 for {320,200,320,0} |
| 392 | video_get_mode † | of_video_get_mode (VID_Init) | `(of_video_mode_t *out)` | copy the current mode | write `{u16 320, u16 200, u16 320, u8 0, u8 0}` after set_mode |
| 408 | video_check_mode † | VID_Init | `int(const of_video_mode_t *in, of_video_mode_t *out)` | normalize; `out` = in with stride = even line bytes | {320,200,0,0} → out {320,200,320,0,0}, return 0 |
| 412 | config_get † | Sys_InitQuakeConfig, COM_AddGameDirectory (`PAK_<game>_<i>`), Sys_IniGetInt (`HOST_SPEEDS`, `CD_ASYNC`), CDAudio_Play (`CDFMT`) | `int(const char *sect, const char *key, char *out, uint32_t len)` | case-insensitive section/key, **last** matching entry wins; 0 on success; -2 NOENT; -28 if `strlen+1 > len`; -22 bad out | §6.2 |
| 424 | config_next † | Sys_InitQuakeConfig (only when `[quake] CONFIG` is absent) | `int(const char *sect, uint32_t *cursor, char *key, uint32_t klen, char *val, uint32_t vlen)` | iterate entries in file order from `*cursor`; 0 + advance; -2 at end | port `kernel/config.c` |

Not used by this ELF (verified absent): timer_*, video_get_timing,
input_is_docked, file_size*, file_slot_register, mixer_play*, audio_stream_*,
video_vsync, video_clear, video_flush_cache.

Struct sizes [DER from `of_input_types.h`, ilp32]: `of_input_state_t` = 24,
`of_keyboard_state_t` = 112, `of_mouse_state_t` = 20, `of_video_mode_t` = 8.

### 3.3 Display buffer-role model (port of `targets/pocket/video.c`) [SRC + CHOICE]

The device's role bookkeeping depends on when vblank happens. **Choose
"instant vblank"**: any swap request (a CPU `FB_SWAP_CTRL` write, or the GPU
executing `CMD_FLIP`) sets `hw_display = idx` with `pending = 0` immediately.
Port these kernel functions verbatim on top of that register model:
`sync_swap_state`, `pick_free_buffer`, `video_reset_buffer_roles`,
`video_force_present_clean_buffer`, `of_video_flip`, `of_video_wait_flip`,
`of_video_acquire_next`, `of_video_set_mode`, `of_video_set_color_mode`,
`of_video_init`. `specwork/vidmodel.py` is a ~40-line reference port. With
the Quake call sequence it gives [DER]:

* after `set_color_mode`+`set_mode`: display 0, draw 1 → `vid.buffer` = uncached(FB1) = 0x50100000
* VID_Init clear loop: kernel flips of FB1, FB2, FB0
* `acquire_next(-1,0)` → **draw_idx = 1**
* per rendered frame (`CMD_FLIP(idx)` then `acquire_next(idx,tok)`): draw index
  sequence **1,0,1,0,…** (FB2 is not used after init under instant vblank; on
  hardware with real vblank latency a 3-buffer rotation is typical)

Consequence: with partial redraws (Sbar/console "numpages"), buffer content
persistence depends on the rotation. It is identical across A/B runs because
the model is fixed.

### 3.4 Audio ring and mixer model [SRC snd_of.c, audio.c, snd_dma.c + CHOICE]

* Quake uses `SND_USE_HW_MIXER=1`. SFX are mixer voices. `paintedtime`
  advances by `host_frametime*48000` per frame (`S_MixerAdvanceTime`). Each
  `audio_drain()` loops `audio_get_free` → `audio_write` in ≤64-pair chunks
  until `submit == paintedtime`. That is ~75 pairs of service calls per frame
  at 0.1 s/frame, and it is real CPU work.
* Emulate the stream ring: `AUDIO_RING_PAIRS=131072`; `active=0` after
  `audio_init`; `write` activates, clamps to room, advances `wr`. `rd`
  advances at **48 000 pairs per second of virtual time** (§7.4), not past `wr`.
  Room formula as in the table above. This keeps the drain loop count
  deterministic and close to the device.
* Mixer handles: see §3.2. Nothing in the engine reads voice state back,
  except `get_position_h` in the CD path.

### 3.5 Kernel-side state that can reach the app

Only through return values and memory writes listed above: FB clears in
video_init/set_mode/set_color_mode, the input zero structs, config strings,
mode structs, handles, audio room. Services never touch other app memory.

---------------------------------------------------------------------------

## 4. Capability descriptor (os25)

### 4.1 hw_features for os25 [SRC axi_periph_slave.v:797-897, core_top.v:2575-2619, variants/os25.mk; DER]

os25 `DEFS := INCLUDE_ANALOGIZER INCLUDE_HW_MIXER INCLUDE_TRANSLUC
INCLUDE_PALETTE INCLUDE_COLUMN_LIST INCLUDE_COMPACT_SPAN INCLUDE_PARAM_SPAN_Q29
INCLUDE_4PLAYER INCLUDE_BANK_ROW_TRACK INCLUDE_CPU_RING INCLUDE_GPU_WRITE_BURSTS
EXCLUDE_GPU_COMMAND_DMA`. There is no INCLUDE_LINK/PARAM_TRI/VERT_TRI/
PARAM_TRI_RECS/TEX_MEM/DIRECT_COLOR/COMBINE/XFORM.

```
  0x00000001  bit0  MIXER (always)
| 0x00000002  bit1  MIXER_HW            (INCLUDE_HW_MIXER)
| 0x00000008  bit3  ANALOGIZER          (INCLUDE_ANALOGIZER)
| 0x00000010  bit4  GPU_SPAN (always)
| 0x00000340  bits6,8,9 MIDI, FPU, SAVE_SLOTS (always)
| 0x0003E000  bits13-17 PERSP, FRAGPIPE, PARAM_SPAN_LIST, PARAM_SPAN_Z, PARAM_SPAN_ZTEST (always)
| 0x00040000  bit18 PARAM_SPAN_Q29_SCALE (INCLUDE_PARAM_SPAN_Q29)
| 0x00200000  bit21 COLUMN_LIST
| 0x00800000  bit23 SPAN_GROUP (INCLUDE_COMPACT_SPAN)
| 0x01000000  bit24 SAVE_DT_WORD (always)
= 0x01A7E35B
```

Clear: bit2 LINK (the `ifdef INCLUDE_LINK` macro is not set for os25), 10
VCOLOR, 11, 12, 19 PARAM_TRI, 20 VERT_TRI, 22 PARAM_TRI_RECS, 25 FAST_TEX, 26,
27, **28 SPAN_CONT (never advertised)**, 29, 30. This value was never seen in a
device log. Quake prints it (`GPU caps: hw=%08x …` to stdout in VID_Init), so a
UART capture from a real Pocket would confirm it.

### 4.2 Full `struct of_capabilities` (100 bytes) [SRC caps_table.c + DER]

| off | field | value | read by Quake? |
|---|---|---|---|
| 0 | magic | 0x43415053 | yes (VID_Init) |
| 4 | version | 4 | yes (in_of.c mouse, of_texture_init, fasttex size) |
| 8 | heap_base | 0x10AA0490 (= bss_end aligned) | no |
| 12 | heap_size | `0x13A00000 - heap_base` = **0x02F5FB70** (49 675 120) | **yes**: main heap size, surface cache size |
| 16 | fb_base | 0x10000000 | no |
| 20 | fb_size | 76800 | no |
| 24 | fb_width | 320 | no |
| 28 | fb_height | 240 | no |
| 32 | fb_stride | 320 | no |
| 36 | sample_base | 0x13A80000 | no |
| 40 | sample_size | 0x00080000 | no |
| 44 | hw_features | **0x01A7E35B** | **yes** |
| 48 | mixer_voices | 32 | no |
| 52 | mixer_rate | 48000 | no |
| 56 | platform_id | 1 (POCKET) | yes (`_gpu_publish_uncached = (id==MISTER)` → 0; mouse decode) |
| 60 | core_variant | 0 | no |
| 64 | sdram_size | 0x04000000 | yes (of_gpu_init range check) |
| 68 | cram_size | 0x01000000 | no |
| 72 | os_version | 0x00000900 (0.9.0) | no |
| 76 | cpu_freq_hz | 100000000 | no |
| 80 | sdram_base | 0x10000000 | yes (of_uncached, texture addrs, palookup) |
| 84 | sdram_uncached_base | 0x50000000 | yes |
| 88 | gpu_base | 0x4A000000 | yes (`_gpu_base`, must be ≠0) |
| 92 | tex_fast_size | 0 | yes |
| 96 | os_features | 1 (MOUSE_COUNTS) | yes |

`heap_size` depends on the ELF's .bss end. See §9 for the A/B consequence.

### 4.3 Code paths these caps select [SRC vid_of.c:629-706, d_scan.c, d_polyse.c, d_sprite.c]

`of_emit_caps` = SPAN | PERSP | FRAGPIPE | FLIP | PARAM_SPAN_LIST |
PARAM_SPAN_Z | PARAM_SPAN_ZTEST | Q29_SCALE | SPAN_BATCH. It does **not**
include TRIANGLES or ALPHA. `g_vert_tri=0`.

* World surfaces: GPU long-form 0x48 param span lists, `attr_mode=PERSP_Q29`,
  **`z_mode=Z_WRITE_ZI`** (the GPU writes world depth; CPU `D_DrawZSpans` is
  skipped when `pq_combined_z_active`).
* Alias models (`D_PolysetHWTriangles` = 0): **CPU raster** (`D_PolysetDraw*`)
  z-tests/writes the GPU-written z-buffer. Mixed with some GPU spans and
  `CLEAR_RECT` from d_polyse.c.
* Sprites: GPU param spans with Z_TEST (bit17).
* Sky / turbulent / HUD / console chars / pics: GPU spans (0x48 compact
  affine groups) from `of_emit_span`/`of_emit_blit`.
* Particles: CPU (`D_DrawParticle`, reads/writes z).
* z-buffer: `zbuffer_cpu_cached=0` → `d_pzbuffer = of_uncached(zbuffer_storage)`.
  Clears are GPU `CLEAR_RECT`.
* Flip: GPU `CMD_FLIP` + `video_acquire_next` (the CAP_FLIP path; `OF_HW_GPU_FLIP`
  is not defined, so the service presence alone enables it).
* Command transport: CPU ring (§5.2). `platform_id==POCKET` → cached staging.
* Textures: `tex_fast_size=0` → everything SDRAM; `of_emit_bind_static/dynamic`
  return early; `_GPU_FAST_TEX_*` regs are not written at run time.

---------------------------------------------------------------------------

## 5. MMIO

### 5.1 The only MMIO the app touches: GPU @ `caps->gpu_base` = 0x4A000000 [ELF `gpu_mmio.py`, SRC of_gpu.h:193-230, gpu_core.v:500-696]

All registers are 32-bit, accessed as whole words through `_gpu_base` (0x104811e4).

| off | name | dir | used | RTL semantics | "instant GPU" emulation |
|---|---|---|---|---|---|
| 0x00 | GPU_CTRL | W | init, every publish | bit1 soft_reset; bit2 ring_reset (`wr_addr=wrptr=0`, rdptr reset, `cpu_ring_mode=0`, overflow=0); bit4 select CPU ring (only if `!dma_busy && wr_addr==wrptr`); bit3 publish (`wrptr = wr_addr` if CPU mode); bit5 select DMA (only with command DMA, absent on os25); bit0 "enable" has no effect (`gpu_enable` tied 1) | as RTL. **On publish, execute all newly published words** (§5.3), then `rdptr = wrptr` |
| 0x04 | GPU_RING_WRPTR | W | every command word | if `cpu_ring_mode`: when `(wr_addr+1)&0xFFF == rdptr` set sticky overflow and drop; else `ring[wr_addr++] = word` | append to a pending buffer (4096-word ring) |
| 0x04 | (read) | R | not read by the app | published wrptr in bytes | `wrptr*4` |
| 0x08 | fast-tex up addr / chanutil val | W/R | no (gated by fast tex) | no-op on os25 | ignore / 0 |
| 0x0C | GPU_DMA_SRC | W | DMA path only (unreachable) | no-op on os25 | ignore |
| **0x10** | GPU_RING_RDPTR | R | `_gpu_ring_ensure`/`can_emit` | `{16'b0, rdptr, 2'b0}` (bytes) | **`(published_wrptr_words & 0xFFF) * 4`** (12-bit word pointer, so the byte value is ≤ 0x3FFC): the instant GPU has consumed everything published |
| **0x14** | GPU_STATUS | R | `of_gpu_use_cpu_ring`, after every CPU-ring upload, DMA waits | bit0 busy, bit1 ring_empty, bit2 dma_busy, bit3 transluc_busy, bits5:4 dma_state, bit6 desc_full, **bit7 CPU_RING_CAP**, **bit8 CPU_RING_MODE**, bit9 overflow | **0x00000082** before select, **0x00000182** after select (idle, ring empty, capability); OR 0x200 if overflow ever happened. **The SDK traps unless `(status & 0x300) == 0x100` after each upload** |
| **0x18** | GPU_FENCE_REACHED | R | `of_gpu_init`, `of_gpu_wait` spins | token of the last retired `CMD_FENCE`/`CMD_FLIP`; 0 after power-on; **not cleared** by soft/ring reset | 0 initially; updated during publish execution |
| 0x1C | GPU_DMA_LEN | W | DMA only | no-op | ignore |
| 0x20/0x24 | TRANSLUC_ADDR/DATA | W | not used by Quake | | ignore |
| **0x28** | GPU_TEX_FLUSH | W | of_gpu_init, **every frame** (VID_WaitSync), draw.c | flush the GPU texture cache | if the GPU model has a tex cache (`gpu.h` `tex_cache`), invalidate it; otherwise no-op |
| 0x2C | GPU_DMA_KICK | W | DMA only | no-op on os25 | ignore |
| **0x30** | GPU_PALOOKUP_BASE | W/R | of_gpu_init (value **0x104BC000** = `&_gpu_palookup_storage`) | `palookup_base <= wdata[25:0]`; read `{6'b0,base}` | store `& 0x03FFFFFF` |
| 0x34 | CHANUTIL_SEL | W | no | profile builds only | ignore |
| 0x38/0x3C | fast-tex enable/data | W/R | gated off (`tex_fast_size=0`) | no-op on os25; 0x3C reads 0 | ignore / 0 |

There are no sysreg, UART, audio, mixer, cycle-counter or CD-register accesses
from the app. `_of_sysreg_ro_zero` stubs the perf-counter aliases in software.

### 5.2 CPU-ring upload protocol as the ELF executes it [SRC of_gpu.h:629-931]

Init (`of_gpu_init`, from VID_Init → `of_emit_init`):

```
_gpu_base = caps->gpu_base
GPU_CTRL = 6 ; 100-iteration volatile spin ; GPU_CTRL = 4 ; GPU_CTRL = 1
_gpu_fence_next = GPU_FENCE_REACHED + 1          // read 0x18 → 0 → tokens start at 1
check batch storage (0x10870D80, 2×16 KB) and palookup storage (0x104BC000, 256 KB, 16 KB aligned) are inside [sdram_base, sdram_base+sdram_size)
GPU_PALOOKUP_BASE = 0x104BC000
GPU_TEX_FLUSH = 1
of_gpu_use_cpu_ring():  status = GPU_STATUS; need bit7 set and bit2 clear
                        GPU_CTRL = 0x10
                        _gpu_cpu_ring = (GPU_STATUS & 0x100) != 0
```

Then `of_gpu_palookup_upload(0, host_colormap, 16384)` writes 4096 words
through the uncached alias at `0x504BC000`. That is RAM, not MMIO.

Staging: commands are built as words in `_gpu_batch_storage[idx]` (cached
SDRAM, alternating idx 0/1 per flush). The header word is
`(cmd<<24) | payload_words`. The SDK's `_gpu_wrptr` counts staged bytes mod
16384. Before staging, `_gpu_ring_ensure(bytes)` checks
`(known_rdptr - wrptr - 4) & 0x3FFF`. If that is too small it flushes, then
re-reads `GPU_RING_RDPTR` until enough space is free. With "rdptr = published"
this always succeeds after one read.

Flush (`_gpu_flush_cmd_stream`), which happens when `of_gpu_kick` sees
`cmd_words ≥ 512` or a staged fence/flip token, or when the stream would exceed
4095 words:

```
for each staged word:  GPU_RING_WRPTR = word          // (loads 4 words, then 4 stores)
status = GPU_STATUS; if ((status & 0x300) != 0x100) ebreak
GPU_CTRL = 0x8                                        // publish
cmd_words = 0; unflushed_sync = 0; switch staging buffer
```

Waits: `of_gpu_wait(token)` spins up to 50 M iterations on
`(int32)(GPU_FENCE_REACHED - token) >= 0`, then `ebreak`.
`of_gpu_finish()` = fence + kick + wait.

### 5.3 Command set actually emitted on os25, and wire formats [SRC of_gpu.h + vid_of.c]

| op | name | payload | emitted by | effect |
|---|---|---|---|---|
| 0x02 | FENCE | 1: token | `of_gpu_fence` (finish, prepare_framebuffer_for_cpu) | `fence_reached = token` (after all pixel writes) |
| 0x11 | CLEAR_RECT | 3: `addr`, `(w<<16)|h`, `(stride<<16)|color` | of_emit_clear (FB at init; **z-buffer every frame**, w=640 bytes, stride 640, color 0), of_emit_clear_rect(_addr) (Draw_Fill, d_polyse) | h rows × w **bytes** starting at `addr&0x3FFFFFF`, row step = stride (0 → SET_FB stride); byte = color & 0xFF |
| 0x20 | SET_TEXTURE | 2: `addr`, `(w<<16)|h` | `of_emit_bind_texture`, called only from the HW-triangle paths (`D_PolysetDraw` when `D_PolysetHWTriangles()`, `R_FlushWorldTriBatch`), which are **off on os25**, so it is not emitted | legacy sticky state |
| 0x23 | SET_FB | 2: `addr`, `stride` | `of_emit_bind_fb` in VID_Init only (clear loop ×3 + final bind; deduplicated by `_gpu_state_*`) | sticky FB base/stride (used by CLEAR_RECT with stride 0) |
| 0x42 | FLIP | 2: `idx&3`, token | **VID_Update, once per rendered frame** | drain writes; request a display swap to FB[idx]; `fence_reached = token` |
| 0x48 | PARAM_SPAN_LIST, compact affine lanes | `4+7N` (N=1..4 → 11/18/25/32 words): `w0=(N<<28)|((flags&~PERSP&0xFF)<<20)`, `w1=tex_width`, `w2=(hmask<<16)|wmask`, `w3=fb_step`; per lane: `fb_addr, tex_addr, (cmap<<28)|(light&0x3F)<<16|count12, s, t, sstep, tstep` | flush_span_batch (affine groups: HUD/console blits, sky, turbulent, sprites fallback) | per lane: scalar affine span (see §5.4) |
| 0x48 | PARAM_SPAN_LIST, long form | `31 + 3*ceil(n/2)`: w0..28 = `fb_base, fb_major, fb_minor, tex_addr, tex_width, wmask, hmask, control, {org,du,dv}×3, light{org,du,dv}, clamp{min,max}×3, z_base, z_major, z_minor`; w29 = record count; w30 = q29 shift; records packed in pairs `{(v0<<16)|u0, (u1<<16)|count0, (count1<<16)|v1}` (odd tail: `{(v<<16)|u, count, 0}`). `control = flags | cmap<<8 | attr_mode<<12 | axis<<16 | z_mode<<24` | world surfaces (d_scan.c, PERSP_Q29 + Z_WRITE_ZI), sprites (Z_TEST), persp span groups (vid_of.c) | §5.4 |
| 0x4C | COLUMN_LIST | 4+5N | not called by Quake | |

Present in the code but **never emitted at run time** on os25 (caps-gated):
0x49 PARAM_TRI (bit19), 0x4A/0x4B (bit20/22), 0x4D, 0x58 SPAN_CONT (bit28).

GPU address rule [SRC gpu_core.v:353]: `GPU_ADDR_W = 26`, so every address
field is `& 0x03FFFFFF`. That is the SDRAM byte offset, the same physical byte
as CPU 0x10000000+off and 0x50000000+off. Commands mix 0x1xxxxxxx (SET_FB at
init, textures as raw pointers, palookup) and 0x5xxxxxxx (vid.buffer, z-buffer).

### 5.4 Raster semantics (for the GPU model; the qsim `gpu.c` owns the exact math)

A reference-level summary. Bit-exact behaviour is in `gpu_core.v`, with
C++ reference models in `openfpgaOS/src/fpga/test/tb_gpu_acceptance_main.cpp`
(`FbModel::apply_span_affine` ~l.1485, `param_affine_ref_span` l.5604,
`quake_ref_span` l.7252, Q29 setup l.7340-7580) and `gpu_math_reference.vh`.

* Affine scalar span: for i<count: `s_int=(s>>16)&mw`, `t_int=(t>>16)&mh`
  (mask 0 → 0xFFFF); `texel = mem[tex + t_int*tex_width + s_int]`; if
  `SKIP_ZERO(bit2)` and texel==**0xFF**, skip; if `COLORMAP(bit0)`:
  `color = mem[palookup_base + cmap*0x4000 + (light&63)*256 + texel]`;
  write `mem[fb]`; `s+=sstep, t+=tstep, fb+=fb_step`.
* Long form: for record (u,v,count), with axis X: `fb = fb_base + v*major + u*minor`;
  attributes are evaluated as planes `org + u*du + v*dv`; per pixel the
  attributes step by `du`, `fb` by `minor`. `ATTR_PERSP`: s/z, t/z, 1/z in
  16.16 with a per-pixel/segment divide. `ATTR_PERSP_Q29`: numerators ×2^13,
  1/z in Q3.29, with `q29_attr_shift` restore. `light` = `(light_plane>>16)&63`.
* z (long form, `z_mode` 1=WRITE_ZI, 2=TEST_ZI, 3=TEST_WRITE): 16-bit words at
  `z_base + v*z_major + u*z_minor` (+`z_minor` per pixel). Value: Q29 mode
  `sat_shl(attr2, shift)[29:14]`, stepping by `sat_shl(attr2_step, shift)` with
  saturating add (gpu_core.v:2682-2693, 2956-2993, 8383); non-Q29 `attr2[16:1]`.
  TEST is GEQUAL, which matches Quake's CPU writer (`izi>>16`, D_DrawZSpans).

**Why the z path matters even for a "command-only" oracle:** CPU alias models
and particles read `d_pzbuffer` bytes the GPU wrote. Without GPU z-writes the
CPU takes different branches (a different profile, different pixels). Both A/B
runs stay consistent, but profile fidelity is lost.

---------------------------------------------------------------------------

## 6. File access

### 6.1 Data slots as the kernel sees them [SRC data.json, Quake.json, syscall.c `dir_probe_slots`]

On the Pocket, APF resolves instance filenames under `Assets/quake/common/`.
The kernel only knows slot ids, sizes (datatable) and GETFILE basenames
(`of_file_get_name` strips everything up to the last '/'; the registry
truncates to 23 chars). `filesystem_init` probes slots 1..31 in ascending
order and registers `(slot, name)` when `size>0`, or when the slot is
nonvolatile-capable (8, 9, 10–19) **and** has a datatable entry. If GETFILE
fails the name is `"slot:N"`.

The slot table is **per app**: qsim builds it from the app profile's `slot =`
lines (`apps/<app>/<app>.app`), in the order written, which is the order the
kernel publishes and the app probes. Below is the table the Quake profile
declares, for the registered-Quake instance (`Quake.json`)
[CHOICE for sizes marked *]:

| slot | registry name | host backing | size | kind |
|---|---|---|---|---|
| 1 | `os.bin` | none (never read by app) | *135400 (constant) | RO |
| 2 | `quake.ini` | generated ini (§6.2) | len(ini) | RO |
| 3 | `quake.elf` | the ELF under test (never read by app) | ***constant, e.g. 876096; NOT the real ELF size** (it is printed to the console, §7.5) | RO |
| 4 | `pak0.pak` | `<--data-dir>/PAK0.PAK` (case-insensitive) | 18 689 235 | RO |
| 5 | `PAK1.PAK` | `<--data-dir>/PAK1.PAK` | 34 257 856 | RO |
| 8 | `quake.cfg` | in-memory 256 KB CRAM window, initial contents [CHOICE: empty] | datatable size = initial length | NV (read/write) |
| 10–19 | `s0.sav` … `s9.sav` | in-memory 256 KB each, zero | 0 | NV |
| 22–31 | CD tracks | **absent** [CHOICE; see §7.3] | — | not registered |
| 6,7,20,21 | — | absent | — | — |

`d_ino` exposed by readdir is `slot_id + 1`. Quake's `Sys_OpenSaveSlot` and
`cd_of.c` depend on that.

### 6.2 The app `.ini` (slot 2) and argv [SRC config.c, main.c:151-339, dist/…/common/quake.ini]

The `config:` slot of the app profile: the device's `.ini` file with `$ARGS`
standing for the workload arguments. Quake's
(`apps/quake/quake.ini.in`, served here with the demo1 workload's ARGS):

```
[os]
GAME=Quake
INSTANCE=Quake
ELF=quake.elf
ARGS=+timedemo demo1 +host_framerate 0.1

[quake]
CONFIG=quake.cfg
PAK_id1_0=pak0.pak
PAK_id1_1=PAK1.PAK
HOST_SPEEDS=0
```

* The kernel builds `argv[0] = ELF value` ("quake.elf"). ARGS is split on
  space/tab/CR/LF with `'…'`/`"…"` quoting and `\` escapes; at most 32 args and
  511 chars. The emulator passes `argv` directly and never parses ARGS. It only
  has to answer `config_get("os",…)` consistently if the app ever asks (it
  doesn't).
* Parser: `[section]`, `KEY=VALUE`, `#`/`;` comments, trimmed, case-insensitive
  lookup, last duplicate wins; 16 KB and 256 entries max.
* Quake's uses: `[quake] CONFIG` → `sys_quake_config_name="quake.cfg"`,
  `sys_quake_ini_present=1`. Because the ini is present, `COM_AddGameDirectory("./id1")`
  opens **only** `PAK_id1_0` / `PAK_id1_1` (keys i=2..9 are missing → skipped,
  with no fopen). `HOST_SPEEDS=0` keeps every host_speeds timer read off.
  `CD_ASYNC` is absent → default 1. `CDFMT` is absent.

### 6.3 `openat` resolution and reads (port `sys_openat`, `file_slot_lookup`, `sys_read`) [SRC syscall.c:445-700, 1513-1688, 1283-1355]

Order for a path `p` with flags `f`:

1. `p==NULL` → -22; `""` → -2. Allocate fd 3..31 (-24 if full).
2. Mounted-ISO VFS: none for Quake (skip).
3. `f & O_DIRECTORY (0x10000)` → root-dir fd (any path; `opendir("/")`).
4. `"slot:N"` → NV slot → NV fd; else the datatable must have an entry for N and
   size>0, otherwise -2; write flags (`O_ACCMODE!=0 || O_TRUNC`) → -13.
5. `"save:N"`/`"save_N"` → NV slot 10+N.
6. **Registry lookup**: strip leading '/', case-insensitive match of the full
   name; if the path contains '/', ask the target resolver (Pocket: always -1);
   then match the **basename**, case-insensitive. Hit → NV-capable slot → NV fd;
   else write flags → -13; else size ≤ 0 → -2; else RO fd with `size = slot size`.
7. `name_N.sav` pattern → NV save slot N; `duke3d.cfg` → slot 9; Pocket config
   slot resolver → -1; target resolver → -1; otherwise **-2 (ENOENT)**.

Examples from the Quake run [DER]:

* `fopen("pak0.pak","rb")` → slot 4. `fopen("PAK1.PAK")` → slot 5. The match is case-insensitive.
* `fopen("demo1.dem")` never happens: `COM_FOpenFile` finds `demo1.dem` inside
  pak0 (offset 6 297 675, len 184 471) and calls `fopen("pak0.pak")` + `fseek`.
* `Sys_FileTime("./id1/autoexec.cfg")` → `fopen("autoexec.cfg")` → -2, then
  `fopen("./id1/autoexec.cfg")` → -2.
* `"progs.dat"` loose override: `fopen("progs.dat")` → -2, twice. It is only
  hit on server spawn, so not in demo playback.
* `config.cfg` is remapped to `quake.cfg` (§6.4).

RO read (`sys_read`): clamp to `size - offset`; 0 at EOF; copy the bytes (the
kernel's 32 KB block cache is transparent); `offset += n`. `_llseek` sets the
offset (SEEK_END uses the slot size). musl stdio issues `readv(fd, {user, len-1},
{FILE buf, 1024})`.

### 6.4 Nonvolatile files: quake.cfg and saves [SRC syscall.c:1389-1511, 1226-1281, 1688-1719; save.c]

`open_nv_fd(slot, flags)`: capacity 0x40000. `size = cached logical size`
(initially the datatable size). If `size==0 && read-only`: save slots report
**size = capacity (262144)**; config slot 8 **infers text length**, scanning
64-byte chunks until a 0x00 or 0xFF byte. `O_TRUNC` sets size 0 (commit).
`O_APPEND` sets offset=size.
Read: `min(count, size-offset)` bytes from the CRAM window. Write:
`offset ≥ capacity` → -27; clamp; copy; `size=max`; dirty. Close: if dirty,
commit size. Pocket persists CRAM at core exit, which the emulator doesn't need.

Quake's config sequence with an empty slot 8 [DER]:
`Sys_FileTime("./id1/config.cfg")` → `fopen("quake.cfg","rb")` (size 0 →
inferred 0) → `Sys_ConfigFileLooksQuake` reads 0 bytes → not Quake → fclose →
`fopen("quake.cfg","wb")` (O_WRONLY|O_CREAT|O_TRUNC) → fprintf
`"// Quake 3.0 config\n"` (20 B) → fclose (commits size 20) → later
`Sys_FileOpenRead` reads it → `exec config.cfg` runs one comment. **The contents
of slot 8 change cvars, so they change rendering. Pin them** (empty is the
reproducible default; a real user cfg changes viewsize, gamma, binds).

Saves: `Sys_OpenSaveSlot` (opendir/readdir, `d_ino-1 == 10+idx`). Not used in timedemo.

### 6.5 stat (statx 291)

Path mode opens as in §6.3 (the NV rules apply, so stat on `s6.sav` reports
262144), copies the size, then drops the fd. It does **not** run `sys_close`,
so there is no size commit. Missing → -2.

### 6.6 opendir / readdir (getdents64 61)

The root-dir fd lists registry entries in probe order (slots 1,2,3,4,5,8,10..19)
as `linux_dirent64 {u64 d_ino=slot+1; i64 d_off=pos+1; u16 reclen=align8(19+len+1); u8 d_type=8; name\0}`,
until the caller's buffer (musl: 2048) is full. Then 0. `dir_pos` persists per fd.

---------------------------------------------------------------------------

## 7. Workload recipe (worked example: Quake)

This section is app-specific by nature: it is how *one* app is driven
deterministically. The mechanisms it uses — argv, the `.ini` `ARGS` key, the
virtual clock quantum, console markers — are generic; the values are Quake's.
In qsim they are supplied by the app profile (`default_args`, `config:`,
`result_line`) and by the app's workload table
(`apps/quake/workloads/workloads.sh`).

### 7.1 argv

```
argv = { "quake.elf", "+timedemo", "demo1", "+host_framerate", "0.1" }   argc = 5
```

* This port never calls `COM_InitArgv`: `com_argv = argv`. `Cmd_StuffCmds_f`
  joins argv[1..] with spaces and splits commands at `+` and at **any `-`**, so
  arguments must not contain '-' (negative numbers are impossible).
* `quake.rc` (pak0, verified): `exec default.cfg` / `exec config.cfg` /
  `exec autoexec.cfg` / `stuffcmds` / `startdemos demo1 demo2 demo3`.
  stuffcmds *inserts* `timedemo demo1` and `host_framerate 0.1` ahead of
  `startdemos`. `startdemos` then sees `demoplayback` and sets `demonum=-1`, so
  the demo loop never restarts. `Host_Init` also appends `gamma 0.7`.
* `+host_framerate 0.1` makes `host_frametime` exactly `(float)0.1`,
  independent of `realtime` arithmetic (§7.4). timedemo itself sets
  `cl.time = cl.mtime[0]` in `CL_LerpPoint` and reads one demo message per host
  frame (`CL_GetMessage`).
* demo1: 975 messages (header `"2\n"` = forcetrack 2), about 970 rendered frames.

### 7.2 Sound

Leave it **on** (the default: production path). The cost is `S_LoadSound` WAV
decode during precache, per-frame `S_Update` spatialization (many
`mixer_set_vol_lr_h`), and ~75 audio drain service-call pairs per frame.
`+nosound 1` is possible (it runs after `S_Init`, so ambient sounds are already
precached) and removes most of that. It is not production-faithful. There is no
`-nosound`/`-nocdaudio` switch in this port.

### 7.3 CD audio

With no track files (the recommendation, and the likely state of a card without
a CD rip), the demo's `svc_cdtrack 2` → `CDAudio_Play(2)` does: `opendir("/")`
scan for d_ino 23 → `fopen("slot:22")` -2 → `config_get CDFMT` -2 → five literal
fopens -2 → `Con_Printf("CDAudio: track 2 (slot 22) not found…")` +
`CDAudio_DumpRoot()` (opendir + `stat` of every registered name + a Con_Printf
line each). **Those console lines appear as on-screen notify text for 3 s of
`realtime` (30 frames) and include slot sizes** (§6.1: keep slot 1/3 sizes
constant). Providing CD `.bin` files enables the async DMA path (`READ_ASYNC`
+ callback + `get_position_h`), which this spec models only loosely.

### 7.4 Timer readers and the virtual clock

Wall-clock reads reachable in the timedemo run [SRC/ELF]:

| reader | per host frame | affects rendering? |
|---|---|---|
| `main` loop `now_us = of_time_us()` (+ one `last_us` before the loop) | 1 (more while `dt < 1000 µs` spins) | **yes**: `time = min(dt·1e-6, 0.1)` → `realtime += time` → console notify expiry, `td_starttime`, `SCR_DrawNet` threshold, `host_frametime` when host_framerate is unset |
| `NET_Poll` → `SetNetTime` → `Sys_FloatTime` | 1 | no (net timeouts) |
| `Sys_FloatTime` others (`host_cmd`, `cl_parse` keepalive, net slist, `R_TimeRefresh`) | 0 in demo playback | no |
| `_Host_Frame`, `R_ScanEdges`, `D_DrawSurfaces`, `SCR_UpdateScreen`, `r_main`, `d_surf`, `r_edge`, `d_edge` profiling | 0 (`host_speeds=0`) | no |
| `cd_of.c` `of_time_ms` (async waits, quiet deadline) | 0 without CD async pending; `cd_quiet_until==0` short-circuits | only via CD behaviour |

**Virtual clock design [CHOICE]** (symbol-free, independent of the number of reads):

* Keep `T` (uint64 µs). `GET_US` returns `(uint32)T`, `GET_MS` returns `(uint32)(T/1000)`.
* Keep a per-ecall-site flag `read_since_advance[pc]`. On a timer ecall at
  `pc`: if `read_since_advance[pc]` is set, first do `T += Q`, then clear
  **all** flags. Set `read_since_advance[pc]` and return `T`.
* **`Q = 200 000 µs`** (strictly > 100 000). The main loop's
  `time = dt·1e-6f` is then always > 0.1f, so it is clamped to exactly `0.1f`,
  whether 1, 2 or 10 advances happened since the last iteration. `realtime`
  therefore grows by exactly `0.1f` per host frame, independent of how often
  anything else reads the clock. With `host_framerate 0.1`, `host_frametime` is
  `0.1f` too.
* Worked trace: `last_us` (site A) = T0. Loop site B first read: no flag, so it
  returns T0 → `dt=0` → `continue`. B again: flag set → `T+=Q` → `dt=Q` →
  frame. Inside the frame `Sys_FloatTime` (site C) reads once, no advance. The
  next B read advances. That is exactly one advance per iteration.
* Spin loops that poll the clock (CD waits, kernel-style deadlines) terminate,
  because each repeated read from the same site advances. The advance count
  only perturbs `T`, never `realtime`.
* Tie every other time-based model to `T`: audio ring drain (48 000 pairs/s),
  mixer positions.
* Wrap: 32-bit µs wraps after 4295 s ≈ 21 474 advances. demo1 uses ~1 000.
  For long runs, either accept the wrap (the code uses unsigned deltas) or use
  a smaller Q ≥ 100 001.

### 7.5 Frame boundary and end of run

* **Primary frame event: the GPU executing `CMD_FLIP` (0x42)** during publish
  (`GPU_CTRL` bit3), payload `{idx, token}`. Every rendered frame emits one,
  from `VID_Update` (CAP_FLIP path): `flush_span_batch(); [wait_flip if a
  present is pending]; token = of_gpu_flip_to(draw_idx); of_gpu_kick()`. All
  CPU pixel writes and all earlier GPU commands for that frame are complete at
  that instant. Snapshot then: `FB[idx]` (CPU 0x10000000+idx·0x100000,
  320×200, stride 320), the z-buffer (§8), the current 256-entry palette, and
  the ELF globals `host_framecount` (0x10481320 in baseline; use the symbol
  table per ELF) and `realtime` (0x1048131c) for alignment.
* The next frame then calls `video_acquire_next(idx, token)` in `VID_WaitSync`.
  Use that as a secondary sanity event.
* Kernel flips (`video_flip` service) happen only during VID_Init (3 clears)
  plus the force-present inside set_mode/set_color_mode. Record them separately.
* **Extra flips**: `Con_Printf` calls `SCR_UpdateScreen` whenever
  `cls.signon != SIGNONS && !scr_disabled_for_loading`. During init, level load
  and signon, one host frame can produce several flips, or none (loading
  plaque). In steady-state demo playback it is exactly one flip per host
  frame. Key records by flip sequence number and carry `host_framecount`.
* **End:** after the last message `fread` fails → `CL_StopPlayback` →
  `CL_FinishTimeDemo` (both inlined, no symbols) →
  `Con_Printf("%i frames %5.1f seconds %5.1f fps\n")` →
  `Sys_Printf` → stdout `writev(1,…)`. Stop the emulator when stdout matches
  `^\d+ frames +[0-9.]+ seconds +[0-9.]+ fps$`. After that the engine keeps
  running with the console shown (`cls.state=ca_disconnected`, `demonum=-1`)
  and never exits by itself.
* Fatal conditions: stdout `[quake] Sys_Error:` (the app then spins forever),
  `ebreak`, exit(93/94), a PC in BRAM.

### 7.6 Interrupts

On the device the vblank IRQ (kernel buffer-role sync, deferred palette
upload) is active, and the data-slot IRQ fires only for async reads. The 1 kHz
OS timer is **not** armed on os25 (HW mixer) and Quake installs no timer
callback or SIGALRM. None of these write app memory. The emulator runs with no
interrupts. (A cache model could optionally add periodic kernel D-cache
pollution.)

---------------------------------------------------------------------------

## 8. Where pixels and depth are written directly by the CPU (Quake)

Addresses [ELF baseline symbols; recompute per ELF from `app.nm`]:

| buffer | CPU address used for writes | how obtained |
|---|---|---|
| FB0/1/2 | `0x50000000/0x50100000/0x50200000` (uncached alias; FB content for 320×200 is +0..+63 999) | `video_buffer_addr(i)` → `of_uncached()` → `vid.buffer = vid.conbuffer = d_viewbuffer`; `screenwidth = vid.rowbytes = 320` |
| z-buffer | `d_pzbuffer = 0x50A78D80` (= `zbuffer_storage` 0x10A78D80 + 0x40000000); 16-bit LE words, width `d_zwidth=320`, 200 rows used (storage 320×240) | VID_Init: `zbuffer_cpu_cached=0` |
| warp buffer | `r_warpbuffer` = a 320×200 **stack array** in `R_RenderView_` (≈0x13A7xxxx); `d_viewbuffer` is redirected there while `r_dowarp` (underwater) | r_main.c:1129, d_init.c:139 |
| surface cache | mmap'd 5 MB block (texture source, CPU-built) | VID_Init malloc |
| palookup slot 0 | `0x504BC000` (uncached) = GPU colormap | of_gpu_palookup_upload |
| GPU command staging | `0x10870D80` (cached, 2×16 KB) | of_gpu_init |

CPU framebuffer/z writers in the timedemo path [SRC]:

* Alias models (monsters, weapons, items, viewmodel with `pq_noz_mode`):
  `D_PolysetDrawFinalVerts` (d_polyse.c:215, store at :243),
  `D_PolysetRecursiveTriangle` (:463, store at :532),
  `D_RasterizeAliasPolySmooth` (:965, `d_pdest` at :1011/:1110) feeding
  `D_PolysetDrawSpans8` / `_NoZ` (:788/:861). These write FB bytes and z words,
  z-testing against GPU-written depth. `D_PolysetFillSpans8` (:933) and a few
  solid runs go to the GPU as `CLEAR_RECT` (:952, :1325). All of them call
  `of_emit_prepare_framebuffer_for_cpu()` first (fence + wait).
* `D_DrawParticle` (d_part.c:62, store at :96): FB + z.
* `D_DrawZSpans` (d_edge.c) only when `pq_combined_z_active==0` (a surface not fully submitted via GPU z).
* `D_BeginDirectRect/D_EndDirectRect` (vid_of.c): 24×24 loading-disc save/draw/restore at (vid.width-24, 0) in **all three** FBs, around every `COM_LoadFile` once `draw_disc` exists.
* `D_WarpScreen` (d_scan.c:337+) prepares and then emits GPU spans reading the stack warp buffer.
* Kernel services clear FBs (video_init/set_mode/set_color_mode).

Everything else visible is GPU-drawn: world, sky, water, sprites, HUD, console, clears.

**Oracle recommendation:** raw command words contain ELF-dependent absolute
addresses (`.bss` statics like `zbuffer_storage`, stack warp buffer, span
record arrays), so hashing the command stream **does not** compare across ELFs.
Hash the *results* at each `CMD_FLIP`: `FB[idx][0..64000)`, `z[0..128000)`,
the palette, and optionally a CPU-write log canonicalized to
(buffer id, offset, value). That requires executing the GPU commands (qsim
`gpu.c`), at least CLEAR_RECT + 0x48 (both forms) + z modes.

---------------------------------------------------------------------------

## 9. Determinism risks

1. **Timer-read count** (fixed by §7.4): any design that advances time per read
   makes `realtime`, notify text and (without host_framerate) `host_frametime`
   depend on code shape.
2. **`caps->heap_size` depends on `.bss` end**: Quake heap =
   `floor_MB(heap − min(heap/8,8MB) − 6MB)` = 35 MB for baseline. It
   crosses to 34 MB if `.bss` grows by ≳0.54 MB; the surface cache is
   `floor_MB(heap/8)` = 5 MB. The hunk/zone/cache sizes then differ and the
   workload changes. For A/B, check that both ELFs compute the same MB values,
   or pin `heap_size` explicitly (then it is not device-exact for the other ELF).
3. **Console text with environment values**: `CDAudio_DumpRoot` slot sizes
   (quake.elf size!), "world path: …", "CD Audio Initialized (async DMA|sync)"
   (depends on `DMA_STAGE_ALLOC` success). Lines printed while `realtime > 0`
   render as notify text for 30 frames. Keep slot sizes and service answers
   constant across A/B.
4. **Initial `quake.cfg` contents** (slot 8) change cvars and therefore the image. Pin them.
5. **Buffer rotation model** (§3.3): instant vblank gives a 1,0,1,0 rotation;
   partial-redraw persistence follows from it. Fixed across A/B, not device-exact.
6. **GPU model completeness**: CPU alias/particle z-tests read GPU-written depth,
   so a GPU without z-writes changes CPU branch profiles and pixels. The GPU
   texture cache (`GPU_TEX_FLUSH`) can serve stale texels on hardware; model it
   or not, but identically for both runs.
7. **FP semantics**: `-ffast-math` builds; an optimized ELF may contract/reorder
   float ops, which legitimately changes plane coefficients and pixels. That is
   a real difference the oracle should catch. The emulator must be bit-exact
   IEEE single (including `fflags`) and deterministic. The device FPU is built
   with `--fpu-ignore-subnormal`, so device results can differ from IEEE for
   subnormal operands. qsim is not device-exact there.
8. **Uninitialized memory**: the device's stack, brk gap and alignment gap hold
   leftover SDRAM; the emulator zeroes them. Any app read of uninitialized
   stack would be nondeterministic on the device but deterministic in qsim.
9. **Audio ring / mixer** answers drive loop counts. Tie them to virtual time (§3.4).
10. **32-bit µs wrap** of the virtual clock on very long runs (§7.4).
11. **Command batching thresholds** (`OF_GPU_KICK_MIN_WORDS=512`,
    `PQ_GPU_SPAN_BATCH_MAX=256`, 4095-word stream) are compiled into the ELF.
    The flush count differs legitimately between ELFs if code changes emission
    order. Compare rendered results, not publish counts.
12. **Alternative device state**: if GETFILE fails for an empty NV slot, the
    device registers `"slot:8"` and `fopen("quake.cfg")` fails (so no config at
    all). The emulator picks the named variant.

## 10. Open questions

* The os25 `hw_features = 0x01A7E35B` is derived from RTL and variant macros,
  not observed. A UART log of Quake's `GPU caps: hw=` line from a real Pocket
  would confirm it (the shipped rbf predates the current `gpu_core.v`).
* The shipped `runtime/pocket/os.bin` differs from a local rebuild (same
  strings). Layout constants (`APP_STACK_TOP` 0x13A80000, `mmap_bottom`
  0x13A00000, FB bases, services order) come from source at HEAD. Checking the
  shipped binary's `services_table_init` order and `syscall_set_app_stack_top`
  math by disassembly (base 0x10320000) is still to do.
* Does the os25 VexiiRiscv raise misaligned load/store traps (mcause 4/6)? The
  kernel has an emulation handler (`kernel/misaligned.c`). If it does, every
  misaligned `lw/sw/flw/fsw/lh/sh` in the app is a ~hundreds-of-instructions
  kernel trap on the device. qsim should at least count such accesses.
* APP_BRAM PMA: `app.ld` says APP_BRAM is uncached (main=0) and fetches bypass
  the I-cache; the shared brief says fetches still go through the I-cache.
  This ELF has no BRAM code, so it only matters for future `OF_FASTTEXT` builds.
* The exact per-pixel perspective divide and Q29 numerics of `gpu_core.v`
  (segment length, reciprocal LUT) are owned by the GPU-model agent (`gpu.h`
  header claims RTL-exact). This spec only fixes the wire format and address rules.
* Device timing vs virtual time for the audio ring (real frames are ~60 ms, the
  virtual step is 100 ms): the drain model is a choice, not device-exact.
* On a real card, are nonvolatile slots with no file present in the datatable
  at all, and does GETFILE return their instance filename? (This decides
  whether `quake.cfg` resolves.)

---------------------------------------------------------------------------

## 11. Errata found during integration (added by the integration agent)

These points were verified by running the baseline ELF under `qsim`. The
implementation is in `env.c`, and README "Integrated tool" has the details.

* **§5.1 is wrong: the app does touch sysreg.**
  * `CL_ParseServerMessage` reads `SYS_CYCLE_LO` at 0x40000004 directly (cl_parse.c:862). It is a 50 000-cycle busy wait before each svc command while `cls.signon < SIGNONS`. demo1 performs 1.98 M reads.
  * `Host_EndGame`/`Host_Error` write `TERM_FB_CTRL` 0x4000000C = 0 (host.c:110,161). demo1 ends with `svc_disconnect`, which goes through `Host_EndGame` before `CL_FinishTimeDemo` prints the result line.
  * `qsim` models the cycle counter as instret × 3 and records TERM_FB_CTRL writes.
* **§8 z-buffer at the flip:** `R_RenderView` queues the z `CLEAR_RECT` for the next frame *before* `VID_Update`'s `CMD_FLIP` (r_main.c:1217). The z-buffer is therefore always zero at the flip. Snapshot depth just before that clear executes.
* **§7.3 console text:** the `Exe: __TIME__ __DATE__` line (host.c:984) is visible on the console in the first ~12 flips. qsim normalizes it in the loaded image.
* **§4.3 particles:** `r_drawparticles` defaults to 0 in this port (r_main.c:134), so `D_DrawParticle` never runs.
