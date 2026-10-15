#!/usr/bin/env python3
"""Disassembly gate for the code the Media Engine runs (TH06 PSP).

  tools/midi/me_code_check.py TH06PSP.elf

Walks every function reachable from meLibOnProcess (psp/audio_me.c, the ME
entry) in the linked ELF -- including library functions such as memset and
__udivdi3, which are walked rather than trusted -- and fails on what the ME
must never execute:
  - FPU / VFPU instructions (cop1 ops, dotted mnemonics, v* ops),
  - indirect jumps (jr other than $ra, jalr): MECC runs the code through the
    kseg0 alias while jump tables and function pointers hold link addresses,
  - syscalls, and traps other than GCC's divide-by-zero guard (break 7),
  - calls to sce* stubs (they are SC syscalls),
  - stores to absolute main-RAM addresses (static data must not be written by
    the ME; its state lives in eDRAM and inputs come through the mailbox).
Loads from absolute addresses (read-only tables such as the synth tables and
__clz_tab) and break-7 guards are listed as notes.
Adapted from th10_port native/tools/rec_me_check.py (TH10 recording gate).
"""
import re
import subprocess
import sys
from collections import deque

FPU_PLAIN = {'lwc1', 'swc1', 'ldc1', 'sdc1', 'mtc1', 'mfc1', 'ctc1', 'cfc1', 'mthc1', 'mfhc1', 'bc1t', 'bc1f',
             'bc1tl', 'bc1fl', 'mtv', 'mfv', 'mtvc', 'mfvc', 'lv', 'sv'}
TRAPS = {'teq', 'tne', 'tge', 'tgeu', 'tlt', 'tltu', 'teqi', 'tnei', 'tgei', 'tgeiu', 'tlti', 'tltiu', 'syscall'}
BRANCH = re.compile(r'^(b\w*|j)$')
STORES = {'sw', 'sh', 'sb', 'swl', 'swr'}
LOADS = {'lw', 'lh', 'lhu', 'lb', 'lbu', 'lwl', 'lwr'}


def run(*a):
    return subprocess.run(a, capture_output=True, text=True, check=True).stdout


