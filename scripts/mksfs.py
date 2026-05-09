#!/usr/bin/python3
"""Create or populate a Simple Filesystem (SFS) disk image.

Usage:
    mksfs.py format <image> <size_mb>
        Create a blank SFS image of <size_mb> megabytes.

    mksfs.py populate <image> <directory>
        Populate an existing SFS image with files from <directory>.

    mksfs.py info <image>
        Print superblock and directory information.

On-disk layout (512-byte sectors):
    Sector 0         - Superblock
    Sector 1..B      - Bitmap (1 bit per sector)
    Sector B+1..D    - Directory table (256 entries x 64 bytes)
    Sector D+1..N    - Data region
"""

import math
import os
import struct
import sys

SECTOR_SIZE = 512
SFS_MAGIC = 0x5346534D  # "SFSM"
SFS_VERSION = 1
SFS_MAX_DIRENTS = 256
SFS_NAME_MAX = 47
SFS_TYPE_FREE = 0
SFS_TYPE_FILE = 1
SFS_TYPE_DIR = 2
SFS_ROOT_PARENT = 0xFFFF

# struct sfs_super: 10 x u32 + 472 reserved = 512 bytes
SUPER_FMT = "<IIIIIIIIII472s"
SUPER_SIZE = struct.calcsize(SUPER_FMT)
assert SUPER_SIZE == SECTOR_SIZE

# struct sfs_dirent: name[48] + u32 size + u32 start_sector + u8 type
#                    + u8 flags + u16 parent_idx + 4 reserved = 64 bytes
DIRENT_FMT = "<48sIIBBH4s"
DIRENT_SIZE = struct.calcsize(DIRENT_FMT)
assert DIRENT_SIZE == 64


