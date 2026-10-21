# MONOX Kernel — laporan analisis & perbaikan bug

Basis: `monox-kernel.zip` asli. Semua komponen dibaca (boot, CPU/GIC, MMU, PMM, VMM,
kmalloc, printk, DTB, scheduler, VFS, MonoxFS, init, SATPAM ×2, GPU, USB, I2C,
touchscreen, mouse, keyboard, WiFi, drivers_all, graphics).

## Ringkasan

| Kategori | Sebelum | Sesudah |
|---|---|---|
| Error kompilasi (`gcc -fsyntax-only`, 25 file .c) | **43** | **0** |
| Simbol link yang tidak terdefinisi | `__data_load`, `__kernel_start`, `handle_sync_exception`, `handle_el0_sync`, `i2c_*`, `vfs_unmount`, `vfs_rmdir`, `mem*` out-of-line | 0 |
| Bug fatal saat boot (kernel tidak mungkin sampai ke `init`) | ~15 | 0 yang diketahui |
| Kebocoran memori / resource | 20+ jalur | semua jalur yang ditemukan ditutup |
| Uji host (ASAN/UBSan/LSan) | – | 8 suite, semua lulus (`tests/host/run_all.sh`) |

## ⚠️ Yang TIDAK bisa saya verifikasi

Sandbox saya hanya punya `gcc` x86-64 — **tidak ada cross-compiler/assembler AArch64, QEMU, atau perangkat keras.**

* `head.S`, `vectors.S`, dan blok `asm` di `kernel.h`/`sched.c`/`mmu.c`/`irq.c` **belum pernah saya
  assemble**. Saya me-review manual dan mengikuti pola Linux (ticket lock, LL/SC, vector table),
  tetapi ini yang pertama harus Anda build: `make CROSS_COMPILE=aarch64-linux-gnu-`.
* Logika C (PMM, kmalloc, VFS, MonoxFS, DTB, printf, scheduler, SHA-256) saya uji di host dengan
  shim; urutan akses register hardware (GIC, xHCI, MMU) hanya hasil review spesifikasi ARM/GIC/xHCI.
* Saran pengujian pertama: `qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a57 -m 1G -nographic -kernel build/out/monox.img`
  (UART PL011 di 0x9000000, RAM di 0x40000000 — cocok dengan asumsi linker script).

## A. Bug fatal (kernel crash / tidak boot)

