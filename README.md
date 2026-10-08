# kingdom craft

把 Minecraft 的玩法接入《天国：拯救 2》的实验性 Windows demo。

当前快照：`combat-identity-20261009`。**这是仍有已知问题的 demo，不是已经完成的完整移植。**

Minecraft 26.3 的 Fabric 客户端在后台运行，负责原版物品栏、移动、放置和破坏、武器、生物与粒子；KCD2 原生加载器提供相机、地形和 NPC 桥接，ReShade addon 合成两个游戏的画面。为兼容当前存档和协议，代码保留 `skycraft` 的包名与标识。

## 当前版本

- Minecraft HUD、Steve、物品栏和左右键输入桥接。
- 原生地面上的方块放置、碰撞形状和附近地形采样。
- Minecraft 武器及生物命中原生 NPC 的伤害桥接。
- 死亡和读档后的输入、移动、相机与协议会话恢复。
- 相机 FOV 与深度采样、画面遮挡、透明粒子深度输出。
- 共用原生场景形状：Minecraft 的碰撞、视线、寻路、支撑与流体读取同一份附近表面。
- 原生 NPC 接触 Minecraft 环境与药水；逐次攻击保留来源和路径，并由原生射线复查遮挡。
- 投影采样校准 FOV 和滚转、移除额外的 Minecraft 世界晃动、修正透明雾层与方块悬空。
- 保留残血 NPC 所受的完整 Minecraft 伤害；箭使用准确命中段，原生手持物体参与包围盒拦截检查。
- 武器攻击先等待原生碰撞确认，再执行 Minecraft 伤害与反馈；墙面拦截箭、装备拦截和旧存档攻击隔离。
- Soul 伤害使用来源角色的原始 64 位 WUID；物理射线继续使用 EntityId，身份缺失时拒绝攻击。

保留上一版的输入、生命/体力参数、击退和粒子修复。新增相机与合成检查验证 2 米移动在 Minecraft 相机及重投影中不被缩放；真实 KCD2 画面中的位移仍未验证。NPC 真正死亡、原生武器格挡和攻击者识别也尚未完成实测。NPC 击退目前是带障碍检查的水平位移，垂直重力仍由原游戏负责。

## 目录

| 目录 | 内容 |
| --- | --- |
| `fabric/` | 当前修改后的 Minecraft Fabric 源码与 Gradle wrapper |
| `native-loader/` | 修改后的 KCD2ModLoader、CMake 和上游许可 |
| `kcd2-port/host/` | KCD2 相机、输入、战斗、地表及碰撞桥接 |
| `kcd2-port/bridge/`、`protocol/` | 共享内存协议与桥接 |
| `kcd2-port/passthrough/` | ReShade addon、shader 和配置 |
| `kcd2-port/tests/` | 回归检查和独立运行检查源码 |
| `demo/` | 当前构建验证记录和本机启动脚本示例 |

## 构建

需要 Windows x64、Visual Studio C++ Build Tools、CMake、JDK 25、ReShade addon SDK；Fabric 依赖由 Gradle 下载。

```powershell
# Minecraft 客户端；JAVA_HOME 指向 JDK 25。
cd fabric
.\gradlew.bat compileJava compileClientJava test jar
cd ..

# KCD2 加载器；首次配置会下载 CMake 中固定版本的依赖。
cmake -S native-loader -B E:/KingdomCraftBuild/native -A x64
cmake --build E:/KingdomCraftBuild/native --config Release --target KCD2ModLoader
```

ReShade addon 可在 VS x64 开发者命令行中构建：

```bat
cl /LD /O2 /EHsc /std:c++20 /MT /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I "<ReShade SDK>" kcd2-port\passthrough\addon.cpp kcd2-port\passthrough\compositor.cpp /Fe"E:\KingdomCraftBuild\SkyCraftKCD2.addon64" /link user32.lib
```

现有安装、打包和运行检查脚本保留了本机 `E:\SkyCraftKCD2Tools`、Steam 安装路径和开发客户端布局，换电脑需要调整这些路径。源码仓库不包含游戏本体、Minecraft 运行时、存档、账号信息或本机工具缓存。

## 验证

当前构建通过 38 个 Java 单元测试、宿主 Lua 契约、原生攻击确认缓存契约和 76 项真实 Minecraft 独立运行检查。原生场景几何与 13 项 ReShade D3D11 渲染检查沿用上一版，相关源码与 shader 指纹保持一致。**独立检查不等同于 KCD2 存档中的 D3D12、NPC 战斗或相机位移验证。**

详细变化见 [最新修复记录](kcd2-port/docs/combat-identity-20261009.md)，完整目标见 [验收范围](kcd2-port/docs/completion-requirements.md)，构建文件指纹见 [demo/build-verification.json](demo/build-verification.json)。

## 来源与许可

本项目基于用户提供的 [SkyCraft](https://github.com/chasmlol/SkyCraft) 源码、[KCD2ModLoader](https://github.com/WizGery/KCD2ModLoader)，并参考 [universal-modder Minecraft GTA5 passthrough](https://github.com/rehan-remade/universal-modder/tree/main/examples/minecraft-gta5-passthrough)。原始许可和署名保留，见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。
