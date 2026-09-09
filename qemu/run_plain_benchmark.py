import struct, gzip, subprocess, sys

def make_cpio_entry(name, content, mode=0o100755):
    filesize = len(content)
    namesize = len(name) + 1
    hdr = f'070701{0:08x}{mode:08x}{0:08x}{0:08x}{1:08x}{0:08x}{filesize:08x}{0:08x}{0:08x}{0:08x}{0:08x}{namesize:08x}{0:08x}'
    entry = hdr.encode('ascii') + name.encode('latin1') + b'\x00'
    if len(entry) % 4 != 0: entry += b'\x00' * (4 - (len(entry) % 4))
    entry += content
    if len(entry) % 4 != 0: entry += b'\x00' * (4 - (len(entry) % 4))
    return entry

def make_trailer():
    name = 'TRAILER!!!'
    namesize = len(name) + 1
    hdr = f'070701{0:08x}{0:08x}{0:08x}{0:08x}{1:08x}{0:08x}{0:08x}{0:08x}{0:08x}{0:08x}{0:08x}{namesize:08x}{0:08x}'
    entry = hdr.encode('ascii') + name.encode('latin1') + b'\x00'
    if len(entry) % 4 != 0: entry += b'\x00' * (4 - (len(entry) % 4))
    if len(entry) % 512 != 0: entry += b'\x00' * (512 - (len(entry) % 512))
    return entry

script = b"""#!/bin/sh
/bin/busybox --install -s /bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs dev /dev
mkdir -p /dev/shm
mount -t tmpfs -o size=600M tmpfs /dev/shm

modprobe virtio_blk
mdev -s

mkdir -p /.modloop
mount -t squashfs /dev/vda /.modloop

M=/.modloop/modules/6.6.134-0-virt
insmod $M/kernel/lib/lz4/lz4_compress.ko
insmod $M/kernel/crypto/lz4.ko
insmod $M/kernel/drivers/block/zram/zram.ko num_devices=1

echo 536870912 > /sys/block/zram0/disksize
mkswap /dev/zram0
swapon /dev/zram0

echo "=== PLAIN LINUX BENCHMARK START ==="
uname -a
echo "Physical DRAM Total:" $(grep MemTotal /proc/meminfo | awk '{print $2}') "kB"
echo "Configured Swap Total:" $(grep SwapTotal /proc/meminfo | awk '{print $2}') "kB"

# Helper function to print telemetry snapshot
dump_stats() {
    STAGE=$1
    echo "--- SNAPSHOT: $STAGE ---"
    free -m
    echo "ZRAM_MM_STAT:" $(cat /sys/block/zram0/mm_stat)
    echo "VMSTAT_PGFAULT:" $(grep pgfault /proc/vmstat | awk '{print $2}')
    echo "VMSTAT_PGMAJFAULT:" $(grep pgmajfault /proc/vmstat | awk '{print $2}')
    echo "VMSTAT_PSWPIN:" $(grep pswpin /proc/vmstat | awk '{print $2}')
    echo "VMSTAT_PSWPOUT:" $(grep pswpout /proc/vmstat | awk '{print $2}')
    echo "VMSTAT_OOM:" $(grep oom_kill /proc/vmstat | awk '{print $2}')
}

dump_stats "0_BASELINE"

# Generate realistic compressible data pattern (repeated telemetry blocks with delta counters)
# 64 MB chunk
echo "Allocating Phase 1: 64 MB..."
dd if=/dev/zero bs=1M count=64 2>/dev/null | tr '\\0' 'A' > /dev/shm/chunk_64m.dat
dump_stats "1_ALLOC_64MB"

# Allocate Phase 2: additional 96 MB (Total 160 MB, approaching physical RAM limit)
echo "Allocating Phase 2: additional 96 MB (Total 160 MB)..."
dd if=/dev/zero bs=1M count=96 2>/dev/null | tr '\\0' 'B' > /dev/shm/chunk_96m.dat
dump_stats "2_ALLOC_160MB"

# Allocate Phase 3: additional 128 MB (Total 288 MB, OVERCOMMIT beyond 218 MB physical DRAM!)
echo "Allocating Phase 3: additional 128 MB (Total 288 MB OVERCOMMIT)..."
dd if=/dev/zero bs=1M count=128 2>/dev/null | tr '\\0' 'C' > /dev/shm/chunk_128m.dat
dump_stats "3_ALLOC_288MB_OVERCOMMIT"

# Allocate Phase 4: additional 128 MB (Total 416 MB, ~200% OVERCOMMIT!)
echo "Allocating Phase 4: additional 128 MB (Total 416 MB HEAVY OVERCOMMIT)..."
dd if=/dev/zero bs=1M count=128 2>/dev/null | tr '\\0' 'D' > /dev/shm/chunk_416m.dat
dump_stats "4_ALLOC_416MB_HEAVY_OVERCOMMIT"

# Access/Readback phase: Force Linux to fault swapped pages back into RAM
echo "Executing Working-Set Traversal (Reading back 416 MB to trigger swap-in & page faults)..."
cat /dev/shm/chunk_64m.dat > /dev/null
cat /dev/shm/chunk_96m.dat > /dev/null
cat /dev/shm/chunk_128m.dat > /dev/null
cat /dev/shm/chunk_416m.dat > /dev/null
dump_stats "5_AFTER_WORKING_SET_TRAVERSAL"

echo "=== PLAIN LINUX BENCHMARK COMPLETE ==="
poweroff -f
"""

data = make_cpio_entry('init', script, 0o100755) + make_trailer()
with gzip.open('qemu/custom_init.cpio.gz', 'wb') as f: f.write(data)
with open('qemu/initramfs-virt', 'rb') as f1, open('qemu/custom_init.cpio.gz', 'rb') as f2:
    with open('qemu/combined.cpio.gz', 'wb') as fout: fout.write(f1.read() + f2.read())

qemu_exe = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
cmd = [
    qemu_exe,
    '-m', '256M',
    '-kernel', 'qemu/vmlinuz-virt',
    '-initrd', 'qemu/combined.cpio.gz',
    '-drive', 'file=qemu/modloop-virt,format=raw,if=virtio,readonly=on',
    '-append', 'console=ttyS0 quiet panic=-1',
    '-nographic',
    '-serial', 'mon:stdio',
    '-no-reboot'
]

print("Launching Plain Linux in QEMU (256 MB DRAM, 512 MB zram LZ4 swap)...")
res = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
print('STDOUT:\n', res.stdout)
