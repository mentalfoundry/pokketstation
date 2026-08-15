#!/usr/bin/env python3
"""patch_choco_id.py — set the FF8 link ID in a Chocobo World save file.

FF8 links its save to a Chocobo World save by a 4-byte ID.
This tool reads that ID from an FF8 save and writes it into a Chocobo World
save so FF8 accepts the chocobo as its own.

Two-file mode (FF8 MCS + separate Chocobo World file):

    python3 patch_choco_id.py <ff8.mcs> <choco.mcs|choco.mcd> [output]

    ff8.mcs   FF8 single-save MCS, exactly 8320 bytes.
              The tool reads the link ID from byte offset 0x1588.

    choco.mcs|choco.mcd  Chocobo World save file.  Two sizes are accepted:
        57472 bytes  PSX single-save MCS exported by ChocoEdit.
                     Save banks are at fixed offsets 0x280 and 0x380.
       131072 bytes  Full 128 KB memory card image.
                     The tool scans the directory to find the Chocobo World
                     slot, then locates the banks.

    output    Optional.  Default: write back to the second argument after
              saving a .bak backup.

Single-card mode (FF8 save and Chocobo World on the same card):

    python3 patch_choco_id.py --card <card.mcd> [--ff8-slot N]

    --card card.mcd  Full 128 KB memory card image.  The tool auto-detects
                     the Chocobo World slot and, by default, the first
                     non-MCX slot that has a non-zero FF8 link ID.

    --ff8-slot N     Override auto-detection.  N is a directory slot number
                     (1–15).  The tool reads the link ID from that slot.

The link ID is a u32 stored at save_bank+0x28 in each save bank.
The active bank has the larger save counter at save_bank+0x08.
Both banks are patched so the file stays valid after a bank switch.
"""

import argparse
import struct
import sys
import shutil

# ── FF8 MCS ───────────────────────────────────────────────────────────────────
FF8_MCS_SIZE  = 8320     # 0x80 directory frame + one 0x2000 data block
FF8_ID_MCS    = 0x1588   # u32 LE link ID within the FF8 MCS buffer
FF8_ID_DATA   = 0x1508   # same offset relative to block data (0x1588 - 0x80)

# ── Chocobo World save banks (relative to each bank's base offset) ────────────
COUNTER_REL  = 0x08      # u32 LE save counter
FF8ID_REL    = 0x28      # u32 LE FF8 link ID

# ── Chocobo World PSX single-save MCS (57472 bytes) ──────────────────────────
# Layout: 0x80 directory frame, then seven 0x2000 data blocks.
# The first 0x200 bytes of block 1 hold the title sector.
# Save banks start at the end of the title sector.
CHOCO_MCS_SIZE = 57472
BANK_A_MCS     = 0x280   # 0x80 header + 0x200 title sector
BANK_B_MCS     = 0x380   # bank B is 0x100 bytes after bank A

# ── Full 128 KB memory card image (131072 bytes) ──────────────────────────────
CARD_SIZE    = 131_072
FRAME_SIZE   = 0x80
BLOCK_SIZE   = 0x2000
TITLE_SECTOR = 0x200     # bytes reserved for the PS1 title sector at block start

# ── Helpers ───────────────────────────────────────────────────────────────────

def u32(buf, off):
    return struct.unpack_from('<I', buf, off)[0]

def put_u32(buf, off, val):
    struct.pack_into('<I', buf, off, val)

def read_file(path):
    with open(path, 'rb') as f:
        return bytearray(f.read())

def write_file(path, data):
    with open(path, 'wb') as f:
        f.write(data)

# ── MCX app detection ─────────────────────────────────────────────────────────

def is_mcx_slot(buf, slot):
    """Return True if the directory slot holds a PocketStation MCX app."""
    frame_off = slot * FRAME_SIZE
    if buf[frame_off] != 0x51:
        return False
    if buf[frame_off + 0x10] != ord('P'):
        return False
    block_off = slot * BLOCK_SIZE
    if len(buf) < block_off + 0x56:
        return False
    return (buf[block_off + 0x52] == ord('M') and
            buf[block_off + 0x53] == ord('C') and
            buf[block_off + 0x54] == ord('X') and
            buf[block_off + 0x55] in (ord('0'), ord('1')))

def find_chocorpg_slot(buf):
    """Return (slot, block_offset) for the Chocobo World app slot, or (None, None)."""
    for slot in range(1, 16):
        if is_mcx_slot(buf, slot):
            return slot, slot * BLOCK_SIZE
    return None, None

# ── FF8 ID extraction ─────────────────────────────────────────────────────────

def ff8_id_from_mcs(buf, path):
    if len(buf) != FF8_MCS_SIZE:
        sys.exit(f"error: {path}: expected {FF8_MCS_SIZE} bytes, got {len(buf)}")
    ff8_id = u32(buf, FF8_ID_MCS)
    print(f"FF8 link ID 0x{ff8_id:08X}  (from {path})")
    return ff8_id

def ff8_id_from_card_slot(buf, slot):
    """Return the FF8 link ID stored in the data block at the given slot."""
    return u32(buf, slot * BLOCK_SIZE + FF8_ID_DATA)

def find_ff8_slot(buf):
    """Return the first non-MCX slot with state=0x51 and a non-zero FF8 link ID,
    or None if none is found.  Lists all candidates for transparency."""
    candidates = []
    for slot in range(1, 16):
        frame_off = slot * FRAME_SIZE
        if buf[frame_off] != 0x51:
            continue
        if is_mcx_slot(buf, slot):
            continue
        link_id = ff8_id_from_card_slot(buf, slot)
        candidates.append((slot, link_id))

    if not candidates:
        return None

    print("FF8 save slots found:")
    chosen = None
    for slot, link_id in candidates:
        tag = ""
        if link_id != 0 and chosen is None:
            chosen = slot
            tag = "  <- auto-selected (first non-zero ID)"
        print(f"  Slot {slot:2d}  FF8ID 0x{link_id:08X}{tag}")
    return chosen