def main(elf):
    here = __file__.rsplit('/tools/', 1)[0]
    users = subprocess.run(['grep', '-rln', 'setTrace', here + '/psp', here + '/src', here + '/shim'],
                           capture_output=True, text=True).stdout.split()
    if users:
        print('FAIL: setTrace is used by %s; the trace callback would run on the ME' % ' '.join(users))
        return 1
    syms = {}
    for line in run('psp-nm', '-C', '--defined-only', '-S', elf).splitlines():
        m = re.match(r'([0-9a-f]+) ([0-9a-f]+) (\w) (.*)', line)
        if m:
            syms.setdefault(int(m.group(1), 16), []).append((m.group(4), int(m.group(2), 16), m.group(3)))
    funcs = {a: (n, sz) for a, l in syms.items() for (n, sz, t) in l if t in 'tTwW' and sz}
    data_syms = sorted((a, sz, n) for a, l in syms.items() for (n, sz, t) in l if t in 'dDbBrRvVgGsS')

    def data_name(addr):
        for a, sz, n in data_syms:
            if a <= addr < a + max(sz, 1):
                return n
        return None

    entry = [a for a, (n, sz) in funcs.items() if n == 'meLibOnProcess']
    if not entry:
        print('FAIL: meLibOnProcess not in ELF')
        return 1
    seen, todo = set(), deque(entry)
    problems, notes, loads = [], [], set()
    while todo:
        a = todo.popleft()
        if a in seen:
            continue
        seen.add(a)
        name, size = funcs[a]
        dis = run('psp-objdump', '-d', '--no-show-raw-insn', '--start-address=0x%x' % a,
                  '--stop-address=0x%x' % (a + size), elf)
        insns = []
        for line in dis.splitlines():
            m = re.match(r'\s*([0-9a-f]+):\s+(\S+)\s*(.*)', line)
            if m:
                insns.append((int(m.group(1), 16), m.group(2), m.group(3).split('<')[0].strip()))
        targets = set()
        for pc, mn, ops in insns:
            if BRANCH.match(mn):
                last = ops.split(',')[-1].strip()
                if re.match(r'^[0-9a-f]+$', last):
                    targets.add(int(last, 16))
        val = {}
        for pc, mn, ops in insns:
            if pc in targets:
                val = {}
            o = [x.strip() for x in ops.split(',')] if ops else []
            # PSP Allegrex HALT is emitted explicitly by MECC's me-lib.c;
            # this binutils prints it as .word, not as a cop1/VFPU opcode.
            # Only this exact word in that exact function is allow-listed.
            known_halt = name == 'meLibHalt' and mn == '.word' and ops == '0x70000000'
            if known_halt:
                notes.append('meLibHalt: known Allegrex HALT 0x70000000 at 0x%x' % pc)
            elif '.' in mn or mn in FPU_PLAIN or (mn.startswith('v') and mn not in ('v0', 'v1')):
                problems.append('%s: FPU/VFPU at 0x%x: %s %s' % (name, pc, mn, ops))
            if mn == 'jalr' or (mn == 'jr' and ops.strip() != 'ra'):
                problems.append('%s: indirect jump at 0x%x: %s %s' % (name, pc, mn, ops))
            if mn in TRAPS:
                problems.append('%s: trap/syscall at 0x%x: %s %s' % (name, pc, mn, ops))
            if mn == 'break':
                code = ops.strip()
                if code in ('0x7', '7'):
                    notes.append('%s: divide-by-zero guard (break 7) at 0x%x' % (name, pc))
                else:
                    problems.append('%s: break %s at 0x%x' % (name, code, pc))
            if mn == 'jal':
                t = int(ops.split()[0], 16)
                tname = funcs.get(t, ('?', 0))[0]
                if t not in funcs:
                    problems.append('%s: call to non-function 0x%x' % (name, t))
                elif tname.startswith('sce'):
                    problems.append('%s: call to SC stub %s' % (name, tname))
                else:
                    todo.append(t)
                continue
            # Tail calls use j/b (including conditional branches), not jal.
            # Walk exact external function entries; an unresolved outward
            # edge is a failure, never a silently omitted closure.
            if BRANCH.match(mn) and o:
                direct_target = o[-1].split()[0]
                if re.match(r'^[0-9a-f]+$', direct_target):
                    t = int(direct_target, 16)
                    if not (a <= t < a + size):
                        if t in funcs:
                            if funcs[t][0].startswith('sce'):
                                problems.append('%s: tail call to SC stub %s' % (name, funcs[t][0]))
                            else:
                                todo.append(t)
                        else:
                            problems.append('%s: unresolved branch outside function at 0x%x -> 0x%x' % (name, pc, t))
            # absolute addresses built with lui/addiu/ori, then used by loads/stores
            if len(o) == 2 and (mn in STORES or mn in LOADS):
                mm = re.match(r'(-?\d+)\((\w+)\)$', o[1])
                if mm and mm.group(2) in val:
                    ea = (val[mm.group(2)] + int(mm.group(1))) & 0xffffffff
                    if 0x08800000 <= ea < 0x0c000000:
                        dn = data_name(ea) or hex(ea)
                        if mn in STORES:
                            problems.append('%s: store to main-RAM data %s at 0x%x' % (name, dn, pc))
                        else:
                            loads.add(dn)
            dest = o[0] if o and mn not in STORES and mn not in ('cache', 'sync', 'nop', 'jr', 'mtc0', 'mult',
                                                                'multu', 'div', 'divu', 'madd', 'maddu', 'msub',
                                                                'msubu') and not BRANCH.match(mn) else None
            new = None
            try:
                if mn == 'lui':
                    new = int(o[1], 0) << 16
                elif mn == 'addiu' and len(o) == 3 and o[1] in val:
                    new = (val[o[1]] + int(o[2], 0)) & 0xffffffff
                elif mn == 'ori' and len(o) == 3 and o[1] in val:
                    new = val[o[1]] | int(o[2], 0)
                elif mn == 'move' and len(o) == 2 and o[1] in val:
                    new = val[o[1]]
            except ValueError:
                new = None
            if dest:
                val.pop(dest, None)
                if new is not None:
                    val[dest] = new
    names = sorted(funcs[a][0] for a in seen)
    print('functions reachable from meLibOnProcess: %d' % len(names))
    for n in names:
        print('  ' + n)
    print('read-only data read by absolute address: %s' % (', '.join(sorted(loads)) or 'none'))
    for n in sorted(set(notes)):
        print('note: ' + n)
    for p in problems:
        print('FAIL: ' + p)
    print('ME CODE CHECK %s (%d problems)' % ('OK' if not problems else 'NG', len(problems)))
    return 1 if problems else 0


if __name__ == '__main__':
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(64)
    sys.exit(main(sys.argv[1]))
