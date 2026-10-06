# Minecraft 跨游戏移植源码对照（2026-10-06）

本次按用户要求查找了 GTA V 示例之外的公开源码，读取了输入、GUI、碰撞及伤害实现。
下面的判断来自固定提交的代码；项目 README 中的可玩性、平台测试描述属于作者声明，
本机没有运行这些模组。这里只下载了相关源码文件，未运行其安装脚本。

源码与各自 LICENSE 保存在 `E:/SkyCraftKCD2Tools/source-references/`。
该目录的 `manifest.json` 记录仓库、固定提交、原始路径及下载文件 SHA256。
这些是研究快照，不能据此声称它们是各项目的最新版本。

## 找到的项目

| 项目 | 宿主游戏 | 固定提交 | 最有用的实现 |
| --- | --- | --- | --- |
| [OWCraft](https://github.com/Yaekai/OWCraft) | 星际拓荒 / Outer Wilds | `cd5f0613e1de04c537e7db28a5813b0329cfbfa9` | 与原 SkyCraft 同类共享内存协议；脚下碰撞优先、三角面与体素分工 |
| [ValCraft](https://github.com/strangemap/valcraft) | 英灵神殿 / Valheim | `70d3389fac5d67b7001a156fc3623b622aa7c0fb` | 输入动作拦截、物品栏鼠标、原生伤害类型映射 |
| [Minepunk](https://github.com/Duocor3/minepunk) | 赛博朋克 2077 | `8d6ca48d7f59d7814b2f68898ee1a7a8913ba3c8` | Raw Input 拦截、箭矢分段命中、物理盒与碰撞过滤 |
| [Minecraft in Death Stranding](https://github.com/mihailinl/minecraft-in-death-stranding) | 死亡搁浅：导演剪辑版 | `52a4afc2ace32ef4194473f782eba9666febfc25` | GUI 坐标和拖拽、原生 Havok 静态盒、引擎伤害消息 |
| [Minecraft Ring](https://github.com/siddoff/Minecraft-Ring) | 艾尔登法环 | `ffb31b559470575cdf09f28c696b382dd7605cd0` | Minecraft 内 NPC 代理、双向伤害、宿主实际 HP 检查 |

这些项目都需要一个 Minecraft 运行时和专门的宿主桥接层。OWCraft 使用宿主引擎渲染
方块网格及 Minecraft HUD；其他已读项目主要合成 Minecraft 的画面。
宿主专用的 C#/RED4ext/Havok/内存地址不能直接用于 KCD2。

## 1. OWCraft：最接近原 SkyCraft 的碰撞参考

已读：

- [host/Player/InputForwarder.cs:80](https://github.com/Yaekai/OWCraft/blob/cd5f0613e1de04c537e7db28a5813b0329cfbfa9/host/Player/InputForwarder.cs#L80)：读取一次键鼠状态，发送 HID/SDL scancode、鼠标按钮、滚轮与菜单光标；失去控制时发 `InReleaseAll`。
- [host/World/CollisionStreamer.cs:143](https://github.com/Yaekai/OWCraft/blob/cd5f0613e1de04c537e7db28a5813b0329cfbfa9/host/World/CollisionStreamer.cs#L143)：把脚下区域插到采样优先队列的第二位。重置后提高附近采样预算，避免 Minecraft 等待脚下碰撞时一直冻结。
- [CollisionStreamer.cs:361](https://github.com/Yaekai/OWCraft/blob/cd5f0613e1de04c537e7db28a5813b0329cfbfa9/host/World/CollisionStreamer.cs#L361)：拿不到网格时，用格点射线采样；相邻四点组成两个三角面，跨越过大高度差时跳过。还从侧面采样墙壁和树干。
- [CollisionStreamer.cs:505](https://github.com/Yaekai/OWCraft/blob/cd5f0613e1de04c537e7db28a5813b0329cfbfa9/host/World/CollisionStreamer.cs#L505)：共享内存环满时在工作线程等待重试，而不是把未送达的碰撞区域当作完成。

对 KCD2 的意义：原 Fabric 玩家启用平滑三角面碰撞器后，地形体素本身不能替代玩家的
脚下三角面。当前候选增加了脚下三角面和发送失败重试，但仍是每列水平面，不能说已
解决斜坡、墙、屋顶。应借鉴相邻采样点的连续面以及脚下区域优先策略。

注意：OWCraft 的输入代码先发送按钮、后发送光标。本项目 GUI 应坚持先发送当前坐标、
再发送按钮，避免同一帧移动并点击时落在上一帧位置。

## 2. ValCraft：宿主动作必须在消费输入前被拦住

已读：

- [valheim/plugin/Controls.cs:369](https://github.com/strangemap/valcraft/blob/70d3389fac5d67b7001a156fc3623b622aa7c0fb/valheim/plugin/Controls.cs#L369)：Minecraft 接管时，宿主 `Attack`、`SecondaryAttack`、`Block` 返回 false；桥接自己的读取用 `bypass` 避免被一并拦截。
- [valheim/plugin/UiForward.cs:33](https://github.com/strangemap/valcraft/blob/70d3389fac5d67b7001a156fc3623b622aa7c0fb/valheim/plugin/UiForward.cs#L33)：光标转成 0..1 坐标，点击消息附带该坐标与 Shift 状态。菜单打开时阻止 `PlayerController.TakeInput` 并释放鼠标锁定。
- [valheim/plugin/Weapons.cs:24](https://github.com/strangemap/valcraft/blob/70d3389fac5d67b7001a156fc3623b622aa7c0fb/valheim/plugin/Weapons.cs#L24)：按剑、斧、镐等映射 slash/chop/pickaxe/pierce，调用目标的 `Damage(HitData)`；宿主树木、岩石和地形有各自处理路径。
- [Weapons.cs:106](https://github.com/strangemap/valcraft/blob/70d3389fac5d67b7001a156fc3623b622aa7c0fb/valheim/plugin/Weapons.cs#L106)：手写物品属性表。
- [Weapons.cs:136](https://github.com/strangemap/valcraft/blob/70d3389fac5d67b7001a156fc3623b622aa7c0fb/valheim/plugin/Weapons.cs#L136)：在宿主侧重算冷却、下落暴击和横扫。

对 KCD2 的意义：把左键发送给 Minecraft 还不够，宿主原有攻击链路也必须停止消费它。
但 ValCraft 的手写属性表及近似暴击条件不适合直接照搬：用户要求原版全部物品特性，
应让真实 Minecraft 的 `Player.attack`、投射物及效果系统完成计算，再转发最终命中。

## 3. Minepunk：输入不止一条通道，碰撞不止一个掩码

已读：

- [cyberpunk/src/plugin.cpp:123](https://github.com/Duocor3/minepunk/blob/8d6ca48d7f59d7814b2f68898ee1a7a8913ba3c8/cyberpunk/src/plugin.cpp#L123)：低层鼠标钩子记录并拦截点击；接管有 500ms 到期机制，脚本停止时恢复输入。
- [plugin.cpp:189](https://github.com/Duocor3/minepunk/blob/8d6ca48d7f59d7814b2f68898ee1a7a8913ba3c8/cyberpunk/src/plugin.cpp#L189)：又分别拦截 `GetRawInputData` 和 `GetRawInputBuffer`，移除鼠标按钮/滚轮，保留原生视角移动。单靠低层鼠标钩子不能阻止 Raw Input。
- [cyberpunk/cet/mcpassthrough/init.lua:637](https://github.com/Duocor3/minepunk/blob/8d6ca48d7f59d7814b2f68898ee1a7a8913ba3c8/cyberpunk/cet/mcpassthrough/init.lua#L637)：通过 `RequestChangingStatPoolValue` 修改 NPC Health；读取变化前血量用于判断致命击退。
- [init.lua:738](https://github.com/Duocor3/minepunk/blob/8d6ca48d7f59d7814b2f68898ee1a7a8913ba3c8/cyberpunk/cet/mcpassthrough/init.lua#L738)：逐 tick 检查箭矢上一位置到当前位置的线段，比较 NPC 和场景哪个先被命中。
- [init.lua:800](https://github.com/Duocor3/minepunk/blob/8d6ca48d7f59d7814b2f68898ee1a7a8913ba3c8/cyberpunk/cet/mcpassthrough/init.lua#L800)：给静态实体添加物理盒，查询过滤和模拟过滤分别设置；实体创建、删除同步 Minecraft 方块。

局限：该提交的剑伤害默认 34%、箭伤害默认 40%，并非全量保留 Minecraft 武器属性；
宿主碰撞盒上限 400，而且用完整方块盒。其 `NoCombat` 限制默认不启用，注释明确指出
这会影响敌人战斗，不能拿全局禁战当作单独拦截玩家攻击的替代。

对 KCD2 的意义：先确定游戏实际消费的输入路径，再拦截；不能直接假设 KCD2 使用
Cyberpunk 的 Raw Input。当前日志能证明 KCD2 调用了 DirectInput 鼠标状态读取。
玩家能碰到方块也不能证明 NPC 的物理层、AI 避障层都已连接。

## 4. Death Stranding：GUI 像素、缩放和拖拽是一整条链路

已读：

- [ds/src/rawinput.cpp:27](https://github.com/mihailinl/minecraft-in-death-stranding/blob/52a4afc2ace32ef4194473f782eba9666febfc25/ds/src/rawinput.cpp#L27)：菜单打开时收集相对移动并阻止宿主收到鼠标/键盘；构建状态只移除按钮及滚轮。
- [mc/src/client/java/dev/rehan/passthrough/client/GuiInput.java:37](https://github.com/mihailinl/minecraft-in-death-stranding/blob/52a4afc2ace32ef4194473f782eba9666febfc25/mc/src/client/java/dev/rehan/passthrough/client/GuiInput.java#L37)：归一化坐标先转窗口像素，再转 GUI 缩放坐标；同步 `MouseHandler` 位置，处理 `mouseMoved`、`mouseDragged`、点击、释放。
- [ds/src/havokbox.cpp:107](https://github.com/mihailinl/minecraft-in-death-stranding/blob/52a4afc2ace32ef4194473f782eba9666febfc25/ds/src/havokbox.cpp#L107)：构造真实 Havok 盒和静态物理资源，加入宿主物理世界；删除由物理世界的安全点处理并释放引用。
- [ds/src/damage.cpp:323](https://github.com/mihailinl/minecraft-in-death-stranding/blob/52a4afc2ace32ef4194473f782eba9666febfc25/ds/src/damage.cpp#L323)：创建宿主攻击上下文和 `MsgDamage`，为每次命中写唯一键，交由宿主的消息队列处理。

局限：伤害使用宿主攻击 ID 映射，包含两击击倒人类等规则，不是照搬所有 Minecraft
数值；源码有分析版本的地址和字节签名检查。README 写明主要在 Linux/Proton 开发
和游玩，Windows 路径未经作者验证。

对 KCD2 的意义：物品栏必须传递光标位置、缩放与按下/释放，不能只发左右键。
鼠标移动和滚轮应由一个输入来源计数；多个输入读取通道重复累计会产生移动翻倍，
`GetDeviceData(DIGDD_PEEK)` 等窥读也不应重复发送同一个事件。

## 5. Minecraft Ring：最有价值的是完整命中链与实际扣血检查

已读：

- [bridge-base/elden-ring/mc-bridge/src/main/java/dev/ermc/bridge/entity/ErEntity.java:209](https://github.com/siddoff/Minecraft-Ring/blob/ffb31b559470575cdf09f28c696b382dd7605cd0/bridge-base/elden-ring/mc-bridge/src/main/java/dev/ermc/bridge/entity/ErEntity.java#L209)：把宿主敌人表示为 Minecraft 内的代理；`hurt(DamageSource, amount)` 处理受击冷却，转发真实 Minecraft 命中。效果 tick 仍运行，但代理不重复执行宿主的移动和 AI。
- [ErEntity.java:363](https://github.com/siddoff/Minecraft-Ring/blob/ffb31b559470575cdf09f28c696b382dd7605cd0/bridge-base/elden-ring/mc-bridge/src/main/java/dev/ermc/bridge/entity/ErEntity.java#L363)：只允许特定状态效果，说明该项目也不是全部效果无条件保留。
- [bridge-base/elden-ring/er-bridge/src/game.cpp:1445](https://github.com/siddoff/Minecraft-Ring/blob/ffb31b559470575cdf09f28c696b382dd7605cd0/bridge-base/elden-ring/er-bridge/src/game.cpp#L1445)：宿主调用 `ModifyHp` 后读取实际 HP；没变就记录当前无敌。宿主数值转换、致命处理、仇恨归属和受击反馈分别实现。
- [bridge-base/elden-ring/mc-bridge/src/main/java/dev/ermc/bridge/entity/CombatBridge.java:121](https://github.com/siddoff/Minecraft-Ring/blob/ffb31b559470575cdf09f28c696b382dd7605cd0/bridge-base/elden-ring/mc-bridge/src/main/java/dev/ermc/bridge/entity/CombatBridge.java#L121)：宿主打玩家时在 Minecraft 构造带攻击者位置的 `DamageSource`，由 Minecraft 处理盾牌、护甲和击退。

局限：固定 Minecraft 1.21.1，当前 SkyCraft 客户端是 26.3，接口不同；该提交宿主侧
还会调用原生 poke 产生受击反馈，未知构建时有直接 HP 写入回退。用户不要“扔石头
代替攻击”，因此不能照抄这类宿主动作反馈作为 Minecraft 攻击本身。

对 KCD2 的意义：应保留现有 Fabric 原版命中计算，补齐 NPC 代理到 KCD2 真实伤害、
生命状态与效果的桥接。调用 API 没有抛异常不等于实际扣血。

## 当前 KCD2 实现对照出的缺口

1. **移动所有权**：安装中的旧版发布 `SKY_HOST_DRIVEN`，同时屏蔽宿主移动键；
   候选已取消该标志，让 Minecraft 移动后驱动隐藏宿主玩家。
2. **脚下碰撞就绪**：候选增加脚下区域三角面及下方区域数据；斜坡连续面、侧墙、
   门、屋顶和动态表面仍未完成。不能仅凭候选编译通过说移动已修复。
3. **GUI 鼠标**：候选补了光标事件与绘制，但多个 DirectInput 读取路径的重复累计、
   窥读事件、坐标缩放、拖拽及实际游戏 Shader 编译仍需要核对。
4. **原生左键动作**：候选新增 `combat_base` 和 `stone_throwing` 的关闭；不能把
   尚未验证的动作组关闭认作已经解决全部输入冲突。未知动作组原状态的恢复也需要修正。
5. **真正的 NPC 扣血**：已读旧日志为 `health 100 -> 100`；现有读取是
   `target.actor:GetHealth()`。KCD2 自带 `Scripts/FeatureTests/player.xml:477`、
   `gameTestJ.xml:291` 使用 `player.soul:GetState("health")`，证明还必须核对游戏真实
   人物生命状态，不能只用通用 actor 读数下结论。现有 `CreateHit` 调用成功仅证明请求
   未抛异常，没有证明真实人物扣血。
6. **NPC 对方块碰撞**：当前候选有宿主静态盒、NPC 掩码和脱离方块补救；需要分别
   验证物理阻挡与 AI 走路。移动 NPC 的位置只是补救，不能代替完整物理交互。
7. **全部物品特性**：没有任何本次读取的宿主桥接能证明全部特性已移植。液体、
   点燃、状态效果、盾牌、爆炸遮挡、宿主可破坏场景、存档与维度切换必须各自接通。

## 采用原则与验收证据

- Minecraft 自己处理玩家移动、背包、物品使用、冷却、附魔、投射物和 Minecraft 方块。
- 宿主 NPC 在 Minecraft 服务端拥有代理命中实体；命中携带身份、伤害源、最终伤害、
  击退和效果。宿主桥接调用 KCD2 专用接口并核验真实生命状态；不生成石头代替剑或箭。
- Minecraft 方块的物理形状传入 KCD2 物理世界；KCD2 场景碰撞传回 Minecraft。
  查询命中、玩家阻挡、NPC 阻挡和 AI 避障分别作为验收项。
- 接管输入时宿主不能同时响应攻击；背包内可以选择、拖放、Shift 点击，切换控制和
  失焦时释放所有按键。每个物理输入事件只转发一次。
- 编译/离线合同测试通过只证明对应代码路径，真实游戏里的运行结果另行记录。

本次调研没有把其他游戏的 DLL 或安装包直接装入 KCD2，也没有把当前移植称为成品。

## 后续应用记录（同日）

用户询问下一步后，已继续修正并安装移动/输入候选：

- 重复 DirectInput 通道只选一个来源；窥读不累计，菜单先发光标再发按钮。
- 光标显示按宿主与 Minecraft 覆盖层的尺寸换算；新增 Raw Input 鼠标按钮拦截，
  不把这条通道的移动再次转发。
- 无法读取原状态的动作组不再被强行关闭或恢复成启用；由输入钩子负责拦截。
- 人物生命状态优先读取 `soul:GetState("health")`。游戏自带测试
  `Scripts/FeatureTests/gameTestJ.xml:81` 还提供了 `soul:SetState('health',60)` 的证据，
  玩家生命恢复使用同一数据源。NPC 命中日志区分提交请求和观察到的血量下降。

Release 宿主、合成 addon 编译通过；鼠标输入、地面碰撞、Lua 合同检查通过。
宿主 DLL、addon 和 Shader 已备份、安装并逐个比对 SHA256。
安装记录为 `E:/SkyCraftKCD2Tools/movement-input-installation.json`。
本候选尚未运行存档内验证，NPC 真实伤害、AI 碰撞及全量物品效果仍不能宣布完成。

## 后续应用：攻击、治疗与真实方块形状

同日随后安装的 `combat-shapes-20261006` 候选包含以下改动：

- 上轮实际日志显示 Soul 血量在 CreateHit 前后均为 100。现在人物伤害采用游戏自带
  `Scripts/Debug/Quick.lua:33` 中的 `entity.soul:DealDamage(health, stamina, __null, true)`。
  同一攻击只走一个伤害接口，不再另外发送通用命中请求；没有确认归属和仇恨接口。
- KCD2 模式的 NPC 代理调用 Minecraft 原版 `LivingEntity.actuallyHurt`，保留抗性、
  护甲和吸收计算，只恢复代理血量。宿主血量反馈给代理，使毒药能够读取真实的低血量。
  原版治疗/再生增量也会传回 NPC；Soul 血量仍按 100 和 Minecraft 20 的比例换算。
- 额外拦截窗口消息和原生按钮状态轮询；转发给 Minecraft 的按钮读取绕过这层过滤。
- 客户端按原版 `VoxelShape.toAabbs()` 传输碰撞形状，宿主按部分创建缩放后的静态代理。
  碰撞模型先读包围盒校准，几何变化时替换，NPC 分离补救也使用相同边界。
  这仍不是 NPC 物理阻挡和 AI 导航已经正常的证据。

MSVC Release、Fabric Java/client 编译及 21 个原有射线/三角碰撞测试通过。
Lua、输入、表面与形状协议合同检查通过。宿主 DLL 已在游戏关闭时安装并比对 SHA256；
后台 Minecraft 通过正常保存/退出方法结束，世界备份保存在 E 盘。运行记录在
`E:/SkyCraftKCD2Tools/combat-shapes-installation.json`。按用户要求，未操作游戏界面，
也未要求用户继续配合诊断。本轮代码尚未获得游戏内效果验证，不能称为全量复刻完成。

## 用户测试后的修正：死亡重读和四项回归

用户随后报告创造栏不能用光标、攻击没有伤害、经常不能移动，死亡读档后接管全部失效。前一版的实际日志确认 Soul DealDamage 调用前后健康不变，因此改为游戏自带测试中出现的 SetState，并立即回读确认。

`session-recovery-20261006` 进一步接入原生读档回调与死亡状态、同级别的会话重置、MC 正常复活、传送确认后的战斗恢复。菜单改用宿主绝对坐标并换算到 SDL 窗口；切出游戏不再重建碰撞，KCD2 移动改用两端一致的体素。具体实现、30 个 Java 测试及验证限制见 [修正记录](session-recovery-20261006.md)。这些修正没有补全原生仇恨归属、犯罪规则或全量物品效果。
