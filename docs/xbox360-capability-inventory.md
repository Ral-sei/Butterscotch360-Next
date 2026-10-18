# Xbox 360 平台层：能力与依赖清单（实施步骤 1）

本文是 `docs/xbox360-platform-plan.md` 步骤 1 的交付物。它对公共运行时所需的每项平台能力逐条盘点，记录：

- **现有接口/契约**：公共代码里已存在的函数、结构或 vtable 入口。
- **端口责任文件**：Xbox 360 侧应新增的文件（全部位于端口专属路径）。
- **依赖/库**：Xbox 360 XDK / OXDK 提供的头文件、库、工具。
- **失败与降级策略**：能力不可用时的行为。
- **验证方式**：如何判定该能力达标。
- **依据·状态**：引用的公共源码位置与 XDK/OXDK 文档页，以及确认度。

> 约束提醒：本清单不授权修改任何公共代码。凡实现必须改动受保护路径的项，一律标为 **阻塞/待审批**，并按规划第 0、风险节停止实施、另行审批。未经目标 SDK 头文件或官方文档确认的函数名、库名、常量、路径，一律标为 **待确认**，不写成既定事实。

> 相关文档：实施进度与真机证据见 `docs/xbox360-progress.md`；图形（`RendererVtable` → D3D9/Xenon）设计见 `docs/xbox360-renderer-design.md`。

## 证据来源与确认度标记

