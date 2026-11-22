AIagent98 - Windows 9x 上的原生 AI 智能体
==========================================

AIagent98 是一个运行在 Windows 95/98/Me 上的 32 位原生 Win32 AI 智能体。
它不只是聊天：能在 9x 本机读写文件、遍历目录、搜索源码、执行 COMMAND.COM
命令（受 workspace 沙箱和确认框约束），单个 EXE 内完成 TLS 1.2 握手，
直连任意 OpenAI 兼容端点（OpenAI / DeepSeek / Moonshot Kimi / SiliconFlow /
Ollama / vLLM / 本地模型），无需中转机、无需代理、无需改造系统库。

本版本亮点（相对 0.2.0）
------------------------
- 自绘 transcript：emoji 用 GDI 位图直接绘制，不再用 RichEdit OLE，
  根治了真实 Win98/Me 上插入图片导致整进程崩溃（GPF）的问题。
- 现代聊天风格：彩色角色标签、代码块着色、工具调用可折叠。
- 43 个工具（原 31 个）：新增 build / replace_across / outline / restore /
  snapshot / hexdump，以及 http_request / http_download / dns_resolve /
  tcp_probe / web_search / gist_upload。
- web_search 国内外都能用：自动按 Bing -> Bing 国内 -> 百度 -> DuckDuckGo
  的顺序回退，哪个能用就用哪个。
- 对话自动压缩：上下文超过阈值时把旧消息摘要成一条，省 token、省内存。
- 标签页设置：全部 24 项配置都能在 Tools -> Settings... 里用鼠标修改。
- 危险选项（可选）：full_access 取消沙箱和命令黑名单，auto_confirm 免确认。
  默认关闭，仅建议在可丢弃的虚拟机上开启。


运行环境
--------
- Windows 95 OSR2 / 98 / 98SE / Me（推荐 Windows 98 SE）
- 486 及以上 CPU，建议 16 MB 以上内存
- 已安装 Winsock 2（或 1.1）；联网需可用的网络驱动


安装
----
1. 把本目录所有文件复制到硬盘目录（例如 C:\AGENT98）。
   不要直接在光盘或只读目录运行——程序会在 exe 同目录创建
   AGENT98.INI、AGENT98.LOG 和 workspace。
2. 运行 AGENT98.EXE，首次会弹出向导：选择 Provider / API key / Model，
   点 Save 即写入 AGENT98.INI；点 Later 跳过。
3. 也可直接编辑 AGENT98.INI 的 [api] 段填 key / base / model，
   或在 Tools -> Settings... 里图形化设置。


文件说明
--------
AGENT98.EXE    图形界面版（推荐）
A98.EXE        命令行版，共享同一个 AGENT98.INI
TLSPROBE.EXE   TLS 连通性测试：TLSPROBE.EXE https://example.com/
SAMPLE.INI     配置示例，改名为 AGENT98.INI 后按需修改
AGENT98.INI    配置文件（自动生成，保存 API 等设置）
AGENT98.LOG    运行日志（排查问题先看它）


使用步骤
--------
1. 运行 AGENT98.EXE
2. Tools -> Settings... 设置 API（三个标签页：API & Model / Agent / Network）
3. 选 Provider 并填 API key；Ollama 等本地服务不需要 key
4. 点 Test connection，看到 HTTP 200 即配置正确
5. 在输入框发消息，回车发送


GUI 功能
--------
- 自绘 transcript：彩色角色标签（You 蓝 / Assistant 黑 / tool 青 /
  result 绿 / error 红），固定宽度自动换行，emoji 显示为 16x16 图标。
- History 下拉框：自动保存到 exe 同目录 sessions\ 下的会话文件，
  File -> New 可开新会话。
- Alt+Up / Alt+Down 翻历史输入（DOSKEY 风格）。
- File -> Export transcript...（Ctrl+E）：导出为 GBK + CRLF 文本。
- 状态栏：spinner 和轮数计数。
- Tools -> List tools...：可滚动查看全部 43 个工具及其参数说明。
- Tools -> Settings... 三个标签页可改全部配置。


配置要点（AGENT98.INI）
----------------------
[api]   base / key / model / stream / max_tokens / temperature / extra_headers
[agent] workspace / allow_shell / allow_outside / max_steps /
        compact_threshold / compact_keep / full_access / auto_confirm /
        connect_timeout / request_timeout
[tls]   verify
[log]   level (0=ERROR .. 4=TRACE)
[emoji] inline (1=用图片显示 emoji，0=显示 :shortcode: 文本)
[net]   allow (1=启用网络工具) / allow_hosts / max_bytes /
        search_url / github_token

max_steps：工具轮数上限，默认 32，0=不限。
compact_threshold：对话压缩阈值（字节），默认 24576，0=关闭。
compact_keep：压缩后保留的最近消息数，默认 6。
full_access / auto_confirm：危险选项，默认 0，仅在可丢弃 VM 上开启。


网络工具
--------
需先在 [net] 里设 allow=1。提供 http_request / http_download / dns_resolve /
tcp_probe / web_search / gist_upload。可用 allow_hosts 限制允许的主机
（逗号分隔，.domain 匹配后缀），max_bytes 限制响应大小。
gist_upload 需在 github_token 填有 gist 权限的令牌。


联系我们
--------
项目主页与最新版下载：
  https://github.com/hsbaoyin/AIagent98/releases/latest
问题反馈：
  https://github.com/hsbaoyin/AIagent98/issues
