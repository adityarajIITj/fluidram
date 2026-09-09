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

modprobe virtio_blk
mdev -s

mkdir -p /.modloop
mount -t squashfs /dev/vda /.modloop

M=/.modloop/modules/6.6.134-0-virt
echo "Loading compression modules from: $M"
insmod $M/kernel/lib/lz4/lz4_compress.ko
insmod $M/kernel/crypto/lz4.ko
insmod $M/kernel/drivers/block/zram/zram.ko num_devices=1

echo 536870912 > /sys/block/zram0/disksize
mkswap /dev/zram0
swapon /dev/zram0

echo "=== SWAP CONFIGURED ==="
free -m
echo "=== ZRAM MM_STAT ==="
cat /sys/block/zram0/mm_stat

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

res = subprocess.run(cmd, capture_output=True, text=True, timeout=15)
print('STDOUT:\n', res.stdout)