- 公共契约（已逐行阅读，属确定事实）：`src/platformdefs.h`、`src/runner.h`、`src/renderer.h`、`src/file_system.h`、`src/audio_system.h`、`src/loop.h`/`src/loop.c`、`src/gettime.h`/`src/gettime.c`、`src/log.h`/`src/log.c`、`src/runner_keyboard.h`、`src/runner_mouse.h`、`src/runner_gamepad.h`、`src/overlay_file_system.h`、`src/common.h`、`CMakeLists.txt`，以及 `src/ps2/`、`src/ps3/`、`src/vita/`、`src/switch/`、`src/backends/sdl2.c` 的平台范例。
- XDK 文档快照：版本 **21256.17**，位于 `C:\Users\lbx50\.config\opencode\skills\xbox360-sdk-doc\xbox360docs\`。本文引用的页均在正文标注。
- OXDK 本地仓库：`H:\OXDK`（`README.md`、`docs/xbox360.md`、`xbox360/xbox360.mk`）。OXDK 是工具链胶水，不是 XDK 替代品，也不分发微软代码。
- `H:\wiliwili`：仅作阅读参考，不得被 source、复用路径或复制为运行时依赖。
- 标记：**[确定]** = 源码/文档直接支持；**[推断]** = 由多处证据得出；**待确认** = 需以目标 XDK 安装与 OXDK 实测核对；**阻塞/待审批** = 实现需要改动公共代码。

---

## A. 架构决策：入口采用「端口自有 main」而非复用 `loop.c`

现有两类入口：

| 方案 | 使用者 | 公共耦合 |
| --- | --- | --- |
| **A. 复用 `src/loop.c`** | `cli`、`vita`、`switch`、`web`（新系统） | 需要实现 `platformdefs.h` 全套钩子；`loop.c` 还耦合 GLAD、`enum GraphicsAPI`、POSIX 信号、`nowNanos()` |
| **B. 端口自有 `main()`** | `ps2`、`ps3`（旧系统，`CMakeLists.txt` 会从 SOURCES 里移除 `src/loop.c`） | 只依赖 `Runner_*` / `Renderer` / `FileSystem` / `AudioSystem` 公共 API，不碰 `loop.c` |

**建议采用方案 B**（与 PS2/PS3 一致）：`src/xbox360/main.c` 自行 init → `Runner_create` → `Runner_initFirstRoom` → 帧循环 → cleanup。理由：

- 避免 `loop.c` 对 GLAD、`enum GraphicsAPI`、信号处理的耦合，从而**不需要**改动任何公共文件（见 C 节缺口 G2/G3）。
- `loop.c` 的 `platformGetProcAddress` 只在 `USE_GLAD` 下被调用，D3D9 端口不需要 GLAD，方案 B 下无需提供该符号 **[确定]**（`src/loop.c` L157–175、`src/backends/sdl2.c` L345–351）。

方案 A 仍记录在 D 节作为对照；若将来要复用 `loop.c`，则 D 节列出的钩子必须全部实现。

---

## B. 能力与依赖清单

### B.1 程序入口、初始化/退出、帧循环、系统退出通知

| 能力项 | 现有接口/契约 | 端口责任文件 | 依赖/库 | 失败与降级 | 验证方式 | 依据·状态 |
| --- | --- | --- | --- | --- | --- | --- |
| 入口 `main` | 方案 B：无公共入口契约，端口自定义 `int main(int, char**)`（PS2/PS3 先例）；方案 A：`loop()`（`src/loop.h`） | `src/xbox360/main.c` | OXDK `title.ld` + `oxdk360/builtins/builtins.c` 提供 CRT 启动 | 入口初始化失败 → 记录日志并返回非 0 | XEX 能启动到最小画面/日志 | `H:\OXDK\xbox360\xbox360.mk` L163–168、L292–297；`H:\OXDK\docs\xbox360.md` L34–45。入口签名与 argv 语义 **待确认**（以 `abitest`/`title` 样例核对） |
| 初始化顺序 | `Runner_create(dataWin, vm, renderer, fileSystem, audioSystem, seed)`、`VM_create`、`DataWin_parse`（`src/runner.h`、`src/vm.h`、`src/data_win.h`） | `src/xbox360/main.c` | Xapilib（系统服务）、libcMT（CRT） | 任一步失败 → 释放已建资源并退出 | 启动日志无错误；`Runner_create` 返回非空 | PS2 `src/ps2/main.c` L491–546 先例 **[确定]** |
| 帧循环 | `Runner_step`、`Runner_drawPre`、`Runner_beginFrame`、`Runner_drawViews`、`renderer->vtable->endFrameInit/endFrameEnd`、`Runner_drawPost`、`Runner_drawGUI`、`Runner_handlePendingRoomChange`、`Runner_free`（`src/runner.h`） | `src/xbox360/main.c` | — | `runner->shouldExit` 为真时跳出；重复退出安全 | 连续多帧无崩溃、房间切换正确 | PS2 `src/ps2/main.c` L657–718、PS3 `src/ps3/main.c` L418–439 **[确定]** |
| 帧呈现 | `renderer->vtable` 的 present 由渲染器内部处理（PS3 在 main 里 `ps3glSwapBuffers`；D3D9 对应 `IDirect3DDevice9::Present`） | `src/xbox360/xbox360_renderer.c` | `d3d9.lib` | 呈现失败 → 记录并继续/退出 | 画面逐帧更新 | `_dx_IDirect3DDevice9_present_graphics.htm` **[确定]** |
| 系统退出通知 | 无公共契约；PS3 用 `sysUtilRegisterCallback(SYSUTIL_EXIT_GAME, …)` | `src/xbox360/xbox360_lifecycle.c` | `xam.h` + `xapilibw.lib`：`XNotifyCreateListener` / `XNotifyGetNext` | 无通知 → 仅靠游戏自身 `game_end`/`shouldExit` | 主机侧退出请求能触发干净关闭 | `XNotifyGetNext.htm`（返回 0=无通知）、`XNotifyCreateListener.htm`；PS3 L90–105 先例 **[推断]** |
| 平台信息（os_type/locale） | `Runner.osType`、`OS_XBOX360`（`src/runner.h` L126）；`resolveWindowSize` 对 `OS_XBOX360` 返回 1920×1080（`src/loop.c` L252–259） | `src/xbox360/main.c` | `XGetGameRegion`、`XGetLanguage`（xapilib） | 查询失败 → 用默认值 | `os_type`/`language` 返回合理值 | `XGetGameRegion.htm`、`XGetLanguage.htm` **[确定]**（是否需要接入 **待确认**） |

### B.2 显示模式、D3D9 设备与资源生命周期、纹理/调色板/着色器、present 与垂直同步

| 能力项 | 现有接口/契约 | 端口责任文件 | 依赖/库 | 失败与降级 | 验证方式 | 依据·状态 |
| --- | --- | --- | --- | --- | --- | --- |
| 渲染器实现 | `RendererVtable` 全部入口 + `struct Renderer` 基类（`src/renderer.h` L136–264）。契约由 `Runner_beginFrame`/`Runner_draw*` 驱动 | `src/xbox360/xbox360_renderer.c`（+ 头） | `d3d9.h` / `d3d9.lib` | 设备创建失败 → 报错退出；可选降级到无图形版本 | 代表性画面与桌面 GL 输出对照 | `_dx_Direct3D9_For_Xenon.htm`、`_dx_IDirect3D9_createdevice_graphics.htm`、`_dx_direct3dcreate9_graphics.htm` **[确定]**。设计映射见 `docs/xbox360-renderer-design.md`；`d3d9.h` 声明 Xbox 360 **不支持** `StretchRect`/`UpdateSurface`/`ColorFill`/`CreateOffscreenPlainSurface`/`GetRenderTargetData` 等 **[确定]** |
| 设备/资源生命周期 | `vtable->init/destroy`、`beginFrame/endFrameInit/endFrameEnd`（`src/renderer.h` L137–143） | 同上 | `Direct3DCreate9` → `IDirect3D9::CreateDevice` | 初始化失败返回 false，主循环退出 | 反复 init/destroy 不泄漏 | `_dx_IDirect3D9_createdevice_graphics.htm` **[确定]** |
| GML surface / EDRAM RT / predicated tiling | 无公共契约（渲染器内部）；`createSurface`/`setRenderTarget`/`surfaceGetPixels` 等 | `src/xbox360/xbox360_renderer.c` | `CreateRenderTarget`、`Resolve`、`BeginTiling`/`EndTiling`、`CreateTexture` | full-size RT 放不下 → LRU 淘汰缓存 RT 后重试；仍失败 → ≤15 tiles 的 predicated tiling；tile 仍放不下 → 记录错误并返回 false | `EdramStress: PASS (8 surfaces)`；`TilingSelfTest: PASS`（2048×2048，1024×1024 RT，4 tiles） | `predicated_tiling.htm`、`xenon_basic_predicated_tiling.htm`、`_dx_IDirect3DDevice9_begintiling_graphics.htm`、`_dx_IDirect3DDevice9_endtiling_graphics.htm` **[确定：已真机验证 2000×120/2080×120 多 surface 与 2048×2048 tiling]**；限制：tiled surface 不 restore 上一帧内容、bracket 内忽略 viewport/scissor、`surface_set_pixels` 不支持 |
| 纹理/调色板/着色器 | `spriteGetTexture`、`surfaceGetTexture`、`gpuSetShader`、`shaderSetUniform*`、`shadersSupported` 等（`src/renderer.h` L220–238） | 同上 | D3D9 纹理/着色器；`d3dx9` 可选 | 着色器编译失败 → 记录并跳过该 draw | 着色器游戏（如带 shader 的场景）正常 | **关键**：Xbox 360 **无固定功能管线**，所有纹理/光照必须走着色器，`SetTransform/SetTextureStageState/SetLight` 等已被移除 → `fixed_function_pipeline.htm` **[确定]** |
| 纹理上传（Xenon tiled layout） | 无公共契约（渲染器内部） | `src/xbox360/xbox360_d3d_device.{c,h}` + 渲染器 | `d3d9.lib` + `xgraphics.lib`：`IDirect3DDevice9::CreateTexture`、`IDirect3DTexture9::LockRect`、`XGTileTextureLevel`、`XGGetGpuFormat` | 上传失败 → 记录并跳过该纹理 | 真实 TXTR 页面在真机按预期采样 | `CreateTexture` 的 `UnusedPool` 必须为 0、`pUnusedSharedHandle` 为 NULL（`_dx_IDirect3DDevice9_createtexture_graphics.htm`）；**Xenon 纹理为 tiled layout，PC 的 `row*pitch+col*texelsize` 不成立**，须用 `XGTileTextureLevel` 或 `XGAddress2DTiled*`（`_dx_IDirect3DTexture9_lockrect_graphics.htm`、`XGTileTextureLevel.htm`、`xenon_texture_storage.htm`）；`XGTILE_NONPACKED`/`XGTILE_BORDER` 需按 `XGIsPackedTexture`/`XGIsBorderTexture` 决定（`xgraphics.h`）**[确定：已真机验证]** |
| 纹理压缩（DXT + sidecar） | 无公共契约（渲染器内部） | `src/xbox360/xbox360_renderer.c`、`src/xbox360/xbox360_textures.{c,h}`、`src/xbox360/xbox360_overlay.{c,h}` | `xgraphics.lib`：`XGCompressSurface`、`XGTileTextureLevel`、`XGGetGpuFormat`；D3D9 `D3DFMT_DXT1/DXT5` | sidecar 缺页/损坏 → 运行时压缩；压缩失败/尺寸不对齐 → RGBA8；只读 game:\ → 不生成、运行时压缩；按住 START → 跳过并保持 complete=0 | 纹理常驻内存下降、画面无明显块伪影；默认构建首次启动生成完整 sidecar，加载界面显示进度并可跳过 | `Supported_Compressed_Texture_Formats.htm`（DXT1/5 原生、linear+tiled、硬件解压）、`XGCompressSurface.htm`（A8R8G8B8 → LIN_DXT1/5）、`XGTileTextureLevel.htm`（压缩源按块行距铺贴）、`Compressed_Texture_Format_Internals.htm`（4×4 块、8/16 字节）**[确定：已真机验证 DXT1/DXT5 加载、sidecar 生成/加载、83 页首次自动生成、加载界面进度与 START 跳过、菜单画面]**。v3 sidecar 在压缩前做透明像素颜色清理（alpha bleed），消除绿色 chroma 背景渗入蓝色精灵边缘；页 39 纯绿伪影从 249 降到 0（剩余 70 为蓝绿可见像素同块混合）**[确定：离屏解码对比]** |
| 纹理采样状态与混合 | `gpuSetBlendMode`、`gpuSetTexFilter`、`gpuSetAlphaTestEnable` 等（`src/renderer.h` L173–185） | 同上 | `IDirect3DDevice9::SetSamplerState`、`SetRenderState` | 状态错误 → 记录并回退默认 | 透明/半透明精灵与背景正确合成 | `_dx_IDirect3DDevice9_setsamplerstate_graphics.htm`；采样 NULL 纹理会触发断言（`_dx_IDirect3DDevice9_settexture_graphics.htm`）**[确定：MinFilter/MagFilter/MipFilter 与 SRCALPHA/INVSRCALPHA 已真机验证]** |
| TPAG 子矩形 → UV | `TexturePageItem`（`src/data_win.h` L770–789）；GL 后端公式 `u0=sourceX/texW`…（`src/gl/gl_renderer.c` L1230–1233） | 同上 | — | 越界/无页 → 跳过该 draw | 非整页条带裁剪正确 | 与公共 GL 后端同式；端口探针用真实 `data.win` 的 `src=(2,2 155x14)`（页 256×64）得到 `UV=(0.0078,0.0313)-(0.6133,0.2500)` **[确定：已真机验证]** |
| present / 垂直同步 | 无公共 present 契约（由渲染器内部调用） | 同上 | `IDirect3DDevice9::Present(NULL,NULL,NULL,NULL)`；内部调用 `SynchronizeToPresentationInterval` | — | 帧率与同步符合预期 | `_dx_IDirect3DDevice9_present_graphics.htm`：参数必须为 NULL；Present 后不可假设 EDRAM 内容保留 **[确定]** |
| 数学库 | 公共 `src/matrix_math.h`（不依赖 D3DX） | 复用公共头 | **避免** D3DX 数学头 | — | 矩阵运算与 GL 路径一致 | `H:\OXDK\xbox360\xbox360.mk` L130–135：C 文件无法直接用 XDK 的 D3DX/XNA 数学头（需 VMX128，clang 无），C 走 header-shim；端口应使用自有/公共数学 **[确定]** |

### B.3 输入：XInput 手柄数量、按键/摇杆映射、断连/重连语义

| 能力项 | 现有接口/契约 | 端口责任文件 | 依赖/库 | 失败与降级 | 验证方式 | 依据·状态 |
| --- | --- | --- | --- | --- | --- | --- |
| 手柄轮询 | `RunnerKeyboard_onKeyDown/onKeyUp`（`src/runner_keyboard.h`）或 `RunnerGamepadState` 槽位（`src/runner_gamepad.h`） | `src/xbox360/xbox360_input.c` | `Xtl.h` + `Xapilib.lib`：`XInputGetState` / `XInputGetStateEx` | 未连接 → 释放所有按键状态 | 多手柄、热插拔、按键边沿不丢 | `XInputGetState.htm`、`XInputGetStateEx.htm` **[确定]** |
| 按键位映射 | 映射到 GML VK / `GP_*`（`src/runner_gamepad.h` L12–33）；不修改公共映射 | 同上 | `XINPUT_GAMEPAD.wButtons` 位掩码（`XINPUT_GAMEPAD.htm`） | 未定义位忽略 | 各按键触发正确 | `XINPUT_GAMEPAD.htm` **[确定]** |
| 摇杆/死区 | `GP_AXIS_*`、`slot->axisValue`（`src/runner_gamepad.h` L30–46） | 同上 | `sThumbLX/LY/RX/RY`；`XINPUT_GAMEPAD_*_THUMB_DEADZONE` | 死区内归零 | 中心/边缘/死区行为 | `XINPUT_GAMEPAD.htm` **[确定]** |
| 断连/重连语义 | PS2 `pollPad` 的 stable 边沿处理为范例（`src/ps2/main.c` L126–171） | 同上 | `ERROR_DEVICE_NOT_CONNECTED`；无线手柄开机未激活即返回该错误 | 断连 → 释放；重连 → 避免幻影按下 | 运行中拔插手柄 | `XInputGetState.htm` Remarks（无线控制器开机时未激活） **[确定]** |
| 震动（可选） | 无公共契约 | 同上 | `XInputSetState`（`XINPUT_VIBRATION`） | 返回 `ERROR_BUSY` → 延后重试 | 震动可用 | `XInputSetState.htm` **[确定]**（是否实现 **待确认**） |
| 键盘/鼠标 | 公共存在 `RunnerKeyboardState`/`RunnerMouseState` | — | — | **明确不支持**：不提供键盘/鼠标输入，不伪装 | 相关 GML 恒为空/未按下 | 规划第 3.3 节要求明确不支持 **[确定]**（设计决定） |

### B.4 游戏数据与配置、只读/可写分离、存储设备选择/权限/空间不足

| 能力项 | 现有接口/契约 | 端口责任文件 | 依赖/库 | 失败与降级 | 验证方式 | 依据·状态 |
| --- | --- | --- | --- | --- | --- | --- |
| 文件系统后端 | `FileSystemVtable`（`src/file_system.h` L22–65）。**不能复用公共 `OverlayFileSystem`**：它把路径统一规范成正斜杠，而 XDK CRT 只接受反斜杠（见缺口 G6） | `src/xbox360/xbox360_file_system.{c,h}` | XTL CRT 文件 API（`fopen`/`_mkdir`/`FindFirstFileA` 等） | 打开失败 → 返回 null/false | 读 `data.win`、读随附文件、列目录 | `developing_with_xbox_360_unified_storage.htm` **[确定]**；真机验证见 `docs/xbox360-progress.md` 第 6 节 |
| 只读游戏数据 | `resolvePath`/`readFileText`/`readFileBinary`（`src/file_system.h`） | 同上 | 逻辑盘 `game:\`（= `d:\`，游戏光盘/模拟盘，零售与开发机均可用） | 缺失 → 上层报错 | 从光盘读资源 | `developing_with_xbox_360_unified_storage.htm`（Logical Drive Names） **[确定]** |
| 可写用户数据 | `writeFileText`/`writeFileBinary`/`createDirectory`（`src/file_system.h`） | 同上 | **XContent**：`XContentCreate` 挂载动态盘符后，用标准文件 API 读写；`XContentClose`/`XContentFlush` 提交 | 空间不足/设备移除 → 报错；`ERROR_DEVICE_NOT_CONNECTED` → 所有句柄作废 | 存档跨会话持久 | `XCreateContent.htm`、`developing_with_xbox_360_unified_storage.htm` **[确定]** |
| 设备选择/权限 | 无公共契约 | 同上 | `XShowDeviceSelectorUI`（选择存储设备）、`XContentGetCreator`（TCR：校验存档归属）、`XContentCreateEnumerator`/`XEnumerate`（枚举） | 用户取消/无可用设备 → 禁用存档并提示 | 用户选择/取消路径 | `XShowDeviceSelectorUI.htm`、`XContentGetCreator.htm`、`XContentCreateEnumerator.htm` **[确定]** |
| 临时/缓存 | 无公共契约 | 同上 | `cache:\`（系统分配的 2 GB 暂存分区，需 `XMountUtilityDrive`；写入需 `XFlushUtilityDrive` 提交） | 无硬盘 → `cache:` 不可用，降级为不缓存 | 缓存读写 | `developing_with_xbox_360_unified_storage.htm`（The Cache Partition） **[确定]** |
| 目录枚举 | `listDirectory`（`src/file_system.h` L64） | 同上 | `FindFirstFile`/`FindNextFile`/`FindClose`（`Xtl.h` + `Xapilib.lib`；无 `.`/`..`） | 打开失败 → 返回 null | `file_find_*` GML 正常 | `FindFirstFile.htm` **[确定]** |
| **存档可用性前提** | — | 端口构建脚本 | **必须设置非零 `XEX_TITLE_ID`** | 标题 ID 为 0 时：`XContent` 存档包/档案/成就全部解析为空，存档“看似成功实则不存在” | 存档重启后仍可读 | `H:\OXDK\xbox360\xbox360.mk` L44–47 **[确定，关键]** |
| 开发机路径 | 无公共契约 | 同上 | `devkit:\` / `e:\`（仅开发机；需先 `DmMapDevkitDrive`；零售标题禁止直连） | 零售机不可用 → 不作为正式方案 | 开发机可读 | `developing_with_xbox_360_unified_storage.htm` **[确定]** |

### B.5 音频：设备、混音格式、缓冲与延迟、设备不可用时的行为

| 能力项 | 现有接口/契约 | 端口责任文件 | 依赖/库 | 失败与降级 | 验证方式 | 依据·状态 |
| --- | --- | --- | --- | --- | --- | --- |
| 音频后端 | `AudioSystemVtable`（`src/audio_system.h` L24–57） | `src/xbox360/xbox360_audio.{c,h}` | `xaudio2.h` + `xaudio2.lib`（`XAudio2Create` → `IXAudio2::CreateMasteringVoice`）；`xmcore.lib` 提供 `XLFQueue*` | 初始化失败 → 保持 `initialized=false`，`playSound` 返回 -1（静音降级） | 声音可播放 | `XAudio2Create.htm`、`IXAudio2_CreateMasteringVoice.htm` **[确定]**；**已真机验证**（第 13 节） |
| 解码格式 | 内嵌 `AUDO` = RIFF WAVE (16-bit PCM)；外部/流 = `.ogg` | 同上 + `src/xbox360/stb_vorbis_impl.c` | vendored `stb_vorbis`（Ogg）；自写 WAV 解析 | 未知容器 → 记 warning 并静音 | 声音可播放 | 实测目标 `data.win`/`audiogroup1.dat` 全为 44100 Hz/16-bit PCM；**已真机验证** |
| 字节序 | `Audio Data and Endianness`：16-bit PCM 须为大端 | 同上 | WAV 就地 16-bit 字节交换；stb_vorbis 输出宿主序 | — | 声音正确（未做听感确认） | `audio_overview_xaudio2_endianness.htm` **[确定]** |
| 无声降级 | `NoopAudioSystem_create()`（公共、可直接复用不改） | `src/xbox360/main.c` | — | `XAudio2Create` 失败即静音启动 | 无声模式不崩溃 | **已实现**：失败路径返回 -1，不伪装成功 |
| 混音格式/通道/采样率 | `setChannelCount`、`playSound`（`src/audio_system.h`） | 同上 | `XAUDIO2_DEFAULT_CHANNELS` / `XAUDIO2_DEFAULT_SAMPLERATE`；每 source voice 用源格式 | 格式不支持 → 不建 voice 并静音 | 采样率/通道数正确 | `XAUDIO2_Constants.htm` **[确定]**；**已真机验证**（44100 Hz 单/双声道） |
| 缓冲/延迟 | 无公共契约 | 同上 | `IXAudio2SourceVoice` 提交整段缓冲 + `XAUDIO2_LOOP_INFINITE` | 整段解码 → 大文件一次性停顿 | 长时间播放稳定 | **部分**：位置推进与循环已真机验证；**分块流式/延迟量测未做** |
| 处理器/线程 | — | 同上 | `XAUDIO2_DEFAULT_PROCESSOR` 在 360 上占用硬件线程 4、5，与默认 NUI 线程 5 重叠 | — | 无线程争用 | `XAudio2Create.htm` Remarks **[确定，注意]** |
| 实例上限 | `X360_MAX_SOUND_INSTANCES`（端口 96） | 同上 | 单实例 + 固定槽位，按优先级回收 | 槽位满 → 驱逐最低优先级已结束实例 | 单实例 | 端口实现；`XAUDIO2_MAX_INSTANCES` **[确定]** |

### B.6 单调计时、睡眠/节流、日志输出、崩溃与调试输出

| 能力项 | 现有接口/契约 | 端口责任文件 | 依赖/库 | 失败与降级 | 验证方式 | 依据·状态 |
| --- | --- | --- | --- | --- | --- | --- |
| 单调计时 `nowNanos()` | **公共** `src/gettime.h` / `src/gettime.c`（`#if` 选择后端，无 X360 分支） | — | `QueryPerformanceCounter`/`QueryPerformanceFrequency`（Time Base 寄存器） | — | 时间单调递增 | `QueryPerformanceCounter.htm` **[确定]**；但见 **缺口 G1（阻塞/待审批）** |
| 睡眠/节流 | 方案 B：端口自定义；方案 A：`platformSleepUntil(uint64_t)`（`src/platformdefs.h` L152） | `src/xbox360/main.c` | `Sleep`（XTL）/ 忙等；`common.h` 的 `YIELD()` 已有 PPC 分支（`or 27,27,27`） | 无精确睡眠 → 忙等兜底 | 帧率限制符合 room speed | `src/common.h` L55–56；`src/ps2/main.c` L712–718 先例 **[确定]** |
| 日志输出 | `logInfo/logWarn/logError/logDebug`（`src/log.h`）→ 需要平台提供 `platformLog(logType, fmt, va_list)`（`src/log.c` L6 声明） | `src/xbox360/xbox360_log.c` | `OutputDebugString`（XTL） | 无调试器 → 输出丢弃，不崩溃 | 日志可见/不崩溃 | `src/log.c`、`OutputDebugString.htm`；`src/switch/log.c` 先例 **[确定]** |
| 崩溃/调试输出 | 方案 B 不编译 `src/loop.c`，无公共信号契约 | 端口可选实现 | `OutputDebugString`；XDK 调试器（XBDM） | 无信号处理 → 直接终止 | 崩溃有可见信息 | `OutputDebugString.htm`；OXDK `xbdm` 可 `listen` 调试输出（`docs/xbox360.md` L106–113） **[推断]** |

