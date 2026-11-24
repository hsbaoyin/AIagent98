AIagent98 0.3.0 - Windows 9x AI Agent
=====================================

在 Windows 95 OSR2 / 98 / Me 上运行的 32 位原生 AI 助手。它能读取和修改
workspace 文件、搜索代码，并在确认后运行 COMMAND.COM 命令。程序在本机完成
DNS、TCP、TLS 1.2、HTTP、SSE 和 agent 工具循环，无需中转机。

快速开始
--------
1. 将 AIagent98-0.3.0.zip 解压到可写目录，例如 C:\AGENT98。
2. 运行 AGENT98.EXE，按首次启动向导填写 API base、key 和 model。
3. 先保持 full_access=0、auto_confirm=0；写入和高风险工具会要求确认。

也可使用命令行版 A98.EXE。运行 TLSPROBE.EXE https://example.com/ 可检查
TLS 和 HTTP 连通性。Winsock 2 是网络连接的前提；Windows 98/Me 已内置。

0.3.0 更新
----------
- 46 个工具，含文件、代码、网络和 skills 工具；可用 tool_profile 选择工具组。
- unchanged-read 缓存、纯文本工具调用恢复、成功检查提示和 CLI /stats。
- 长对话自动压缩；max_steps 默认 0（不限制），仍受循环保护约束。
- 加固 HTTP 头部和响应解析、路径检查、UTF-8/ACP 转换及日志同步。
- 更新 OpenWatcom 构建、PE 兼容性检查和 GUI/网络 mock smoke tests。
- 可选 emoji 图片位于 emoji\bmp\24；不需要时可删除 emoji 目录。

安全说明
--------
默认 workspace 沙箱开启，shell、网络工具和自动确认按配置控制。full_access=1
会关闭路径沙箱、允许 shell 并自动确认工具；只建议在可丢弃的测试虚拟机中使用。
API key 和 GitHub token 保存在同目录的 AGENT98.INI 中，请保护该文件。

文件
----
AGENT98.EXE       图形版（推荐）
A98.EXE           命令行版
TLSPROBE.EXE      TLS / HTTP 探测工具
SAMPLE.INI        配置示例
TOOLS.md          工具参考（GitHub 仓库）
emoji\            可选的 emoji BMP 图片资源

项目与完整更新记录： https://github.com/hsbaoyin/AIagent98
许可证：MIT（emoji 字体图片另见 emoji\LICENSE-NOTO.txt）
