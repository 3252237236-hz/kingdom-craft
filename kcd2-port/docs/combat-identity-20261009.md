# Soul 攻击者身份修正

版本：`combat-identity-20261009`，接续 `combat-handshake-20261009-r2`。完整目标继续按 [验收范围](completion-requirements.md) 执行。

旧版将 CryEngine EntityId 传给 `soul.DealDamage` 的攻击者参数。当前本机 WHGame 的接口读取 ScriptHandle，并在 Soul 注册表中查找类型为 5 的 64 位 WUID；CryEngine EntityId 不属于这个身份空间。这一错误使宿主日志里的“攻击者编号”无法证明原生接口收到可识别的攻击者。

现在直接从来源角色的 `soul.__ThisWUID` 读取游戏附加的原始句柄，保留 userdata 类型，不转换为 Lua number、字符串或 uint32。物理射线和 generic `CreateHit` 继续使用其要求的 CryEngine EntityId。日志分别记录实际传入的身份、身份类型、实体编号与失败原因。

依据来自本机二进制的只读检查：`DealDamage` 实现 `0x180ee018c` 用 `0x180712164` 读取第三参数，再用 `0x181f985d0` 解析 Soul；解析器检查高字节 5，并核对完整 64 位值与盐。`0x18068b510` 附近的绑定代码把 Soul 的 WUID 写入脚本表的 `__ThisWUID`，再将表附加到角色的 `soul` 属性；同一字段也是 Soul 方法解析接收者的来源。地址只用于记录当前版本证据，运行代码不硬编码这些地址。

显式攻击者消失、Soul 句柄缺失、字段访问失败、null、数字或字符串身份都拒绝伤害；碰撞预批准阶段也先核对身份，避免身份无效时仍让 Minecraft 播放成功命中。提交阶段重新读取活的来源角色。无主环境伤害继续不指定攻击者。MC 生物的独立原生身份仍待实现，不能把 Henry 的身份冒充成生物来源。

保留 `stamina=0, health=实际伤害`、原生命中反应和现有死亡观察流程，也保留原生攻击确认、相机、尺度、画面遮挡、输入、共享场景与存档隔离修正。

## 验证与限制

- 宿主 Lua 契约重新运行：用不同的 opaque userdata 模拟 Soul、EntityData 与 null 身份；核对物理编号和伤害身份分离、玩家来源、原生代理来源、身份缺失/访问失败、来源消失、环境伤害和 generic actor API。
- 原生 Release 重新构建；攻击确认缓存契约重新运行。
- Java JAR 与全部 `fabric/src` 指纹逐项匹配上一版，沿用该版 38 项 Java 测试和 76 项实际隔离 Minecraft 检查，并明确记录复用。
- 原生场景几何和 13 项 shader 检查沿用未改动基线。
- 以上不能证明实际 KCD2 的死亡、犯罪系统或 NPC 反击行为已经正确，`inGameVerified=false`。本次修复的是已经核实的接口身份错误，完整移植仍未完成。

构建、发布包和备份位于 E 盘。正式安装同时更新 DLL、JAR、classes、resources 与源码，保留玩家存档；不安装测试 harness。
