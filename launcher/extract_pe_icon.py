#!/usr/bin/env python3
"""Extract the first RT_GROUP_ICON from a PE (.exe) file and reconstruct a
standalone .ico file, with no external dependencies (icoutils/pefile are
not available in this environment). Reads only; never modifies the source
exe.
"""
import struct
import sys


def read_pe_sections(data):
    if data[:2] != b"MZ":
        raise ValueError("not a PE file (missing MZ)")
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        raise ValueError("missing PE signature")
    coff_off = e_lfanew + 4
    machine, num_sections, _, _, _, opt_hdr_size, _ = struct.unpack_from(
        "<HHIIIHH", data, coff_off)
    opt_hdr_off = coff_off + 20
    magic = struct.unpack_from("<H", data, opt_hdr_off)[0]
    is_pe32_plus = magic == 0x20B
    # Data directory count is at a fixed offset depending on PE32 vs PE32+.
    num_rva_and_sizes_off = opt_hdr_off + (108 if is_pe32_plus else 92)
    num_rva_and_sizes = struct.unpack_from("<I", data, num_rva_and_sizes_off)[0]
    data_dir_off = num_rva_and_sizes_off + 4
    if num_rva_and_sizes < 3:
        raise ValueError("no resource data directory present")
    # Data directory entry index 2 = resource table (RVA, Size).
    res_rva, res_size = struct.unpack_from("<II", data, data_dir_off + 2 * 8)

    sections = []
    sec_off = opt_hdr_off + opt_hdr_size
    for i in range(num_sections):
        entry = data[sec_off + i * 40: sec_off + i * 40 + 40]
        name = entry[0:8].rstrip(b"\0").decode("ascii", "replace")
        vsize, vaddr, raw_size, raw_ptr = struct.unpack_from("<IIII", entry, 8)
        sections.append((name, vaddr, vsize, raw_ptr, raw_size))
    return res_rva, res_size, sections


def rva_to_offset(rva, sections):
    for name, vaddr, vsize, raw_ptr, raw_size in sections:
        if vaddr <= rva < vaddr + max(vsize, raw_size):
            return raw_ptr + (rva - vaddr)
    raise ValueError(f"RVA 0x{rva:x} not found in any section")


IMAGE_RESOURCE_DIRECTORY_SIZE = 16
IMAGE_RESOURCE_DIRECTORY_ENTRY_SIZE = 8
RT_ICON = 3
RT_GROUP_ICON = 14


def read_dir(data, res_base, dir_off):
    """Returns list of (id_or_name, is_subdir, offset) for one resource
    directory level, where `offset` is relative to res_base."""
    characteristics, timestamp, maj, minr, n_named, n_id = struct.unpack_from(
        "<IIHHHH", data, res_base + dir_off)
    entries = []
    entry_base = res_base + dir_off + IMAGE_RESOURCE_DIRECTORY_SIZE
    for i in range(n_named + n_id):
        name_or_id, offset_to_data = struct.unpack_from(
            "<II", data, entry_base + i * IMAGE_RESOURCE_DIRECTORY_ENTRY_SIZE)
        is_subdir = bool(offset_to_data & 0x80000000)
        offset = offset_to_data & 0x7FFFFFFF
        is_named = bool(name_or_id & 0x80000000)
        if is_named:
            # Named entries not needed for RT_ICON/RT_GROUP_ICON lookup here.
            ident = name_or_id & 0x7FFFFFFF
        else:
            ident = name_or_id
        entries.append((ident, is_subdir, offset))
    return entries


def find_type_dir(data, res_base, type_id):
    top = read_dir(data, res_base, 0)
    for ident, is_subdir, offset in top:
        if ident == type_id and is_subdir:
            return offset
    return None


def first_leaf_data_entry(data, res_base, dir_off):
    """Walk Name-level then Language-level to the first IMAGE_RESOURCE_DATA_ENTRY,
    returning (rva, size)."""
    name_level = read_dir(data, res_base, dir_off)
    if not name_level:
        return None
    _, is_subdir, name_offset = name_level[0]
    if not is_subdir:
        raise ValueError("expected a name-level subdirectory")
    lang_level = read_dir(data, res_base, name_offset)
    if not lang_level:
        return None
    _, is_subdir2, data_offset = lang_level[0]
    if is_subdir2:
        raise ValueError("unexpected extra directory level")
    rva, size, codepage, reserved = struct.unpack_from(
        "<IIII", data, res_base + data_offset)
    return rva, size


