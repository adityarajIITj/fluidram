import struct, gzip, subprocess

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

script = b'''#!/bin/sh
/bin/busybox --install -s /bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs dev /dev

modprobe virtio_net
ifconfig eth0 up
udhcpc -i eth0 -n -t 2
echo "nameserver 1.1.1.1" > /etc/resolv.conf
mkdir -p /etc/apk /lib/apk/db
touch /lib/apk/db/installed
echo "https://dl-cdn.alpinelinux.org/alpine/v3.20/main" > /etc/apk/repositories
echo "https://dl-cdn.alpinelinux.org/alpine/v3.20/community" >> /etc/apk/repositories

apk update 2>&1
apk add --no-cache python3 2>&1
python3 -c "print('PYTHON IN GUEST WORKS SUCCESSFULLY!')"
poweroff -f
'''

data = make_cpio_entry('init', script, 0o100755) + make_trailer()
with gzip.open('qemu/custom_init.cpio.gz', 'wb') as f: f.write(data)
with open('qemu/initramfs-virt', 'rb') as f1, open('qemu/custom_init.cpio.gz', 'rb') as f2:
    with open('qemu/combined.cpio.gz', 'wb') as fout: fout.write(f1.read() + f2.read())

qemu_exe = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
cmd = [
    qemu_exe,
    '-m', '512M',
    '-kernel', 'qemu/vmlinuz-virt',
    '-initrd', 'qemu/combined.cpio.gz',
    '-netdev', 'user,id=net0',
    '-device', 'virtio-net-pci,netdev=net0',
    '-append', 'console=ttyS0 quiet panic=-1',
    '-nographic',
    '-serial', 'mon:stdio',
    '-no-reboot'
]

res = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
print('STDOUT:\n', res.stdout[-1500:])