### B.7 内存/对齐、字节序、链接库、可执行文件格式、内容布局与部署

| 能力项 | 现有接口/契约 | 端口责任文件 | 依赖/库 | 失败与降级 | 验证方式 | 依据·状态 |
| --- | --- | --- | --- | --- | --- | --- |
| 分配器 | 公共 `src/utils.h`（`safeMalloc` 等，调用 libc `malloc`） | 端口复用 | libcMT 的 CRT；可选 `XMemAlloc` + `XALLOC_ATTRIBUTES` | 分配失败 → `safeMalloc` 报错 | 大内存场景不 OOM | `XMemAlloc.htm` **[确定]**；是否改用 `XMemAlloc` **待确认** |
| 对齐 | `BS_ALIGN(x)`（`src/common.h` L36–40） | — | clang attribute | — | 对齐断言通过 | `src/common.h` **[确定]** |
| 字节序 | `IS_BIG_ENDIAN`（`src/common.h` L22–24，由 `__BIG_ENDIAN__`/`__BYTE_ORDER__` 决定） | — | Xenon 为大端 | — | `data.win` 解析正确 | `src/common.h`、`big_endian_and_little_endian.htm` **[确定]**；规划第 4 节要求不得无证据改公共解析器 |
| 编译器/目标 | — | `scripts/xbox360/toolchain.cmake` | 带 Xenon ABI 补丁的 clang，target `powerpc-unknown-none-elf`，`+xenon-abi` | 无 Xenon ABI → 拒绝构建 | `abitest` 数值正确 | `xbox360.mk` L174–187、`docs/xbox360.md` L37–45 **[确定]** |
| 链接与库 | — | `scripts/xbox360/oxdklink_wrapper.py` | `oxdklink.py` + `title.ld`；XDK 库经 `coff2elf` 转 ELF；默认 `xapilib,libcMT` | 缺库 → 链接失败 | 链接无缺失符号 | `xbox360.mk` L292–297、`docs/xbox360.md` L115–136 **[确定]** |
| 可执行格式 | — | `scripts/xbox360/build.sh` | XEX2，镜像基址默认 `0x82000000`（title 区间），栈默认 `0x100000` | 格式错误 → 主机拒载 | `xexcheck`/主机加载 | `xbox360.mk` L42–43、`docs/xbox360.md` L3–6 **[确定]** |
| CRT 缺口 | — | — | `oxdk360/builtins/builtins.c`（补 64 位算术助手）；`oxdklink` 强制 `_fltused` 使 `printf("%f")` 可用 | 缺 `_fltused` → `KeBugCheck` | 浮点格式化正常 | `docs/xbox360.md` L94–102、`xbox360.mk` L211–215 **[确定]** |
| 内容布局/部署 | 方案 B 需自定 `dataWinPath`/`saveFolder`（对照 `src/switch/main.c` L46–47、`src/vita/main.c` L51–52） | `src/xbox360/main.c` + 部署脚本 | XBDM（端口 730）：`deploy`、`magicboot title=… directory=…`、`screenshot` | 无真机 → 仅交叉编译，不得宣称真机验收 | 主机上启动/截图 | `docs/xbox360.md` L104–113、`xbox360.mk` L316–321 **[确定]** |
| `argv[0]`/工作目录 | PS3 用 `argv[0]` 推导 data.win 路径（`src/ps3/main.c` L177–193） | `src/xbox360/main.c` | XEX 启动参数 | 无 argv → 用固定盘符路径 | 路径正确 | **待确认**（XEX 启动时 argv/工作目录语义需实测） |

