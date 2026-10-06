"""Offline verification of the player-aim hooks against the installed EDF.dll.

Run before any in-game test: it proves, from the shipped binary alone, the facts the module depends on.
  1. the image gate (TimeDateStamp / SizeOfImage) the plugin checks;
  2. each hooked vtable's slot 4 holds the exact body the module expects;
  3. which classes reach which body (so one patch per body covers them);
  4. the entry bytes of each hooked body;
  5. the candidate aim fields are written inside the player's input body.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # tools/edfre2.py ships next to this file
import edfre2 as R

# must match src/playeraim.cpp kInputClasses
HOOKED = [
    ('HumanBase / SoldierBase / People / HeavyArmor', 0x17CFD20, 0x572DF0),
    ('AssultSoldier', 0x17CDF28, 0x550A30),
    ('HumanoidBase / Humanoid_Basic', 0x17C29C0, 0x470CA0),
    ('EDF6_SoftBodyHumanoidBase', 0x17C1EF8, 0x470CA0),
]
# every human-family class that can be a player, with the body its slot 4 reaches
FAMILY = {
    'HumanBase': 0x17CFD20, 'SoldierBase': 0x17D24D8, 'AssultSoldier': 0x17CDF28,
    'People': 0x17D1868, 'HeavyArmor': 0x17CF5B8,
    'HumanoidBase': 0x17C29C0, 'Humanoid_Basic': 0x17C4D70, 'EDF6_SoftBodyHumanoidBase': 0x17C1EF8,
}
# reachable bodies: the two implementations, plus the one-instruction thunk AssultSoldier uses
BODIES = {0x572DF0, 0x550A30, 0x470CA0}

ok = True


def check(name, good, detail=''):
    global ok
    ok = ok and good
    print(f'{"PASS" if good else "FAIL"}  {name}{(" - " + detail) if detail else ""}')


def slot4(vt):
    return R.q(vt + 4 * 8) - R.BASE


print('=== 1. image gate ===')
e = struct.unpack_from('<I', R.img, 0x3C)[0]
ts = struct.unpack_from('<I', R.img, e + 8)[0]
soi = struct.unpack_from('<I', R.img, e + 0x50)[0]
check('TimeDateStamp', ts == 0x678CCB46, f'{ts:#x}')
check('SizeOfImage', soi == 0x22CE000, f'{soi:#x}')

print()
print('=== 2. each hooked vtable slot 4 holds the expected body ===')
for name, vt, body in HOOKED:
    got = slot4(vt)
    check(f'{name}', got == body, f'vtable {vt:#x} slot4 -> {got:#x} (expected {body:#x})')

print()
print('=== 3. every player-capable human class reaches one of the hooked bodies ===')
for name, vt in FAMILY.items():
    got = slot4(vt)
    reached = got in BODIES
    check(f'{name:26}', reached, f'slot4 -> {got:#x}')
    if got == 0x550A30:
        # a thunk: the real body is the jmp target
        target = None
        if R.img[got] == 0xE9:
            target = got + 5 + struct.unpack_from('<i', R.img, got + 1)[0]
        check(f'  ...thunk {0x550A30:#x} jmp target', target == 0x572DF0, f'{target if target is None else hex(target)}')

print()
print('=== 4. entry bytes of the hooked bodies ===')
for _, _, body in HOOKED:
    print(f'      {body:#08x}: {R.sig(body, 16)}')
    if R.img[body] == 0xE9:   # a one-instruction thunk to the real body: fine to patch, it is what the slot holds
        target = body + 5 + struct.unpack_from('<i', R.img, body + 1)[0]
        check(f'{body:#x} is a jmp thunk', target in BODIES, f'-> {target:#x}')
    else:
        check(f'{body:#x} looks like a function entry', R.img[body] in (0x48, 0x40, 0x4C, 0x53))

print()
print('=== 5. the candidate aim fields are written inside 0x572df0 ===')
body = R.img[0x572DF0:0x572DF0 + 0x1200]
for name, off in (('A +0x1230', 0x1230), ('B +0x1234', 0x1234), ('C +0x12d8', 0x12D8), ('D +0x12dc', 0x12DC)):
    pat = struct.pack('<I', off)
    hits = sum(1 for i in range(len(body) - 4) if body[i:i + 4] == pat)
    check(f'{name} written in the body', hits > 0, f'{hits} occurrence(s)')

print()
print('RESULT:', 'ALL CHECKS PASS' if ok else 'SOME CHECKS FAILED')
sys.exit(0 if ok else 1)
