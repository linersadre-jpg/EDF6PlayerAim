// EDF6PlayerAim: the player soldier's crosshair (and shots) point at the nearest enemy, for every class.
//
// A standalone EDFModLoader plugin. It shares only common/ (edf6common: the memory / patch / image code) and
// third_party/EDFModLoader/PluginAPI.h with the other plugins in this repository, and touches none of their
// sources or files. All addresses are RVAs into EDF.dll TimeDateStamp 0x678CCB46; the profile is checked
// before anything is written, and a mismatch leaves the game exactly as it was.
//
// How it works (docs/player-aim-re.md has the evidence):
//
//   * The local player's human is found the way the other plugins find it: a pad (+0x340), the
//     player-controlled flag (+0x354) and a weak-this check, so NPC soldiers and remote players are never
//     touched.
//   * Human input runs through one of two per-frame bodies, and every human class reaches one of them:
//       - HumanBase slot 4, 0x572df0 - HumanBase / SoldierBase / People / HeavyArmor, and AssultSoldier
//         through a one-instruction thunk (0x550a30: jmp 0x572df0).
//       - HumanoidBase slot 4, 0x470ca0 - HumanoidBase / Humanoid_Basic / EDF6_SoftBodyHumanoidBase.
//     After the stock body has run (the player's own stick is applied), this plugin writes the two aim angles
//     the observed facing is built from:
//       human+0x1230 = pitch A, human+0x1234 = yaw B, radians, world axes, fwd = (sin B, -sin A, cos B).
//     Writing them points the crosshair, the camera and every weapon at the target. Verified in game
//     2026-10-04: the angle to the nearest enemy sat at 0.0-0.9 deg while walking, against tens of degrees
//     (up to 108) with the plugin watching only.
//   * Optionally (PlayerAimFire=1) every call to the muzzle builder 0x6969a0 is redirected so the barrel's
//     aim basis is rotated onto the target too - the shot itself is bent.
#include <Windows.h>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#pragma warning(push)
#pragma warning(disable:4201)
#include "PluginAPI.h"
#pragma warning(pop)
#include "edf/host.h"
#include "edf/layout.h"
#include "edf/memory.h"
#include "edf/patch.h"

