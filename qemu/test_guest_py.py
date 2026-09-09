import subprocess, gzip

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

echo "=== TESTING PYTHON3 IN QEMU GUEST ==="
/usr/bin/python3 -c "import sys; print('PYTHON3 IN QEMU IS FULLY OPERATIONAL:', sys.platform, sys.version)"
poweroff -f
"""

data = make_cpio_entry('init', script, 0o100755) + make_trailer()
with gzip.open('qemu/init_script.cpio.gz', 'wb') as f: f.write(data)

with open('qemu/full_initrd.cpio.gz', 'wb') as fout:
    with open('qemu/initramfs-virt', 'rb') as f1: fout.write(f1.read())
    with open('qemu/guest_python.cpio.gz', 'rb') as f2: fout.write(f2.read())
    with open('qemu/init_script.cpio.gz', 'rb') as f3: fout.write(f3.read())

qemu_exe = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
cmd = [
    qemu_exe,
    '-m', '256M',
    '-kernel', 'qemu/vmlinuz-virt',
    '-initrd', 'qemu/full_initrd.cpio.gz',
    '-append', 'console=ttyS0 quiet panic=-1',
    '-nographic',
    '-serial', 'mon:stdio',
    '-no-reboot'
]

res = subprocess.run(cmd, capture_output=True, text=True, timeout=20)
print('STDOUT:\n', res.stdout)
