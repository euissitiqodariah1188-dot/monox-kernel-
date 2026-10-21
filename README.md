# MONOX Kernel v1.0.0 "Singleblaze"

**ARM64 (ARMv8-A → ARMv9-A) Kernel**  
Lightweight · Gaming-ready · Mobile-first

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                        MONOX KERNEL                             │
├──────────────┬─────────────────────────────────────────────────┤
│  BOOT LAYER  │  head.S → MMU enable → monox_start()           │
├──────────────┼─────────────────────────────────────────────────┤
│  SECURITY    │  Traffic Satpam │ System Satpam (AV + Integrity)│
├──────────────┼─────────────────────────────────────────────────┤
│  SCHEDULER   │  MLFQ 16-level │ Per-CPU run queues │ SMP       │
├──────────────┼─────────────────────────────────────────────────┤
│  MEMORY      │  PMM (Buddy) │ VMM │ Slab/kmalloc               │
│              │  RAM Split: ZONE_DMA │ ZONE_GPU │ ZONE_NORMAL   │
├──────────────┼─────────────────────────────────────────────────┤
│  FILESYSTEM  │  VFS │ MonoxFS (.Runap .Config) │ 2 partitions  │
├──────────────┼─────────────────────────────────────────────────┤
│  INIT SYSTEM │  PID 1 │ Parallel startup │ .socket .target .timer│
├──────────────┼─────────────────────────────────────────────────┤
│  GRAPHICS    │  DRM │ OpenGL ES (GPU-detected) │ Vulkan 1.x    │
├──────────────┼─────────────────────────────────────────────────┤
│  DRIVERS     │  GPU │ CPU │ USB │ WiFi │ + 50 everyday drivers │
├──────────────┼─────────────────────────────────────────────────┤
│  INPUT       │  Touchscreen │ Mouse (USB-C/3.x) │ Keyboard     │
└──────────────┴─────────────────────────────────────────────────┘
```

---

## Key Design Decisions

### No Hardcoded Addresses
All hardware base addresses (UART, GIC, GPU, USB, WiFi, sensors) are
parsed from the **Device Tree Blob (DTB)** passed by the bootloader.

### RAM Split (> 4 GiB)
If the device has more than 4 GiB of RAM:
- `ZONE_DMA`    → 0 .. 4 GiB (accessible by all DMA masters)
- `ZONE_GPU`    → 4 .. 8 GiB (exclusive GPU memory for maximum performance)
- `ZONE_NORMAL` → 8 GiB+ (CPU-only, high memory)

### Storage Partitioning
MonoxFS splits storage into two mandatory partitions:
- **Partition 0 (SYSTEM)** → mounted at `/`  (kernel, OS, read-only)
- **Partition 1 (USER)**   → mounted at `/user` (apps, data, read-write)

### File Types
| Extension | Purpose |
|-----------|---------|
| `.Runap`  | Runnable application bundle |
| `.Config` | System or user configuration file |

### Security: Dual Satpam
| Guard | Role |
|-------|------|
| **Traffic Satpam** | Controls hardware access routing, blocks BIOS/UEFI access from untrusted code, scans for code injection |
| **System Satpam**  | Kernel integrity (SHA-256 of .text), malware signature scanner, syscall argument validator, process quarantine |

---

## Building

### Requirements
- `aarch64-linux-gnu-gcc` (or `aarch64-none-elf-gcc`)
- GNU Make
- GNU Binutils for AArch64

### Build
```bash
# Default (aarch64-linux-gnu-)
make

# Custom toolchain
make CROSS_COMPILE=aarch64-none-elf-

# Output files
build/out/monox.elf   - ELF with debug symbols
build/out/monox.img   - Boot image (Linux ARM64 protocol)
build/out/monox.bin   - Raw binary
build/monox.map       - Linker map
```

---

## Boot Flow

```
Bootloader (UEFI/U-Boot)
  │
  ├─ Loads monox.img to RAM
  ├─ Passes DTB physical address in x0
  └─ Jumps to _start (head.S)
        │
        ├─ Detect EL (drop to EL1 from EL2 if needed)
        ├─ Zero BSS
        ├─ Set VBAR_EL1 (exception vectors)
        ├─ Detect CPU features (ARMv8/v9)
        ├─ Early UART init (from DTB)
        └─ monox_start(dtb_phys)
              │
              ├─ Parse DTB
              ├─ MMU enable (4-level page tables, 48-bit VA)
              ├─ PMM init (buddy allocator, RAM zone split)
              ├─ VMM + kmalloc (slab allocator)
              ├─ GICv3 interrupt controller
              ├─ Security (Traffic Satpam + System Satpam)
              ├─ Scheduler (MLFQ, per-CPU)
              ├─ VFS + MonoxFS mount
              ├─ GPU, USB, WiFi, Touchscreen drivers
              ├─ SMP: boot secondary CPUs
              ├─ Harden kernel memory permissions
              └─ PID 1: monox_init (parallel service startup)
```

---

## Bootloader Note

The GRUB config (`boot/grub/grub.cfg`) checks for an OS-ready marker at boot:
- **Kernel-only** → GRUB displays an error (this is a kernel, not a full OS)
- **Full OS installed** → boots normally

---

## Supported Hardware

| Category | Supported |
|----------|-----------|
| CPU | ARM Cortex-A (any ARMv8.0–ARMv9.2) |
| GPU | Mali Bifrost/Valhall/5th-gen, Adreno 5xx–7xx, PowerVR |
| RAM | 1 GiB – 256 GiB |
| Storage | NVMe, eMMC, UFS, NAND |
| Display | MIPI-DSI, HDMI, DisplayPort |
| Input | Touchscreen (GT9xx, HID-I2C), USB Mouse, USB Keyboard |
| USB | USB-C, USB 3.0, USB 3.2 (xHCI) |
| Network | Any WiFi chip (cfg80211 interface), Ethernet |
| Sensors | Accelerometer, Gyroscope, Proximity, Light, Baro, GPS |
| Security | TrustZone, MTE (ARMv8.5+), BTI (ARMv8.5+) |

---

## License

MIT License — see individual source files.

---

*MONOX Kernel — built for speed, built for safety.*


## Quick start (QEMU)

```
make CROSS_COMPILE=aarch64-linux-gnu-
make run          # qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a57 -nographic -kernel build/out/monox.img
make qemu         # clean rebuild with raw boot markers (1 2 3 M V) on the PL011, then run
```

The same flat image boots on bare metal and QEMU: it discovers its own load address
(QEMU puts it at RAM base + 2 MiB) and everything else (RAM, UART, GICv3, PSCI conduit)
from the device tree. See `CHANGELOG-BUGFIX.md` (section H) for the boot flow and
for how to read the progress markers when a boot stops silently.