---

## C. 公共钩子缺口与阻塞项

以下缺口按规划“风险与停止条件”记录。**在获得授权前不得修改公共代码**；替代方案须由维护者裁定。

### G1 — `nowNanos()` 无 Xbox 360 分支（阻塞/待审批）

- 事实：`src/gettime.c` 用 `#if defined(PLATFORM_PS2)/PLATFORM_PS3/_WIN32/__APPLE__/…` 选择后端，**没有** Xbox 360 分支 **[确定]**。
- 影响面：`nowNanos()` 被公共代码广泛使用——`src/runner.c`、`src/vm_builtins.c`、`src/profiler.c`、`src/physics/physics_box2d_timer.c`、`src/video/ffmpeg/ffmpeg.c` **[确定]**。
- 可选路径：
  1. 在 `src/gettime.c` 增加 `#elif defined(PLATFORM_XBOX360)` 分支 → **修改受保护文件，需审批**。
  2. 端口在自身源文件清单中**排除 `src/gettime.c`**，并提供端口专属的 `nowNanos()` 定义（不改公共文件，但重复公共逻辑，需维护者认可）。
- 现状：端口已按**方案 2** 落地——`port/xbox360/CMakeLists.txt` 排除 `src/gettime.c`，`src/xbox360/xbox360_time.c` 提供基于 `QueryPerformanceCounter` 的 `nowNanos()`，完整运行时已交叉编译并链接成 XEX（见 `docs/xbox360-progress.md` 第 3 节）。**未修改任何公共文件**，但重复了公共逻辑，仍**待维护者认可**；若改选方案 1，端口应改回。
- 状态：**阻塞/待审批（方案 2 已实现，待认可）**。

