import os
import sys
import gzip
import subprocess
import time

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

# Create init script
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
mkswap /dev/zram0 >/dev/null 2>&1
swapon /dev/zram0

# Execute the raw guest benchmark
/usr/bin/python3 /guest_raw_benchmark.py

poweroff -f
"""

init_entry = make_cpio_entry('init', script, 0o100755) + make_trailer()
with gzip.open('qemu/init_script.cpio.gz', 'wb') as f:
    f.write(init_entry)

with open('qemu/final_benchmark_initrd.cpio.gz', 'wb') as fout:
    with open('qemu/initramfs-virt', 'rb') as f1: fout.write(f1.read())
    with open('qemu/guest_python.cpio.gz', 'rb') as f2: fout.write(f2.read())
    with open('qemu/init_script.cpio.gz', 'rb') as f3: fout.write(f3.read())

qemu_exe = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
cmd = [
    qemu_exe,
    '-m', '256M',
    '-kernel', 'qemu/vmlinuz-virt',
    '-initrd', 'qemu/final_benchmark_initrd.cpio.gz',
    '-drive', 'file=qemu/modloop-virt,format=raw,if=virtio,readonly=on',
    '-append', 'console=ttyS0 quiet panic=-1',
    '-nographic',
    '-serial', 'mon:stdio',
    '-no-reboot'
]

print("Starting QEMU Bare-Metal Linux Benchmark (256 MB DRAM)...")
t0 = time.time()
res = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
elapsed = time.time() - t0
print(f"QEMU execution finished in {elapsed:.2f} seconds.")

# Save raw output
with open('qemu/raw_benchmark_execution.log', 'w', encoding='utf-8') as f:
    f.write(res.stdout)
    if res.stderr:
        f.write("\nSTDERR:\n" + res.stderr)

print("QEMU stdout summary:")
print(res.stdout)
