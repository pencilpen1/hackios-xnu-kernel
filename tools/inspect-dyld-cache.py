#!/usr/bin/env python3
"""Read-only arm64 dyld_v1 cache inspection, including split files and ObjC methods.

Addresses are unslid unless --slide is explicitly supplied. This tool does not
modify/extract the system image or infer that a nearby symbol caused a crash.
"""
import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import struct
import uuid


@dataclass
class Mapping:
    address: int
    size: int
    offset: int
    path: Path
    delta_mask: int = 0
    value_add: int = 0


class Cache:
    def __init__(self, directory):
        directory = Path(directory)
        self.main = directory / "dyld_shared_cache_arm64"
        self.mappings = []
        self.selector_base = None
        for path in sorted(directory.glob("dyld_shared_cache_arm64*")):
            if ".symbols" in path.name:
                continue
            with path.open("rb") as file:
                header = file.read(512)
                if header[:16] != b"dyld_v1   arm64\0":
                    raise ValueError("unsupported cache: " + str(path))
                offset, count = struct.unpack_from("<II", header, 16)
                if not 0 < count < 100 or offset + count * 32 > path.stat().st_size:
                    raise ValueError("invalid mapping table")
                file.seek(offset)
                maps = [Mapping(*struct.unpack("<QQQII", file.read(32))[:3], path) for _ in range(count)]
                slide_offset, slide_count = struct.unpack_from("<II", header, 312)
                for i in range(slide_count):
                    file.seek(slide_offset + i * 56)
                    address, size, pos, info, info_size, flags, maxprot, prot = struct.unpack("<6QII", file.read(56))
                    if not info_size:
                        continue
                    file.seek(info)
                    version = struct.unpack("<I", file.read(4))[0]
                    if version != 2:
                        raise ValueError("unsupported pointer slide version: " + str(version))
                    file.seek(info + 24)
                    mask, add = struct.unpack("<QQ", file.read(16))
                    match = next(m for m in maps if (m.address, m.size, m.offset) == (address, size, pos))
                    match.delta_mask, match.value_add = mask, add
                self.mappings.extend(maps)
        with self.main.open("rb") as file:
            header = file.read(512)
            offset, count = struct.unpack_from("<QQ", header, 136)
            if not 0 < count < 100000 or offset + count * 32 > self.main.stat().st_size:
                raise ValueError("invalid image table")
            file.seek(offset)
            table = file.read(count * 32)
            self.images = []
            for i in range(count):
                uid, address, size, path = struct.unpack_from("<16sQII", table, i * 32)
                file.seek(path)
                name = file.read(4096).split(b"\0")[0].decode("utf-8")
                self.images.append({"path": name, "address": address, "size": size,
                                    "uuid": str(uuid.UUID(bytes=uid))})

    def mapping(self, address, size=1):
        if size < 0:
            raise ValueError("negative read size")
        matches = [m for m in self.mappings if m.address <= address and address + size <= m.address + m.size]
        if len(matches) != 1:
            raise ValueError("unmapped/ambiguous VM read: " + hex(address))
        return matches[0]

    def read_vm(self, address, size):
        mapping = self.mapping(address, size)
        with mapping.path.open("rb") as file:
            file.seek(mapping.offset + address - mapping.address)
            data = file.read(size)
        if len(data) != size:
            raise ValueError("truncated cache read")
        return data

    def pointer(self, address):
        mapping = self.mapping(address, 8)
        raw = struct.unpack("<Q", self.read_vm(address, 8))[0]
        value = raw & ~mapping.delta_mask
        return value + mapping.value_add if value else 0

    def string(self, address):
        mapping = self.mapping(address)
        data = self.read_vm(address, min(4096, mapping.address + mapping.size - address))
        if b"\0" not in data:
            raise ValueError("unterminated string")
        return data.split(b"\0")[0].decode("utf-8")

    def image_at(self, address):
        return next(i for i in self.images if i["address"] <= address < i["address"] + i["size"])

    def commands(self, image):
        magic, cpu, subtype, kind, count, size, flags, reserved = struct.unpack("<8I", self.read_vm(image["address"], 32))
        if magic != 0xfeedfacf or cpu != 0x100000c or count > 1000 or size > 1048576:
            raise ValueError("unsupported Mach-O header")
        commands = self.read_vm(image["address"] + 32, size)
        pos = 0
        for _ in range(count):
            command, length = struct.unpack_from("<II", commands, pos)
            if length < 8 or pos + length > size:
                raise ValueError("invalid load command")
            yield command, commands[pos:pos + length]
            pos += length

    def sections(self, image):
        for command, body in self.commands(image):
            if command == 0x19:
                count = struct.unpack_from("<I", body, 64)[0]
                if 72 + count * 80 > len(body):
                    raise ValueError("invalid section table")
                for i in range(count):
                    name, segment, address, size = struct.unpack_from("<16s16sQQ", body, 72 + i * 80)
                    yield name.rstrip(b"\0").decode(), address, size

    def methods(self, image, class_name):
        for section, address, size in self.sections(image):
            if section != "__objc_classlist":
                continue
            for pos in range(address, address + size, 8):
                cls = self.pointer(pos)
                ro = self.pointer(cls + 32) & ~7
                if self.string(self.pointer(ro + 24)) != class_name:
                    continue
                for instance, target in ((True, cls), (False, self.pointer(cls))):
                    ro = self.pointer(target + 32) & ~7
                    methods = self.pointer(ro + 32)
                    if not methods:
                        continue
                    flags, count = struct.unpack("<II", self.read_vm(methods, 8))
                    relative = bool(flags & 0x80000000)
                    direct = bool(flags & 0x40000000)
                    stride = flags & 0xfffc
                    if stride not in (12, 24) or count > 10000:
                        raise ValueError("unsupported ObjC method list")
                    for j in range(count):
                        entry = methods + 8 + j * stride
                        if relative:
                            name, types, imp = struct.unpack("<iii", self.read_vm(entry, 12))
                            if direct:
                                # dyld's direct small methods name selectors relative
                                # to objc_opt_t, not relative to the method entry.
                                if self.selector_base is None:
                                    objc = next(i for i in self.images if i["path"].endswith("/libobjc.A.dylib"))
                                    opt = next(a for n, a, z in self.sections(objc) if n == "__objc_opt_ro")
                                    version = struct.unpack("<I", self.read_vm(opt, 4))[0]
                                    if version != 16:
                                        raise ValueError("unsupported ObjC optimization version")
                                    self.selector_base = opt + struct.unpack("<q", self.read_vm(opt + 40, 8))[0]
                                name += self.selector_base
                            else:
                                name = self.pointer(entry + name)
                            types += entry + 4
                            imp += entry + 8
                        else:
                            name, types, imp = (self.pointer(entry + k) for k in (0, 8, 16))
                        yield {"class": class_name, "instance": instance, "selector": self.string(name),
                               "types": self.string(types), "imp": hex(imp)}

    def local_symbols(self, path, image):
        # A production cache's regular symbol table says <redacted> for most
        # functions. Verify the separately extracted symbols UUID before use.
        with self.main.open("rb") as file:
            file.seek(400)
            expected_uuid = file.read(16)
        with Path(path).open("rb") as file:
            header = file.read(512)
            if header[:16] != b"dyld_v1   arm64\0" or header[88:104] != expected_uuid:
                raise ValueError("symbol file UUID does not match cache")
            offset, size = struct.unpack_from("<QQ", header, 72)
            if offset + size > Path(path).stat().st_size:
                raise ValueError("truncated local symbols")
            file.seek(offset)
            nlist, ncount, strings, ssize, entries, count = struct.unpack("<6I", file.read(24))
            if count > 100000 or entries + count * 16 > size or nlist + ncount * 16 > size or strings + ssize > size:
                raise ValueError("invalid local symbol table")
            file.seek(offset + entries)
            table = file.read(count * 16)
            for i in range(count):
                address, start, number = struct.unpack_from("<QII", table, i * 16)
                if address != image["address"] - 0x180000000:
                    continue
                if start + number > ncount:
                    raise ValueError("invalid image symbol range")
                file.seek(offset + nlist + start * 16)
                symbols = file.read(number * 16)
                for j in range(number):
                    name, kind, section, desc, value = struct.unpack_from("<IBBHQ", symbols, j * 16)
                    if (kind & 0xe) != 0xe or not value or name >= ssize:
                        continue
                    file.seek(offset + strings + name)
                    text = file.read(min(4096, ssize - name)).split(b"\0")[0].decode("utf-8")
                    yield value, text
                return
            raise ValueError("image missing from local symbols")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--address", type=lambda v: int(v, 0))
    parser.add_argument("--slide", type=lambda v: int(v, 0), default=0)
    parser.add_argument("--image", help="exact image path, or its unique basename")
    parser.add_argument("--class-name")
    parser.add_argument("--symbols", type=Path, help="optional matching .symbols file for exact local function names")
    args = parser.parse_args()
    cache = Cache(args.cache)
    if args.address is not None:
        unslid = args.address - args.slide
        image = cache.image_at(unslid)
    elif args.image:
        matches = [i for i in cache.images if i["path"] == args.image or Path(i["path"]).name == args.image]
        if len(matches) != 1:
            parser.error("image is missing or ambiguous")
        image = matches[0]
    else:
        parser.error("--address or --image is required")
    report = {"image": image}
    if args.address is not None:
        start = max(image["address"], unslid - 128)
        data = cache.read_vm(start, 256)
        report.update(runtime_address=hex(args.address), slide=hex(args.slide), unslid_address=hex(unslid),
                      window_address=hex(start), window_sha256=hashlib.sha256(data).hexdigest(), window_hex=data.hex())
    if args.class_name:
        report["methods"] = list(cache.methods(image, args.class_name))
    if args.symbols and args.address is not None:
        matches = sorted((a, n) for a, n in cache.local_symbols(args.symbols, image) if a <= unslid)
        report["nearest_symbols"] = [{"address": hex(a), "name": n, "offset": hex(unslid - a)} for a, n in matches[-5:]]
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