### G2 — `src/loop.c` 的 POSIX 信号/CRT 耦合（仅方案 A）

- 事实：`src/loop.c` 在 `#ifndef __wasi__` 下 `#include <signal.h>`，且 `USE_CRASH_SIGNAL_HANDLER` 在“非 `_WIN32`、非 `PLATFORM_VITA`、非 `__SWITCH__`、非 `__wasi__`”时启用，要求 `sigaction`/`sigemptyset`/`struct sigaction` **[确定]**。
- 影响：Xbox 360 既不属于上述排除项，freestanding/`-ffreestanding` 下可能没有完整 `signal.h`/`sigaction`。
- 规避：**采用方案 B（自有 main）即可绕开**，不编译 `src/loop.c`。
- 状态：方案 A 下 **阻塞/待审批**；方案 B 下 **不适用**。

### G3 — `enum GraphicsAPI` 缺少 D3D9（仅方案 A）

- 事实：`enum GraphicsAPI { SOFTWARE, MODERN_GL, LEGACY_GL, NOOP }`（`src/platformdefs.h` L41–46），`gfx` 在 `src/loop.c` 中用于选择渲染器 **[确定]**。
- 影响：方案 A 下要接入 D3D9 渲染器需新增枚举值 → 改动受保护头文件。
- 规避：**方案 B 不使用 `gfx`/`loop.c` 的渲染器选择**。
- 状态：方案 A 下 **阻塞/待审批**；方案 B 下 **不适用**。

