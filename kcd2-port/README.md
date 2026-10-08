# kingdom craft：KCD2 桥接

当前 demo 快照为 `combat-continuation-20261009`。完整源码已集中到本 Git 仓库，项目介绍和构建步骤见 [根目录 README](../README.md)。

最近修复与验证边界见 [2026-10-09 战斗记录](docs/combat-continuation-20261009.md)。旧的修复记录保留在 `docs/`，代表各自版本的情况，不能据其宣称当前全部功能已经完成。

- `host/`：输入、相机、原生地表、碰撞与战斗。
- `bridge/`：共享内存协议适配。
- `passthrough/`：ReShade 图像合成。
- `tests/`：契约检查和测试专用客户端；测试 harness 不装进正式 Minecraft 客户端。
- `tools/`：本机安装、打包与正常保存退出工具。
- `content/`：早期控制台方块原型，属于历史代码，不是当前 Minecraft 集成。

Minecraft 源码位于 `../fabric`，修改后的加载器位于 `../native-loader`，原始 v11 协议位于 `../protocol`。

安装脚本中的 E 盘工具目录及 Steam 路径是开发机设置，换电脑需调整。这个 demo 的原生 NPC 死亡、遮挡、粒子和物理/AI 配合仍有待实际游戏确认的问题。