| # | File | Bug | Perbaikan |
|---|---|---|---|
| 1 | `boot/vectors.S` | Tiap slot vector memuat seluruh save/call/restore (~42 instruksi > 32 yang muat di 128 B). Karena `.balign 0x80`, semua entry setelah yang pertama berada di offset salah → IRQ/fault pertama mengeksekusi tengah handler lain. | Slot = 1 `b`; handler di luar tabel. |
| 2 | `boot/monox.ld` | `__data_load` & `__kernel_start` tidak terdefinisi → link gagal. | Didefinisikan (image flat, tidak ada copy ROM→RAM). |
| 3 | `vectors.S`/`irq.c` | `handle_sync_exception` & `handle_el0_sync` tidak ada di mana pun → link gagal; fault tak terdekode. | Ditulis: decode ESR/FAR/ELR lalu panic dengan info lengkap. |
| 4 | `kernel.h` spinlock | `eor` tanpa `lsr #16` (deadlock saat kontensi), `stxr w1,w1` (UNPREDICTABLE), hanya 2 temp (butuh 3), layout `{u32 owner,next}` tidak cocok dengan asm. | Ditulis ulang (pola Linux), struct `{u16 owner,next}`. |
| 5 | `kernel.h` atomic | Memakai `stadd/ldadd` (LSE) padahal `-march=armv8-a` → assembler menolak; `atomic_fetch_add` tanpa loop & mengembalikan nilai baru; rwlock tidak mengembalikan counter saat gagal. | LL/SC baseline, `cmpxchg`, rwlock benar. |
| 6 | spinlock saat MMU off | `ldaxr/stxr` pada memori Device (MMU mati) hang di banyak core. | Lock jadi no-op sampai `mmu_enable()` (single CPU saat itu). |
| 7 | `mmu.c` | Hanya image kernel yang dipetakan: **RAM, UART, GIC tidak pernah dipetakan** → fault pertama di `pmm_init`. Tabel dijelajahi via alamat tinggi saat MMU masih mati; TTBR dihitung dari pointer fisik dikurangi offset virtual; descriptor *block* diperlakukan sebagai pointer tabel (PTE ditulis di atas memori nyata); tidak ada `unmap`. | Ditulis ulang: linear map RAM (blok 2 MiB), pemetaan MMIO sebelum enable, split block, `mmu_unmap_range`, IPS/AS sesuai CPU, flush cache tabel. |
| 8 | `pmm.c` | Header free-list ditulis **di atas image kernel, DTB, dan carve-out firmware** (reservasi baru dilakukan sesudahnya, dan `pmm_reserve_range` memang tidak bekerja). `PAGE_MASK` tak terdefinisi. | Tabel reservasi dipakai saat membangun free list; reserve sebelum/sesudah init berfungsi. |
| 9 | `sched.c` | `task_exit` membebaskan **stack yang sedang dipakai** (dan menolnya), membebaskan `struct task` yang masih `rq->current`, lalu switch *dari* memori bebas. | Reaping oleh task berikutnya setelah switch selesai. |
| 10 | `sched.c` | `vmm_alloc_pages(page_count)` dipakai sebagai *order*: stack init 64 KiB meminta order 16 (256 MiB) → PID 1 tidak pernah jalan. | Hitung order dari ukuran. |
| 11 | `sched.c` | `__sched_switch` pakai extended-asm yang inputnya boleh dialokasikan compiler di x19–x28 (yang ditimpa asm itu sendiri). | Rutin assembly murni. |
| 12 | `monoxfs.c` | Mount membaca blok 4096 B langsung ke `p->sb` (~340 B) → overflow heap ±3.7 KB di setiap mount. | Baca ke buffer sementara + validasi geometri. |
| 13 | `kmalloc.c` | Kelas 8 B menyimpan list node 16 B (merusak objek tetangga); kelas 4096 B punya 0 objek → `kmalloc(2049..4096)` = panic. | Kelas 16…2048; >2048 ke buddy. |
| 14 | `printk.c` | `early_uart_init()` tak pernah dipanggil setelah DTB di-parse → **tidak ada output sama sekali**. `kvsnprintf` tak mendukung `%-6s`, lebar `10/20`, `%.63s`, `%hhu`: argumen bergeser (pointer `%s` salah). Backtrace mengikuti `x29` sembarang (build `-fomit-frame-pointer`). | UART diinit ulang + log di-replay; printf lengkap (identik dengan libc di uji); backtrace tervalidasi + `-fno-omit-frame-pointer`. |
| 15 | `head.S` | SCTLR_EL1 hanya di-clear sebagian (bit UNKNOWN), VPIDR/VMPIDR/CNTHCTL/CPTR/`ICC_SRE_EL2` tidak diset sebelum turun ke EL1 (akses ICC trap ke EL2 → hang), secondary CPU poll slot 0 selalu & tidak pernah mengaktifkan MMU. | Diperbaiki semuanya. |
| 16 | `irq.c`/GIC | `GICR_WAKER` dibaca lewat frame SGI (salah frame); stride redistributor 0x10000 (seharusnya 0x20000); bit ARE salah (bit 5 bukan 4); tidak ada `IROUTER` (SPI tak pernah terkirim); PPI di-enable lewat distributor; EOI + `DIR` bersamaan; `ICC_SRE_EL1` tak di-set. | Semua diperbaiki. |
| 17 | `dtb.c` | `ts_dtb_props[32]` ditulis sampai indeks 60 (overflow stack); `u64*` pada data 4-byte-aligned (alignment fault MMU off); `#address/size-cells` dipulihkan dari level salah → ukuran `/memory` salah jika `/cpus` ada sebelumnya; node >16 level men-desync parser; tidak ada bounds-check; `interrupts` memakai nomor SPI mentah (INTID = SPI+32). | Parser ditulis ulang, di-fuzz 20 000 iterasi di bawah ASAN. |
| 18 | `main.c` | `struct board_info` didefinisikan dua kali; `t->pid` pada tipe incomplete; `__kernel_start` tak terdefinisi; board_info (besar) di stack 16 KiB. | Diperbaiki; board_info `static`. |

## B. Error kompilasi (43) — ringkas
Duplikat definisi tipe antara `.h` dan `.c` (`gpu_vendor_t`, `usb_speed_t`, `wifi_security_t`, `struct wifi_ops`, `ts_event`, `mouse_event`, `key_event`, `board_info`); `static` vs non-static (`gpu_irq_handler`, `usb_irq_handler`); `#define XHCI_USBCMD_HCRST(1 << 1)` (spasi hilang); `MX_ENOTDIR` tak ada; `hang()` & `unit_start_task` tanpa deklarasi; `satpam_result_t` tak terdeklarasi di `usb.c`; `struct vfs_stat` pertama muncul di dalam prototipe.

