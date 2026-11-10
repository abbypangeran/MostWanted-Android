#!/usr/bin/env python3
"""Generate the static callback-policy manifest for the authentic XBE.

The manifest is reviewed data, not executable code.  It is derived from:

  * the authentic image's table data and section map;
  * the full generated function mapping (membership and digest);
  * a checked recompiler output seeded with every live table target, the
    dynamic list callback, and their direct callees (ABI, stack reads,
    return cleanup, direct/indirect calls, traps and writes).

Usage:

  python3 ios/recompiler-fixtures/callback_policy_generate.py \
      --xbe assets/default.xbe \
      --checked-source .local/ios-port/phase2d12-callback-census/generated/x86_recomp.0.cpp \
      --full-mapping generated/game/x86_func_mapping.cpp \
      --output ios/recompiler-fixtures/callback_policy_manifest.txt

The checked source is produced by the local callback census config (all live
targets plus 0x0032062A).  It is not tracked: only this manifest and the
generator are.
"""
import argparse
import re
import struct
from collections import Counter

TABLES = {
    'A': (0x4720C8, 1),
    'B': (0x41E7E0, 3),
    'C': (0x41F94C, 6),
    'D': (0x41E7F0, 1110),
}
AUX = [
    (0x4720CC, 0x326A43, 'single_pre'),
    (0x4720D0, 0x326A43, 'single_post'),
]
LIST_NODE = 0x4E87D8
LIST_CALLBACK = 0x32062A
LIST_REGISTER_SITE = 0x31BC0F
LIST_HELPER = 0x2B5B8A
LIST_WALKER = 0x2B597D
LIST_WALK_ONE = 0x2B5C76
LIST_WALK_ZERO = 0x2B5C86
LIST_TERMINATE = 0x2B5CA0
LIST_ARGS = 1

SIGNATURES = [
    (0x0031BC00, '68D8874E00'),                  # push 004E87D8
    (0x0031BC05, 'C705E0874E002A063200'),        # mov [004E87E0], 0032062A
    (0x0031BC0F, 'E8769FF9FF'),                  # call 002B5B8A
    (0x002B5999, 'FF742410'),                    # push [esp+0x10]
    (0x002B59A1, 'FF5008'),                      # call [eax+8]
    (0x002B5C76, 'E802FDFFFF'),                  # call 002B597D, arg 1
    (0x002B5C86, 'E8F2FCFFFF'),                  # call 002B597D, arg 0
    (0x002B5CA0, 'FF15649E3C00'),                # call [003C9E64], ordinal 258
    (0x0046C8C0, 'FFFFFFFF'),                    # LockCount
    (0x0046C8C4, '00000000'),                    # RecursionCount
    (0x0046C8C8, '00000000'),                    # OwningThread
    (0x0046C8CC, 'CCC84600CCC84600'),            # sentinel Flink/Blink
]


def fnv1a(data, value=0x811C9DC5):
    for byte in data:
        value = ((value ^ byte) * 0x01000193) & 0xFFFFFFFF
    return value


def parse_xbe(path):
    data = open(path, 'rb').read()
    base, = struct.unpack_from('<I', data, 0x104)
    header_size, image_size = struct.unpack_from('<II', data, 0x108)
    nsec, sec_addr = struct.unpack_from('<II', data, 0x11C)
    sections = []
    for i in range(nsec):
        offset = sec_addr - base + i * 0x38
        flags, va, vsize, raw, rsize = struct.unpack_from('<IIIII', data, offset)
        sections.append((flags, va, vsize, raw, rsize))
    return data, base, image_size, sections


def read_bytes(data, sections, va, size):
    for flags, sva, vsize, raw, rsize in sections:
        if sva <= va < sva + vsize:
            offset = raw + (va - sva)
            if offset + size <= raw + rsize:
                return data[offset:offset + size]
    return None


def read32(data, sections, va):
    raw = read_bytes(data, sections, va, 4)
    return struct.unpack('<I', raw)[0] if raw else None


def read_prefix(data, sections, va, size):
    for flags, sva, vsize, raw, rsize in sections:
        if sva <= va < sva + vsize:
            offset = raw + (va - sva)
            available = max(0, min(size, rsize - (va - sva)))
            return data[offset:offset + available] if available else b''
    return b''


def section_of(sections, va):
    for flags, sva, vsize, raw, rsize in sections:
        if sva <= va < sva + vsize:
            return flags, sva, vsize
    return None


def executable(sections, va):
    found = section_of(sections, va)
    return bool(found and (found[0] & 4))


def table_digest(data, sections, base, count):
    raw = read_bytes(data, sections, base, count * 4)
    if raw is None:
        raise SystemExit(f'table {base:08X}+{count} is not file-backed')
    return fnv1a(raw)


