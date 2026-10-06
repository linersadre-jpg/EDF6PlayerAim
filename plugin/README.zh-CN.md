# EDF6PlayerAim：玩家步兵的准星自动指向敌人

EDFModLoader 插件，独立构建（`EDF6PlayerAim.dll`）。只支持 EDF.dll TimeDateStamp `0x678CCB46`（当前 Steam 版）；
版本不符或要打补丁的代码签名对不上时，插件只写一行 `REFUSED` 日志后自行退出，不改动游戏的任何内容。

**它只依赖**：仓库的 `common/`（edf6common：内存读写、代码/vtable 补丁、EDF.dll 识别）和
`third_party/EDFModLoader/PluginAPI.h`。不链接、不 include、也不修改 `EDF6VehicleCrew`（`src/`）和
`EDF6AutoTurret`（`autoturret/`）的任何文件，可以和它们同时装、也可以单独装。

## 做什么

玩家角色（步兵，全兵种通用）的**武器自动指向最近的敌人**。只对**本机玩家**生效——NPC 士兵、友军、
联机里的其他玩家都不受影响。

两种瞄准方式（ini `Aim`）：

| `Aim` | 效果 | 适用 |
|---|---|---|
| **`weapons`（默认）** | 只把**枪口/弹道**转向敌人：玩家的准星与视角**一动不动**，屏幕稳定 | 想要"人物照常看、子弹自己找敌人" |
| `view` | 连玩家的瞄准角一起写：准星、相机和弹道都转过去 | 想要"屏幕自己锁住敌人"；目标多时画面会在目标间切换 |
| `both` | 两者都做 | — |

`weapons` 模式另加三条防抖与可用性措施：

- **目标锁定**（`KeepRange`）：锁定后一直打同一个，直到它死或跑远；
- **换目标锥角**（`Cone`）：只换到当前朝向附近的敌人，且枪口/模型最多偏离视角这么多度；
- **遮挡判断**（`RequireLineOfSight`）：用游戏自己的 Havok 地图射线（层 22，只打地形与建筑）检查
  「玩家→敌人」这条线是否被挡住；**被挡住的当前目标会被丢掉**，改打别的敌人——不会对着墙一直开枪。

`BodyYaw`（默认 0.6）让**人物模型也转向目标**：只有枪口转、人不动的话，背影看起来像子弹在拐弯。
模型最多偏离视角 `Cone` 度，目标超出这个范围时模型保持不动（枪口也不会硬拧）。

实测（2026-10-04/05，Ranger 步行）：

| | 结果 |
|---|---|
| `view` 模式 | 准星与最近敌人的夹角稳定在 **1° 以内**（0.0–0.9°），但视角会被拽着转 |
| `weapons` 模式 | 视角由玩家自己控制不动，枪口/弹道转向敌人（本篇的默认行为） |

## 怎么做的

逆向笔记见 [`docs/player-aim-re.md`](../docs/player-aim-re.md)。要点：

1. **找到本机玩家**：`+0x340` 有手柄、`+0x354` 玩家标志为真，再用对象的 weak-this 控制块确认是同一个对象
   （和 `EDF6VehicleCrew` 的判定相同）。
2. **挂人类每帧输入**：人类类只有**两条** slot 4 实现，全部挂上——
   `0x572df0`（HumanBase / SoldierBase / People / HeavyArmor，以及游骑兵 `AssultSoldier` 经由
   `0x550a30: jmp 0x572df0` 的跳转块）、`0x470ca0`（HumanoidBase / Humanoid_Basic / EDF6_SoftBodyHumanoidBase）。
   原函数先跑（玩家自己的摇杆照常生效），本插件在之后写角度，所以有最后一句话权。
3. **写瞄准角**：`human+0x1230` 是俯仰 A、`human+0x1234` 是偏航 B（弧度、世界系），
   玩家朝向 `fwd = (sin B, -sin A, cos B)`。于是：

   ```
   目标偏航 = atan2(dx, dz)
   目标俯仰 = -atan2(dy, √(dx²+dz²))
   新角度   = 旧角度 + Gain × 差值(绕回)
   ```

4. **敌人**取游戏自己的锁定注册表（和 `EDF6AutoTurret` 读的是同一份），只挑与玩家队伍关系为 2（敌人）的
   有效、可锁锁定点；比 `MinDistance` 更近的不瞄，避免贴脸时视角原地乱转。
5. **射击方向**：子弹飞的方向来自武器虚函数 `WeaponBase::slot22`（`0x691fa0`）——生成弹丸的 `0x696fd0`
   会调用它并把结果直接存进弹丸；它的函数体只有三条指令：`dir = (weapon+0x190) * (weapon+0x24c) / 常数`。
   武器模型和枪口特效则来自 `0x6969a0`（枪口世界矩阵）。**这是两条独立的路**：只改枪口会出现
   "模型没指到敌人、子弹却能击中"。插件把两处都按同一份方向和同一个转向速率改写，所以三者始终一致。