## C. Kebocoran memori / resource

| Lokasi | Kebocoran | Perbaikan |
|---|---|---|
| `kfree()` blok >4 KiB | Hanya mencetak error → bocor (DTB, bitmap FS, buffer besar) | Registry blok besar; `kfree`/`krealloc`/`kfree_pages` melepasnya |
| `pmm` zona | RAM setelah "hole" tak masuk zona → tak bisa di-free; RAM 4–8 GiB hilang bila total ≤4 GiB; split yang menyisakan ekor | Zona = span nyata; aturan 4/8 GiB benar |
| `vmm_free_region`/`vmm_unmap` | PTE tetap valid (use-after-free), VA bump-only tak pernah didaur ulang → tabel halaman habis → panic | Unmap nyata, free-list VA, tabel dari PMM bila pool habis |
| `vmm_*` jalur error | Mapping/region bocor; region_add gagal diabaikan | Rollback penuh |
| `vfs.path_resolve` | Dentry baru hanya punya 1 ref (dcache + caller) → `dentry_put` membebaskan dentry yang masih ada di list parent (UAF) | Ref dcache terpisah, dedupe race |
| `vfs_mount` | Inode mount-point ditimpa tanpa release; inode root dipakai 2 dentry tanpa ref (double free); `list_add_tail` pada node yang masih terhubung (list rusak) | Ref count benar, `covered_inode`, pindah child dengan benar |
| Tidak ada `vfs_unmount` / `fs->unmount` | bitmap, priv, root dentry bocor selamanya | Diimplementasikan (busy-check, prune dcache, flush) |
| `vfs_register_fs` | Duplikat di tiap `vfs_mount_root()` | `MX_EEXIST` |
| `vfs_open(CREAT)` | Parsing path menulis **sebelum** buffer untuk path kosong | `split_path()` tervalidasi |
| `MonoxFS` | Bitmap/superblock tak pernah ditulis balik (alokasi blok hilang tiap reboot); mount "ro" sebenarnya writable; baca stack tak-terinisialisasi saat error indirect-block; blok baru bocor isi disk lama | Flush saat sync/close/unmount, ro ditegakkan, error dicek, blok baru di-nol |
| `smp_boot_secondaries` | Stack 16 KiB per CPU dialokasikan lalu tak pernah dibebaskan | Dibebaskan bila CPU tak online |
| `gpu_init`/`usb_init`/`graphics_init` | Mapping MMIO, ring DMA, framebuffer bocor di jalur error & re-init | Unwind lengkap, guard re-init |
| `usb`/`kbd`/`mouse` | Slot device tidak pernah dipakai ulang (ENOSPC permanen) | Slot didaur ulang |
| `gpu_mem_alloc` | `return obj->handle` setelah unlock (UAF) | Handle disalin sebelum unlock |
| `drivers_all` | `ioremap` NULL tak dicek (tulis ke NULL+offset), window lama bocor saat re-init, `cpufreq` 0 entry baca OOB | Dicek / di-unmap |
| `wifi` | Parser membaca melewati terminator ke stack tak-terinisialisasi; PSK tersisa di stack | Dihentikan di terminator + scrub |

## D. Logika / keamanan
* `traffic_satpam`: aturan "allow" untuk `monox_init`/kernel berada **setelah** wildcard deny (first-match → tak pernah tercapai); loop `i < g_nr_rules` melewati rule setelah `remove_rule`; pola "smc #0" sebenarnya `SVC #0` (menandai semua program sebagai injeksi); PID counter mulai 2 sehingga "PID 1 trusted" tak pernah ada.
* `system_satpam`: `strncpy(.., name_sz - 1)` dengan `name_sz == 0` (overflow); `args == NULL` dideref; statistik quarantine salah; `-1` dianggap pointer kernel.
* `gpu_submit_commands`: `len > capacity` → memcpy melewati buffer; doorbell Mali `0x06` = HARD_RESET|CACHE_CLN (reset GPU tiap submit) → hanya cache-clean; cache tak di-flush.
* `usb`: controller dijalankan dengan DCBAAP/CRCR/ERST = 0 (DMA ke alamat fisik 0) → ring + scratchpad dialokasikan sebelum Run; IRQ handler memberi **buffer stack tak-terinisialisasi** ke callback HID (event acak).
* `task_wake` meng-enqueue task yang sedang berjalan dua kali; `schedule()` mengaktifkan IRQ sebelum switch.
* `timer_oneshot` hanya stub (callback langsung) → `task_sleep` = yield, task WiFi me-scan tanpa henti. Diganti timer virtual ARM (PPI 27); tanpa GIC yang berfungsi, perilaku lama dipertahankan agar tidak ada task yang tidur selamanya.
* `smp_processor_id()` (Aff0 & 0xFF) bisa mengindeks array 16 elemen di luar batas → di-clamp.
* `cpu.c`: `jscvt` salah field, deteksi ARMv9 salah (field EL1-handling bukan versi arsitektur), bit RES1 SCTLR dihapus, `M` di-set tanpa tabel di secondary CPU.
* MonoxFS: `monoxfs_dirent` 60 B (format 64 B), `monoxfs_inode` 228 B (format 256 B) → di-assert `_Static_assert`.

