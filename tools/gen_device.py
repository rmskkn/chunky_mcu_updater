#!/usr/bin/env python3
"""Device catalog generator: device/<part>/device.toml -> build artifacts.

Writes, into --out:
  geometry.h  the part's geometry, flash rules, timing and map as literal
              #defines, for lib/flash.h (which still checks the layout itself)
  memory.ld   a GNU ld MEMORY block with one region per map entry, at its
              absolute address, for linking the bootloader or an image

Only the standard library (tomllib, Python 3.11+). Every value is resolved
here, so the generated header holds numbers, not arithmetic.

--erase-size / --sector-size override the TOML for the test matrix, which
runs one part at several geometries; the erase size is a property of the
part, so overriding it is for testing only.
"""
import argparse
import os
import re
import sys
import tomllib

# The regions lib/ addresses by name, in the order it expects them.
REQUIRED = ["bootloader", "identity", "meta0", "meta1", "slot_a", "slot_b"]


class LayoutError(Exception):
    pass


def size_of(v, what):
    """Bytes from an int or a string like "15K" / "1M" / "4096"."""
    if isinstance(v, int) and not isinstance(v, bool):
        n = v
    elif isinstance(v, str):
        m = re.fullmatch(r"\s*(\d+)\s*([KkMm]?)\s*", v)
        if not m:
            raise LayoutError(f"{what}: cannot read size {v!r}")
        n = int(m.group(1)) * {"": 1, "K": 1024, "M": 1024 * 1024}[m.group(2).upper()]
    else:
        raise LayoutError(f"{what}: size must be an integer or a string, not {v!r}")
    if n <= 0:
        raise LayoutError(f"{what}: size must be positive")
    return n


def pow2(n):
    return n > 0 and n & (n - 1) == 0


