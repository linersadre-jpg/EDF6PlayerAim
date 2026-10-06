# EDF6PlayerAim —— 地球防卫军6 玩家步兵自瞄插件

玩家角色的**武器自动指向最近的敌人**，全兵种通用（Ranger / Wing Diver / Air Raider / Fencer）。
只对本机玩家生效：NPC、友军、联机中的其他玩家都不受影响。

这是一个**独立插件**：只依赖 [EDFModLoader](https://github.com/hajisensai/edf6-all-forces) 的 `PluginAPI.h`
和本仓库内的 `common/` 构建胶水，不修改、不依赖任何其他插件的源码。

## 特性

- **相机完全由你控制**：默认模式（`Aim=weapons`）只把武器与子弹转向敌人，准星与视角一动不动；
- **人物模型跟着转**：模型朝向按可调的时间平滑转向目标，看起来像人在转身，而不是只有子弹在拐弯；
- **子弹方向与武器一致**：游戏里"子弹方向"（`WeaponBase` vtable slot 22）与"武器模型朝向"是两条独立的
  路径，本插件两处都改，所以模型指哪、子弹就打哪；
- **准星优先**：在锁定锥内按"偏离准星的角度 + 距离/600"打分，优先锁定最靠近准星的目标；
- **遮挡判断**：用游戏自己的 Havok 地图射线检查玩家到敌人之间是否有地形/建筑；
  **被挡住的当前目标会被丢弃**并改打别的敌人，不会对着墙一直开枪；
- **平滑转向**：切换目标时武器/子弹"扫过去"而不是瞬移（`SlewMs`），模型另有独立的平滑时间（`BodySmoothMs`）；
- **运行中调参**：所有配置保存即生效（约 1 秒），不用重启游戏；还带一对校准补偿量与自动扫描开关。

## 安装

1. 安装 [EDFModLoader](https://github.com/hajisensai/edf6-all-forces)（`winmm.dll` 代理）；
2. 把编译好的 `EDF6PlayerAim.dll` 和 `EDF6PlayerAim.ini` 放进
   `<EDF6 安装目录>\Mods\Plugins\`；
3. 启动游戏即可。想关闭就把 `Enabled=0`。

## 编译

需要 Visual Studio 2022 的 C++ 工具链（MSVC）与 Windows SDK。

```bat
:: 生成 build-ws.cmd（直接驱动 cl/link，绕开 CMake 在某些受限环境下的问题）
python tools\gen_build.py
:: 构建插件，产物在 build\Mods\Plugins\
build-ws.cmd playeraim
```

也提供 CMake 工程（`CMakeLists.txt`）：

```bat
cmake -S . -B build -G Ninja
cmake --build build --target EDF6PlayerAim
```

> 若 CMake 在受限沙箱里卡在"探测编译器"一步，用上面的 `build-ws.cmd` 即可。

## 配置

全在 `EDF6PlayerAim.ini`，运行中保存即生效：

| 键 | 默认 | 说明 |
|---|---|---|
| `Enabled` | 1 | 总开关 |
| `Debug` | 0 | 1 = 往 `EDF6PlayerAim.log` 写调试日志 |
| `Aim` | `weapons` | `weapons`（只动武器，视角不动）/ `view`（视角也转）/ `both` |
| `Barrel` | `fire` | 枪口挂钩范围：`fire`（只挂开火点，**稳定**）/ `all`（5 个全挂，实测关卡结束会崩）/ `off` |
| `BodyYaw` / `BodyPitch` | 1.0 | 模型水平/垂直跟随目标的比例（1.0 = 完全对准） |
| `BodySmoothMs` | 250 | 模型转向的平滑时间（0 = 不滤波，瞬间到位） |
| `SlewMs` | 700 | 武器与子弹转满 180° 所需毫秒数（切换目标时"扫过去"） |
| `Cone` | 20 | 锁定锥角（度），上下左右统一；也是模型/枪口偏离视角的上限 |
| `ConeGraceMs` | 600 | 目标转出锥角后多久解绑（毫秒） |
| `Range` / `MinDistance` / `KeepRange` | 200 / 25 / 260 | 搜索半径 / 近身不瞄 / 锁定保持距离（米） |
| `RequireLineOfSight` | 1 | 只打视线可达的敌人（被挡住就换目标） |
| `Gain` | 0.35 | 仅 `Aim=view`：每帧转多少 |
| `PitchBias` / `YawBias` / `BiasRamp` | 0 | 校准用补偿量（弧度）与自动扫描速度 |

两个转向速度的取值对照见 [`plugin/转向速度对照.md`](plugin/转向速度对照.md)。

## 文档

- [`plugin/README.zh-CN.md`](plugin/README.zh-CN.md) —— 插件的功能、设计与设置说明；
- [`plugin/转向速度对照.md`](plugin/转向速度对照.md) —— 两个转向速度参数的取值对照表；
- [`docs/player-aim-re.md`](docs/player-aim-re.md) —— **逆向笔记**：瞄准角语义、两条人类输入路径、
  武器/子弹/模型三条方向路径、遮挡射线、崩溃排查、实测结论；
- [`docs/raycast-re.md`](docs/raycast-re.md) —— Havok 地图射线的逆向记录。

## 离线校验

改过地址或签名之后，先跑这两个脚本：它们直接读**本机安装的 `EDF.dll`** 校验（不需要启动游戏）。
游戏不在默认 Steam 库时，先设 `EDF6_DIR`（或 `EDF6_DLL`）指向游戏目录：

```bat
set EDF6_DIR=D:\SteamLibrary\steamapps\common\EARTH DEFENSE FORCE 6
python tools\verify_playeraim.py     :: 人类输入钩子：vtable / 函数入口 / 签名
python tools\verify_shotdir.py       :: 射击方向钩子：WeaponBase slot 22 的 29 个 vtable
```

两个脚本依赖 `pefile`、`capstone`、`numpy`：

```bat
pip install pefile capstone numpy
```

期望输出是 `RESULT: ALL CHECKS PASS`；任何一项 `FAIL` 都说明该版本的 `EDF.dll` 与插件里的地址/签名不一致，
此时插件会在加载阶段写一行 `REFUSED` 并退出，不会改动游戏。

## 已知限制

- 版本门禁：只适配 `EDF.dll` TimeDateStamp `0x678CCB46`（当前 Steam 版）。签名对不上时插件只写一行
  `REFUSED` 就退出，不改动游戏；
- `Fire`（枪口）挂钩只覆盖开火时的那个调用点；把 `Barrel` 改成 `all` 会在关卡结束回菜单时崩溃
  （已定位到 `EDF.dll+0x978fb0` 的 `shared_ptr` 重复释放）；
- 未做联机测试：只写本机玩家的字段；
- 提前量（弹道下坠）未实现，远距离抛物线武器仍按直线瞄准。

## 致谢与许可

- `common/` 与 `third_party/EDFModLoader/` 的构建胶水来自
  [hajisensai/edf6-all-forces](https://github.com/hajisensai/edf6-all-forces)（MIT），本仓库保留其许可声明；
- 逆向结论基于该项目的既有分析与本机 `EDF.dll` 的实测日志。

本仓库采用 MIT 许可，见 `LICENSE`。