## E. Perubahan perilaku yang disengaja (cek kalau Anda bergantung padanya)
1. **Mount `"ro"` sekarang benar-benar read-only** (`MX_EACCES` pada write).
2. **PID pertama = 1** (sebelumnya 2) — memang yang diasumsikan SATPAM.
3. `init` **tidak lagi panic** bila root FS tak bisa di-mount (layer blok `blk_find_dev` di `main.c` masih stub → sebelumnya kernel *selalu* panic di ujung boot). Sekarang jalan dalam mode degraded tanpa unit.
4. Ukuran buddy terbesar 4 MiB → **32 MiB** (framebuffer 1080×2340 butuh ±10 MB).
5. Layout `spinlock_t` = `{u16 owner, u16 next}`.
6. Data HID dari USB tidak lagi dikirim ke callback (tadinya sampah); event ring di-drain & di-ack. Pengiriman data HID nyata butuh transfer ring (belum ada di driver aslinya).
7. `drivers/drivers_all.c`, `graphics/graphics.c`, `lib/string.c`, `drivers/i2c/i2c.c` **sekarang masuk build** (sebelumnya tidak ada di Makefile; I2C hanya fallback `weak` → `MX_ENODEV` sampai ada driver kontroler).
8. Flag build: `-mgeneral-regs-only -mstrict-align -fno-omit-frame-pointer -fno-tree-loop-distribute-patterns -MMD` (kode C berjalan dengan MMU mati di awal boot; GCC tidak boleh memakai SIMD/unaligned/`memset` rekursif).

## F. Keterbatasan yang masih ada (bukan bug yang saya perkenalkan)
* **Tidak ada driver block device** (`blk_find_dev` stub) dan **PSCI `CPU_ON`** tidak dipanggil → tidak ada storage dan SMP tidak bangun otomatis.
* Hanya GICv3 yang didukung (QEMU: wajib `gic-version=3`; DTB GICv2 → kernel jalan tanpa interrupt, bukan hang).
* Driver xHCI belum punya transfer ring/command handling; Adreno doorbell (`0x0110`) tetap placeholder.
* MonoxFS: double-indirect belum ada; tidak ada create/mkdir/unlink.
* Stack MonoxFS memakai beberapa buffer 4 KiB (aman di stack 64 KiB init, tidak di stack kecil).

## H. Boot di QEMU `aarch64 virt` — titik krusial yang ditemukan & diperbaiki (putaran kedua)

Ini analisis jalur boot khusus QEMU. **Belum dijalankan** (sandbox tanpa QEMU/toolchain); kecuali disebut, perbaikan diverifikasi lewat uji host (software page-table walker, fuzz DTB, dll.).