def load_mapping(path):
    text = open(path).read()
    order = [int(m.group(1), 16) for m in re.finditer(r'\{0x([0-9A-Fa-f]+)u,', text)]
    digest = fnv1a(b''.join(a.to_bytes(4, 'little') for a in order))
    return order, set(order), digest


def load_blocks(path):
    text = open(path).read()
    blocks = {}
    for match in re.finditer(r'X86_FUNC\(__imp__sub_([0-9A-Fa-f]+)\) \{(.*?)\n\}\nX86_WEAK_FUNC', text, re.S):
        blocks[int(match.group(1), 16)] = match.group(2)
    return blocks


def analyse(body):
    rets, traps, direct, imports = [], [], [], []
    abs_writes, trap_pcs = [], []
    indirect_calls = indirect_writes = fs = 0
    args = 0
    frame = False
    ambiguous = False
    esp_rel = 0
    ebp_rel = None
    branch_seen = False
    first_stack_read = False

    for line in body.splitlines():
        trap = re.search(r'x86_checked_trap\(ctx,\s*0x([0-9A-Fa-f]+),\s*"([^"]*)"\)', line)
        if trap:
            trap_pcs.append(int(trap.group(1), 16))
            traps.append(trap.group(2).replace(' ', '_'))
        comment = re.match(r'\s*//\s*([0-9A-Fa-f]{8})\s+(.*)$', line)
        if not comment:
            continue
        ins = comment.group(2).strip()
        low = ins.lower()

        if low.startswith('push'):
            esp_rel -= 4
        elif low.startswith('pop'):
            esp_rel += 4
        elif re.match(r'sub\s+esp,\s*0x([0-9A-Fa-f]+)', low):
            esp_rel -= int(re.match(r'sub\s+esp,\s*0x([0-9A-Fa-f]+)', low).group(1), 16)
        elif re.match(r'add\s+esp,\s*0x([0-9A-Fa-f]+)', low):
            esp_rel += int(re.match(r'add\s+esp,\s*0x([0-9A-Fa-f]+)', low).group(1), 16)
        elif low.startswith('mov ebp, esp'):
            ebp_rel = esp_rel
        elif low.startswith('leave'):
            esp_rel = (ebp_rel if ebp_rel is not None else esp_rel) + 4
        elif re.match(r'(j[a-z]+|jmp)\b', low):
            branch_seen = True

        if low.startswith('ret'):
            match = re.match(r'ret\s*(?:0x([0-9A-Fa-f]+))?', low)
            rets.append(int(match.group(1), 16) if match.group(1) else 0)

        if low.startswith('call '):
            operand = ins[5:].strip()
            slot_match = re.match(r'\[0x([0-9A-Fa-f]+)\]', operand)
            direct_match = re.match(r'0x([0-9A-Fa-f]+)', operand)
            if slot_match:
                slot = int(slot_match.group(1), 16)
                if 0x3C9D20 <= slot < 0x3CF1D8:
                    raw = read32_body(body, slot)
                    if raw is not None and raw & 0x80000000:
                        imports.append((slot, raw & 0xFFFFF))
                else:
                    indirect_calls += 1
            elif direct_match:
                direct.append(int(direct_match.group(1), 16))
            else:
                indirect_calls += 1

        if re.search(r'fs:\[0x[0-9A-Fa-f]+\]', ins):
            fs += 1

        stack = re.search(r'\[(esp|ebp)([+-])0x([0-9A-Fa-f]+)\]', ins)
        if stack:
            displacement = int(stack.group(3), 16)
            if stack.group(2) == '-':
                displacement = -displacement
            if stack.group(1) == 'esp':
                offset = esp_rel + displacement
                if not first_stack_read:
                    first_stack_read = True
                    if offset <= 0:
                        frame = True
                    elif offset >= 4 and (offset - 4) % 4 == 0:
                        args = max(args, (offset - 4) // 4 + 1)
                    else:
                        ambiguous = True
                if offset <= 0:
                    frame = True
                if branch_seen and not args:
                    ambiguous = True
            else:
                offset = (ebp_rel if ebp_rel is not None else 0) + displacement
                if not first_stack_read:
                    first_stack_read = True
                    if offset >= 4 and (offset - 4) % 4 == 0:
                        args = max(args, (offset - 4) // 4 + 1)
                    elif 0 <= offset < 4:
                        frame = True
                    elif offset >= 4:
                        ambiguous = True
                if 0 <= offset < 4:
                    frame = True

        for match in re.finditer(r'(?:mov|or|and|xor|add|sub)\s+(?:dword ptr )?\[0x([0-9A-Fa-f]{8})\],', ins):
            abs_writes.append(int(match.group(1), 16))
        if re.search(r'(?:mov|or|and|xor|add|sub)\s+(?:dword ptr )?\[(?:e[a-z]{2}|[a-z]{2}[a-z]?)\s*(?:[+-]|)\]', ins) \
                and not re.search(r'\[0x[0-9A-Fa-f]{8}\]', ins):
            indirect_writes += 1

    if not rets:
        cleanup = 'NONE'
    elif len(set(rets)) == 1:
        cleanup = 'RET' if rets[0] == 0 else f'RET{rets[0]}'
    else:
        cleanup = 'VARIES'
    return {
        'cleanup': cleanup,
        'traps': traps,
        'trap_pcs': trap_pcs,
        'direct': direct,
        'imports': imports,
        'args': args,
        'frame': frame,
        'ambiguous': ambiguous,
        'abs_writes': abs_writes,
        'indirect_calls': indirect_calls,
        'indirect_writes': indirect_writes,
        'fs': fs,
    }


def read32_body(body, va):
    # import slot values are read from the image by the caller; this helper is
    # replaced by a closure in the main loop.
    return None


def classify(target, mapped, kind, info, call_site_args):
    if not mapped and kind != 'INTERIOR_RET':
        return 'INVALID_MAPPING'
    if info['traps'] or info['args'] > call_site_args or info['frame']:
        return 'PROHIBITED'
    if info['cleanup'] in ('NONE', 'VARIES') or info['ambiguous']:
        return 'UNRESOLVED'
    if info['indirect_calls'] or info['indirect_writes']:
        return 'INFERRED'
    return 'VERIFIED_STATIC'


def abi_for(info):
    if info['cleanup'].startswith('RET') and info['cleanup'] != 'RET':
        return 'STDCALL'
    if info['cleanup'] == 'RET':
        return 'CDECL'
    return 'UNKNOWN'


def flags_for(info, arg_mismatch=False, recheck=True):
    flags = []
    if info['traps']:
        flags.append('TRAP')
    if arg_mismatch:
        flags.append('ARG_MISMATCH')
    if info['frame']:
        flags.append('FRAME')
    if info['indirect_calls']:
        flags.append('IND_CALL')
    if info['indirect_writes']:
        flags.append('IND_WRITE')
    if info['abs_writes']:
        flags.append('ABS_WRITE')
    if info['imports']:
        flags.append('IMPORT')
    if info['fs']:
        flags.append('FS')
    if info['cleanup'] == 'NONE':
        flags.append('TAIL')
    if recheck:
        flags.append('RECHECK')
    return ','.join(flags) if flags else '-'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--xbe', required=True)
    parser.add_argument('--checked-source', required=True)
    parser.add_argument('--full-mapping', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()

    data, base, image_size, sections = parse_xbe(args.xbe)
    mapping_order, mapping_set, mapping_digest = load_mapping(args.full_mapping)
    blocks = load_blocks(args.checked_source)

    # bind analyses to the image so import slots can be resolved
    global read32_body
    read32_body = lambda body, va: read32(data, sections, va)

    slot_records = []
    for name, (table_base, count) in TABLES.items():
        for index in range(count):
            address = table_base + index * 4
            value = read32(data, sections, address)
            if value is None:
                raise SystemExit(f'table {name} slot {index} is not file-backed')
            if value not in (0, 0xFFFFFFFF):
                slot_records.append((name, index, address, value))

    unique = sorted({record[3] for record in slot_records})
    missing_blocks = [t for t in unique if t not in blocks]
    missing_mapping = [t for t in unique if t not in mapping_set]
    if missing_blocks or missing_mapping:
        raise SystemExit(f'missing blocks={[hex(x) for x in missing_blocks]} mapping={[hex(x) for x in missing_mapping]}')
    if LIST_CALLBACK not in blocks or LIST_CALLBACK not in mapping_set:
        raise SystemExit('list callback is not in the checked source or mapping')

    analyses = {}
    for target in unique:
        analyses[target] = analyse(blocks[target])
    list_info = analyse(blocks[LIST_CALLBACK])

    # 00326A43 is a bare RET stored in the two auxiliary slots; it is not a
    # mapped function entry, so its contract is reviewed explicitly.
    aux_info = {
        'cleanup': 'RET', 'traps': [], 'trap_pcs': [], 'direct': [], 'imports': [],
        'args': 0, 'frame': False, 'ambiguous': False, 'abs_writes': [],
        'indirect_calls': 0, 'indirect_writes': 0, 'fs': 0,
    }
    aux_target = AUX[0][1]
    if read_bytes(data, sections, aux_target, 1) != b'\xC3':
        raise SystemExit('auxiliary target 00326A43 is not a bare RET')

    target_records = {}
    for target in unique:
        info = analyses[target]
        target_records[target] = {
            'address': target,
            'mapped': target in mapping_set,
            'kind': 'ENTRY',
            'info': info,
            'call_site_args': 0,
            'status': classify(target, target in mapping_set, 'ENTRY', info, 0),
        }
    target_records[LIST_CALLBACK] = {
        'address': LIST_CALLBACK,
        'mapped': True,
        'kind': 'ENTRY',
        'info': list_info,
        'call_site_args': LIST_ARGS,
        'status': classify(LIST_CALLBACK, True, 'ENTRY', list_info, LIST_ARGS),
    }
    target_records[aux_target] = {
        'address': aux_target,
        'mapped': False,
        'kind': 'INTERIOR_RET',
        'info': aux_info,
        'call_site_args': 0,
        'status': 'INFERRED',
    }

    lines = []
    lines.append('VERSION 1')
    lines.append(f'IMAGE_SHA1 f636a16af04171089f879a4bed646e185c734233')
    lines.append(f'IMAGE_BASE {base:08X}')
    lines.append(f'IMAGE_END {base + image_size:08X}')
    lines.append(f'MAPPING_SOURCE generated/game/x86_func_mapping.cpp')
    lines.append(f'MAPPING_COUNT {len(mapping_order)}')
    lines.append(f'MAPPING_DIGEST {mapping_digest:08X}')
    for name, (table_base, count) in TABLES.items():
        live = sum(1 for record in slot_records if record[0] == name)
        lines.append(f'TABLE {name} {table_base:08X} {count} {live} {table_digest(data, sections, table_base, count):08X} 1')
    for address, target, role in AUX:
        lines.append(f'AUX {address:08X} {target:08X} {role}')
    for name, index, address, value in slot_records:
        lines.append(f'SLOT {name} {index} {address:08X} {value:08X}')

    for target in sorted(target_records):
        record = target_records[target]
        info = record['info']
        section = section_of(sections, target)
        if not section:
            raise SystemExit(f'target {target:08X} has no section')
        fingerprint = fnv1a(read_prefix(data, sections, target, 16))
        edges = ','.join(f'{value:08X}' for value in info['direct']) or '-'
        writes = ','.join(f'{value:08X}' for value in info['abs_writes']) or '-'
        imports = ','.join(f'{slot:08X}:{ordinal}' for slot, ordinal in info['imports']) or '-'
        traps = ','.join(f'{value:08X}' for value in info['trap_pcs']) or '-'
        lines.append(
            'TARGET '
            f'{target:08X} {1 if record["mapped"] else 0} {record["kind"]} '
            f'{section[1]:08X} {section[0]:08X} {abi_for(info)} {info["args"]} {info["cleanup"]} '
            f'{record["status"]} {flags_for(info, info["args"] > record["call_site_args"])} '
            f'{len(info["traps"])} {len(info["imports"])} {len(info["abs_writes"])} '
            f'{info["indirect_calls"]} {info["indirect_writes"]} {fingerprint:08X} 0 '
            f'E={edges} W={writes} I={imports} T={traps}'
        )
    lines.append(
        f'LIST {LIST_NODE:08X} {LIST_CALLBACK:08X} {LIST_REGISTER_SITE:08X} '
        f'{LIST_HELPER:08X} {LIST_WALKER:08X} {LIST_WALK_ONE:08X} {LIST_WALK_ZERO:08X} '
        f'{LIST_ARGS} {target_records[LIST_CALLBACK]["info"]["cleanup"]} '
        f'{target_records[LIST_CALLBACK]["status"]}'
    )
    for address, signature in SIGNATURES:
        lines.append(f'SIG {address:08X} {signature}')

    with open(args.output, 'w') as output:
        output.write('\n'.join(lines) + '\n')

    counts = Counter(record['status'] for record in target_records.values())
    print(f'targets={len(target_records)} slots={len(slot_records)} mapping={len(mapping_order)} '
          f'verified={counts["VERIFIED_STATIC"]} inferred={counts["INFERRED"]} '
          f'unresolved={counts["UNRESOLVED"]} prohibited={counts["PROHIBITED"]} '
          f'invalid={counts["INVALID_MAPPING"]}')
    for name, (table_base, count) in TABLES.items():
        live = [record for record in slot_records if record[0] == name]
        table_counts = Counter(target_records[record[3]]['status'] for record in live)
        print(f'{name}: {len(live)}/{count} live, {dict(table_counts)}')


if __name__ == '__main__':
    main()