class SFSImage:
    """In-memory representation of an SFS disk image."""

    def __init__(self, data: bytearray):
        self.data = data
        self.total_sectors = len(data) // SECTOR_SIZE
        self._read_super()

    def _read_super(self):
        raw = self.data[:SUPER_SIZE]
        fields = struct.unpack(SUPER_FMT, raw)
        self.magic = fields[0]
        self.version = fields[1]
        self.sb_total_sectors = fields[2]
        self.bitmap_start = fields[3]
        self.bitmap_sectors = fields[4]
        self.dir_start = fields[5]
        self.dir_sectors = fields[6]
        self.data_start = fields[7]
        self.n_dirents = fields[8]
        self.free_sectors = fields[9]

    def _write_super(self):
        raw = struct.pack(
            SUPER_FMT,
            self.magic,
            self.version,
            self.sb_total_sectors,
            self.bitmap_start,
            self.bitmap_sectors,
            self.dir_start,
            self.dir_sectors,
            self.data_start,
            self.n_dirents,
            self.free_sectors,
            b"\x00" * 472,
        )
        self.data[:SUPER_SIZE] = raw

    def read_dirent(self, idx):
        """Read directory entry at index."""
        entries_per_sector = SECTOR_SIZE // DIRENT_SIZE
        sector = self.dir_start + idx // entries_per_sector
        off = (idx % entries_per_sector) * DIRENT_SIZE
        base = sector * SECTOR_SIZE + off
        raw = self.data[base : base + DIRENT_SIZE]
        fields = struct.unpack(DIRENT_FMT, raw)
        name_bytes = fields[0].split(b"\x00", 1)[0]
        return {
            "name": name_bytes.decode("utf-8", errors="replace"),
            "size": fields[1],
            "start_sector": fields[2],
            "type": fields[3],
            "flags": fields[4],
            "parent_idx": fields[5],
        }

    def write_dirent(self, idx, name, size, start_sector, dtype, flags, parent_idx):
        """Write directory entry at index."""
        entries_per_sector = SECTOR_SIZE // DIRENT_SIZE
        sector = self.dir_start + idx // entries_per_sector
        off = (idx % entries_per_sector) * DIRENT_SIZE
        base = sector * SECTOR_SIZE + off
        name_bytes = name.encode("utf-8")[:SFS_NAME_MAX]
        name_padded = name_bytes + b"\x00" * (48 - len(name_bytes))
        raw = struct.pack(
            DIRENT_FMT,
            name_padded,
            size,
            start_sector,
            dtype,
            flags,
            parent_idx,
            b"\x00" * 4,
        )
        self.data[base : base + DIRENT_SIZE] = raw

    def bitmap_get(self, sector):
        """Check if sector is marked allocated in the bitmap."""
        bit_off = sector
        bm_sector = self.bitmap_start + bit_off // (SECTOR_SIZE * 8)
        byte_off = (bit_off % (SECTOR_SIZE * 8)) // 8
        bit_pos = bit_off % 8
        base = bm_sector * SECTOR_SIZE + byte_off
        return bool(self.data[base] & (1 << bit_pos))

    def bitmap_set(self, sector, val):
        """Set or clear a sector's allocated bit in the bitmap."""
        bit_off = sector
        bm_sector = self.bitmap_start + bit_off // (SECTOR_SIZE * 8)
        byte_off = (bit_off % (SECTOR_SIZE * 8)) // 8
        bit_pos = bit_off % 8
        base = bm_sector * SECTOR_SIZE + byte_off
        if val:
            self.data[base] |= 1 << bit_pos
        else:
            self.data[base] &= ~(1 << bit_pos)

    def bitmap_alloc(self, count):
        """Allocate a contiguous run of sectors. Returns start sector or 0."""
        run_start = self.data_start
        run_len = 0
        for s in range(self.data_start, self.sb_total_sectors):
            if not self.bitmap_get(s):
                if run_len == 0:
                    run_start = s
                run_len += 1
                if run_len == count:
                    for i in range(run_start, run_start + count):
                        self.bitmap_set(i, True)
                    self.free_sectors -= count
                    return run_start
            else:
                run_len = 0
        return 0

    def alloc_dirent(self):
        """Find a free directory entry. Returns index or -1."""
        for i in range(self.n_dirents):
            d = self.read_dirent(i)
            if d["type"] == SFS_TYPE_FREE:
                return i
        return -1

    def find_or_create_dir(self, name, parent_idx):
        """Find existing directory child or create one."""
        for i in range(self.n_dirents):
            d = self.read_dirent(i)
            if (
                d["type"] == SFS_TYPE_DIR
                and d["parent_idx"] == parent_idx
                and d["name"] == name
            ):
                return i
        idx = self.alloc_dirent()
        if idx < 0:
            raise RuntimeError(f"No free directory entries for '{name}'")
        self.write_dirent(idx, name, 0, 0, SFS_TYPE_DIR, 0, parent_idx)
        return idx

    def create_file(self, name, parent_idx, data):
        """Create a file with the given data."""
        idx = self.alloc_dirent()
        if idx < 0:
            raise RuntimeError(f"No free directory entries for '{name}'")
        nsectors = math.ceil(len(data) / SECTOR_SIZE) if data else 0
        start = 0
        if nsectors > 0:
            start = self.bitmap_alloc(nsectors)
            if start == 0:
                raise RuntimeError(f"No space for {nsectors} sectors")
            # Write file data
            base = start * SECTOR_SIZE
            self.data[base : base + len(data)] = data
            # Zero remainder of last sector
            remainder = nsectors * SECTOR_SIZE - len(data)
            if remainder > 0:
                self.data[base + len(data) : base + nsectors * SECTOR_SIZE] = (
                    b"\x00" * remainder
                )
        self.write_dirent(idx, name, len(data), start, SFS_TYPE_FILE, 0, parent_idx)


def cmd_format(image_path, size_mb):
    """Create a blank SFS image."""
    total_bytes = int(size_mb) * 1024 * 1024
    total_sectors = total_bytes // SECTOR_SIZE

    bitmap_start = 1
    bitmap_sectors = math.ceil(total_sectors / (SECTOR_SIZE * 8))
    dir_start = bitmap_start + bitmap_sectors
    dir_bytes = SFS_MAX_DIRENTS * DIRENT_SIZE
    dir_sectors = math.ceil(dir_bytes / SECTOR_SIZE)
    data_start = dir_start + dir_sectors
    free_sectors = total_sectors - data_start

    data = bytearray(total_bytes)

    # Write superblock
    super_raw = struct.pack(
        SUPER_FMT,
        SFS_MAGIC,
        SFS_VERSION,
        total_sectors,
        bitmap_start,
        bitmap_sectors,
        dir_start,
        dir_sectors,
        data_start,
        SFS_MAX_DIRENTS,
        free_sectors,
        b"\x00" * 472,
    )
    data[:SUPER_SIZE] = super_raw

    # Mark reserved sectors allocated in bitmap
    img = SFSImage(data)
    for s in range(data_start):
        img.bitmap_set(s, True)

    # Create root directory entry at index 0
    img.write_dirent(0, "/", 0, 0, SFS_TYPE_DIR, 0, SFS_ROOT_PARENT)
    img._write_super()

    with open(image_path, "wb") as f:
        f.write(data)
    print(f"Formatted {image_path}: {total_sectors} sectors, {free_sectors} free")


