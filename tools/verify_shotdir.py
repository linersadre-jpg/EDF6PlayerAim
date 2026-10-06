"""Offline check of the shot-direction hook (WeaponBase slot 22) against the installed EDF.dll.

Run: python tools/verify_shotdir.py
Exit code 0 = every weapon vtable's slot 22 still points at 0x691fa0 and its entry bytes match the signature
the plugin gates on, so the patch targets are what the plugin expects.
"""
import os
import sys

# tools/edfre2.py ships next to this file and reads the installed EDF.dll (set EDF6_DIR if it is not in the
# default Steam library folder).
_HERE = os.path.dirname(os.path.abspath(__file__))
_WS = os.path.abspath(os.path.join(_HERE, '..'))
sys.path.insert(0, _HERE)          # tools/edfre2.py ships next to this file
import edfre2 as R  # noqa: E402

SHOT_DIR = 0x691FA0
SIG = bytes([0x48, 0x83, 0xEC, 0x18, 0x0F, 0x10, 0x91, 0x90, 0x01, 0x00, 0x00,
             0x48, 0x8B, 0xC2, 0xF3, 0x0F])
VTABLES = {
    0x17E25E8: 'WeaponBase',
    0x17E3590: 'Weapon_BasicShoot_Base',
    0x17E36E0: 'Weapon_BasicShoot',
    0x17E3830: 'Weapon_BasicSemiAuto',
    0x17E3AF0: 'Weapon_ChargeShoot',
    0x17E3D50: 'Weapon_Gatling',
    0x17E3F18: 'Weapon_HeavyShoot',
    0x17E40A0: 'Weapon_HomingShoot',
    0x17E42A0: 'Weapon_ImpactHammer',
    0x17E4A80: 'Weapon_PileBanker',
    0x17E4DC8: 'Weapon_PreChargeShoot',
    0x17E5950: 'Weapon_Swing',
    0x17E5B60: 'Weapon_Throw',
    0x17E3328: 'Weapon_Accessory',
    0x17E5440: 'Weapon_Sub',
    0x17E1CB0: 'Weapon_Drone_Area',
    0x17E1F00: 'Weapon_Drone_LaserMarker',
    0x17E2130: 'Weapon_Drone_MarkerShoot',
    0x17E22B8: 'Weapon_Drone_Switch',
    0x17E45E8: 'Weapon_LaserMarker',
    0x17E4750: 'Weapon_LaserMarkerCallFire',
    0x17E4908: 'Weapon_MarkerShooter',
    0x17E4FB0: 'Weapon_RadioContact',
    0x17E51A0: 'Weapon_Shield',
    0x17E57D8: 'Weapon_SubDrone',
    0x17E5E40: 'Weapon_VehicleMaser',
    0x17E5FA8: 'Weapon_VehicleRailGun',
    0x17E6120: 'Weapon_VehicleShoot',
    0x17E6320: 'Weapon_VehicleSwingShoot',
}

fail = 0
entry = R.sig(SHOT_DIR, 16)
print('=== entry bytes of 0x691fa0 (the shot direction) ===')
print(f'      {entry}')
if entry.lower() == SIG.hex(' ').lower():
    print('PASS  0x691fa0 matches the signature the plugin gates on')
else:
    print(f'FAIL  0x691fa0 entry differs: expected {SIG.hex(" ")}')
    fail += 1

print()
print('=== every weapon vtable slot 22 points at 0x691fa0 ===')
for v, name in sorted(VTABLES.items()):
    got = R.q(v + 0xB0) - R.BASE
    if got == SHOT_DIR:
        print(f'PASS  {name:26} vtable {v:#010x} slot 22 -> {SHOT_DIR:#x}')
    else:
        print(f'FAIL  {name:26} vtable {v:#010x} slot 22 -> {got:#x}')
        fail += 1

print()
print('RESULT:', 'ALL CHECKS PASS' if fail == 0 else f'{fail} CHECK(S) FAILED')
sys.exit(0 if fail == 0 else 1)
