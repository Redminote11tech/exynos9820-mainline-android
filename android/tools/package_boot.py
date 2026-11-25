#!/usr/bin/env python3
# Assemble a beyond2lte mainline boot image from measured stock header values.
#
# Boot chain: S-Boot loads boot.img kernel slot (= uniLoader payload) and jumps;
# uniLoader then boots the embedded mainline kernel + DTB, and patches the
# embedded ramdisk location into the DTB /chosen node. The boot.img ramdisk
# field is kept from the donor image so S-Boot sees a familiar layout; uniLoader
# ignores it and uses its embedded copy.
#
# Usage: package_boot.py <donor-boot.img> <uniLoader-payload> <out.img>
# The donor image must be the boot partition currently on the phone, so the
# ROM's ramdisk matches the installed ROM.
import gzip
import io
import pathlib
import struct
import subprocess
import sys

donor, payload, output = map(pathlib.Path, sys.argv[1:])

if output.resolve() in {p.resolve() for p in (donor, payload)}:
    raise SystemExit('Output must differ from every input.')

with donor.open('rb') as f:
    header = f.read(1648)
if len(header) != 1648 or header[:8] != b'ANDROID!':
    raise SystemExit('Expected Android boot image header v1.')

def u32(offset):
    return struct.unpack_from('<I', header, offset)[0]

if u32(40) != 1 or u32(36) != 2048:
    raise SystemExit('Donor header must be v1 with 2048-byte pages (beyond2lte).')
if u32(24) or u32(1632):
    raise SystemExit('Unexpected second-stage/extra payload in donor image.')

page = u32(36)
kernel_size = u32(8)
ramdisk_size = u32(16)
ramdisk_offset_in_file = (page + kernel_size + page - 1) // page * page
with donor.open('rb') as f:
    f.seek(ramdisk_offset_in_file)
    rom_ramdisk = f.read(ramdisk_size)
# Verify the ramdisk is complete gzip before repacking it.
with gzip.open(io.BytesIO(rom_ramdisk), 'rb') as g:
    while g.read(1024 * 1024):
        pass

with payload.open('rb') as f:
    loader = f.read()
# Boot partition on SM-G975F is 57,671,680 bytes; keep a 2 MiB margin.
boot_partition_limit = 57_671_680 - 2 * 1024 * 1024
cmdline = (header[64:576].split(b'\0', 1)[0] +
           header[608:1632].split(b'\0', 1)[0]).decode('ascii')

# Keep the ROM ramdisk in the boot.img ramdisk field: S-Boot loads it as usual,
# uniLoader ignores it in favor of its embedded copy (same bytes).
rom_ramdisk_path = output.with_suffix('.ramdisk.gz')
rom_ramdisk_path.write_bytes(rom_ramdisk)

args = [
    'mkbootimg',
    '--header_version', '1',
    '--kernel', str(payload),
    '--ramdisk', str(rom_ramdisk_path),
    '--base', '0',
    '--kernel_offset', hex(u32(12)),
    '--ramdisk_offset', hex(u32(20)),
    '--second_offset', hex(u32(28)),
    '--tags_offset', hex(u32(32)),
    '--pagesize', str(page),
    '--board', header[48:64].split(b'\0', 1)[0].decode('ascii'),
    '--cmdline', cmdline,
]
os_version = u32(44)
if os_version:
    a, b, c = (os_version >> 25) & 127, (os_version >> 18) & 127, (os_version >> 11) & 127
    month, year = os_version & 15, ((os_version >> 4) & 127) + 2000
    if not 1 <= month <= 12:
        raise SystemExit('Invalid donor OS patch-level encoding.')
    args += ['--os_version', f'{a}.{b}.{c}', '--os_patch_level', f'{year:04d}-{month:02d}']
args += ['-o', str(output)]
subprocess.run(args, check=True)

final_size = output.stat().st_size
print(f'BOOT_IMG_ASSEMBLED_UNTESTED: {output}')
print(f'size: {final_size} bytes (boot partition limit with margin: {boot_partition_limit})')
print(f'donor cmdline: "{cmdline}"')
print(f'ROM ramdisk: {ramdisk_size} bytes (also embedded inside uniLoader payload)')
if final_size > boot_partition_limit:
    output.unlink()
    rom_ramdisk_path.unlink(missing_ok=True)
    raise SystemExit('Assembled image exceeds the boot partition size; aborted.')
rom_ramdisk_path.unlink(missing_ok=True)