def cmd_populate(image_path, directory):
    """Populate an SFS image with files from a directory."""
    with open(image_path, "rb") as f:
        data = bytearray(f.read())

    img = SFSImage(data)
    if img.magic != SFS_MAGIC:
        print(f"Error: {image_path} is not a valid SFS image", file=sys.stderr)
        sys.exit(1)

    file_count = 0
    for root, dirs, files in os.walk(directory):
        rel_root = os.path.relpath(root, directory)

        # Find parent dirent index
        if rel_root == ".":
            parent_idx = 0
        else:
            parts = rel_root.split(os.sep)
            parent_idx = 0
            for part in parts:
                parent_idx = img.find_or_create_dir(part, parent_idx)

        # Create files
        for fname in sorted(files):
            fpath = os.path.join(root, fname)
            with open(fpath, "rb") as f:
                fdata = f.read()
            img.create_file(fname, parent_idx, fdata)
            file_count += 1

        # Ensure directories exist
        for dname in sorted(dirs):
            img.find_or_create_dir(dname, parent_idx)

    img._write_super()

    with open(image_path, "wb") as f:
        f.write(img.data)
    print(f"Populated {image_path}: {file_count} files")


def cmd_info(image_path):
    """Print SFS image information."""
    with open(image_path, "rb") as f:
        data = bytearray(f.read())

    img = SFSImage(data)
    if img.magic != SFS_MAGIC:
        print(f"Error: {image_path} is not a valid SFS image", file=sys.stderr)
        sys.exit(1)

    print(f"SFS Image: {image_path}")
    print(f"  Magic:         0x{img.magic:08X}")
    print(f"  Version:       {img.version}")
    print(f"  Total sectors: {img.sb_total_sectors}")
    print(f"  Bitmap:        sector {img.bitmap_start}, {img.bitmap_sectors} sectors")
    print(f"  Directory:     sector {img.dir_start}, {img.dir_sectors} sectors")
    print(f"  Data:          sector {img.data_start}")
    print(f"  Max dirents:   {img.n_dirents}")
    print(f"  Free sectors:  {img.free_sectors}")
    print()
    print("Directory entries:")
    for i in range(img.n_dirents):
        d = img.read_dirent(i)
        if d["type"] != SFS_TYPE_FREE:
            type_str = {SFS_TYPE_FILE: "FILE", SFS_TYPE_DIR: "DIR"}.get(d["type"], "?")
            parent = (
                "root" if d["parent_idx"] == SFS_ROOT_PARENT else str(d["parent_idx"])
            )
            print(
                f"  [{i:3d}] {type_str:4s} parent={parent:>4s} "
                f"size={d['size']:>8d} start={d['start_sector']:>6d} "
                f"name={d['name']}"
            )


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    cmd = sys.argv[1]
    if cmd == "format":
        if len(sys.argv) != 4:
            print("Usage: mksfs.py format <image> <size_mb>")
            sys.exit(1)
        cmd_format(sys.argv[2], sys.argv[3])
    elif cmd == "populate":
        if len(sys.argv) != 4:
            print("Usage: mksfs.py populate <image> <directory>")
            sys.exit(1)
        cmd_populate(sys.argv[2], sys.argv[3])
    elif cmd == "info":
        if len(sys.argv) != 3:
            print("Usage: mksfs.py info <image>")
            sys.exit(1)
        cmd_info(sys.argv[2])
    else:
        print(f"Unknown command: {cmd}")
        print(__doc__)
        sys.exit(1)


if __name__ == "__main__":
    main()