| # | Gejala di QEMU | Penyebab | Perbaikan |
|---|---|---|---|
| H1 | **Hang senyap, nol log** | `monox_printk` membaca `LEVEL_STR[]` (tabel *pointer*) saat MMU masih mati. Pointer statis berisi alamat link **tinggi** yang belum dipetakan → data abort pada `pr_info` pertama. | Tabel diganti matriks `char[8][6]` (diakses PC-relatif). Flag `-fno-jump-tables -fno-tree-switch-conversion` agar GCC tidak membuat tabel pointer sendiri dari `switch` di kode pra-MMU. |
| H2 | **Panic/hang segera setelah banner** | QEMU memuat Image ber-`text_offset=0` di **RAM base + 2 MiB** (0x40200000; 0x40000000 dipakai stub bootloader QEMU). Kode lama mengasumsikan image selalu di 0x40000000 dan `panic` bila tidak. | Image sekarang di-link di `0xFFFFFFC000000000` (tak bertabrakan dengan linear map) dan dipetakan ke alamat fisik *runtime* (`adr_l _start`). Biner yang sama jalan di 0x40000000, 0x40200000, 0x80080000. |
| H3 | **Loop tak berujung / crash acak di `list_*`, `kmalloc`, scheduler** | Kode berjalan di alias fisik (rendah) sementara inisialisasi statis (`LIST_HEAD_INIT`, tabel ops/string) berisi alamat tinggi; `head->next == head` membandingkan dua alias berbeda dan gagal. | `__enter_virt` (head.S) pindah ke alamat link tinggi tepat setelah MMU menyala (pola `__primary_switch` Linux): `monox_start()` (pra-MMU) → `monox_start_virt()` (alamat tinggi, VBAR tinggi, SP alias tinggi). Secondary CPU ikut lompat ke alamat tinggi. |
| H4 | GIC tidak ditemukan → tanpa interrupt/timer | DTB QEMU menamai GICv3 `intc@8000000` (kode lama hanya mencari `interrupt-controller@…`/`gic@…`), dan node anak `its@` (`arm,gic-v3-its`) bisa menimpa alamat GIC. | Deteksi lewat `compatible` **persis** `arm,gic-v3/v4`, dievaluasi di `END_NODE` (urutan properti bebas). UART juga via `arm,pl011`. Diuji dengan DTB meniru QEMU. |
| H5 | Poweroff/reset gagal (exception UNDEF) | PSCI selalu `smc`; QEMU `-M virt` tanpa EL2 memakai `hvc`. | Conduit dibaca dari `psci/method` di DTB. |
| H6 | Build gagal / sysreg tak dikenal pada binutils lama | Nama register GICv3 (`icc_*_el1`) | Diganti encoding numerik `S3_0_Cn_Cm_op2`. |
| H7 | Sulit tahu sampai mana boot berhenti | UART baru hidup setelah DTB di-parse | `make EARLY_CON=0x09000000`: penanda mentah `1 2 3` (head.S), `M` (MMU on), `V` (di alamat tinggi) + konsol C hidup sebelum parse DTB. |
| H8 | Hardening hanya separuh | Alias rendah dan tinggi image kini PTE berbeda | `mmu_protect_kernel` menerapkan permission ke **kedua** alias (diuji). |

### Menjalankan
```
make CROSS_COMPILE=aarch64-linux-gnu-          # atau aarch64-none-elf-
make run                                       # qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a57 -nographic -kernel build/out/monox.img
make qemu                                      # clean rebuild dengan penanda boot (EARLY_CON) lalu jalan
make run-el2                                   # boot di EL2 (menguji turun EL2->EL1)
make run-debug                                 # -S -s lalu: gdb-multiarch build/out/monox.elf -ex 'target remote :1234'
```
Catatan: pakai **`monox.img`** (flat), bukan `monox.elf`, untuk `-kernel`; wajib `gic-version=3` dan `-cpu cortex-a57` (atau `max`).

### Urutan log yang diharapkan
`123` (hanya jika `EARLY_CON`) → banner `MONOX KERNEL v1.0.0` → `DTB: …` → `MMU: enabled` → `M` → `V` → `MONOX: running at virtual address 0xffffffc0…` → `PMM: ZONE_DMA …` → `GIC: …` → `TIMER: generic virtual timer ready` → … → `INIT: cannot mount root filesystem … running without units` (normal: belum ada driver block).
Jika berhenti setelah `3` → masalah di C pra-MMU; setelah `M` → lompatan ke alamat tinggi (`__enter_virt`); setelah `V` → inisialisasi subsistem (lihat log terakhir).

## G. Uji host (`bash tests/host/run_all.sh`)
MMU: software page-table walker (alias tinggi + identity + linear map 1 GiB, split block, unmap, hardening dua alias; **mutation test** membuktikan bug lama tertangkap) ·
printf vs libc · SHA-256 vs hashlib (305 panjang) · parser DTB (+ fuzz 20 000 run, ASAN) ·
VFS+MonoxFS end-to-end (hole sparse, append, flush bitmap, busy-unmount, ro, superblok korup, **LeakSanitizer**) ·
PMM+kmalloc (RAM berlubang, reservasi tak tersentuh, 200 000 operasi acak, coalescing penuh, double-free, tanpa kebocoran) ·
scheduler (round-robin, sleep/wake, reap stack tepat sekali, tanpa UAF).
