# 遮挡与跨游戏伤害修正（2026-10-07）

本版修正三条链路，沿用原始 SkyCraft 的 Minecraft 客户端、物品和伤害计算。

## 场景遮挡

- ReShade 主配置及各运行时配置启用 `DepthCopyBeforeClears=1`、自动清屏索引和相同比例的深度资源筛选。
- 宿主相机投影矩阵提供实际 near/far；保留 CameraFrame v1 的布局，以 flags bit 2 标记标定有效。
- 世界层需要 `bufready_depth` 和有效投影，按米比较两个游戏的深度。深度尚未就绪时不显示世界层，手、HUD 和物品栏仍可见。
- 删除加载器 UI 中没有深度测试的 ImGui 世界绘制调用，避免回退绘制再次透过建筑和人物。
- 宿主日志记录投影系数；ReShade 日志记录深度就绪状态、资源尺寸和裁剪距离。资源存在不等于确认选中了 KCD2 主场景深度。

## 武器与致死伤害

Minecraft 仍负责剑、箭、冷却、暴击、护甲、附魔和伤害来源。宿主保留已有真实扣血日志支持的正生命 Soul 写入并立即回读。

原版拒绝 `SetState("health", 0)`。致死命中改为一次 `soul:DealDamage(9999, 9999)`，签名来自 KCD2 Cheat 的 kill/mass_kill 实现以及游戏自身的两参数测试。该值只触发原生死亡，不改变 Minecraft 的攻击样式，也没有扔石头的替代攻击。

提交后记录目标，等待 Soul 或 actor 的死亡信号。未确认前不发布死亡；3 秒无确认会明确记录 `death unconfirmed`，不会重复追加高伤害或切换第二个伤害 API。重新读档会清空这些记录。受剧情保护的角色可能拒绝死亡，此处不会声称已经杀死。

NPC/动物代理的宽、高改为读取宿主包围盒，避免所有动物都使用人的固定身形。

## Minecraft 生物

为使用 GoalSelector 的敌对生物增加原生 NPC 代理目标。仅忽略代理用于隐藏渲染的 invisibility，保留 vanilla 的攻击范围、视线和攻击行为；被动和中立生物不被统一变成主动攻击者。

伤害桥接沿用相同的 Minecraft 伤害路径，以可选 flags bit 4 记录 Mob 来源，原始 SkyCraft v11 协议布局没有改变。包括 Warden、Piglin 等采用 Brain/特殊仇恨规则的生物，不能仅凭通用目标 goal 保证都会主动攻击宿主 NPC；此版没有声称完成这些特殊选敌规则的等价移植。

## 验证与边界

- Lua 合同：正生命扣血、即时/异步死亡、拒绝调用、不会重复致死、不会把未确认死亡当作成功、读档重置、动物尺寸，以及既有会话/碰撞/输入合同。
- native 合同：输入/会话、碰撞形状、表面和普通/倒置/转置/无限远/无效投影矩阵。
- Fabric：独立候选构建，30 项 JUnit 测试通过。
- 隔离复制的 Minecraft 世界运行测试，10 项通过：真实 Player.attack、真实箭命中路径、真实僵尸 doHurtTarget、冷却后的后续命中、构造器 mixin、隐形目标搜索与实际 mob target。该测试没有连接 KCD2，也没有改动用户的原始世界。
- 隐藏 D3D11/ReShade 6.8.0 测试：实际 FX 编译，前方宿主表面遮住 MC、前方 MC 可见、深度缺失时隐藏世界而保留 HUD、投影无效时隐藏世界。深度纹理为测试夹具，不是 KCD2 的真实画面；KCD2 使用 D3D12。
- DLL 与 addon 的 MSVC Release 编译通过。旧有 LNK4070 输出名称警告仍存在。

本次没有进入 KCD2 存档进行端到端验证，因此不能将上述隔离测试表述为 NPC 已在真实游戏死亡或 KCD2 主深度已经实测选对。安装记录会明确保留 `inGameVerified=false`。

## 参考

- [KCD2 Cheat NPC API](https://github.com/Ovo307000/kcd2-mods/blob/main/reference/mod-templates/cheat-mod-unpacked/scripts/cheat/cheat_core_npc.lua)，kill/mass_kill 两参数致死调用。
- [ReShade Generic Depth](https://github.com/crosire/reshade/blob/main/examples/09-depth/generic_depth_addon.cpp)，保存深度和 `bufready_depth` 来源，BSD-3-Clause。
- 原始 `D:\SkyCraft-main.zip` 与本地 Minecraft 26.3 字节码。

参考文件的本地副本和 SHA256 记录随本版发布包保存。