# ── Bank patching ─────────────────────────────────────────────────────────────

def patch_banks(buf, base_a, base_b, ff8_id):
    counter_a = u32(buf, base_a + COUNTER_REL)
    counter_b = u32(buf, base_b + COUNTER_REL)
    active = "B" if counter_b > counter_a else "A"

    old_a = u32(buf, base_a + FF8ID_REL)
    old_b = u32(buf, base_b + FF8ID_REL)
    print(f"  Bank A  counter {counter_a:10d}  current FF8ID 0x{old_a:08X}")
    print(f"  Bank B  counter {counter_b:10d}  current FF8ID 0x{old_b:08X}")
    print(f"  Active bank: {active}")

    put_u32(buf, base_a + FF8ID_REL, ff8_id)
    put_u32(buf, base_b + FF8ID_REL, ff8_id)
    print(f"  Patched FF8ID -> 0x{ff8_id:08X} in both banks")

# ── Single-file-card workflow ─────────────────────────────────────────────────

def run_card_mode(card_path, ff8_slot_arg, out_path):
    buf = read_file(card_path)
    if len(buf) != CARD_SIZE:
        sys.exit(f"error: {card_path}: expected {CARD_SIZE} bytes, got {len(buf)}")

    choco_slot, choco_block = find_chocorpg_slot(buf)
    if choco_slot is None:
        sys.exit("error: no Chocobo World app (MCX0/MCX1) found in card directory")
    print(f"Chocobo World app at directory slot {choco_slot} "
          f"(block offset 0x{choco_block:05X})")

    if ff8_slot_arg is not None:
        ff8_slot = ff8_slot_arg
        link_id = ff8_id_from_card_slot(buf, ff8_slot)
        print(f"FF8 link ID 0x{link_id:08X}  (card slot {ff8_slot})")
    else:
        ff8_slot = find_ff8_slot(buf)
        if ff8_slot is None:
            sys.exit("error: no FF8 save with a non-zero link ID found on card.\n"
                     "  Use --ff8-slot N to specify a slot explicitly.")
        link_id = ff8_id_from_card_slot(buf, ff8_slot)

    if link_id == 0:
        print(f"warning: FF8 link ID is 0x00000000 (slot {ff8_slot} was never linked "
              "to a PocketStation).  Patching anyway.")

    base_a = choco_block + TITLE_SECTOR
    base_b = choco_block + TITLE_SECTOR + 0x100
    patch_banks(buf, base_a, base_b, link_id)

    target = out_path if out_path else card_path
    if target == card_path:
        backup = card_path + ".bak"
        shutil.copy2(card_path, backup)
        print(f"Backup: {backup}")
    write_file(target, buf)
    print(f"Output: {target}")

# ── Two-file workflow ─────────────────────────────────────────────────────────

def run_two_file_mode(ff8_path, choco_path, out_path):
    ff8_id    = ff8_id_from_mcs(read_file(ff8_path), ff8_path)
    choco_buf = read_file(choco_path)
    size      = len(choco_buf)

    if size == CHOCO_MCS_SIZE:
        print(f"Format: Chocobo World PSX single-save MCS ({CHOCO_MCS_SIZE} bytes)")
        patch_banks(choco_buf, BANK_A_MCS, BANK_B_MCS, ff8_id)
    elif size == CARD_SIZE:
        print(f"Format: full 128 KB memory card image ({CARD_SIZE} bytes)")
        slot, block_off = find_chocorpg_slot(choco_buf)
        if slot is None:
            sys.exit("error: no Chocobo World app (MCX0/MCX1) found in card directory")
        print(f"  Chocobo World app at directory slot {slot} "
              f"(block offset 0x{block_off:05X})")
        base_a = block_off + TITLE_SECTOR
        base_b = block_off + TITLE_SECTOR + 0x100
        patch_banks(choco_buf, base_a, base_b, ff8_id)
    else:
        sys.exit(
            f"error: {choco_path}: unrecognised size {size} bytes.\n"
            f"  Expected {CHOCO_MCS_SIZE} (PSX single-save MCS) "
            f"or {CARD_SIZE} (full 128 KB card image).")

    target = out_path if out_path else choco_path
    if target == choco_path:
        backup = choco_path + ".bak"
        shutil.copy2(choco_path, backup)
        print(f"Backup: {backup}")
    write_file(target, choco_buf)
    print(f"Output: {target}")

# ── Entry point ───────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Set the FF8 link ID in a Chocobo World save file.",
        add_help=False)
    parser.add_argument('--card',      metavar='CARD')
    parser.add_argument('--ff8-slot',  metavar='N', type=int)
    parser.add_argument('--output',    metavar='OUT')
    parser.add_argument('positional',  nargs='*')
    parser.add_argument('-h', '--help', action='store_true')

    args = parser.parse_args()
    if args.help:
        print(__doc__)
        sys.exit(0)

    if args.card:
        run_card_mode(args.card, args.ff8_slot, args.output)
    elif len(args.positional) >= 2:
        out = args.output or (args.positional[2] if len(args.positional) > 2 else None)
        run_two_file_mode(args.positional[0], args.positional[1], out)
    else:
        print(__doc__)
        sys.exit(0)

if __name__ == '__main__':
    main()
