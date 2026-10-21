#!/bin/bash
# Host-side regression tests for the logic of the kernel (x86-64 Linux, gcc,
# AddressSanitizer + UBSan). They cannot test AArch64 assembly or real
# hardware: see CHANGELOG-BUGFIX.md "What was NOT verified".
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; REPO="$(cd "$HERE/../.." && pwd)"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
python3 "$HERE/gen_host_headers.py" "$REPO" "$W/inc"
CF="-std=gnu11 -w -g -fsanitize=address,undefined -fno-sanitize=alignment -ffreestanding -fno-builtin -DMONOX_KERNEL -I$W/inc -I$REPO"
pass() { echo "  [PASS] $1"; }

echo "== printf (vs libc)"
python3 - "$REPO" "$HERE" "$W" <<'PY'
import sys
repo,here,w=sys.argv[1:4]
s=open(repo+'/kernel/printk.c').read()
a=s.index("struct pf_out"); b=s.index("/* -------------------------------------------------------\n * Kernel log ring buffer")
t=open(here+'/printf/t_template.c').read(); i=t.index("struct pf_out"); j=t.index("static int fails=0;")
open(w+'/pf.c','w').write(t[:i]+s[a:b]+"\n"+t[j:])
PY
gcc -w -o "$W/pf" "$W/pf.c" && "$W/pf" | tail -1 | grep -q "ALL OK" && pass printf

echo "== SHA-256 (vs hashlib)"
python3 - "$REPO" "$HERE" "$W" <<'PY'
import sys,subprocess,hashlib
repo,here,w=sys.argv[1:4]
s=open(repo+'/security/syscall_filter/system_satpam.c').read()
a=s.index("static const u32 K256[64]"); b=s.index("/* -------------------------------------------------------\n * sysatpam_init")
t=open(here+'/sha/sha_t_template.c').read(); i=t.index("static const u32 K256[64]"); j=t.index("int main")
open(w+'/sha.c','w').write(t[:i]+s[a:b].replace("static __always_inline","static inline")+"\n"+t[j:])
subprocess.check_call(['gcc','-O1','-w','-o',w+'/sha',w+'/sha.c'])
for n in list(range(0,300))+[1023,1024,4096,65537,1<<20]:
    x=12345; d=bytearray()
    for _ in range(n): x=(x*1664525+1013904223)&0xffffffff; d.append(x>>24)
    assert subprocess.check_output([w+'/sha',str(n)]).decode().strip()==hashlib.sha256(bytes(d)).hexdigest(), n
PY
pass sha256

echo "== DTB parser (+ 20000-run ASAN fuzz)"
( cd "$W" && python3 "$HERE/dtb/mkfdt.py" && gcc $CF -o dtbh "$HERE/dtb/harness.c" "$REPO/kernel/dtb.c" && ./dtbh | grep -q "ram=2147483648" && pass "dtb parse" \
  && gcc $CF -o dtbf "$HERE/dtb/fuzz.c" "$REPO/kernel/dtb.c" && ./dtbf | grep -q "no ASAN errors" && pass "dtb fuzz" )

echo "== VFS + MonoxFS (leak check on)"
( cd "$W" && gcc $CF -c "$HERE/fs/test.c" -o t.o && gcc -w -g -fsanitize=address -c "$HERE/fs/shim.c" -o s.o \
  && gcc $CF -c "$REPO/fs/vfs/vfs.c" -o v.o && gcc $CF -c "$REPO/fs/monoxfs/monoxfs.c" -o m.o \
  && gcc -fsanitize=address,undefined -o fst t.o s.o v.o m.o && ASAN_OPTIONS=detect_leaks=1 ./fst | grep -q PASSED && pass "vfs/monoxfs" )