### G4 — `platformGetProcAddress` / GLAD（不适用）

- 事实：该符号仅在 `USE_GLAD`（即 `ENABLE_LEGACY_GL`/`ENABLE_MODERN_GL`）下被调用与定义 **[确定]**。D3D9 端口不启用 GL，方案 B 下无需提供。
- 状态：**不适用**。

### G5 — `src/*.c` 通配会带入 GL/视频/音频后端（构建隔离）

- 事实：根 `CMakeLists.txt` 用 `file(GLOB SOURCES src/*.c)` 并追加 `src/${PLATFORM}/*.c`、音频/GL/视频源 **[确定]**。端口**不得**走这条路径。
- 规避：端口自有 CMake 工程**显式枚举**所需公共源文件，不 `add_subdirectory` 根工程，不改根 `CMakeLists.txt`（规划第 2 节）。
- 状态：**已由规划约束覆盖**。

### G6 — 路径分隔符：公共 `OverlayFileSystem` 在 Xenon 上不可用（已解决，无需改公共代码）

- 事实（devkit 实测）：XDK CRT 只接受反斜杠路径——`fopen("game:\\options.ini")` 成功，`fopen("game:/options.ini")` 失败。
- 影响：公共 `src/overlay_file_system.c` 的 `normalizePath` 把所有路径规范成正斜杠，因此在 Xbox 360 上完全无法访问存储；能力清单 B.4 原先“可复用 OverlayFileSystem”的记录**已作废**。
- 规避：端口实现反斜杠原生的 `src/xbox360/xbox360_file_system.c`（不改公共文件），保留同样的两区语义。真机验证见 `docs/xbox360-progress.md` 第 6 节。
- 状态：**已解决**。若上游希望消除重复，可考虑让 `OverlayFileSystem` 的分隔符可配置；是否接受该上游改动需另行审批。

