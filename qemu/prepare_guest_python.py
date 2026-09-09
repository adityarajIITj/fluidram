import os, urllib.request, tarfile, io

PACKAGES = [
    'python3-3.12.13-r0.apk',
    'libffi-3.4.6-r0.apk',
    'mpdecimal-4.0.0-r0.apk',
    'readline-8.2.10-r0.apk',
    'sqlite-libs-3.45.3-r0.apk'
]

BASE_URL = 'https://dl-cdn.alpinelinux.org/alpine/v3.20/main/x86_64/'
DEST_DIR = os.path.join(os.path.dirname(__file__), 'guest_root')
os.makedirs(DEST_DIR, exist_ok=True)

print(f"Preparing guest root at: {DEST_DIR}")
for pkg in PACKAGES:
    url = BASE_URL + pkg
    print(f"Downloading {pkg}...")
    try:
        data = urllib.request.urlopen(url, timeout=15).read()
        # Alpine .apk is tar.gz containing data.tar.gz or raw tar files
        # Actually .apk is standard gzipped tar archive with .PKGINFO and root filesystem files
        with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as tar:
            for member in tar.getmembers():
                if member.name.startswith('.'):
                    continue
                tar.extract(member, path=DEST_DIR)
        print(f"  Extracted {pkg} successfully.")
    except Exception as e:
        print(f"  Failed {pkg}: {e}")

print("Guest root preparation complete.")