echo "== PMM + kmalloc (simulated RAM with holes/reservations)"
( mkdir -p "$W/mmt/mm" "$W/mmt/arch/arm64/mm" && cd "$W/mmt" \
  && cp "$REPO"/mm/{pmm.c,pmm.h,kmalloc.c,vmm.h} mm/ && cp "$HERE/mm/fake_mmu.h" arch/arm64/mm/mmu.h \
  && CF2="$CF -I$W/mmt" && gcc $CF2 -c "$HERE/mm/test.c" -o t.o && gcc -w -g -fsanitize=address -c "$HERE/mm/shim.c" -o s.o \
  && gcc $CF2 -c mm/pmm.c -o p.o && gcc $CF2 -c mm/kmalloc.c -o k.o \
  && gcc -fsanitize=address,undefined -o mmt t.o s.o p.o k.o && ASAN_OPTIONS=detect_leaks=0 ./mmt | grep -q PASSED && pass "pmm/kmalloc" )

echo "== Scheduler (ucontext stand-in for the AArch64 switch routine)"
( mkdir -p "$W/sc/kernel/sched" "$W/sc/mm" "$W/sc/arch/arm64/cpu" && cd "$W/sc" \
  && cp "$REPO/mm/vmm.h" "$REPO/mm/pmm.h" mm/ && cp "$REPO/arch/arm64/cpu/cpu.h" arch/arm64/cpu/ && cp "$REPO/kernel/sched/sched.h" kernel/sched/ \
  && python3 - "$REPO" <<'PY'
import re,sys
s=open(sys.argv[1]+'/kernel/sched/sched.c').read()
a=s.index('__asm__(\n    ".text'); e='.size __sched_switch, .-__sched_switch \\n"\n);'; b=s.index(e)+len(e); s=s[:a]+s[b:]
a=s.index("void __attribute__((naked)) task_trampoline(void)"); b=s.index("/* -------------------------------------------------------\n * pick_next")
s=s[:a]+"void task_trampoline(void) {}\n\n"+s[b:]
s=s.replace('#include "../mm/vmm.h"','#include "mm/vmm.h"').replace('#include "../mm/pmm.h"','#include "mm/pmm.h"').replace('#include "../arch/arm64/cpu/cpu.h"','#include "arch/arm64/cpu/cpu.h"')
s=re.sub(r'__asm__ volatile\("mrs %0, cntvct_el0"\s*:\s*"=r"\(cnt\)\);','cnt = ++g_fake_cnt;',s)
s=re.sub(r'__asm__ volatile\("mrs %0, cntfrq_el0"\s*:\s*"=r"\(freq\)\);','freq = 1000000;',s)
open('kernel/sched/sched.c','w').write("static unsigned long long g_fake_cnt;\n"+s)
PY
  CF3="-std=gnu11 -w -g -fsanitize=address,undefined -fno-sanitize=alignment -ffreestanding -fno-builtin -DMONOX_KERNEL -I$W/inc -I$W/sc -I$REPO/include" && gcc $CF3 -c "$HERE/sched/test.c" -o t.o && gcc -w -g -fsanitize=address -c "$HERE/sched/shim.c" -o s.o \
  && gcc -fsanitize=address,undefined -o sct t.o s.o && ASAN_OPTIONS=detect_leaks=0:detect_stack_use_after_return=0 ./sct 2>/dev/null | grep -q PASSED && pass "scheduler" )

echo "== MMU page tables (software walker: image high+identity alias, linear map, split, unmap, hardening)"
( mkdir -p "$W/mmu/arch/arm64/mm" "$W/mmu/mm" && cd "$W/mmu" \
  && cp "$REPO/arch/arm64/mm/mmu.h" arch/arm64/mm/ && cp "$REPO/mm/pmm.h" mm/ \
  && python3 - "$REPO" <<'PY'
import re,sys
s=open(sys.argv[1]+'/arch/arm64/mm/mmu.c').read()
s=re.sub(r'__asm__ volatile\([^;]*\);','/* asm removed for host */',s)     # AArch64 asm cannot run on x86
s=s.replace('#include "../../../mm/pmm.h"','#include "mm/pmm.h"')
open('arch/arm64/mm/mmu.c','w').write(s)
PY
  CF4="-std=gnu11 -w -g -fsanitize=address,undefined -fno-sanitize=alignment -ffreestanding -fno-builtin -DMONOX_KERNEL -I$W/inc -I$W/mmu" \
  && gcc $CF4 -o mmut "$HERE/mmu/test.c" && ./mmut | grep -q PASSED && pass "mmu" )
echo "ALL HOST TESTS PASSED"
