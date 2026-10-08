# 原生攻击确认与读档隔离

版本：`combat-handshake-20261009-r2`，接续 `combat-continuation-20261009`。完整移植目标仍按 [验收范围](completion-requirements.md) 执行；本版没有将整个目标宣布完成。

Minecraft 武器命中原生 NPC 时，先送出带有目标、来源和准确线段的原生碰撞请求。原生引擎检查墙面与手持实体包围盒，返回允许、墙面拦截、装备拦截或拒绝。只有允许后，才在原来的 Minecraft 调用栈执行 vanilla 伤害、减伤、跳劈与击退，避免旧版先播放成功命中、随后宿主才拒绝扣血的问题。

被原生墙面挡住的箭转交 vanilla 方块命中，留在墙面；被手持装备挡住的攻击返回失败，触发 Minecraft 原有格挡事件或箭反弹。原生装备仍是实际实体局部包围盒检测，原生格挡动画、声音、装备耐久和完整动画网格碰撞尚未接入。

原生确认只授权一次，并绑定存档 epoch、目标、攻击者、武器和完整线段。有效 vanilla 伤害随后提交，原生端不再重复检查已经批准的碰撞，也不再用 200 点上限截断高额原生伤害。重复、过期、来源或线段不匹配的提交均被拒绝。药水、火焰、爆炸等保留已有环境路径，不被错误地归为近战武器。

最终宿主检查另修正了无主投射物：发射器等没有 shooter 的箭仍检查原生遮挡；不能因缺少攻击者被归为可跳过射线的环境伤害。

等待只发生在 Minecraft 服务端，独立碰撞消费线程投递结果。每个 server tick 的请求共享最多 50 ms 的回执等待预算，失联、超时、环形队列发送失败和无效回执均拒绝攻击。低帧率或拥塞下仍可能有攻击被拒绝；实际 KCD2 内的延迟与体验尚需验证。此预算限制回执等待，不包括 vanilla 计算或共享内存锁竞争的耗时。

读档、断开连接及宿主进程更换会撤销旧确认，清理原生确认缓存并重新创建 Minecraft NPC 代理。排队攻击保留发生时的 epoch，不将旧攻击改成新存档的 epoch。所有调用栈上下文均在 finally 中恢复。

保留此前的相机、两米尺度、FOV、滚转、接地、画面遮挡、透明粒子以及共享场景修正。

## 验证边界

- Java 38 项单元测试与宿主 Lua 契约；原生 Release 构建。
- 原生确认缓存契约 555 项断言，包括 512 项容量填充，另外覆盖单次提交、重复、字段不匹配、过期、时间倒退和重置。测试直接使用正式代码中的确认缓存。
- 实际隔离 Minecraft 客户端检查剑、弓、跳劈、有效残血伤害、护甲、吸收、墙面箭插入与持续嵌入、装备拦截、后台回执、断线、换存档和换宿主。运行日志与最终检查数放在构建验证记录。
- 场景几何及 shader 检查复用未改动的基线证据，打包时校验源码 SHA256；Minecraft 场景、环境和相机检查本轮实际运行。
- 首轮发现药水误分类与字节数组对齐问题，已修正；另一次测试使用了新版 Minecraft 已移除的箭字段，改用现有访问方法。失败日志保留。
- r2 仅进一步修改原生宿主的无主投射物检查，原生 Release 和 Lua 契约重新运行。Java JAR 与全部 fabric/src 指纹逐一匹配 r1，沿用该轮 38 项单元测试和 76 项 Minecraft 检查的证据，并在构建记录明确标记复用。
- 本轮尚未在实际 KCD2 存档验证完整交互，`inGameVerified=false`。不能以隔离检查证明原生 NPC 已能正确死亡、识别或反击所有 Minecraft 生物。

## 构建与安装

所有候选、发布包和备份位于 E 盘。安装同时更新正式 DLL、JAR、main/client classes、resources 与源码，保留配置和玩家存档；测试 mod 不安装。

```powershell
./kcd2-port/tools/run-attack-contract.ps1
python kcd2-port/tools/package-shared-scene.py --candidate combat-handshake-20261009 --release-name combat-handshake-20261009-r2 --minimum-combat-checks 45 --require-attack-approval-checks --native-dll E:/SkyCraftKCD2Tools/candidates/shared-scene-20261009/native-build/Release/d3d12_.dll --preserved-baseline E:/SkyCraftKCD2Tools/releases/shared-scene-20261009 --preserved-minecraft-baseline E:/SkyCraftKCD2Tools/releases/combat-handshake-20261009
./kcd2-port/tools/install-shared-scene.ps1 -Candidate combat-handshake-20261009-r2 -MinimumMinecraftChecks 76
```

双向生物 AI 的原生身份、反向伤害来源与瞬态角色生命周期仍缺少完整实现。只读核对原生脚本确认存在 `XGenAIModule.SpawnEntity`，但 `NPC_NAI` 明确不创建 AI 对象；不能把此类简单替身当作已验证的可感知战斗对象。没有据此安装未经验证的隐形 NPC 生成逻辑。原生场景破坏、装备映射、垂直物理和所有机制的融合也仍属于完整目标的未完成部分。
