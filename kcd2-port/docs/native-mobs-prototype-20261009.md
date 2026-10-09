# 原生生物身份与生命周期组件

此文保留组件最初制作时的状态。后续传输实现、验证和安装方式见 [生物传输记录](mob-transport-20261009.md)。

本组件位于 `kcd2-port/native-mobs`，属于完整移植的后续开发，**尚未连接 Minecraft 跨进程传输，也未安装到正式游戏**。正式安装版本仍为 `combat-identity-20261009`。

自带路径跟随测试角色会自己移动和 Despawn；`NPC_NAI` 与 `bIsDummy` 又不能提供所需 AI 对象。因此组件新增自己的 Soul、brain、subbrain 与行为树，保留可被感知的原生 NPC 身份。行为树仅等待、核对桥接所有权，并使用 XGen 的 `Despawn` 清理失效角色；没有自主移动、攻击或扔石子逻辑。

控制器使用实际 `XGenAIModule.SpawnEntity` 形状和 `System.GetEntity`，保存 Cry 实体句柄与 `soul.__ThisWUID`，通过 Minecraft entity ID、UUID、客户端 session 和存档 epoch 管理绑定。隐藏原生渲染槽时保留实体，不用实体 Hide 关闭其物理和感知。每次最多尝试生成 4 个，附近最多 64 个；API、Soul 或移动读回失败时不发布可用身份。

独立的行为树每 100 ms 核对租约；连续 2 秒没有更新、时间倒退、读档、会话更换和生物移除都会释放所有权，由其原生脑清理角色。此设计不以 `System.RemoveEntity` 代替完整 Soul/XGen 生命周期。实际死亡和存档加载是否仍执行这个行为树，必须在游戏中验证，当前没有以纯 Lua 检查证明持久化安全。

反向受伤原型读取原生 Soul 生命变化，恢复已观察的生命损失并形成带 serial、session、epoch 与 UUID 的回执。回执在确认前保持不变；错身份、错会话和重复确认失败。512 个回执堵满后停止增加原生角色，并保留现有角色未发送的伤害；移出范围也不丢弃该积压。跨进程接收方尚未实现，当前不会改变任何实际 Minecraft 生物生命值。

生命差采样无法获知被 HP 下限截断的过量伤害，也缺少确切原生攻击者、攻击类型和时刻。正式双向战斗需要接入原生命中事件，以保持 MC 护甲、盾牌、免疫帧、吸收与不死图腾的语义，不能把本原型的健康差采样当成精确的完整战斗实现。当前身体仍是原生人形碰撞体，尚未按 MC 生物宽度和高度调整。

## 依据与验证

依据为本机原生游戏脚本和公开文档的只读检查，不发布提取的游戏脚本或资源：

- `CombatDebug.lua` 与 `PathfindDebugUtils.lua` 中的 XGen 生成调用。
- 本机 `Tables.pak` 中的表结构、版本及现有主键。
- [Warhorse 数据库表说明 KM-A-12](https://youtrack.warhorsestudios.cz/articles/KM-A-12)：soul、brain、brain2subbrain、subbrain、subbrain_behaviour_tree 都是支持补丁的 old 表。
- [Warhorse mod.manifest 说明 KM-A-57](https://youtrack.warhorsestudios.cz/articles/KM-A-57)。

运行 `native_mob_controller.py --game-tables <本机 Tables.pak>` 通过 277 项检查。包括 opaque Soul/实体身份、坐标转换、回执确认、entity ID 重用、epoch/session 改变、租约超时、时间倒退、API 故障、生成预算和回执拥堵；逐一核对新增主键不与本机表中的所有相关分表冲突。

这份证据仅证明控制器与配置的静态/隔离契约，**不证明 KCD2 已加载新脑，也不证明 NPC 已能看见或反击 Minecraft 生物**。

开发包只含本项目创作的 Lua、XML 和 manifest，输出在 E 盘。后续仍需完成帧协议、Java 服务端收发、原生入口、准确命中事件、碰撞尺寸和实际存档验证，完整范围见 [验收范围](completion-requirements.md)。
