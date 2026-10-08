# 伤害与原生手持物体检测

版本：`combat-continuation-20261009`。在 `shared-scene-20261009` 上继续修改，保留相机位移、FOV、滚转、世界投影、接地、透明雾层、遮挡以及场景/环境交互修正。本版仍是未完成的 demo。

## 修改

- 修复残血原生 NPC 的伤害截断。此前用代理扣血前后的差值取有效伤害，Minecraft 将 HP 截到零后会丢掉超过剩余生命的部分。现在捕获 vanilla 在 `setHealth` 之前算出的完整减伤结果，继续保留护甲、药水和吸收计算，再恢复由原生引擎管理的代理生命。剑、箭与生物伤害不再因为代理残血而被削成极小数值。
- 移除等待死亡确认期间吞掉后续攻击的分支。死亡轮询只读取状态；每次新的实际攻击都能继续送往原生引擎。保持 `actor:IsDead()` 确认规则，昏迷仍不算死亡，没有强制清零生命、删除 NPC 或额外重复扣血。
- 修正箭的路径。原路径从 `projectile.position - velocity` 开始，会把已经飞过的一段也拿去查障碍。现在在 vanilla 分派命中时保存当前起点和 `HitResult` 的准确落点，异常或正常返回时都恢复线程上下文。直接伤害调用的备用路径只检查当前运动段。
- 原生手持实体拦截：通过 `human:GetItemInHand(HS_RIGHT/HS_LEFT)`、`ItemManager.GetItem` 的 `entity` 字段和 `System.GetEntity` 获取当前物体；通过 `ToLocal(-1, point)` 与 `GetLocalBBox()` 将攻击线段转换到真实物体的局部坐标。仅当前物体包围盒在受害者身体之前被命中时阻止扣血。旋转、移动、隐藏、空手和无效变换均有宿主契约检查。

手持检测是**真实实体包围盒的几何近似**，不是完整武器三角网格或动画扫掠碰撞。接口缺失、装备没有活动实体或没有有效包围盒时，沿用已有物理射线检查，不凭空生成盾牌或按概率格挡。尚未接入原生格挡动画、反击与 Minecraft 格挡回执；此时 MC 侧的受击动画或箭的处理可能先于原生拦截。

## 接口依据

只读核对了当前安装的 WHGame Lua 注册与实现；运行代码调用已注册 Lua 接口，没有直接调用研究时记录的函数地址。

- `GetItemInHand` 注册于 `0x18128c23a`；参数为 `handId`，绑定实现 `0x182abc1f8` 返回库存 handle。
- `ItemManager.GetItem` 实现 `0x182abc0bc`；返回表包含 `id`、`class`、`health`、`amount`、`entity`。
- `ToLocal` 注册于 `0x181450887`；原版 `PressurizedObject.lua` 也使用 `ToLocal(SELF_NOSLOT, hit.pos)`。
- `soul.DealDamage` 的 `stamina,health` 顺序与上一版一致：本版仍提交 `0, damage, attacker, false, "torso"`。手持物体检测不虚构新的伤害或死亡接口。

研究记录位于本机 `E:\SkyCraftKCD2Tools\candidates\combat-continuation-20261009`，不向仓库提交游戏二进制或提取的游戏源码。

## 验证

- Java 构建和 38 项单元测试通过；原生 Release DLL 构建通过。
- 宿主 Lua 契约通过，新增手持物体变换/遮挡和后续攻击不被吞掉的检查。
- 实际隔离 Minecraft 客户端通过 58 项检查：场景 13、环境 12、战斗 27、相机 6。新增检查覆盖完整残血伤害、护甲、吸收、准确箭命中段以及命中上下文清理。客户端正常保存退出。
- 首轮新检查的吸收测试夹具没有设置 vanilla 最大吸收属性，导致检查失败；修正夹具后通过。失败日志保留，不将其列为成功证据。
- 场景 C++ 几何与 13 项 ReShade 渲染检查沿用上一版证据；打包器逐一比较对应源码和 shader 的 SHA256，确认本版没有改动它们。没有把复用的检查冒充为本次重新运行。
- **本轮没有在 KCD2 存档里验证武器包围盒获取、NPC 真正死亡或攻击者识别。`inGameVerified=false`。**

## 安装与剩余工作

发布与备份使用 E 盘。沿用事务性安装器，同时更换游戏 DLL、正式 Minecraft 的 main/client classes、resources、src 和 JAR。正式启动器直接加载 classes，因此只换 JAR 不足以更新。测试 harness 和测试世界不安装，原有 shader、addon、配置和玩家存档保留。

```powershell
python kcd2-port/tools/package-shared-scene.py --candidate combat-continuation-20261009 --minimum-combat-checks 27 --native-dll E:/SkyCraftKCD2Tools/candidates/shared-scene-20261009/native-build/Release/d3d12_.dll --preserved-baseline E:/SkyCraftKCD2Tools/releases/shared-scene-20261009
./kcd2-port/tools/install-shared-scene.ps1 -Candidate combat-continuation-20261009 -MinimumMinecraftChecks 58
```

完整双向原生 AI、生物攻击者身份、剑盾碰撞反馈、原生 NPC 装备映射、原生场景破坏以及全部 Minecraft 机制移植仍未完成。残血伤害的代码错误已修正，不能据隔离检查声称所有 NPC 的原生死亡和仇恨问题都已解决。真实 KCD2 的两米位移仍未测量；之前通过的相机与合成检查保持。