def ceil_div(a, b):
    return -(-a // b)


def round_up(a, b):
    return ceil_div(a, b) * b


def resolve(doc, erase_override=None, sector_override=None):
    part = doc.get("part") or {}
    upd = doc.get("update") or {}
    name = part.get("name")
    if not isinstance(name, str) or not name:
        raise LayoutError("part.name is required")

    flash = size_of(part.get("flash_size"), "part.flash_size")
    erase = erase_override or size_of(part.get("erase_size"), "part.erase_size")
    sector = sector_override or size_of(upd.get("sector_size", erase), "update.sector_size")
    base_addr = part.get("flash_base", 0)
    if not isinstance(base_addr, int) or base_addr < 0:
        raise LayoutError("part.flash_base must be a non-negative integer")

    if not pow2(erase):
        raise LayoutError(f"erase size {erase} is not a power of two")
    if not pow2(sector):
        raise LayoutError(f"sector size {sector} is not a power of two")
    if sector % erase:
        raise LayoutError(f"sector size {sector} is not a whole number of {erase} B erase units")
    if sector > 32768:
        raise LayoutError("sector size over 32768: STEP_WRITE offsets are u16")
    if flash % erase:
        raise LayoutError(f"flash size {flash} is not a whole number of erase units")

    regions = doc.get("region")
    if not isinstance(regions, list) or not regions:
        raise LayoutError("no [[region]] entries")

    names = [r.get("name") for r in regions]
    for n in names:
        if not isinstance(n, str) or not re.fullmatch(r"[a-z][a-z0-9_]*", n):
            raise LayoutError(f"region name {n!r} must match [a-z][a-z0-9_]*")
    if len(set(names)) != len(names):
        raise LayoutError("region names must be unique")
    order = [n for n in names if n in REQUIRED]
    if order != REQUIRED:
        raise LayoutError(f"regions {REQUIRED} must all be present, in that order")

    rest = [r for r in regions if isinstance(r.get("size"), str) and r["size"].startswith("rest")]
    if rest and regions[-len(rest):] != rest:
        raise LayoutError("rest regions must come last")
    for r in rest:
        m = re.fullmatch(r"rest(?:/(\d+))?", r["size"])
        if not m or int(m.group(1) or 1) != len(rest):
            raise LayoutError(f"region {r['name']}: size {r['size']!r} must be rest/{len(rest)} "
                              f"to match the {len(rest)} rest regions")

    out = []
    cursor = 0
    rest_share = None
    for r in regions:
        rname = r["name"]
        align_kind = r.get("align", "erase")
        if align_kind not in ("erase", "sector"):
            raise LayoutError(f"region {rname}: align must be \"erase\" or \"sector\"")
        align = sector if align_kind == "sector" else erase
        cursor = round_up(cursor, align)
        journal = bool(r.get("journal", False))

        if r in rest:
            if rest_share is None:
                # Every rest region gets the same share of what is left from
                # here, rounded down to its alignment.
                aligns = [sector if x.get("align") == "sector" else erase for x in rest]
                if len(set(aligns)) != 1:
                    raise LayoutError("rest regions must share one alignment")
                if any(x.get("journal") for x in rest):
                    raise LayoutError("a rest region cannot carry a journal")
                rest_share = (flash - cursor) // len(rest) // align * align
                if rest_share <= 0:
                    raise LayoutError("no flash left for the rest regions")
            body = rest_share
        else:
            body = round_up(size_of(r.get("size"), f"region {rname}"), erase)

        size = body + (erase if journal else 0)
        out.append({
            "name": rname, "base": cursor, "size": size, "body": body,
            "journal": journal, "protect": bool(r.get("protect", False)),
        })
        cursor += size

    if cursor > flash:
        raise LayoutError(f"the map needs {cursor} B, the part has {flash} B")

    by = {r["name"]: r for r in out}
    if not by["meta0"]["journal"] or not by["meta1"]["journal"]:
        raise LayoutError("meta0 and meta1 need journal = true")
    if by["meta0"]["size"] != by["meta1"]["size"]:
        raise LayoutError("meta0 and meta1 must be the same size")
    if by["slot_a"]["size"] != by["slot_b"]["size"]:
        raise LayoutError("slot_a and slot_b must be the same size")
    for s in ("slot_a", "slot_b"):
        if by[s]["base"] % sector or by[s]["size"] % sector:
            raise LayoutError(f"{s} must start and end on a sector boundary")
    if by["identity"]["size"] != erase:
        raise LayoutError("identity must be exactly one erase unit")

    prog = part.get("program") or {}
    timing = part.get("timing") or {}

    def rng(key, default):
        v = timing.get(key, default)
        if not (isinstance(v, list) and len(v) == 2 and all(isinstance(x, (int, float)) for x in v)):
            raise LayoutError(f"part.timing.{key} must be [min, max]")
        return float(v[0]), float(v[1])

    return {
        "name": name, "flash_base": base_addr, "flash": flash,
        "erase": erase, "sector": sector, "regions": out, "by": by,
        "program_once": bool(prog.get("once", True)),
        "zero_exempt": bool(prog.get("zero_exempt", True)),
        "erase_ms": rng("erase_ms", [20.0, 40.0]),
        "prog_us": rng("prog_us", [20.0, 40.0]),
        "mhz": float(timing.get("mhz", 72.0)),
        "sha256_cyc_b": float(timing.get("sha256_cyc_b", 25.0)),
    }


def geometry_h(g, src):
    e = g["erase"]
    by = g["by"]
    meta_hdr_units = by["meta0"]["body"] // e
    L = [
        f"/* Generated by tools/gen_device.py from {src}. Do not edit. */",
        "#ifndef CB_DEVICE_GEOMETRY_H",
        "#define CB_DEVICE_GEOMETRY_H",
        "#include <stddef.h>",
        "",
        f'#define CB_DEVICE_NAME "{g["name"]}"',
        f"#define CB_FLASH_BASE  0x{g['flash_base']:08X}u   /* address of flash offset 0 */",
        "",
        "/* Erase unit: what the part can erase. Sector: the delta unit. */",
        f"#define FLASH_SIZE   {g['flash']}",
        f"#define ERASE_SIZE   {e}",
        f"#define SECTOR_SIZE  {g['sector']}",
        "",
        "/* Program rules, enforced by flash_program() in lib/flash.c. */",
        f"#define CB_FLASH_PROGRAM_ONCE  {int(g['program_once'])}",
        f"#define CB_FLASH_ZERO_EXEMPT   {int(g['zero_exempt'])}",
        "",
        "/* Timing model: [min, max] per erase unit and per half-word program. */",
        f"#define CB_FLASH_ERASE_MS_MIN  {g['erase_ms'][0]!r}",
        f"#define CB_FLASH_ERASE_MS_MAX  {g['erase_ms'][1]!r}",
        f"#define CB_FLASH_PROG_US_MIN   {g['prog_us'][0]!r}",
        f"#define CB_FLASH_PROG_US_MAX   {g['prog_us'][1]!r}",
        f"#define CB_FLASH_MHZ           {g['mhz']!r}",
        f"#define CB_FLASH_SHA256_CYC_B  {g['sha256_cyc_b']!r}",
        "",
        "/* The map: byte offsets from the start of flash. */",
    ]
    for r in g["regions"]:
        n = r["name"].upper()
        L.append(f"#define CB_REGION_{n}_BASE  0x{r['base']:05X}u")
        L.append(f"#define CB_REGION_{n}_SIZE  {r['size']}u")
    L += [
        "",
        "/* Every region in map order, for code that walks the map without",
        " * naming regions: X(name, base, size, protect). */",
        "#define CB_REGION_LIST(X) \\",
    ]
    L += [f"    X(\"{r['name']}\", CB_REGION_{r['name'].upper()}_BASE, "
          f"CB_REGION_{r['name'].upper()}_SIZE, {int(r['protect'])}) \\"
          for r in g["regions"]]
    L += ["    /* end */"]
    L += [
        "",
        "/* The names lib/flash.h reads. Meta banks and the units before them",
        " * are in erase units; the slots are in bytes. */",
        f"#define BL_UNITS        {by['bootloader']['size'] // e}",
        f"#define ID_UNIT         {by['identity']['base'] // e}",
        f"#define META_HDR_UNITS  {meta_hdr_units}   /* meta_t area */",
        f"#define META_UNITS      {by['meta0']['size'] // e}   /* + the journal's own unit */",
        f"#define META0_UNIT      {by['meta0']['base'] // e}",
        f"#define META1_UNIT      {by['meta1']['base'] // e}",
        f"#define SLOT_A_BASE     ((size_t)0x{by['slot_a']['base']:05X})",
        f"#define SLOT_B_BASE     ((size_t)0x{by['slot_b']['base']:05X})",
        f"#define SLOT_SIZE       {by['slot_a']['size']}",
        "",
        "#endif",
        "",
    ]
    return "\n".join(L)


def memory_ld(g, src):
    L = [f"/* Generated by tools/gen_device.py from {src}. Do not edit. */",
         "MEMORY", "{"]
    w = max(len(r["name"]) for r in g["regions"])
    for r in g["regions"]:
        attr = "r" if r["name"].startswith("meta") or r["name"] == "identity" else "rx"
        L.append(f"  {r['name'].upper():<{w}} ({attr:<2}) : ORIGIN = 0x{g['flash_base'] + r['base']:08X}, "
                 f"LENGTH = {r['size']}")
    L += ["}", ""]
    return "\n".join(L)


def write(path, text):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write(text)
    os.replace(tmp, path)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("toml", help="device/<part>/device.toml")
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--erase-size", type=int, help="override part.erase_size (testing only)")
    ap.add_argument("--sector-size", type=int, help="override update.sector_size")
    a = ap.parse_args(argv)

    try:
        with open(a.toml, "rb") as f:
            doc = tomllib.load(f)
        g = resolve(doc, a.erase_size, a.sector_size)
    except (OSError, tomllib.TOMLDecodeError, LayoutError) as e:
        print(f"gen_device: {a.toml}: {e}", file=sys.stderr)
        return 1

    os.makedirs(a.out, exist_ok=True)
    write(os.path.join(a.out, "geometry.h"), geometry_h(g, a.toml))
    write(os.path.join(a.out, "memory.ld"), memory_ld(g, a.toml))
    return 0


if __name__ == "__main__":
    sys.exit(main())