---

## D. 对照：若复用 `src/loop.c`（方案 A）所需的 `platformdefs.h` 钩子全集

仅在方案 A 下需要实现（`src/platformdefs.h` L141–152，全部为公共声明、端口提供定义）：

| 钩子 | 语义 | D3D9 端口的合理实现 |
| --- | --- | --- |
| `platformInit(reqW, reqH, title, headless)` | 建立窗口/设备/控制器 | 建立 D3D9 设备、XInput |
| `platformInitFunctions(Runner*)` | 绑定 `runner->setWindowTitle/getWindowSize/setWindowSize/getWindowPosition/setWindowPosition/windowHasFocus/setCursor` | 多数可为最小桩 |
| `platformExit(void)` | 释放平台资源 | 释放设备、控制器 |
| `platformSwapBuffers(void)` | 呈现 | `Present(NULL,…)` |
| `platformGetProcAddress(const char*)` | GL 函数解析 | 仅 GLAD 需要，D3D9 可返回 NULL |
| `platformHandleEvents(void)` | 事件泵，返回 true=请求关闭 | XInput 轮询 + `XNotifyGetNext` |
| `platformGetMousePos(double*,double*)` | 鼠标坐标 | 不支持 → 写 0 |
| `platformGetWindowSize/GetScaledWindowSize(int32_t*,int32_t*)` | 帧缓冲/窗口尺寸 | 返回固定分辨率 |
| `platformSetWindowSize(int32_t,int32_t)` | 改尺寸 | 固定分辨率 → no-op |
| `platformSetWindowTitle(const char*)` | 标题 | no-op |
| `platformSleepUntil(uint64_t)` | 帧节流 | 忙等 + `YIELD()` |