def all_ids_in_type(data, res_base, type_dir_off):
    """List every (id, name_dir_offset) directly under a type directory."""
    return read_dir(data, res_base, type_dir_off)


def data_entry_from_lang_dir(data, res_base, lang_dir_off):
    """`lang_dir_off` already points at a Language-level directory (one
    level below Name) -- its entries point straight at data, not another
    subdirectory."""
    lang_level = read_dir(data, res_base, lang_dir_off)
    if not lang_level:
        return None
    _, is_subdir, data_offset = lang_level[0]
    if is_subdir:
        raise ValueError("expected a language-level data entry, got another subdirectory")
    rva, size, codepage, reserved = struct.unpack_from(
        "<IIII", data, res_base + data_offset)
    return rva, size


def extract_icon(exe_path, ico_path):
    with open(exe_path, "rb") as f:
        data = f.read()
    res_rva, res_size, sections = read_pe_sections(data)
    res_base = rva_to_offset(res_rva, sections)

    group_type_off = find_type_dir(data, res_base, RT_GROUP_ICON)
    icon_type_off = find_type_dir(data, res_base, RT_ICON)
    if group_type_off is None or icon_type_off is None:
        raise ValueError("no RT_GROUP_ICON/RT_ICON resources found in this exe")

    # First group icon (first name-level entry under RT_GROUP_ICON).
    group_entries = all_ids_in_type(data, res_base, group_type_off)
    if not group_entries:
        raise ValueError("RT_GROUP_ICON has no entries")
    _, is_subdir, name_off = group_entries[0]
    rva, size = first_leaf_data_entry(data, res_base, group_type_off)
    grp_file_off = rva_to_offset(rva, sections)
    grp_data = data[grp_file_off: grp_file_off + size]

    # GRPICONDIR: reserved(2) type(2) count(2), then GRPICONDIRENTRY[count]:
    # width(1) height(1) colorcount(1) reserved(1) planes(2) bitcount(2)
    # bytesinres(4) id(2)  == 14 bytes each.
    reserved, rtype, count = struct.unpack_from("<HHH", grp_data, 0)
    entries = []
    for i in range(count):
        off = 6 + i * 14
        w, h, cc, res, planes, bitcount, bytesinres, icon_id = struct.unpack_from(
            "<BBBBHHIH", grp_data, off)
        entries.append((w, h, cc, planes, bitcount, bytesinres, icon_id))

    # Map RT_ICON id -> raw image bytes.
    icon_type_entries = all_ids_in_type(data, res_base, icon_type_off)
    id_to_dir_off = {ident: off for ident, is_subdir, off in icon_type_entries}

    images = []
    for w, h, cc, planes, bitcount, bytesinres, icon_id in entries:
        lang_off2 = id_to_dir_off.get(icon_id)
        if lang_off2 is None:
            raise ValueError(f"RT_ICON id {icon_id} referenced but not found")
        rva2, size2 = data_entry_from_lang_dir(data, res_base, lang_off2)
        # size2 should match bytesinres; trust bytesinres from the group entry.
        img_off = rva_to_offset(rva2, sections)
        img_bytes = data[img_off: img_off + bytesinres]
        images.append((w, h, cc, planes, bitcount, img_bytes))

    # Build standalone ICONDIR + ICONDIRENTRY[] + image data.
    out = bytearray()
    out += struct.pack("<HHH", 0, 1, len(images))
    header_size = 6 + 16 * len(images)
    data_offset = header_size
    entry_bytes = bytearray()
    image_bytes = bytearray()
    for w, h, cc, planes, bitcount, img in images:
        entry_bytes += struct.pack(
            "<BBBBHHII", w, h, cc, 0, planes, bitcount, len(img), data_offset)
        image_bytes += img
        data_offset += len(img)
    out += entry_bytes
    out += image_bytes

    with open(ico_path, "wb") as f:
        f.write(out)
    return [(w, h, bitcount) for w, h, cc, planes, bitcount, img in images]


if __name__ == "__main__":
    exe_path, ico_path = sys.argv[1], sys.argv[2]
    sizes = extract_icon(exe_path, ico_path)
    print(f"Wrote {ico_path} with {len(sizes)} image(s):")
    for w, h, bitcount in sizes:
        print(f"  {w}x{h} @ {bitcount}bpp")