## 设置

全在 `EDF6PlayerAim.ini`（`[PlayerAim]` 段，中文注释），运行中保存即生效（约 1 秒内，不用重启）：

| 键 | 默认 | 说明 |
|---|---|---|
| `Enabled` | 1 | 总开关 |
| `Debug` | 0 | 1 = 写 `PLAYERAIM` / `PLAYERAIM lock` / `PLAYERAIM barrel` / `PLAYERAIM drop` 日志 |
| `Aim` | `weapons` | `weapons`（只动枪口/弹道，视角不动）/ `view`（视角也转）/ `both` |
| `Barrel` | `fire` | 挂钩范围：`fire`（只挂开火那一个调用点，稳）/ `all`（5 个全挂，实测关卡结束会崩）/ `off` |
| `BodyYaw` | 0.6 | 模型水平转向目标的比例（0 = 不转，1 = 完全对准），上限 `Cone` 度 |
| `BodyPitch` | 1.0 | 模型俯仰跟随射击的比例（0 = 只转水平，1 = 完全跟随） |
| `SlewMs` | 700 | 转满 180° 所需的毫秒数：切换目标时武器「扫过去」而不是瞬间到位。0 = 瞬间 |
| `RequireLineOfSight` | 1 | 1 = 只打视线可达的敌人（被地形/建筑挡住的当前目标会被丢掉换人） |
| `Range` | 200 | 目标搜索半径（米） |
| `MinDistance` | 25 | 比这更近的不瞄（米） |
| `KeepRange` | 260 | 锁定后保持同一目标的距离上限（米） |
| `Cone` | 30 | 目标必须在当前朝向这个角度内（度）；也是模型/枪口偏离视角的上限 |
| `ConeGraceMs` | 600 | 目标转出 `Cone` 后多久解绑（毫秒），避免转头抖动导致换目标 |
| `Gain` | 0.35 | 仅 `view`：每帧走多少 |
| `Test` / `TestAxis` | 0 / 0 | 标定探针，正常玩不要动 |

## 安装

游戏目录 `<EDF6>\Mods\Plugins\` 放 `EDF6PlayerAim.dll` 和 `EDF6PlayerAim.ini` 两个文件即可。
卸载把这两个文件删掉。

## 构建

`playeraim/CMakeLists.txt` 定义了 `EDF6PlayerAim`，DLL 输出到 `build/Mods/Plugins/`。
仓库根 `build.cmd` 会连同另外两个插件一起构建。

> 注：这套工具链下 CMake 在受限环境里可能卡在"探测编译器"一步（它需要捕获子进程输出）。
> 这种情况下用 `tools/gen_build.py` 生成 `build-ws.cmd` 直接驱动 cl/link：
> `build-ws.cmd playeraim`（或 `plugin` / `autoturret` / 不带参数构建全部）。

## 已知限制 / 下一步

1. **手动脉冲**：现在每帧都覆盖瞄准角，玩家自己推右摇杆/鼠标时会被自瞄拉向目标；
   可以照 `autoturret` 的 `DragDeadzone` 做法，检测到玩家输入就短暂让位。
2. **目标迟滞**：每帧取最近敌人，等距目标可能来回切（实测日志里出现过一次换目标导致的 33.9° 跳变）；
   可以照 `autoturret` 的 `SlewWeight` / `kKeepTarget` 加迟滞。
3. **提前量**：目前直接指当前敌人位置；远距离抛物线武器可照 `autoturret` 的 `Ballistic` 解算提前量。
4. **步行时的 `Fire`**：步行玩家用的武器集合（`human+0x1950` 那张表，`[[[human+0x1950]+id*8]]+0xD8`
   返回武器）还没接上，所以 `Fire` 目前基本只在座位武器上生效。自瞄（`Steer`）不依赖它。
5. **联机**：只写本机玩家的那两个字段，其他玩家由他们自己的机器模拟；未联机测试。

## 许可

MIT，与仓库其余部分相同。

## 实机校准（不用重启游戏）

`PitchBias` / `YawBias` 是给模型额外叠加的俯仰/偏航补偿，**单位是弧度**，保存 ini 约 1 秒生效：

```ini
PitchBias=0.0     ; 正 = 武器额外上抬
YawBias=0.0       ; 正 = 武器额外右转
BiasRamp=0.0      ; 非 0：补偿量按此速度做三角波扫描（弧度/秒），配合 Debug=1 的
                  ; PLAYERAIM bias= 日志，一次会话就能扫出正确值
```

推荐流程（测试场里最省事）：

1. 把人物放在开阔地，锁定敌人；
2. 设 `BiasRamp=0.5` 并保持 `Debug=1`，看着武器**正好指向敌人**的那一刻记下时间；
3. 从日志里找出那一时刻的 `PLAYERAIM bias=` 值，把它填进 `PitchBias`，再把 `BiasRamp` 设回 0。