另有 `platformLog(logType, const char*, va_list)`（`src/log.c` 依赖），方案 A/B 都需要。

---

## E. 端口源文件清单草案（供步骤 2 的构建工程使用）

以下为**形状**而非最终清单；端口工程应显式维护并在上游变化时明确失败（规划第 2 节）。

- 公共运行时（显式枚举，示例）：`src/*.c` 中除 `src/loop.c` 外的核心（`runner.c`、`vm.c`、`vm_builtins.c`、`data_win.c`、`instance.c`、`renderer` 相关公共助手等）。
- **排除** `src/loop.c`（方案 B）、`src/gl*`、`src/backends/*`、`src/audio/{miniaudio,openal,ps2}`、`src/video/ffmpeg`。
- 物理：`src/physics/*.c`（`ENABLE_PHYSICS`）或 `src/physics/physics.c` + `src/physics/disabled/physics_disabled.c`（对照根 `CMakeLists.txt` L160–166）。
- 音频：端口自有 `src/xbox360/xbox360_audio.c`（XAudio2 后端，见进度第 13 节）；不链接 `src/audio/{miniaudio,openal,ps2}`。公共 `NoopAudioSystem` 仍编译进运行时，但不再使用。
- 文件：公共 `src/overlay_file_system.c`（不改）+ 端口 `src/xbox360/xbox360_file_system.c`。
- 端口专属：`src/xbox360/main.c`、`xbox360_renderer.c`、`xbox360_input.c`、`xbox360_log.c`、`xbox360_file_system.c`、`xbox360_audio.{c,h}`、`stb_impl.c`、`stb_vorbis_impl.c`（单一定义 TU：`STB_IMAGE_IMPLEMENTATION` / `stb_vorbis`，对照 `src/switch/stb_impl.c`）。
- vendor：`vendor/{stb,miniz,bzip2,md5,sha1,base64}`、`vendor/box2d`（若启用物理）。
- 需要新增的替换实现（受 G1 影响）：`nowNanos()`。

---

## F. 待确认清单（需以目标 XDK 安装 / OXDK 实测核对）

1. XEX 入口签名与 `argv[0]`/工作目录语义（`main` vs 其他）——以 OXDK `abitest`/`title` 样例核对。
2. 目标 XDK 版本、`Xtl.h`/`d3d9.h`/`xaudio2.h`/`xam.h` 的实际存在与版本差异。
3. ~~XAudio2 在 360 上的默认通道数/采样率与缓冲模型~~：已实施（进度第 13 节）——主混音用 `XAUDIO2_DEFAULT_CHANNELS`/`XAUDIO2_DEFAULT_SAMPLERATE`，每 source voice 按源格式（目标数据 44100 Hz）；分块流式与延迟量测仍未做。
4. `game:\` 是否为 `data.win` 的正式加载根，以及内容打包（XEX 内嵌 vs 独立 content package）方式。
5. 是否/如何使用 `XMemAlloc` + `XALLOC_ATTRIBUTES` 取代 libc `malloc`。
6. `XNotifyCreateListener`/`XNotifyGetNext` 是否为本端口生命周期所必需（对照 PS3 的 `SYSUTIL_EXIT_GAME`）。
7. 是否实现 `XInputSetState` 震动。
8. 开发机 vs 零售机的路径与权限差异对存档策略的最终影响（规划要求：策略不清则不实现写存档）。
9. 是否需要 `XGetGameRegion`/`XGetLanguage` 驱动 `os_type`/locale。

---

## G. 下一步（进入步骤 2 的前置）

1. 在受支持的 Linux/macOS/WSL 主机上运行 OXDK `doctor`，构建补丁 LLVM，并用 `abitest`/`d3dclear`/`title` 验证工具链与 XEX 加载（规划步骤 2）。
2. 落地端口专属 `env.sh`/`build.sh`/`toolchain.cmake`/`oxdklink_wrapper.py`，先产出**空 XEX**。
3. 就 G1（`nowNanos()`）向维护者提交阻塞说明与两个候选方案，取得裁决后再继续无图形/无音频最小版本。

---

## 附：本文引用的公共源码位置

- 平台钩子：`src/platformdefs.h` L141–152
- Runner 契约：`src/runner.h`（`Runner_create`、`Runner_beginFrame`、`Runner_draw*`、`Runner_step` 等）
- 渲染器契约：`src/renderer.h` L136–264
- 文件系统契约：`src/file_system.h` L22–65；两区沙箱 `src/overlay_file_system.h`
- 音频契约：`src/audio_system.h` L24–57；`src/noop_audio_system.h`
- 计时：`src/gettime.h` / `src/gettime.c`
- 日志：`src/log.h` / `src/log.c`
- 输入：`src/runner_keyboard.h`、`src/runner_mouse.h`、`src/runner_gamepad.h`
- 公共宏：`src/common.h`（`IS_BIG_ENDIAN`、`BS_ALIGN`、`YIELD`）
- 平台范例：`src/ps2/main.c`、`src/ps3/main.c`、`src/vita/main.c`、`src/switch/main.c`、`src/switch/switch_input.c`、`src/switch/log.c`、`src/backends/sdl2.c`
- 构建：`CMakeLists.txt`