namespace playeraim {
// --- EDF.dll: the human input bodies and their vtables (docs/player-aim-re.md section 2) ---
struct InputClass { const char* name; unsigned vtable; unsigned body; };
constexpr InputClass kInputClasses[]={
    {"HumanBase / SoldierBase / People / HeavyArmor",0x17CFD20,0x572DF0},
    {"AssultSoldier",0x17CDF28,0x550A30},
    {"HumanoidBase / Humanoid_Basic",0x17C29C0,0x470CA0},
    {"EDF6_SoftBodyHumanoidBase",0x17C1EF8,0x470CA0},
};
constexpr std::size_t kInputSlot=4;
constexpr int kInputCount=static_cast<int>(sizeof(kInputClasses)/sizeof(kInputClasses[0]));
// The entry bytes each hooked body has to start with (the version gate: a different EDF.dll fails these).
constexpr unsigned kHumanBaseInput=0x572DF0,kAssultThunk=0x550A30,kHumanoidInput=0x470CA0;
constexpr unsigned char kHumanBaseSig[]={0x48,0x8B,0xC4,0x48,0x89,0x58,0x18,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56};
constexpr unsigned char kThunkSig[]={0xE9,0xBB,0x23,0x02,0x00};
constexpr unsigned char kHumanoidSig[]={0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0xFF,0x81,0x40,0x0D,0x00,0x00};
// The human: facing matrix (rows right / up / forward, row 3 the position), pad / player flag, player index,
// the two aim angles, and the vehicle a rider is in.
constexpr std::size_t kHumanMatrix=0x60,kHumanPad=0x340,kHumanPlayer=0x354;
constexpr std::size_t kAnglePitch=0x1230,kAngleYaw=0x1234,kAngleC=0x12D8,kAngleD=0x12DC;
// GameObject: the weak-this pair used to tell one object from another at the same address.
constexpr std::size_t kSelfCtrl=0x30,kCtrlUses=0x8;
// Weapon: the muzzle array / count / stride, the muzzle's local matrix, its mode and the bone rows.
constexpr std::size_t kMuzzleLocal=0x10,kBoneRows=0xB0;
constexpr std::size_t kMuzzleArray=0x1D0,kMuzzleCount=0x1E0,kMuzzleStride=0xF0;
// The muzzle's own struct: the local matrix at +0x10 and the mode at +0xE0. The builder 0x6969a0 uses the
// aim rows a caller passes when the mode is 0, but reads its own object's rows at +0xB0 when it is 1 or 2 -
// which is why a hook that only passes 16 floats of its own can be ignored outright.
constexpr std::size_t kMuzzleMode=0xE0,kMuzzleRows=0xB0;
// The height above the player's origin that the model and the shot are aimed from: an enemy at chest level
// should mean a level weapon, not one tilted up because the origin is at the feet.
constexpr float kChestHeight=1.3f;
// The muzzle builder and its call sites. Only the fire-time one (0x698569, inside the weapon's aim setup
// 0x698500, which is where the shot's direction is decided) is redirected by default: the others run inside
// the per-frame weapon update, ~155 times a second, and hooking them was the version that crashed at the
// end of a mission (EDF.dll+0x978fb0, a shared_ptr released twice). "Barrel=all" restores that behaviour for
// comparison, "Barrel=off" leaves the weapon alone entirely.
constexpr unsigned kMuzzleBuilder=0x6969A0;
constexpr unsigned kMuzzleCallFire=0x698569;   // the shot's own aim setup (0x698500)
constexpr unsigned kMuzzleCallSites[]={0x6904D2,0x6905DE,0x69366A,0x694873,0x698569};
constexpr int kMuzzleCallCount=static_cast<int>(sizeof(kMuzzleCallSites)/sizeof(kMuzzleCallSites[0]));
constexpr float kMuzzleReach=4.0f;   // metres: a barrel farther from the player is not theirs

// --- The shot direction: what a bullet actually flies along ---
// There are two separate paths, and they are not the same quantity:
//
//   * 0x691fa0 - WeaponBase vtable slot 22, shared by every weapon class. The spawn step (0x696fd0) calls it
//     and copies its output straight into the round, so this is the direction a bullet flies. Its body is
//     three instructions: dir = (weapon+0x190) * (weapon+0x24c) / constant. Steering only the muzzle made
//     the bullets hit while the model's weapon still pointed elsewhere - which is exactly what looked wrong
//     in game - so this is now steered as well, with the same slew limit, and the two agree.
//   * 0x6969a0 - the muzzle builder, which composes the muzzle's world matrix (what the weapon model and its
//     effects are drawn from).
constexpr unsigned kShotDir=0x691FA0,kWeaponDirSlot=22;
constexpr unsigned kWeaponDirVtables[]={
    0x17E25E8,   // WeaponBase (covers every class that does not override slot 22)
    0x17E3590,   // Weapon_BasicShoot_Base
    0x17E36E0,   // Weapon_BasicShoot
    0x17E3830,   // Weapon_BasicSemiAuto
    0x17E3AF0,   // Weapon_ChargeShoot
    0x17E3D50,   // Weapon_Gatling
    0x17E3F18,   // Weapon_HeavyShoot
    0x17E40A0,   // Weapon_HomingShoot
    0x17E42A0,   // Weapon_ImpactHammer
    0x17E4A80,   // Weapon_PileBanker
    0x17E4DC8,   // Weapon_PreChargeShoot
    0x17E5950,   // Weapon_Swing
    0x17E5B60,   // Weapon_Throw
    0x17E3328,   // Weapon_Accessory
    0x17E5440,   // Weapon_Sub
    0x17E1CB0,   // Weapon_Drone_Area
    0x17E1F00,   // Weapon_Drone_LaserMarker
    0x17E2130,   // Weapon_Drone_MarkerShoot
    0x17E22B8,   // Weapon_Drone_Switch
    0x17E45E8,   // Weapon_LaserMarker
    0x17E4750,   // Weapon_LaserMarkerCallFire
    0x17E4908,   // Weapon_MarkerShooter
    0x17E4FB0,   // Weapon_RadioContact
    0x17E51A0,   // Weapon_Shield
    0x17E57D8,   // Weapon_SubDrone
    0x17E5E40,   // Weapon_VehicleMaser
    0x17E5FA8,   // Weapon_VehicleRailGun
    0x17E6120,   // Weapon_VehicleShoot
    0x17E6320,   // Weapon_VehicleSwingShoot
};
constexpr int kWeaponDirVtableCount=static_cast<int>(sizeof(kWeaponDirVtables)/sizeof(kWeaponDirVtables[0]));
// 48 83 EC 18 0F 10 91 90 01 00 00 48 8B C2 F3 0F: sub rsp,18 / movups xmm2,[rcx+190] / mov rax,rdx / ...
constexpr unsigned char kShotDirSig[]={0x48,0x83,0xEC,0x18,0x0F,0x10,0x91,0x90,0x01,0x00,0x00,0x48,0x8B,0xC2,0xF3,0x0F};
// The lock-target registry and the team relations (the game's own enemy list).
constexpr std::size_t kRegistry=0x20B2AB0,kRegList=0x8,kNodeTarget=0x10;
constexpr std::size_t kTargetObject=0x8,kTargetAim=0x10,kTargetValid=0x29,kTargetLockable=0x2A;
constexpr std::size_t kObjectDead=0x2E8,kObjectTeam=0x314;
constexpr std::size_t kTeams=0x20B2978,kTeamArray=0x38,kTeamStride=0x38,kTeamRelation=0x18;
constexpr std::int32_t kEnemyRelation=2;
constexpr int kMaxTeam=64,kMaxNodes=8192;
constexpr float kPi=3.14159265f;
// The game's Havok ray wrapper and the plain "nearest hit of any kind" collector it uses here, to ask
// whether map geometry (terrain, buildings: collision layer 22) stands between two points
// (docs/raycast-re.md; the same call src/heli.cpp makes for its map visibility tests).
constexpr std::size_t kHavokGlobal=0x20B2958,kCastRay=0x11A7EE0,kHitVtbl=0x1768B78,kHitReset=0xFDF00;
constexpr unsigned char kCastRaySig[]={0x40,0x53,0x56,0x57,0x48,0x81,0xEC,0xA0,0x00,0x00,0x00,0x48,0x8B,0x05,0x66,0x71};

using edf::At;
using edf::Put;
using edf::Readable;

unsigned char* image=nullptr;
HMODULE module=nullptr;
wchar_t logPath[MAX_PATH]{};
wchar_t iniPath[MAX_PATH]{};

void Log(const char* format,...) noexcept {
    if(!logPath[0])return;
    char text[1000]{};va_list args;va_start(args,format);vsnprintf_s(text,sizeof(text),_TRUNCATE,format,args);va_end(args);
    FILE* f=nullptr;if(_wfopen_s(&f,logPath,L"ab") || !f)return;
    SYSTEMTIME t{};GetLocalTime(&t);
    fprintf(f,"[%02u:%02u:%02u] %s\r\n",t.wHour,t.wMinute,t.wSecond,text);fclose(f);
}

float Dot(const float* a,const float* b) noexcept { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
float Len(const float* a) noexcept { return std::sqrt(Dot(a,a)); }
float Clamp(float v,float lo,float hi) noexcept { return v<lo ? lo : (v>hi ? hi : v); }
float Wrap(float a) noexcept { while(a>kPi)a-=2*kPi; while(a< -kPi)a+=2*kPi; return a; }

// --- The live config: an immutable snapshot, swapped whole when the ini changes ---
// What exactly is aimed:
//   weapons - only the barrel (the muzzle builder's aim basis) turns onto the target. The player's own view
//             is never written, so the screen does not move; the shot leaves along the barrel, so it still
//             hits. This is the default.
//   view    - the player's aim angles are written too, so the crosshair, the camera and the shot all turn.
//             (The older behaviour: effective, but the view swings between targets.)
//   both    - weapons + view.
enum class AimAt { weapons, view, both };
enum class Barrel { fire, all, off };
struct Config {
    AimAt aim=AimAt::weapons;   // see above
    Barrel barrel=Barrel::fire; // which muzzle-builder call sites are redirected (see kMuzzleCallFire)
    bool enabled=true;          // the module does anything at all
    bool debug=false;           // a PLAYERAIM line about twice a second
    float gain=0.35f;           // view mode only: 1 = snap this frame, below 1 = slew at this share
    float range=200.0f;         // enemies farther than this from the player are not considered
    float minDistance=25.0f;    // enemies closer than this are left alone (no spinning at point blank)
    float keepRange=260.0f;     // the target is kept until it dies or leaves this range (no flip-flopping)
    DWORD coneGraceMs=600;      // ...and it is dropped this long after it leaves the cone, so a glance does
                                // not hand the aim to another enemy
    float cone=20.0f;           // degrees: a target farther than this from where the gun already points is
                                // not taken (and the barrel never turns more than this off the camera)
    float bodyYaw=1.0f;         // the model is turned towards the target by this share of the way in yaw
    float bodyPitch=1.0f;       // share of the offset from the camera's centre line the arms take: 1.0 lands
                                // the weapon on the target, 0 leaves it resting on the camera
    float bodySmoothMs=250.0f; // time for the model facing to cover most of a change of target
                                // (0 = the model turns at once, the behaviour before this feature)
    float pitchBias=0.0f;       // extra pitch added to the model, in RADIANS, for calibrating by hand while
                                // the game runs: edit it in the ini and save, it is picked up in about a second
    float yawBias=0.0f;         // ...and the same for the yaw, in radians
    float biasRamp=0.0f;        // if non-zero, the bias makes a TRIANGLE sweep: it walks up to +biasRamp,
                                // then back down to -biasRamp, repeating. One setting then sweeps the whole
    DWORD slewMs=700;           // time for the barrel and the shot to swing a whole this long to swing a whole
                                // pans across instead of snapping (0 = snap immediately)
    bool requireLos=true;       // only aim at enemies the shot can actually reach: a ray with the map's own
                                // filter (terrain and buildings) must not stop before the target. A target
                                // that gets blocked is dropped, so the aim moves to another enemy instead of
                                // shooting a wall forever.
    float test=0.0f;            // calibration probe: radians added to one angle every frame (0 = off)
    int testAxis=0;             // ...0..3 = human+0x1230 / +0x1234 / +0x12d8 / +0x12dc
};
Config cfg{};
Config* published=&cfg;

// Aim if the barrel is allowed to turn, the view if the aim angles are written.
inline bool AimsWeapons(const Config& c) noexcept { return c.aim!=AimAt::view && c.barrel!=Barrel::off; }
inline bool AimsView(const Config& c) noexcept { return c.aim!=AimAt::weapons; }

// Defined with the ini code below; the frame hook calls it to pick up a saved ini.
void ReloadConfigIfChanged() noexcept;

// --- The player ---
// Kept with its weak-this control block, so the next mission's object at the same address is not taken for it.
struct PlayerRef {
    const void* obj=nullptr;
    const void* ctrl=nullptr;
    bool Is(const void* object) const noexcept {
        return obj && obj==object && ctrl && Readable(object,kSelfCtrl+8)
               && At<const void*>(object,kSelfCtrl)==ctrl
               && Readable(ctrl,kCtrlUses+4) && At<std::int32_t>(ctrl,kCtrlUses)>0;
    }
    static PlayerRef Of(const void* object) noexcept {
        PlayerRef r;
        if(!Readable(object,kSelfCtrl+8))return r;
        r.obj=object;
        r.ctrl=At<const void*>(object,kSelfCtrl);
        return r;
    }
};
PlayerRef playerRef;
ULONGLONG playerAt=0;
constexpr ULONGLONG kPlayerMs=2000;

// A human driven by a pad on this machine (the same test src/plugin.cpp's IsPlayer makes).
bool IsPlayer(const unsigned char* human) noexcept {
    return Readable(human,kHumanPlayer+1) && human[kHumanPlayer] && At<const void*>(human,kHumanPad)!=nullptr;
}

unsigned char* PlayerHuman() noexcept {
    __try {
        if(!playerRef.obj || GetTickCount64()-playerAt>kPlayerMs)return nullptr;
        auto* human=const_cast<unsigned char*>(static_cast<const unsigned char*>(playerRef.obj));
        if(!Readable(human,kHumanPlayer+1) || !playerRef.Is(human) || !IsPlayer(human))return nullptr;
        return human;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// --- Enemies: the game's own lock-target registry (every lockable enemy, all around) ---
const std::int32_t* Relations(std::int32_t team) noexcept {
    if(team<0 || team>=kMaxTeam)return nullptr;
    const auto manager=At<const unsigned char*>(image,kTeams);
    if(!Readable(manager,kTeamArray+8))return nullptr;
    const auto rows=At<const unsigned char*>(manager,kTeamArray);
    if(!Readable(rows+team*kTeamStride,kTeamStride))return nullptr;
    const auto relation=At<const std::int32_t*>(rows+team*kTeamStride,kTeamRelation);
    return Readable(relation,kMaxTeam*4) ? relation : nullptr;
}

// The enemy the aim is steering at this frame, the direction both steering paths were last given,
// and the swing limiter they share. Declared before their users so the order in this file is plain.
struct Target { float aim[3]; float camera[3]; bool any; unsigned char* human; ULONGLONG at; };
Target target{};

float muzzleDir[3]{};               // the direction the barrel was last given (for the slew limit)
ULONGLONG muzzleDirAt=0;
float bodyDir[3]{};      // the smoothed facing written to the model
ULONGLONG smoothAt=0;    // when that facing was last advanced

// Rate-limits the barrel: it may not swing more than `slewMs` worth of its current error per frame, so
// switching targets pans the weapon across instead of snapping onto the new one. `slewMs` is the time the
// barrel needs to cover a whole 180 deg turn, which makes the pan the same for any angle. Pass -1 for an
// instant swing (SlewMs=0). `prev`/`prevAt` keep the last direction the barrel was given.
void SlewTo(float* dir,float slewMs,float* prev,ULONGLONG* prevAt,ULONGLONG now) noexcept {
    const float dl=Len(dir);
    if(!(dl>1.0e-6f))return;
    for(int i=0;i<3;++i)dir[i]/=dl;
    const float pl=Len(prev);
    if(!(pl>1.0e-6f) || !(slewMs>0.0f)) {
        for(int i=0;i<3;++i)prev[i]=dir[i];
        *prevAt=now;
        return;
    }
    float p[3]={prev[0]/pl,prev[1]/pl,prev[2]/pl};
    const float maxStep=180.0f/Clamp(slewMs,1.0f,10000.0f)*(now>*prevAt ? static_cast<float>(now-*prevAt) : 0.0f);
    if(maxStep<=0.0f) {   // two calls in the same millisecond: hold the current direction
        for(int i=0;i<3;++i)dir[i]=p[i];
        return;
    }
    const float cosOff=Clamp(Dot(p,dir),-1.0f,1.0f);
    const float off=std::acos(cosOff);
    const float maxRad=maxStep*kPi/180.0f;
    if(off<=maxRad) {
        for(int i=0;i<3;++i)prev[i]=dir[i];
        *prevAt=now;
        return;
    }
    const float s=std::sin(off);
    if(s<1.0e-5f)return;
    const float a=std::sin(off-maxRad)/s,b=std::sin(maxRad)/s;
    for(int i=0;i<3;++i)dir[i]=a*p[i]+b*dir[i];
    const float nl=Len(dir);
    if(nl>1.0e-6f)for(int i=0;i<3;++i)dir[i]/=nl;
    for(int i=0;i<3;++i)prev[i]=dir[i];
    *prevAt=now;
}

// --- Map line of sight: does terrain or a building stand between the player and a point? ---
struct alignas(16) RayInput { float from[4],to[4]; std::uint32_t filter,unk24; std::uint64_t pad; };
static_assert(sizeof(RayInput)==0x30,"EdfRayInput");
struct alignas(16) RayHits { unsigned char raw[0xA0]; };
bool rayOk=false;

// Metres along a->b to the nearest map geometry, or -1 with none (or no physics world). A ray with the
// layer-22 filter only meets terrain and buildings, so characters and vehicles never register.
float CastMapRay(const float* a,const float* b) noexcept {
    if(!rayOk)return -1.0f;
    const auto g=At<unsigned char*>(image,kHavokGlobal);
    if(!g || !Readable(g,0x70) || !At<const void*>(g,0x68))return -1.0f;
    const RayInput in{{a[0],a[1],a[2],1.0f},{b[0],b[1],b[2],1.0f},0x16,0,0};
    RayHits col{};
    *reinterpret_cast<const void**>(col.raw)=image+kHitVtbl;
    reinterpret_cast<void(*)(void*)>(image+kHitReset)(&col);
    reinterpret_cast<void(*)(void*,void*,const RayInput*)>(image+kCastRay)(g+0x10,&col,&in);
    if(*reinterpret_cast<const std::int32_t*>(col.raw+0x0C)==0)return -1.0f;
    const float f=*reinterpret_cast<const float*>(col.raw+0x50);
    if(!std::isfinite(f) || f<0.0f || f>1.0f)return -1.0f;
    const float d[3]={b[0]-a[0],b[1]-a[1],b[2]-a[2]};
    return f*std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
}

// Is map geometry in the way from the player to a point? The player's own reported height is not known (feet,
// centre or eye), and a ray that starts too low grazes the ground within a couple of metres - which in game
// logged as "blocked at hit=1.9 m with the enemy 146 m away". So the ray is cast from a few heights above the
// reported position, and the target counts as visible when any of them reaches it. The margin absorbs the
// last stretch (the ray stopping on the enemy's own feet or the ground at the end of the line).
constexpr float kRayMargin=2.0f;
constexpr float kEyeHeights[]={1.0f,1.6f,2.2f};
constexpr int kEyeCount=static_cast<int>(sizeof(kEyeHeights)/sizeof(kEyeHeights[0]));

bool Blocked(const float* pos,const float* at,float* outHit=nullptr,float* outLen=nullptr) noexcept {
    if(outHit)*outHit=-1.0f;
    if(outLen)*outLen=0.0f;
    for(int i=0;i<kEyeCount;++i) {
        const float eye[3]={pos[0],pos[1]+kEyeHeights[i],pos[2]};
        const float d[3]={at[0]-eye[0],at[1]-eye[1],at[2]-eye[2]};
        const float len=std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
        if(i==0 && outLen)*outLen=len;
        if(!(len>2.0f))return false;   // too close to be talking about a wall
        const float hit=CastMapRay(eye,at);
        if(hit>=0.0f && hit<len-kRayMargin) {
            if(outHit)*outHit=hit;
            continue;                  // this height is blocked: try the next one
        }
        return false;                  // one clear height is enough
    }
    return true;
}

// One lock point of an enemy.
struct EnemyRef { const void* object; float aim[3]; float dist; float off; };

// How much a metre of distance is worth against a radian of angle when picking between two enemies inside the
// cone (1/600: 600 m of distance equals one radian, so the enemy nearest the crosshair wins).
constexpr float kScoreDistance=1.0f/600.0f;

// Why candidates were turned down, for the Debug log: with RequireLineOfSight on, a target that keeps being
// refused is either really behind cover or the map ray is too eager, and the counts tell the two apart.
// losHit/losLen keep the last blocked candidate's numbers (metres), so a run that blocks everything shows
// whether the ray stopped right at the player (a bad origin) or genuinely short of the enemy.
struct Rejects { unsigned seen,range,min,cone,los,dropped; float losHit,losLen,losY; };
Rejects rejects{};

// The enemy to aim at, with the target kept from frame to frame: a target is held until it dies, expires or
// leaves `keepRange`; only then is another picked (the nearest to `from`). This is what stops the aim from
// flipping between two enemies that are equally close - the flip-flopping that made the view unusable.
const void* keptTarget=nullptr;
ULONGLONG keptAt=0;
ULONGLONG keptOutAt=0;   // when the kept target left the cone (0 while it is inside)
ULONGLONG logDropAt=0;   // rate limit for the drop log line
ULONGLONG logLockAt=0;   // rate limit for the lock log line
ULONGLONG bodyLoggedAt=0;   // rate limit for the model-pitch log line
float biasAccum=0.0f;       // BiasRamp sweep value (calibration)
ULONGLONG biasAt=0;
int biasDir=1;              // sweep direction (+1 up, -1 down)
ULONGLONG biasLoggedAt=0;   // rate limit for the sweep log line
ULONGLONG turnLoggedAt=0;   // rate limit for the model-turn log line
const void* lastLogged=nullptr;   // the enemy the target log line last named

bool NearestEnemy(const unsigned char* human,const float* from,const Config& c,
                  const float* facing,EnemyRef* out) noexcept {
    const auto relation=Relations(At<std::int32_t>(human,kObjectTeam));
    const auto registry=At<const unsigned char*>(image,kRegistry);
    if(!relation || !Readable(registry,kRegList+0x10))return false;
    const auto head=At<const unsigned char*>(registry,kRegList);
    if(!Readable(head,0x10))return false;
    const float min2=c.minDistance*c.minDistance;
    const float keep2=c.keepRange*c.keepRange;
    const float coneRad=Clamp(c.cone,1.0f,179.0f)*kPi/180.0f;
    EnemyRef keptRef{},cand{};
    bool haveKept=false;
    // Ray casts are the expensive part (the registry can hold a couple of hundred lock points), so the scan
    // first picks by range and angle alone, and only the target that won is tested for line of sight.
    float bestScore=1.0e9f;
    // The kept target is held while it stays inside the cone (a short grace period absorbs the jitter of a
    // glance) and inside its own wider range. Turning away from it hands the aim to whoever is in front.
    const float fl=Len(facing);
    int n=0;
    for(auto node=At<const unsigned char*>(head,0);node!=head && n<kMaxNodes;
        node=At<const unsigned char*>(node,0),++n) {
        const auto lockPoint=At<const unsigned char*>(node,kNodeTarget);
        if(!lockPoint || lockPoint[0]!=0 || !lockPoint[kTargetValid] || !lockPoint[kTargetLockable])continue;
        const auto object=At<const unsigned char*>(lockPoint,kTargetObject);
        if(!object || object==human || object[kObjectDead])continue;
        const auto other=At<std::int32_t>(object,kObjectTeam);
        if(other<0 || other>=kMaxTeam || relation[other]!=kEnemyRelation)continue;
        const float* a=reinterpret_cast<const float*>(lockPoint+kTargetAim);
        if(!std::isfinite(a[0]) || !std::isfinite(a[1]) || !std::isfinite(a[2]))continue;
        const float d[3]={a[0]-from[0],a[1]-from[1],a[2]-from[2]};
        const float dist2=Dot(d,d);
        const float dist=std::sqrt(dist2);
        ++rejects.seen;
        if(dist2<min2){++rejects.min;continue;}
        // The angle off the crosshair: what "the enemy the player is looking at" means.
        float off=0.0f;
        if(facing && fl>0.0001f && dist>0.0001f)off=std::acos(Clamp(Dot(facing,d)/(fl*dist),-1.0f,1.0f));
        if(object==keptTarget) {
            if(dist2>keep2 || off>coneRad) {
                // Out of its range, or the player has turned away: it loses the lock after a short grace
                // period, so a glance does not make the aim jump between enemies.
                if(!keptOutAt)keptOutAt=GetTickCount64();
                else if(GetTickCount64()-keptOutAt>c.coneGraceMs) {
                    ++rejects.dropped;
                    if(c.debug && GetTickCount64()-logDropAt>=500) {
                        logDropAt=GetTickCount64();
                        Log("PLAYERAIM drop target=%p reason=%s off=%.1fdeg dist=%.0f",
                            object,off>coneRad ? "turned-away" : "out-of-range",
                            static_cast<double>(off*180.0f/kPi),static_cast<double>(dist));
                    }
                    keptTarget=nullptr;
                    keptOutAt=0;
                    break;
                }
            } else keptOutAt=0;
            keptRef.object=object;
            std::memcpy(keptRef.aim,a,12);
            keptRef.dist=dist;
            keptRef.off=off;
            haveKept=true;
            break;
        }
        // A new target has to be inside the cone...
        if(off>coneRad) {
            ++rejects.cone;
            continue;
        }
        // ...and is picked by how close it is to the crosshair, with the distance as a secondary weight. 600 m
        // of distance is worth one radian of angle, so an enemy dead ahead beats a nearer one off to the side.
        const float score=off+dist*kScoreDistance;
        if(score>=bestScore)continue;
        bestScore=score;
        cand.object=object;
        std::memcpy(cand.aim,a,12);
        cand.dist=dist;
        cand.off=off;
    }
    // The line-of-sight test, on the kept target first: it is dropped when something now stands in the way,
    // so the aim hands over to another enemy instead of shooting a wall forever.
    if(haveKept && c.requireLos) {
        float hit=-1.0f,len=0.0f;
        if(Blocked(from,keptRef.aim,&hit,&len)) {
            ++rejects.dropped;
            rejects.losHit=hit;
            rejects.losLen=len;
            rejects.losY=keptRef.aim[1];
            keptTarget=nullptr;
            haveKept=false;
        }
    }
    if(!haveKept && cand.object && c.requireLos) {
        float hit=-1.0f,len=0.0f;
        if(Blocked(from,cand.aim,&hit,&len)) {
            ++rejects.los;
            rejects.losHit=hit;
            rejects.losLen=len;
            rejects.losY=cand.aim[1];
            cand.object=nullptr;
        }
    }

    if(haveKept) {
        *out=keptRef;
    } else if(cand.object) {
        keptTarget=cand.object;
        *out=cand;
    } else {
        keptTarget=nullptr;
        return false;
    }
    keptAt=GetTickCount64();
    return true;
}

// --- The shot direction (WeaponBase vtable slot 22) ---
// The stock body reads the weapon's own state; this hook replaces the result with the direction the plugin is
// steering, under the same slew limit as the model, so the bullet and the weapon can never disagree.
using ShotDirFn=void(__fastcall*)(void*,void*,void*,void*);
ShotDirFn shotDirOriginal=nullptr;
ULONGLONG shotDirLoggedAt=0;
int shotDirCalls=0;   // how many times the hook has run (the first few are logged in full)
float shotDirApplied[3]{};   // the direction that was last written for a bullet (unit), 0 when none yet
float shotDirAppliedLen=0.0f;   // the stock function's own length at that moment (0 = it gave nothing)

// How far the nearest of a weapon's muzzles is from the player (metres2), or -1 when the weapon has none the
// plugin can read. Used both to decide whether a weapon is the player's and to say why in the log.
float MuzzleNearDist2(const unsigned char* weapon,const unsigned char* human,unsigned* outCount=nullptr) noexcept {
    if(outCount)*outCount=0;
    if(!human)return -1.0f;
    const float* mine=reinterpret_cast<const float*>(human+kHumanMatrix)+12;
    const auto muzzles=At<const unsigned char*>(weapon,kMuzzleArray);
    const auto count=At<std::int32_t>(weapon,kMuzzleCount);
    if(!muzzles || count<1 || count>64)return -1.0f;
    float best=-1.0f;
    for(int i=0;i<count && i<8;++i) {
        const auto muzzle=muzzles+static_cast<std::size_t>(i)*kMuzzleStride;
        // The muzzle's world position is row 3 of the bone matrix at muzzle+0 (the same read the muzzle hook
        // and EDF6AutoTurret's gunner.cpp make).
        const auto bone=At<const unsigned char*>(muzzle,0);
        if(!bone || !Readable(bone,kBoneRows+0x40))continue;
        const float* b=reinterpret_cast<const float*>(bone+kBoneRows);
        const float d[3]={b[12]-mine[0],b[13]-mine[1],b[14]-mine[2]};
        const float d2=Dot(d,d);
        if(best<0.0f || d2<best)best=d2;
    }
    if(outCount)*outCount=static_cast<unsigned>(count);
    return best;
}

void __fastcall ShotDirHook(void* self,void* out,void* a,void* b) noexcept {
    if(!shotDirOriginal)return;
    shotDirOriginal(self,out,a,b);
    __try {
        auto* const weapon=static_cast<unsigned char*>(self);
        // The first few calls say everything about whether this path is the player's: who the object is, what
        // its muzzle array looks like, and how far the nearest muzzle is from the player.
        if(published->debug && shotDirCalls<8) {
            ++shotDirCalls;
            const auto human=target.human;
            unsigned count=0;
            const float d2=MuzzleNearDist2(weapon,human,&count);
            const auto muzzles=At<const unsigned char*>(weapon,kMuzzleArray);
            Log("PLAYERAIM shotdir call=%d weapon=%p muzzles=%p count=%u near2=%.1f any=%d age=%llu enabled=%d target=%p",
                shotDirCalls,weapon,muzzles,count,static_cast<double>(d2),target.any?1:0,
                static_cast<unsigned long long>(GetTickCount64()-target.at),
                published->enabled ? 1 : 0,human);
        }
        if(!out || !published->enabled || !AimsWeapons(*published))return;
        if(!target.any || GetTickCount64()-target.at>=200)return;
        {
            const float near2=MuzzleNearDist2(weapon,target.human);
            if(!(near2>=0.0f) || near2>kMuzzleReach*kMuzzleReach)return;
        }
        auto* dir=static_cast<float*>(out);
        if(!std::isfinite(dir[0]) || !std::isfinite(dir[1]) || !std::isfinite(dir[2]))return;
        // The stock magnitude is kept: a round's speed comes from the weapon's data, not from here, and
        // preserving it means a scaled value (if any weapon used one) still behaves.
        const float mag=Len(dir);
        const float* from=reinterpret_cast<const float*>(target.human+kHumanMatrix)+12;
        // The direction the shot should take, put through the swing limit: this is what makes a target change a
        // pan rather than a snap.
        float want[3]={target.aim[0]-from[0],target.aim[1]-from[1],target.aim[2]-from[2]};
        SlewTo(want,static_cast<float>(published->slewMs),muzzleDir,&muzzleDirAt,GetTickCount64());
        const float wl=Len(want);
        if(!(wl>1.0e-6f))return;
        // How far the shot is being turned from where the stock function pointed it, logged so a run shows
        // whether the bullet's own direction is really being steered (and by how much), not just the model.
        // The stock direction is normally a real vector; when it reads as zero the function is not the one
        // that decides where a bullet goes, and the write below is going into a buffer nobody reads.
        {
            const float sl=Len(dir);
            if(published->debug && GetTickCount64()-shotDirLoggedAt>=400) {
                shotDirLoggedAt=GetTickCount64();
                const float cosA=(sl>1.0e-6f) ? Clamp(Dot(dir,want)/(sl*wl),-1.0f,1.0f) : 1.0f;
                Log("PLAYERAIM shotdir stock=%.2f stockVsWant=%.1fdeg",
                    static_cast<double>(sl),static_cast<double>(std::acos(cosA)*180.0f/kPi));
            }
            // Remembered for the "aim" line, which needs to stay measurable well past the first shots: if the
            // bullets really follow the target while a stock call reads zero, the override is working and the
            // mismatch the eye sees is somewhere else.
            shotDirAppliedLen=sl;
            for(int i=0;i<3;++i)shotDirApplied[i]=(sl>1.0e-6f) ? dir[i]/sl : 0.0f;
        }
        for(int i=0;i<3;++i)dir[i]=(mag>1.0e-4f ? mag : 1.0f)*want[i]/wl;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

// --- The barrel (the auto-aim itself in the default weapons mode) ---
using MuzzleFn=void(__fastcall*)(void*,const float*);
MuzzleFn muzzleBuilder=nullptr;
ULONGLONG muzzleLoggedAt=0;
ULONGLONG muzzleModeAt=0;   // rate limit for the muzzle-mode log line
volatile long long muzzleAimed=0;   // barrels turned onto a target since load (Debug logging)

// Builds a row-major 4x4 whose rows 0..2 are an orthonormal basis with row 2 (forward) along `dir`, spinning
// the given `right` as little as possible; row 3 is copied from `src` (the position).
void BasisFrom(const float* dir,const float* right,const float* src,float* m) noexcept {
    const float l=Len(dir);
    if(!(l>1.0e-6f))return;
    const float f[3]={dir[0]/l,dir[1]/l,dir[2]/l};
    float r[3]={right[0],right[1],right[2]};
    const float d=Dot(r,f);
    for(int i=0;i<3;++i)r[i]-=f[i]*d;
    float rl=Len(r);
    if(rl<1.0e-5f) {   // the reference is along the forward: take any perpendicular
        const float ax[3]={f[2],0.0f,-f[0]};
        const float al=Len(ax);
        if(al>1.0e-5f){r[0]=ax[0]/al;r[1]=ax[1]/al;r[2]=ax[2]/al;rl=1.0f;}
        else {r[0]=1.0f;r[1]=0.0f;r[2]=0.0f;rl=1.0f;}
    }
    for(int i=0;i<3;++i)r[i]/=rl;
    const float u[3]={f[1]*r[2]-f[2]*r[1],f[2]*r[0]-f[0]*r[2],f[0]*r[1]-f[1]*r[0]};
    for(int i=0;i<3;++i){m[i]=r[i];m[4+i]=u[i];m[8+i]=f[i];m[12+i]=src[12+i];}
    m[3]=m[7]=m[11]=0.0f;m[15]=1.0f;
}

// Smooths the model's own facing so the body turns at a visible rate instead of snapping onto a new enemy.
// It is a separate piece of state on purpose: the aim point and the shot direction are never routed through
// it, so the bullets cannot be affected by it. `ms` is the time for the facing to cover most of a change
// (0 disables the filter and the body turns at once, as it does without this feature).
void SmoothDir(float* state,const float* want,float ms) noexcept {
    const float wl=Len(want);
    if(!(wl>1.0e-6f))return;
    const float w[3]={want[0]/wl,want[1]/wl,want[2]/wl};
    const float sl=Len(state);
    if(!(sl>1.0e-4f) || !(ms>1.0f)) {   // first use, or the filter is off
        for(int i=0;i<3;++i)state[i]=w[i];
        return;
    }
    // The clock is only moved when the filter really advances. This body runs several times per frame (the
    // human input hook is called more than once), and moving the clock on every call left each later call of
    // the same frame with a 0 ms step - which quietly turned the whole filter off and made the model snap
    // onto the target, so BodySmoothMs looked like it did nothing.
    const ULONGLONG now=GetTickCount64();
    const float dt=(smoothAt && now>smoothAt) ? static_cast<float>(now-smoothAt)*0.001f : 0.0f;
    if(!(dt>0.0f))return;
    smoothAt=now;
    const float k=Clamp(dt/(ms*0.001f),0.0f,1.0f);
    for(int i=0;i<3;++i)state[i]+=(w[i]-state[i])*k;
    const float nl2=Len(state);
    if(nl2>1.0e-4f)for(int i=0;i<3;++i)state[i]/=nl2;
}

// How fast the smoothed facing is really turning, in degrees per second, measured between two visits of the
// log line. Printed so "the setting does nothing" can be told apart from "the target change was too small to
// notice": with 900 ms of smoothing a real swing shows up as tens of degrees per second.
float BodyRateDegPerSec(const float* dir) noexcept {
    static float last[3]{};
    static ULONGLONG lastAt=0;
    const ULONGLONG now=GetTickCount64();
    float rate=0.0f;
    const float dl=Len(dir),ll=Len(last);
    if(dl>1.0e-4f && ll>1.0e-4f && lastAt && now>lastAt) {
        const float cosA=Clamp(Dot(dir,last)/(dl*ll),-1.0f,1.0f);
        rate=std::acos(cosA)*180.0f/kPi/(static_cast<float>(now-lastAt)*0.001f);
    }
    for(int i=0;i<3;++i)last[i]=dir[i];
    lastAt=now;
    return rate;
}

// Turns the model/skeleton towards the target, so the weapon the model carries points where the shot goes.
//
// The yaw and the pitch do NOT share a frame. The combinations were narrowed down in game one axis at a time:
//   * yaw: the target's WORLD bearing (atan2(x, z)), blended from the camera's bearing by BodyYaw. Taken from
//     the camera instead, the weapon stopped tracking left and right at all - while the world bearing had been
//     tracking correctly through every earlier version;
//   * pitch: the OFFSET from the camera's centre line (the game adds the camera's own pitch to it). The
//     target's world elevation was tried there and left the weapon resting on the camera whenever the camera
//     was tilted, which is the "tilted camera, weapon not pointing" report.
// So the two are built separately and only then combined into one world direction.
//
// Only the model's own frame at +0x5e0 is written. The facing quaternion at +0xd90 is deliberately left
// alone: the game's aim code writes it itself every frame, so pre-empting it was the risky write of the two.
void TurnBody(unsigned char* human,const float* camRight,const float* camUp,const float* camFwd,
              const float* worldUp,const float* to,float bodyYaw,float bodyPitch,float coneDeg,
              float yawBias,float pitchBias) noexcept {
    if(!(bodyYaw>0.0f) && !(bodyPitch>0.0f))return;
    const float tl=Len(to);
    if(!(tl>1.0e-4f))return;
    (void)camRight;(void)camUp;(void)worldUp;
    const float maxOff=Clamp(coneDeg,1.0f,179.0f)*kPi/180.0f;
    const float tgt[3]={to[0]/tl,to[1]/tl,to[2]/tl};
    // The horizontal part of the turn: how far round the camera is from the target's bearing.
    const float camFlat[3]={camFwd[0],0.0f,camFwd[2]};
    const float camFlatLen=Len(camFlat);
    const float tgtFlat[3]={tgt[0],0.0f,tgt[2]};
    const float tgtFlatLen=Len(tgtFlat);
    float dir[3];
    if(camFlatLen>1.0e-4f && tgtFlatLen>1.0e-4f) {
        const float cf[3]={camFlat[0]/camFlatLen,0.0f,camFlat[2]/camFlatLen};
        const float tf[3]={tgtFlat[0]/tgtFlatLen,0.0f,tgtFlat[2]/tgtFlatLen};
        // Whether the target is close enough to the crosshair to be aimed at: the angle between the camera's
        // own direction and the target, which is what "off the crosshair" means. (Measuring the pitch part
        // alone rejected everything as soon as the camera tilted: a level target then sits far below the
        // centre line, and the whole aim stopped.)
        const float offCross=std::acos(Clamp(Dot(tgt,camFwd),-1.0f,1.0f));
        if(offCross>maxOff)return;
        // How to aim it. The bearing is the target's own world bearing; the pitch is the target's elevation
        // measured from where the camera's horizontal plane sits (the arms carry that offset), so a target on
        // the player's own level needs `-camera pitch`, not 0.
        //
        // Both halves of the elevation are in METRES: `to[1]` against the true horizontal distance. Using the
        // normalised direction's horizontal length (a unitless ratio, at most 1) here instead mixed metres with
        // a ratio, which turned a target 172 m away and 1 m up into 30-70 degrees - the weapon pointed at the
        // sky.
        const float toFlatLen=sqrtf(to[0]*to[0]+to[2]*to[2]);
        const float pitchRaw=std::atan2(to[1],toFlatLen)-std::atan2(camFwd[1],camFlatLen);
        // Yaw: the target's own world bearing (the one that tracked left and right all along).
        const float yaw=std::atan2(tf[0],tf[2])+yawBias;
        const float pitch=(pitchRaw*Clamp(bodyPitch,0.0f,1.0f))+pitchBias;
        // The inputs behind the pitch, so a wrong value can be read straight off: `toY` is in metres, the two
        // lengths should be positive, and both halves of the difference should be small angles.
        if(published->debug && GetTickCount64()-turnLoggedAt>=500) {
            turnLoggedAt=GetTickCount64();
            Log("PLAYERAIM turn to=(%.1f,%.1f,%.1f) |to|=%0.1f flatMeters=%0.1f camFwd=(%.2f,%.2f,%.2f) |camFwd|=%0.2f tgtPitch=%.1fdeg camPitch=%.1fdeg pitchRaw=%.1fdeg",
                static_cast<double>(to[0]),static_cast<double>(to[1]),static_cast<double>(to[2]),
                static_cast<double>(tl),static_cast<double>(toFlatLen),
                static_cast<double>(camFwd[0]),static_cast<double>(camFwd[1]),static_cast<double>(camFwd[2]),
                static_cast<double>(Len(camFwd)),
                static_cast<double>(std::atan2(to[1],toFlatLen)*180.0f/kPi),
                static_cast<double>(std::atan2(camFwd[1],camFlatLen)*180.0f/kPi),
                static_cast<double>(pitchRaw*180.0f/kPi));
        }
        const float cp=std::cos(pitch);
        dir[0]=cp*std::sin(yaw);
        dir[1]=std::sin(pitch);
        dir[2]=cp*std::cos(yaw);
    } else {
        for(int i=0;i<3;++i)dir[i]=tgt[i];
    }
    if(!(std::isfinite(dir[0]) && std::isfinite(dir[1]) && std::isfinite(dir[2])))return;
    // The model's frame at +0x5e0: rows right / up / forward / position. Only rows 0..2 are replaced, so the
    // position (row 3, the frame the model is drawn at) is the game's own.
    const auto* m=reinterpret_cast<const float*>(human+0x5e0);
    float out[16];
    BasisFrom(dir,m,m,out);
    std::memcpy(human+0x5e0,out,sizeof(out));
}

// The target taken this frame, handed to the muzzle hook.

// The barrel's world position, as 0x6969A0 composes it from the muzzle's bone rows and local matrix.
bool MuzzleWorld(const unsigned char* muzzle,float* pos) noexcept {
    const auto bone=At<const unsigned char*>(muzzle,0);
    if(!Readable(bone,kBoneRows+0x40))return false;
    float b[4][4],l[4][4];
    std::memcpy(b,bone+kBoneRows,sizeof(b));
    std::memcpy(l,muzzle+kMuzzleLocal,sizeof(l));
    for(int c=0;c<3;++c)
        pos[c]=l[3][0]*b[0][c]+l[3][1]*b[1][c]+l[3][2]*b[2][c]+l[3][3]*b[3][c];
    return std::isfinite(pos[0]) && std::isfinite(pos[1]) && std::isfinite(pos[2]);
}

// Rebuilds the aim basis rows (right / up / forward, 16 bytes each) so its forward row points at `aim`,
// keeping the current up as the roll reference, and never further than `maxOff` radians off `limit` (the
// camera direction: the barrel turns at most that far from where the player looks).
bool AimAt(const float* from,const float* aim,const float* limit,float maxOff,float* rows) noexcept {
    float f[3]={aim[0]-from[0],aim[1]-from[1],aim[2]-from[2]};
    float fl=Len(f);
    if(!(fl>0.5f))return false;
    for(int i=0;i<3;++i)f[i]/=fl;
    if(limit) {
        const float ll=Len(limit);
        if(ll>0.0001f) {
            const float l[3]={limit[0]/ll,limit[1]/ll,limit[2]/ll};
            const float cosOff=Clamp(Dot(f,l),-1.0f,1.0f);
            const float off=std::acos(cosOff);
            if(off>maxOff) {
                // Slerp the barrel direction from the camera direction towards the target, only as far as
                // the cone allows, so a target behind the player does not twist the gun.
                const float t=maxOff/off;
                const float s=std::sin(off);
                if(s<1.0e-4f)return false;
                const float a=std::sin((1.0f-t)*off)/s,b=std::sin(t*off)/s;
                for(int i=0;i<3;++i)f[i]=a*l[i]+b*f[i];
                fl=Len(f);
                if(!(fl>1.0e-4f))return false;
                for(int i=0;i<3;++i)f[i]/=fl;
            }
        }
    }
    const float* fd=f;
    const float up[3]={rows[4],rows[5],rows[6]};
    float r[3]={up[1]*fd[2]-up[2]*fd[1],up[2]*fd[0]-up[0]*fd[2],up[0]*fd[1]-up[1]*fd[0]};
    const float rl=Len(r);
    if(!(rl>1.0e-6f))return false;
    for(int i=0;i<3;++i)r[i]/=rl;
    const float u[3]={fd[1]*r[2]-fd[2]*r[1],fd[2]*r[0]-fd[0]*r[2],fd[0]*r[1]-fd[1]*r[0]};
    for(int i=0;i<3;++i){rows[i]=r[i];rows[4+i]=u[i];rows[8+i]=fd[i];}
    rows[3]=rows[7]=rows[11]=0.0f;
    return true;
}

// Every call to 0x6969A0 goes through here: a barrel of the local player gets a rotated copy of the aim
// basis, so the shot leaves along the target direction while the player's view stays where it is.
void __fastcall MuzzleHook(void* muzzle,const float* aimRows) noexcept {
    if(!muzzleBuilder)return;
    __try {
        float pos[3]{};
        const auto human=target.human;
        bool theirs=false;
        if(human && target.any && published->enabled && AimsWeapons(*published)
           && GetTickCount64()-target.at<200
           && MuzzleWorld(static_cast<const unsigned char*>(muzzle),pos)) {
            const float* mine=reinterpret_cast<const float*>(human+kHumanMatrix)+12;
            const float d[3]={pos[0]-mine[0],pos[1]-mine[1],pos[2]-mine[2]};
            theirs=Dot(d,d)<kMuzzleReach*kMuzzleReach;
        }
        if(!theirs || !aimRows || !Readable(aimRows,0x40)) {
            muzzleBuilder(muzzle,aimRows);
            return;
        }
        // What the muzzle's own bone says now, before this call rebuilds it: the forward row of the world
        // matrix the game itself wrote at the end of the previous frame (muzzle+0xB0, row 2). The `rows` the
        // hook builds are only its INPUT to the builder, so measuring those said the plugin wanted the right
        // direction, not that the weapon actually ended up on it - which is the thing that matters visually.
        float actual[3]{};
        float actualPos[3]{};
        bool haveActual=false;
        {
            const auto bone=At<const unsigned char*>(muzzle,0);
            if(bone && Readable(bone,kBoneRows+0x40)) {
                const float* b=reinterpret_cast<const float*>(bone+kBoneRows);
                actual[0]=b[8];actual[1]=b[9];actual[2]=b[10];
                actualPos[0]=b[12];actualPos[1]=b[13];actualPos[2]=b[14];
                haveActual=Len(actual)>0.5f;
            }
        }
        float rows[16];
        std::memcpy(rows,aimRows,sizeof(rows));
        // What the caller passed and what it holds at +0xB0 (the builder reads that instead of the rows when
        // the muzzle's mode is 1 or 2), so the log says which of the two the builder is actually using.
        if(published->debug && GetTickCount64()-muzzleModeAt>=1000) {
            muzzleModeAt=GetTickCount64();
            const float* big=aimRows+kMuzzleRows/sizeof(float);
            Log("PLAYERAIM muzzlemode=%d aimRows=%p rowsAtB0=(%.2f,%.2f,%.2f)",
                At<std::int32_t>(muzzle,kMuzzleMode),aimRows,
                static_cast<double>(big[8]),static_cast<double>(big[9]),static_cast<double>(big[10]));
        }
        const float maxOff=Clamp(published->cone,1.0f,179.0f)*kPi/180.0f;
        // The direction the barrel should end up on, with the same swing limit as the shot.
        float want[3]={target.aim[0]-pos[0],target.aim[1]-pos[1],target.aim[2]-pos[2]};
        SlewTo(want,static_cast<float>(published->slewMs),muzzleDir,&muzzleDirAt,GetTickCount64());
        const float aimAt[3]={pos[0]+want[0],pos[1]+want[1],pos[2]+want[2]};
        if(AimAt(pos,aimAt,target.camera,maxOff,rows)) {
            muzzleBuilder(muzzle,rows);
            // The barrel really was turned: say so, with the angles, so a log shows the weapon aiming itself
            // and not only the target being seen (Debug=1).
            ++muzzleAimed;
            if(published->debug && GetTickCount64()-muzzleLoggedAt>=500) {
                muzzleLoggedAt=GetTickCount64();
                const float old[3]={aimRows[8],aimRows[9],aimRows[10]};
                const float* nw=rows+8;
                const float ol=Len(old),nl=Len(nw);
                const float cosA=(ol>0.0001f && nl>0.0001f) ? Clamp(Dot(old,nw)/(ol*nl),-1.0f,1.0f) : 1.0f;
                const float to[3]={target.aim[0]-pos[0],target.aim[1]-pos[1],target.aim[2]-pos[2]};
                const float dl=Len(to);
                const float cosT=(dl>0.0001f && nl>0.0001f) ? Clamp(Dot(nw,to)/(nl*dl),-1.0f,1.0f) : 1.0f;
                // The three numbers that matter: what the muzzle bone really points at (actualVsTarget), how
                // far that is from what the plugin asked for (actualVsWant), and the shot direction.
                const float al=Len(actual);
                const float cosAT=(haveActual && dl>0.0001f) ? Clamp(Dot(actual,to)/(al*dl),-1.0f,1.0f) : 1.0f;
                const float cosAW=(haveActual && nl>0.0001f) ? Clamp(Dot(actual,nw)/(al*nl),-1.0f,1.0f) : 1.0f;
                const float bl=Len(shotDirApplied);
                const float cosB=(bl>0.0001f && nl>0.0001f) ? Clamp(Dot(nw,shotDirApplied)/(nl*bl),-1.0f,1.0f) : 1.0f;
                Log("PLAYERAIM barrel n=%llu mode=%d turned=%.1fdeg wantVsTarget=%.1fdeg actualVsTarget=%.1fdeg actualVsWant=%.1fdeg shotVsWant=%.1fdeg stock=%.2f actualPos=(%.1f,%.1f,%.1f) muzzlePos=(%.1f,%.1f,%.1f)",
                    static_cast<unsigned long long>(muzzleAimed),
                    At<std::int32_t>(muzzle,kMuzzleMode),
                    static_cast<double>(std::acos(cosA)*180.0f/kPi),
                    static_cast<double>(std::acos(cosT)*180.0f/kPi),
                    static_cast<double>(std::acos(cosAT)*180.0f/kPi),
                    static_cast<double>(std::acos(cosAW)*180.0f/kPi),
                    static_cast<double>(std::acos(cosB)*180.0f/kPi),
                    static_cast<double>(shotDirAppliedLen),
                    static_cast<double>(actualPos[0]),static_cast<double>(actualPos[1]),
                    static_cast<double>(actualPos[2]),
                    static_cast<double>(pos[0]),static_cast<double>(pos[1]),static_cast<double>(pos[2]));
            }
            return;
        }
        muzzleBuilder(muzzle,aimRows);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        if(muzzleBuilder)muzzleBuilder(muzzle,aimRows);
    }
}

// --- The human input hooks ---
using HumanInputFn=void(__fastcall*)(void*,void*,void*,void*);
HumanInputFn originalInput[kInputCount]{};
volatile long installed=0;
ULONGLONG loggedAt=0;

// One frame, once the human is known to be the local player. The stock body runs first, so the player's own
// stick is applied and this plugin has the last word for the frame.
void Watch(HumanInputFn original,void* self,void* a,void* b,void* c) noexcept {
    auto* const human=static_cast<unsigned char*>(self);
    __try {
        original(self,a,b,c);
        ReloadConfigIfChanged();
        if(!published->enabled)return;
        // Remember the player (the pad/player test is the game's own flag) under a weak-this check.
        if(IsPlayer(human)) {
            playerRef=PlayerRef::Of(human);
            playerAt=GetTickCount64();
        }
        const auto m=reinterpret_cast<const float*>(human+kHumanMatrix);
        const float* pos=m+12;
        const float facing[3]={m[8],m[9],m[10]};
        EnemyRef e{};
        const bool enemy=NearestEnemy(human,pos,*published,facing,&e);
        target.any=enemy;
        target.human=human;
        target.at=GetTickCount64();
        if(enemy) {
            // The real aim point, untouched: the model turns onto it at once, and the engine's own pose
            // blending is what makes that look smooth. The swing limit lives where the shot direction is
            // produced (the muzzle hook and the shot-direction hook), so the barrel and the bullet pan.
            std::memcpy(target.aim,e.aim,12);
            std::memcpy(target.camera,facing,12);
            // A line whenever the lock moves to another enemy, so "which enemy is it on" is answerable from
            // the log alone while the test range spawns one wave after another.
            if(published->debug && e.object!=lastLogged) {
                lastLogged=e.object;
                Log("PLAYERAIM target=%p off=%.1fdeg dist=%.0f aim=(%.1f,%.1f,%.1f)",
                    e.object,static_cast<double>(e.off*180.0f/kPi),static_cast<double>(e.dist),
                    static_cast<double>(e.aim[0]),static_cast<double>(e.aim[1]),static_cast<double>(e.aim[2]));
            }
        }
        if(enemy && (published->bodyYaw>0.0f || published->bodyPitch>0.0f)) {
            // The model turns towards the target (the gun alone moving looked wrong), still bounded by the
            // same cone: a target out of the body's reach leaves the model alone, so the gun never points
            // out of the side of a body that faces elsewhere. Yaw and pitch are separate shares: the model
            // should carry the weapon up and down with the shot, or the weapon visibly disagrees with it.
            const float dx=e.aim[0]-pos[0],dz=e.aim[2]-pos[2];
            if(dx*dx+dz*dz>1.0f) {
                // The target vector in METRES, with all three parts measured from the same origin - the
                // player's own position. Mixing a relative horizontal offset with an absolute world height is
                // what produced the absurd pitch: a target 178 m away and 1.7 m up came out as
                // atan2(1.7, 1.0) = 60 deg, and the weapon pointed at the sky.
                const float toTarget[3]={dx,e.aim[1]-pos[1],dz};
                // The model follows a filtered copy of the target direction, so switching enemies pans the
                // body across instead of snapping. target.aim is untouched, so the muzzle and the bullets keep
                // their own path and their own limit.
                float toBody[3]={toTarget[0],toTarget[1],toTarget[2]};
                SmoothDir(bodyDir,toTarget,published->bodySmoothMs);
                // The camera basis straight from the player's own aim matrix (rows right / up / forward): the
                // model's aim offsets are applied in this frame by the game's pose code.
                const float* cr=m;
                const float* cu=m+4;
                const float* cf=m+8;
                const float up[3]={0.0f,1.0f,0.0f};
                // A bias that can ramp: with BiasRamp set, the value walks by that much per second instead of
                // holding, so one ini edit sweeps a whole range while the weapon is watched (calibration).
                if(published->biasRamp!=0.0f) {
                    const ULONGLONG now=GetTickCount64();
                    const float limit=std::fabs(published->biasRamp);
                    if(biasAt && now>biasAt) {
                        const float dt=static_cast<float>(now-biasAt)*0.001f;
                        biasAccum+=static_cast<float>(biasDir)*limit*dt;
                        if(biasAccum>limit){biasAccum=limit;biasDir=-1;}
                        if(biasAccum<-limit){biasAccum=-limit;biasDir=1;}
                    }
                    biasAt=now;
                    if(published->debug && GetTickCount64()-biasLoggedAt>=200) {
                        biasLoggedAt=GetTickCount64();
                        Log("PLAYERAIM bias=%.3frad (%.1fdeg)",static_cast<double>(biasAccum),
                            static_cast<double>(biasAccum*180.0f/kPi));
                    }
                }
                TurnBody(human,cr,cu,cf,up,toBody,published->bodyYaw,published->bodyPitch,published->cone,
                         published->yawBias+biasAccum,published->pitchBias+biasAccum);
                // What the model's own frame now says, so a log tells up-aim from down-aim at a glance: with
                // the game's convention (fwd = (sin B, -sin A, cos B)) a forward Y above 0 means aiming down.
                if(published->debug && GetTickCount64()-bodyLoggedAt>=500 && Readable(human+0x5E0,0x40)) {
                    bodyLoggedAt=GetTickCount64();
                    const float* m2=reinterpret_cast<const float*>(human+0x5E0);
                    const float tl=Len(toBody);
                    const float relYaw=(tl>0.0001f) ? std::atan2(Dot(toBody,cr)/tl,Dot(toBody,cf)/tl) : 0.0f;
                    const float relPitch=(tl>0.0001f) ? std::asin(Clamp(Dot(toBody,cu)/tl,-1.0f,1.0f)) : 0.0f;
                    const float rate=BodyRateDegPerSec(toBody);
                    Log("PLAYERAIM body relYaw=%.1fdeg relPitch=%.1fdeg modelFwd=(%.2f,%.2f,%.2f) camPitch=%.1fdeg rate=%.0fdegps bias=%.3f",
                        static_cast<double>(relYaw*180.0f/kPi),static_cast<double>(relPitch*180.0f/kPi),
                        m2[8],m2[9],m2[10],
                        static_cast<double>(-std::asin(Clamp(cf[1],-1.0f,1.0f))*180.0f/kPi),
                        static_cast<double>(rate),static_cast<double>(published->pitchBias+biasAccum));
                }
            }
        }
        if(enemy && AimsView(*published)) {
            // View mode: fwd = (sin B, -sin A, cos B), so writing the two angles aims the camera and the
            // crosshair (A at +0x1230, B at +0x1234; docs/player-aim-re.md section 1).
            const float dx=e.aim[0]-pos[0],dy=e.aim[1]-pos[1],dz=e.aim[2]-pos[2];
            const float flat=std::sqrt(dx*dx+dz*dz);
            const float wantYaw=std::atan2(dx,dz);
            const float wantPitch=(flat>0.01f) ? -std::atan2(dy,flat) : 0.0f;
            const float yaw=At<float>(human,kAngleYaw);
            const float pitch=At<float>(human,kAnglePitch);
            const float g=Clamp(published->gain,0.02f,1.0f);
            Put<float>(human,kAngleYaw,yaw+g*Wrap(wantYaw-yaw));
            Put<float>(human,kAnglePitch,pitch+g*(wantPitch-pitch));
        }
        if(published->test!=0.0f) {
            const std::size_t which=published->testAxis==1 ? kAngleYaw
                                 : published->testAxis==2 ? kAngleC
                                 : published->testAxis==3 ? kAngleD : kAnglePitch;
            Put<float>(human,which,At<float>(human,which)+published->test);
        }
        if(published->debug && GetTickCount64()-loggedAt>=500) {
            loggedAt=GetTickCount64();
            const float dx=e.aim[0]-pos[0],dy=e.aim[1]-pos[1],dz=e.aim[2]-pos[2];
            const float to[3]={dx,dy,dz};
            const float fl=Len(facing),tl=enemy ? Len(to) : 0.0f;
            const float cosOff=(enemy && fl>0.0001f && tl>0.0001f) ? Dot(facing,to)/(fl*tl) : 0.0f;
            const float off=std::acos(Clamp(cosOff,-1.0f,1.0f))*180.0f/kPi;
            Log("PLAYERAIM v=%p pos=(%.1f,%.1f,%.1f) pitch=%.3f yaw=%.3f fwd=(%.2f,%.2f,%.2f) enemy=%d dist=%.0f off=%.1f aim=%s rej=%u/%u/%u/%u/%u los(hit=%.1f len=%.1f aimY=%.1f)",
                human,pos[0],pos[1],pos[2],At<float>(human,kAnglePitch),At<float>(human,kAngleYaw),
                facing[0],facing[1],facing[2],enemy?1:0,static_cast<double>(e.dist),static_cast<double>(off),
                published->aim==AimAt::weapons ? "weapons" : (published->aim==AimAt::view ? "view" : "both"),
                rejects.seen,rejects.range,rejects.cone,rejects.los,rejects.dropped,
                static_cast<double>(rejects.losHit),static_cast<double>(rejects.losLen),
                static_cast<double>(rejects.losY));
            // Which enemy holds the lock (an address, its angle off the crosshair and its distance): during a
            // scripted scene - enemies still rising out of the ground, say - this shows whether the aim is on
            // something unhittable, or on nothing at all.
            if(enemy)
                Log("PLAYERAIM lock target=%p off=%.1fdeg dist=%.0f aimY=%.1f",
                    e.object,static_cast<double>(e.off*180.0f/kPi),static_cast<double>(e.dist),
                    static_cast<double>(e.aim[1]));
            // rej = candidates seen / out of range / outside the cone / blocked by the map / the kept target
            // dropped for being blocked. Cleared every second, so the line is about the last second only.
            rejects=Rejects{};
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

// One hook per body (same signature, but each needs its own original to chain through).
template<int I> void __fastcall InputHook(void* self,void* a,void* b,void* c) noexcept {
    if(!originalInput[I])return;
    auto* const human=static_cast<unsigned char*>(self);
    if(!image || !published->enabled || !IsPlayer(human)) {
        originalInput[I](self,a,b,c);
        return;
    }
    Watch(originalInput[I],self,a,b,c);
}
template<int... I> struct Hooks { static constexpr HumanInputFn table[]={&InputHook<I>...}; };
template<int... I> constexpr HumanInputFn Hooks<I...>::table[];
using AllHooks=Hooks<0,1,2,3>;
static_assert(sizeof(AllHooks::table)/sizeof(AllHooks::table[0])==kInputCount,"one hook per input body");

// The version gate: the hooked bodies have to start with the bytes this plugin was written against.
bool CheckProfile() noexcept {
    return edf::Matches(image,kHumanBaseInput,kHumanBaseSig,sizeof(kHumanBaseSig))
           && edf::Matches(image,kAssultThunk,kThunkSig,sizeof(kThunkSig))
           && edf::Matches(image,kHumanoidInput,kHumanoidSig,sizeof(kHumanoidSig));
}

// The map ray (the line-of-sight test). Its own gate: without it RequireLineOfSight can do nothing, and the
// aim keeps its old behaviour rather than the plugin refusing to load.
bool CheckRayProfile() noexcept {
    rayOk=edf::Matches(image,kCastRay,kCastRaySig,sizeof(kCastRaySig));
    return rayOk;
}

// The shot direction (WeaponBase slot 22). Gate of its own: without it a bullet cannot be steered at all, and
// only the model/muzzle side is left - the case that looked wrong in game.
bool CheckShotDirProfile() noexcept {
    return edf::Matches(image,kShotDir,kShotDirSig,sizeof(kShotDirSig));
}

// --- The ini / its live reload ---
edf::IniWatch ini;
const wchar_t* kSection=L"PlayerAim";

float ReadFloat(const wchar_t* key,float fallback) noexcept {
    wchar_t text[64]{};
    GetPrivateProfileStringW(kSection,key,L"",text,64,iniPath);
    if(!text[0])return fallback;
    return static_cast<float>(_wtof(text));
}
bool ReadBool(const wchar_t* key,bool fallback) noexcept {
    wchar_t text[16]{};
    GetPrivateProfileStringW(kSection,key,L"",text,16,iniPath);
    if(!text[0])return fallback;
    return text[0]==L'1' || text[0]==L't' || text[0]==L'T' || text[0]==L'y' || text[0]==L'Y';
}
int ReadInt(const wchar_t* key,int fallback) noexcept {
    wchar_t text[32]{};
    GetPrivateProfileStringW(kSection,key,L"",text,32,iniPath);
    if(!text[0])return fallback;
    return _wtoi(text);
}

void LoadConfig() noexcept {
    Config n{};
    n.enabled=ReadBool(L"Enabled",n.enabled);
    n.debug=ReadBool(L"Debug",n.debug);
    // Aim: weapons (default, the view never moves) / view / both.
    wchar_t mode[32]{};
    GetPrivateProfileStringW(kSection,L"Aim",L"",mode,32,iniPath);
    if(!_wcsicmp(mode,L"view"))n.aim=AimAt::view;
    else if(!_wcsicmp(mode,L"both"))n.aim=AimAt::both;
    else n.aim=AimAt::weapons;
    GetPrivateProfileStringW(kSection,L"Barrel",L"",mode,32,iniPath);
    if(!_wcsicmp(mode,L"all"))n.barrel=Barrel::all;
    else if(!_wcsicmp(mode,L"off"))n.barrel=Barrel::off;
    else n.barrel=Barrel::fire;
    n.gain=ReadFloat(L"Gain",n.gain);
    n.range=ReadFloat(L"Range",n.range);
    n.minDistance=ReadFloat(L"MinDistance",n.minDistance);
    n.keepRange=ReadFloat(L"KeepRange",n.keepRange);
    n.coneGraceMs=static_cast<DWORD>(ReadInt(L"ConeGraceMs",static_cast<int>(n.coneGraceMs)));
    n.cone=ReadFloat(L"Cone",n.cone);
    n.bodyYaw=ReadFloat(L"BodyYaw",n.bodyYaw);
    n.bodyPitch=ReadFloat(L"BodyPitch",n.bodyPitch);
    n.bodySmoothMs=ReadFloat(L"BodySmoothMs",n.bodySmoothMs);
    n.pitchBias=ReadFloat(L"PitchBias",n.pitchBias);
    n.yawBias=ReadFloat(L"YawBias",n.yawBias);
    n.biasRamp=ReadFloat(L"BiasRamp",n.biasRamp);
    n.slewMs=static_cast<DWORD>(ReadInt(L"SlewMs",static_cast<int>(n.slewMs)));
    n.requireLos=ReadBool(L"RequireLineOfSight",n.requireLos);
    n.test=ReadFloat(L"Test",n.test);
    n.testAxis=ReadInt(L"TestAxis",n.testAxis);
    if(n.gain<0.02f)n.gain=0.02f;
    if(n.gain>1.0f)n.gain=1.0f;
    if(n.range<0.0f)n.range=0.0f;
    if(n.minDistance<0.0f)n.minDistance=0.0f;
    if(n.keepRange<n.range)n.keepRange=n.range;
    if(n.coneGraceMs>10000)n.coneGraceMs=10000;
    if(n.cone<1.0f)n.cone=1.0f;
    if(n.cone>179.0f)n.cone=179.0f;
    if(n.bodyYaw<0.0f)n.bodyYaw=0.0f;
    if(n.bodyYaw>1.0f)n.bodyYaw=1.0f;
    if(n.bodyPitch<0.0f)n.bodyPitch=0.0f;
    if(n.bodyPitch>1.0f)n.bodyPitch=1.0f;
    if(n.bodySmoothMs<0.0f)n.bodySmoothMs=0.0f;
    if(n.bodySmoothMs>5000.0f)n.bodySmoothMs=5000.0f;
    if(n.slewMs>10000)n.slewMs=10000;
    if(n.test<-1.5f)n.test=-1.5f;
    if(n.test>1.5f)n.test=1.5f;
    if(n.testAxis<0)n.testAxis=0;
    if(n.testAxis>3)n.testAxis=3;
    cfg=n;
    Log("CONFIG enabled=%d debug=%d aim=%s barrel=%s gain=%.2f range=%.0f min=%.0f keep=%.0f coneGrace=%lums cone=%.0f bodyYaw=%.2f bodyPitch=%.2f bodySmooth=%.0f slew=%lums los=%d test=%.3f axis=%d",
        cfg.enabled,cfg.debug,cfg.aim==AimAt::weapons ? "weapons" : (cfg.aim==AimAt::view ? "view" : "both"),
        cfg.barrel==Barrel::all ? "all" : (cfg.barrel==Barrel::off ? "off" : "fire"),
        static_cast<double>(cfg.gain),static_cast<double>(cfg.range),static_cast<double>(cfg.minDistance),
        static_cast<double>(cfg.keepRange),cfg.coneGraceMs,static_cast<double>(cfg.cone),
        static_cast<double>(cfg.bodyYaw),static_cast<double>(cfg.bodyPitch),static_cast<double>(cfg.bodySmoothMs),cfg.slewMs,cfg.requireLos ? 1 : 0,static_cast<double>(cfg.test),cfg.testAxis);
}

// A saved ini is picked up on the next frame (the ini's write time is looked at once a second at most).
void ReloadConfigIfChanged() noexcept { if(ini.Changed())LoadConfig(); }
}  // namespace playeraim

using namespace playeraim;

extern "C" __declspec(dllexport) bool EDFMLAPI EML6_Load(PluginInfo* info) {
    if(!info)return false;
    GetModuleFileNameW(module,iniPath,MAX_PATH);
    auto dot=wcsrchr(iniPath,L'.');if(!dot)return false;
    wcscpy_s(dot,MAX_PATH-(dot-iniPath),L".ini");
    wcscpy_s(logPath,iniPath);
    dot=wcsrchr(logPath,L'.');wcscpy_s(dot,MAX_PATH-(dot-logPath),L".log");
    info->infoVersion=PluginInfo::MaxInfoVer;
    info->name="EDF6 Player Aim";
    info->version=PLUG_VER(0,1,0,0);
    Log("EDF6PlayerAim 0.1.0 loading");
    ini.Start(iniPath);
    LoadConfig();
    image=edf::IdentifyImage(GetModuleHandleW(L"EDF.dll"));
    if(!image){Log("REFUSED: unsupported EDF.dll");return false;}
    if(!CheckProfile()){Log("REFUSED: unexpected EDF.dll code");return false;}
    Log("PROFILE map ray (line of sight)=%d",CheckRayProfile() ? 1 : 0);
    // From here on the plugin stays loaded whatever fails: a patched slot must not point into free code.
    int hooked=0;
    if(InterlockedCompareExchange(&installed,1,0)==0) {
        for(int i=0;i<kInputCount;++i) {
            auto slot=reinterpret_cast<void**>(image+kInputClasses[i].vtable)+kInputSlot;
            void* current=*slot;
            if(!current) {
                Log("HOOK input: %s slot %u empty",kInputClasses[i].name,static_cast<unsigned>(kInputSlot));
                continue;
            }
            if(current!=image+kInputClasses[i].body)
                Log("HOOK input: %s holds %p, not the expected %p (chaining anyway)",kInputClasses[i].name,
                    current,image+kInputClasses[i].body);
            originalInput[i]=reinterpret_cast<HumanInputFn>(current);
            if(edf::PatchVtableSlot(slot,current,reinterpret_cast<void*>(AllHooks::table[i])))++hooked;
            else originalInput[i]=nullptr;
        }
    }
    Log("HOOK player aim=%d/%d (human input slot %u)",hooked,kInputCount,static_cast<unsigned>(kInputSlot));
    // The shot direction: WeaponBase slot 22 of every weapon class. This is what a spawned round actually
    // flies along, so steering here is what makes the bullets and the model agree.
    if(AimsWeapons(cfg)) {
        const bool shotOk=CheckShotDirProfile();
        Log("PROFILE shot direction=%d",shotOk ? 1 : 0);
        if(shotOk) {
            shotDirOriginal=reinterpret_cast<ShotDirFn>(image+kShotDir);
            int dirs=0;
            for(int i=0;i<kWeaponDirVtableCount;++i) {
                auto slot=reinterpret_cast<void**>(image+kWeaponDirVtables[i])+kWeaponDirSlot;
                if(*slot!=image+kShotDir) {
                    Log("HOOK shot dir: vtable %#x slot %zu holds %p, not the expected %p (skipped)",
                        kWeaponDirVtables[i],kWeaponDirSlot,*slot,image+kShotDir);
                    continue;
                }
                if(edf::PatchVtableSlot(slot,image+kShotDir,reinterpret_cast<void*>(&ShotDirHook)))++dirs;
            }
            Log("HOOK shot direction=%d/%d (weapon vtables)",dirs,kWeaponDirVtableCount);
        } else {
            Log("HOOK shot direction: profile mismatch, the bullets keep the game's own direction");
        }
    }
    // The barrel: the call sites of 0x6969A0, which builds the muzzle's world matrix - what the weapon model
    // and the muzzle effects are drawn from. Installed unless Aim=view (that mode only writes the aim angles).
    if(AimsWeapons(cfg)) {
        muzzleBuilder=reinterpret_cast<MuzzleFn>(image+kMuzzleBuilder);
        int barrels=0;
        const int count=cfg.barrel==Barrel::all ? kMuzzleCallCount : 1;
        for(int i=0;i<count;++i) {
            // Barrel=fire redirects only the fire-time call site; Barrel=all every one of them.
            const unsigned site=cfg.barrel==Barrel::all ? kMuzzleCallSites[i] : kMuzzleCallFire;
            bool changed=false;
            if(edf::RedirectCall(image+site,image+kMuzzleBuilder,
                                 reinterpret_cast<void*>(&MuzzleHook),changed) && changed)++barrels;
            else Log("HOOK barrel: call site %#x was not redirected (another plugin's hook?)",site);
        }
        Log("HOOK player aim barrels=%d (mode %s)",barrels,
            cfg.barrel==Barrel::all ? "all" : (cfg.barrel==Barrel::off ? "off" : "fire"));
    } else {
        Log("HOOK player aim barrels=skipped (Barrel=off or Aim=view)");
    }
    return true;
}

BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH){module=instance;DisableThreadLibraryCalls(instance);}
    return TRUE;
}
