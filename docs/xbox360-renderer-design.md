# Xbox 360 D3D9 渲染器设计（实施步骤 3.4 / 4）

本文是 `docs/xbox360-platform-plan.md` 步骤 3.4（图形）与步骤 4（资源、内容与性能）的设计与实施交付物。它把「公共 `RendererVtable` 契约」映射到「Xenon Direct3D 9 + XGraphics 的实际约束」，并明确区分**已真机验证**、**已实现未验证**与**未实现**，供后续实现与评审使用。

状态口径与 `docs/xbox360-progress.md` 第 10 节一致：任何一项不得从「已实现」推断为「已可用」。

> 边界提醒：本文只描述新增端口文件（`src/xbox360/`、`port/xbox360/`、`scripts/xbox360/`）内的实现。**不允许**为 Xbox 360 修改 `src/renderer.h`、`src/gl*`、`src/runner.c` 等公共路径。已确认不触碰公共路径；若某能力必须改公共契约，按规划标记为**阻塞/待审批**并停止。

> **2026-10-17 修订**：修正 §5 的纹理页计数、§3.4/§3.6/§3.7 与实现不符的描述；补充 §2 的像素中心约定、§7 的帧率与房间切换规则；新增 §10 修复记录与真机复测结果。此前标注「已真机验证」但仓库内没有对应日志的条目，保留原文并在 §10.5 说明证据缺口。

---

## 1. 目标与非目标

**目标**

- 用端口自有实现满足 `src/renderer.h` 的 `RendererVtable`，使 `Runner_draw*` 能真正出画面。
- 全部绘制走着色器（Xenon 无固定功能管线）。
- 纹理上传遵循 Xenon tiled layout。
- 帧呈现与 EDRAM 生命周期满足 Xbox 360 的复制/解析要求。

**非目标（本阶段明确不做）**

- 不追求性能优化；先保证帧正确，再量测（规划步骤 3.4）。本阶段仅记录帧时间，不做优化。
- 不实现桌面 GL 的等价特性（如 GLAD、VAO、GPU 端矩阵栈）。
- 不实现在线功能与认证相关能力。

---

## 2. 已验证的 Xenon D3D9 约束

以下约束均有 XDK 文档依据，且多数已由端口探针或渲染器在 devkit 上实测（证据见 `docs/xbox360-progress.md` 第 8、11、14 节）。

| 约束 | 内容 | 依据 | 端口状态 |
| --- | --- | --- | --- |
| 无固定功能管线 | 所有几何/光照必须走着色器；`SetTransform`/`SetTextureStageState`/`SetLight`/`GetTransform` 等已从 API 移除。`SetFVF`/`SetVertexDeclaration` 仍在头文件中（`SetFVF` 属 legacy 兼容路径），端口一律用 `D3DVERTEXELEMENT9` + 着色器 | `fixed_function_pipeline.htm`、`_dx_Direct3D9_For_Xenon.htm`、`d3d9.h` | 已遵循 |
| 非 COM | D3D 对象不使用 COM 引用计数语义；`Release` 由 XDK 内联实现 | `_dx_Direct3D9_For_Xenon.htm` | 已遵循 |
| 设备创建 | `CreateDevice(Adapter, DeviceType, hUnusedFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface)`，`hUnusedFocusWindow` 未使用 | `_dx_IDirect3D9_createdevice_graphics.htm` | **已真机验证** |
| BeginScene/EndScene | 在 Xbox 360 上已废弃、什么都不做 | `_dx_IDirect3DDevice9_beginscene_graphics.htm`、`_dx_IDirect3DDevice9_endscene_graphics.htm` | 已遵循（不调用） |
| Present | 四个参数都未使用，须为 `NULL`；内部调用 `SynchronizeToPresentationInterval`，同帧不要再手动调用它；Present 后**不可假设 EDRAM 内容保留** | `_dx_IDirect3DDevice9_present_graphics.htm` | **已真机验证** |
| HD 帧缓冲下限 | HD 显示模式下帧缓冲不得小于 1280×720，否则触发认证警告 | KDNet 运行时警告（实测） | **已遵循**（back buffer 1280×720） |
| 顶点声明 | `D3DVERTEXELEMENT9` 数组须以 `D3DDECL_END()` 结尾 | `_dx_IDirect3DDevice9_createvertexdeclaration_graphics.htm` | **已真机验证** |
| GPU 格式 | `XGGetGpuFormat(D3DFORMAT)` 取纹理的 GPU 格式 | `XGGetGpuFormat.htm`、`xgraphics.h` | **已真机验证** |
| 纹理创建 | `CreateTexture(..., DWORD UnusedPool, ..., HANDLE *pUnusedSharedHandle)`：`UnusedPool` 必须是 0，`pUnusedSharedHandle` 必须是 NULL | `_dx_IDirect3DDevice9_createtexture_graphics.htm`、`d3d9.h` 内联实现 | **已真机验证** |
| 纹理 tiled layout | PC 的 `row*pitch+col*texelsize` **不成立**；只能经 `XGTileTextureLevel` / `XGUntileTextureLevel` / `XGAddress2DTiled*` 定位纹素 | `_dx_IDirect3DTexture9_lockrect_graphics.htm`、`XGTileTextureLevel.htm`、`xenon_texture_storage.htm` | **已真机验证** |
| tiling 旗标 | `XGTILE_NONPACKED`/`XGTILE_BORDER` 须由 `XGIsPackedTexture`/`XGIsBorderTexture` 决定，不能默认 0（除 packed 且无 border） | `XGTileTextureLevel.htm`、`XGIsBorderTexture.htm`、`XGIsPackedTexture.htm` | 已遵循（`x360TileFlags()`） |
| 线性源/目标的 32bpp 排布 | `XGTileTextureLevel` 的线性源按 32 位字 `0xAARRGGBB` 解释（`ImageDecoder_decodeToRgba` 输出 RGBA 字节，须在端口中就地转换为 ARGB 字） | 探针 + 本次红色爱心实拍反证 | **已真机验证**（红爱心/白字颜色正确） |
| 采样 NULL 纹理 | Xenon 上对 NULL 纹理采样会触发断言（与 PC 返回黑色不同） | `_dx_IDirect3DDevice9_settexture_graphics.htm` | 已遵循（未纹理绘制绑定 1×1 白纹理） |
| 渲染目标不可锁定 / 不可直接采样 | 不能创建可锁定 render target，也不能直接从 render target 采样；须 `Resolve` 到纹理后才能锁定/采样 | `_dx_IDirect3DDevice9_createrendertarget_graphics.htm`、`_dx_IDirect3DDevice9_Resolve_graphics.htm` | **已实现并真机验证**（自检 PASS） |
| EDRAM 容量 | EDRAM 固定 10 MB，且须容纳 back buffer + 其他 RT；放不下时需 predicated tiling | `predicated_tiling.htm`、`xenon_basic_predicated_tiling.htm` | **未实现**（见第 6 节；当前只做 1280×720 back buffer + 最多一张应用 surface） |
| 视口 | `SetViewport` 会把视口裁剪到 render target 0 尺寸；`SetRenderTarget`/`SetDepthStencilSurface`/`BeginScene` 会重置视口为默认值 | `_dx_IDirect3DDevice9_setviewport_graphics.htm` | 已遵循（每次绑定目标后显式设置视口/剪刀） |
| 2D 像素中心约定 | Xenon D3D9 默认 `D3DRS_HALFPIXELOFFSET = FALSE`，即 D3D1–9 约定（像素中心在整数坐标）。GameMaker 的投影按 OpenGL/D3D10 约定（像素中心在半整数坐标）编写；置 TRUE 会对每个顶点施加等效 `(-0.5, -0.5)` 的 post-viewport 偏移，使采样落在纹素中心，消除半像素偏移和采样接缝 | `_dx_d3drenderstatetype_graphics.htm`（`D3DRS_HALFPIXELOFFSET`） | **已遵循**（`x360Init` 设 TRUE；2026-10-17） |
| 帧率与 VBlank | 同步 Present 会阻塞到 VBlank（约 16.6 ms，即 60 Hz）；30 fps 标题必须自行节流，不能把 Present 当作 30 fps 限制器 | `_dx_IDirect3DDevice9_present_graphics.htm`、`synchronous_and_asynchronous_swaps_on_xbox_360.htm` | **已实现**（`main()` 按 `Runner_getEffectiveGameSpeed` 节流；真机稳态 33.39 ms/帧） |
| 不支持的 D3D9 方法 | `StretchRect`、`UpdateSurface`、`UpdateTexture`、`ColorFill`、`CreateOffscreenPlainSurface`、`GetRenderTargetData`、`GetFrontBufferData`、`ProcessVertices`、`BeginStateBlock`/`EndStateBlock` 等 | `d3d9.h`（`IDirect3DDevice9` 内联方法前的注释列表） | 已按此约束设计（复制改用绘制；读回改用 `Resolve`） |
| 像素着色器不支持 `discard`/`texkill` | 实测 `fxc` 报 `X3500: Microcode Compiler not yet implemented: 'discard' statement` | 本次构建实测 | 已改：alpha test 用 `D3DRS_ALPHATESTENABLE`/`ALPHAREF`/`ALPHAFUNC`，不在着色器里做 |
| D3DX 数学头 | C 文件无法直接用 XDK 的 D3DX/XNA 数学头（需 VMX128）；端口用公共 `src/matrix_math.h` | `H:\OXDK\xbox360\xbox360.mk`、公共头约定 | 已遵循 |

---

## 3. `RendererVtable` → D3D9/Xenon 映射（实施结果）

`RendererVtable` 定义见 `src/renderer.h` L136–239，由 `Runner_beginFrame`/`Runner_drawViews`/`Runner_drawGUI` 等驱动。端口实现在 `src/xbox360/xbox360_renderer.c`（工厂 `X360Renderer_create`，呈现 `X360Renderer_present`）。

状态口径：**已真机验证**＝在 devkit 上看到画面/日志证据；**已实现**＝代码完整、交叉编译通过，但本阶段未在目标机复现；**未实现**＝无实现或明确降级。

### 3.1 生命周期与帧

| vtable 入口 | 端口实现策略 | 状态 |
| --- | --- | --- |
| `init` / `destroy` | 创建设备（1280×720、无深度缓冲、interval one）、顶点声明、着色器、1×1 白纹理、页表；幂等释放 | **已真机验证** |
| `beginFrame(gameW,gameH,winW,winH)` | 绑定应用 surface RT，设置全屏视口/剪刀 | **已真机验证** |
| `endFrameInit` | 结束应用 surface 阶段（无操作，绘制为立即模式） | **已真机验证** |
| `endFrameEnd` | 应用 surface `Resolve` → back buffer `Clear` 黑 + 字母框合成（仅 `usingAppSurface && appSurfaceAutoDraw`） | **已真机验证** |
| `clearScreen(color, alpha)` | `Clear`（`D3DCLEAR_TARGET`），清当前 RT | **已真机验证**（自检：清红后读回 255,0,0,255） |
| 帧呈现 | 端口 `main()` 在 Draw GUI 之后调用 `X360Renderer_present()`（back buffer `Present`）；**`runner->pendingRoom != -1` 时跳过 Present**，保持上一帧（黑场）直到新房间加载完成 | **已实现**；Chapter 5 启动/房间切换真机跑通，转场逐帧复测见 §10.5 |
| 帧时间 / `delta_time` | 每帧开始用 `nowNanos()` 计算 `runner->deltaTime`；帧末按 `Runner_getEffectiveGameSpeed` 节流（`Sleep` 到目标周期） | **已实现**；真机稳态 33.39 ms/帧（≈29.95 fps） |

### 3.2 视图与投影

| vtable 入口 | 端口实现策略 | 状态 |
| --- | --- | --- |
| `beginView`/`endView` | 设置视口 + 剪刀，按当前 view 的相机矩阵上传 WVP | **已真机验证**（房间视图正确出画） |
| `applyProjection` | `Renderer_applyProjection` 后把 `MATRIX_WORLD_VIEW_PROJECTION` 上传到 `c0` | **已真机验证** |
| `setMatrix` | 同 GL 语义：重算 WVP 并上传 | 已实现 |

> 关键差异：D3D 的 render target 第 0 行是顶行，与 GameMaker 的 Y 向下一致，因此端口**不做** GL 的 `flipClipY`，直接上传公共相机矩阵。

### 3.3 GUI / 表面

| vtable 入口 | 端口实现策略 | 状态 |
| --- | --- | --- |
| `beginGUI`/`setGuiProjection`/`endGUI` | host 目标：绑定 back buffer，字母框视口/剪刀；surface 目标：绑定 surface RT；GUI 投影用 `Matrix4f_viewProjection(0,0,guiW,guiH,0)`（y=0 在顶部） | **已真机验证**（GUI/Post Draw 路径随房间渲染工作） |
| `createSurface`/`surfaceFree`/`surfaceExists` | `CreateRenderTarget` + 可锁定 resolve 纹理；失败时槽位无效（`surfaceExists` 返回 false） | **已真机验证**（自检 create/free PASS） |
| `surfaceResize` | 释放并重建 RT + resolve 纹理 | 已实现（未在本阶段复现） |
| `setRenderTarget`/`ensureApplicationSurface` | 绑定 surface RT；按隐式应用 surface / view surface / SURFACE_CAMERA 三种情形设置视口与投影；应用 surface 惰性创建 | **已真机验证** |
| `getSurfaceWidth`/`getSurfaceHeight`/`surfaceGetTexture` | 返回元数据与编码句柄（`0x40000000 | surfaceID`） | 已实现 |
| `drawSurface`/`drawSurfaceColor`/`drawSurfaceTiled` | 采样 surface 的 resolve 纹理做四边形绘制 | 已实现 |
| `surfaceCopy` | **不能**用 `StretchRect`/`UpdateSurface`；改为把源 resolve 纹理绘制到目标 RT（禁用混合），并保存/恢复目标、视口、投影、剪刀 | 已实现 |
| `surfaceGetPixels` | `Resolve` → `LockRect` → `XGUntileTextureLevel` → ARGB→RGBA | **已真机验证**（自检读回精确像素） |
| `surfaceSetPixels`/`surfaceUploadPixels` | 上传到 resolve 纹理（`XGTileTextureLevel`）；若目标当前绑定则重绘该区域，否则标记 `rtStale` 于下次绑定时恢复 | `surfaceSetPixels` **已真机验证**（自检读回精确像素）；`surfaceUploadPixels` 未单独复现 |
| `createSpriteFromSurface`/`deleteSprite` | 读回 surface 像素→新建页纹理 + TPAG + sprite（`DataWin_allocSpriteSlot`）；删除时释放动态页/TPAG | **已真机验证**（自检创建/删除 PASS） |

### 3.4 精灵与几何

| vtable 入口 | 端口实现策略 | 状态 |
| --- | --- | --- |
| `drawSprite`/`drawSpritePart`/`drawSpritePartColor`/`drawSpritePos` | 由 TPAG 取 UV/目标矩形，生成带四顶点色/UV 的四边形，批处理提交 | **已真机验证**（真实 DELTARUNE 精灵正确出画） |
| `drawSpriteTiled`/`drawTiledPart` | `drawSpriteTiled` 移植 GL 的网格锚点算法（**未**移植 GL 的可见区域裁剪优化，大房间 tiled 背景会多生成离屏 quad）；`drawTiledPart` 是端口自有实现（GL 后端为 `nullptr`），2026-10-17 修正 UV 原点为 `sourceX/Y + srcX/Y`（此前误用 `targetX/Y + srcX/Y`，九宫格会采样错位） | 已实现；九宫格路径未真机复现 |
| `drawRectangle`/`drawRectangleColor`/`drawLine`/`drawLineColor`/`drawTriangle` | 用白色纹理生成单色几何 | 已实现 |
| `primitiveBegin`/`primitiveBeginTexture`/`primitiveEnd`/`drawVertex` | CPU 顶点累积，`primitiveEnd` 用 `DrawPrimitiveUP` 提交 | 已实现 |
| `drawVertexBuffer` | 把用户 `VertexBuffer`（position/color/texcoord）转成端口顶点后 `DrawPrimitiveUP`；无 texcoord 用白纹理 | 已实现 |
| `flush` | 提交当前批次（`DrawIndexedPrimitiveUP`） | **已真机验证** |
| `drawTile` | 置 `nullptr`，走公共默认 tile 路径（与 GL 一致） | 已遵循 |

### 3.5 字体

| vtable 入口 | 端口实现策略 | 状态 |
| --- | --- | --- |
| `drawText`/`drawTextColor` | 移植 GL 的字形布局逻辑（行/水平垂直对齐/字距/渐变四顶点色），字形来自 FONT+TPAG | **已真机验证**（真实字体文本正确出画） |
| `drawTextUI` | 用内嵌调试字体（`src/debug_font`）自建合成 Font + 纹理 | 已实现（本阶段 main 未调用） |

### 3.6 GPU 状态

| vtable 入口 | 端口实现策略 | 状态 |
| --- | --- | --- |
| `gpuSetBlendMode`/`gpuSetBlendModeExt`/`gpuGetBlendMode` | 映射 GameMaker blend 到 `D3DRS_*`。`separateAlphaBlend` 影子状态保留 `gpu_set_blendmode_ext` 的分离 alpha；内部绘制结束后的 `x360ApplyBlendState` 不再把它重置为 FALSE（2026-10-17 修复） | 默认模式**已真机验证**；ext 待复测 |
| `gpuGetBlendFactors`/`gpuSetBlendEnable`/`gpuGetBlendEnable` | `D3DRS_ALPHABLENDENABLE` + 因子影子状态 | 已实现 |
| `gpuSetTexFilter` | `D3DSAMP_MINFILTER`/`MAGFILTER`/`MIPFILTER` + `ADDRESSU/V`，遍历 sampler 0..7（2026-10-17 起；此前只设 stage 0，用户 shader 的 s1+ 不受控） | **已真机验证**（POINT，stage 0） |
| `gpuSetAlphaTestEnable`/`gpuGetAlphaTestEnable`/`gpuSetAlphaTestRef` | `D3DRS_ALPHATESTENABLE`/`ALPHAREF`/`ALPHAFUNC`（GREATER） | 已实现 |
| `gpuSetColorWriteEnable`/`gpuGetColorWriteEnable` | `D3DRS_COLORWRITEENABLE` | 已实现 |
| `gpuSetFog` | 像素着色器常量（`c0`）实现 rgb 混合，0 为不启用 | 已实现 |

### 3.7 着色器与纹理句柄

用户脚本 shader（data.win 的 `hlsl9_*`）由**离线预编译的 sidecar** 支撑，见第 4.2 节：`scripts/xbox360/precompile_shaders.py` 在主机侧用 Xbox `fxc` 把每个 shader 编译为微码，写进 `data.x360shaders`；`src/xbox360/xbox360_shadersets.c` 在运行时加载并创建 D3D9 着色器对象，向渲染器暴露常量/采样器表。**未预编译**（sidecar 缺失、该 index 无记录、或只有一半被编译）时，逐 shader 回落到内置着色器并打一次 warning——与旧行为一致，只是不再是无条件降级。

| vtable 入口 | 端口实现策略 | 状态 |
| --- | --- | --- |
| `shaderIsCompiled` | `hlsl9_*` 已在 sidecar 中且 VS/PS **两半齐备**时 true；否则 false（无运行时编译，未预编译即不可用） | 已实现；机制**已真机验证**（自检 sidecar） |
| `shadersSupported` | true（平台本身是着色器管线） | 已实现 |
| `gpuSetShader`/`gpuResetShader` | 切换/复位 device 的 VS/PS 到 sidecar 中的用户 shader；不可用时回落内置并每 shader 一次 warning | 已实现；机制**已真机验证**（自检 draw 走用户 shader） |
| `shaderGetUniform`/`shaderGetSamplerIndex` | 查 sidecar 的 uniform 表（含 `_` 前缀回退匹配）；句柄即表内下标 / 寄存器号；不可用返回 -1 | 已实现；机制**已真机验证**（自检 `outColor`→1、`gm_BaseTexture`→0） |
| `shaderSetUniform*` | 按 uniform 的 stage/寄存器上传常数（F/I，含 `f_array`）；`gm_Matrices` 等内置块按 transpose 旗标决定是否转置。2026-10-17 起 `gm_Matrices` 只上传 fxc 报告的 `regCount`（此前固定 20 个寄存器，可能覆盖后续常量）。`f_array` 仍按 `ceil(count/4)` 上传，未处理 `float3[]` 的 D3D9 打包 | 已实现；机制**已真机验证**（自检 `outColor` 生效） |
| `spriteGetTexture`/`surfaceGetTexture` | 句柄约定：sprite = `tpagIndex+1`，surface = `0x40000000|id`；0 = 无纹理 | 已实现 |
| `textureGetTexelWidth`/`textureGetTexelHeight`/`textureGetUVs`/`textureSetStage` | 解析句柄、查询尺寸/UV；`textureSetStage` 支持任意 stage，stage 0 的 `texHandle==0` 也绑定 1×1 白纹理（2026-10-17 起），绝不绑 NULL | 已实现（UV 公式、stage 0 绑定**已真机验证**） |

---

## 4. 着色器策略与可复现性（实施结果）

### 4.1 内置着色器（几何/文字/精灵的默认路径）

Xenon 无固定功能管线，因此基础精灵/矩形/文字都需要一组「顶点 + 像素」着色器。端口现有一组预编译着色器：

- `src/xbox360/shaders/renderer_vs.hlsl`（`vs_3_0`）：`position:POSITION`(float4) + `color:COLOR0`(float4) + `texcoord:TEXCOORD0`(float2)，`mul(uWorldViewProjection, float4(pos.xyz,1))`。
- `src/xbox360/shaders/renderer_ps.hlsl`（`ps_3_0`）：`tex2D(s0,uv) * color`，再做 `lerp(rgb, fog.rgb, fog.a)`。
- 顶点声明：`D3DVERTEXELEMENT9` `{POSITION FLOAT4@0, COLOR FLOAT4@16, TEXCOORD FLOAT2@32}`，stride 40，已真机验证。
- **预编译，不在运行时编译。** `scripts/xbox360/compile_shaders.py` 调用安装在 Windows 主机的 Xbox 360 `fxc.exe`（`XEDK` 指向本机 XDK，不写死仓库），产物写入**构建目录** `build/xbox360/generated/`，**不提交**到仓库（XDK 微码属专有产物）。`port/xbox360/CMakeLists.txt` 用 `add_custom_command` 声明依赖，干净构建可重现。
- **矩阵约定。** HLSL 默认 `column_major` 打包与公共 `Matrix4f`（列主序、列向量）一致；`SetVertexShaderConstantF(0, m, 4)` 写入 c0..c3。端口不做 GL 的 clip-Y 翻转。
- **alpha test 不放着色器。** 实测 Xenon 微码编译器不支持 `discard`（`X3500`），故按 D3D render state 做（`D3DRS_ALPHATESTENABLE` 等），测试像素为 PS 输出 alpha。

参考：`graphics_tools_fxc.htm`（Xbox `fxc` 命令行）。

### 4.2 用户脚本 shader 离线预编译（规划步骤 4.3，实施结果）

规划步骤 4.3「用户脚本 shader 离线预编译」已实施：把 data.win 的 HLSL9 shader 在**主机侧**编译为 Xenon 微码，运行时只加载编译产物，从而在**不修改任何公共文件**、**不依赖 `d3dx9`** 的前提下实现 `shaderIsCompiled` / `gpuSetShader` / `shaderGetUniform` / `shaderGetSamplerIndex` / `shader_set_uniform_*`。

**数据流**

```
data.win 的 SHDR.hlsl9_*  ─┐
（或 --hlsl-dir 的成对源码） ├─►  Xbox 360 fxc  ─►  data.x360shaders（sidecar，构建产物，不提交）
                            ┘                     └─►  运行时 X360ShaderSet_load → D3D 着色器对象 + uniform 表
```

- `scripts/xbox360/precompile_shaders.py`（端口自有）：两种输入模式
  - `--datawin <data.win>`：自解析 `SHDR` chunk，取每个 shader 的 `hlsl9_Vertex`/`hlsl9_Fragment` 源码，**按 data.win 的 shader index 编号**编译。
  - `--hlsl-dir <dir>`：编译 `<name>.v.hlsl` / `<name>.f.hlsl` 成对源码（按名字排序编号），用于自检与覆盖。
- **sidecar 格式**：大端、逐字节容器（magic `BSX3SHD\0`，version 1）。所有整数按大端写，**大端 Xenon 读取端无需字节序转换**。记录：index、name、flags、VS/PS 微码、uniform 表（name/stage/regClass/regIndex/regCount/type）。
- `src/xbox360/xbox360_shadersets.{c,h}`（端口自有）：加载 sidecar、为每个 shader `CreateVertexShader`/`CreatePixelShader`（微码先做**对齐拷贝**），暴露 uniform 表与查找（`_` 前缀回退匹配）。

**编译期处理（Xenon 约束）**

- **`discard` 剥离。** Xenon fxc 拒绝 `discard`（`X3500`）。工具剥掉 `discard;`，置 `X360_SHADER_FLAG_ALPHATEST_STRIPPED`，运行时改由 `D3DRS_ALPHATESTENABLE`/`ALPHAREF`/`ALPHAFUNC` 做 alpha test（与内置着色器同一路径）。
- **transpose 检测。** 检测源码里的 `transpose(_gm_Matrices...)`，置 `X360_SHADER_FLAG_MATRICES_TRANSPOSED`；运行时对这类 shader 上传**转置**的 `gm_Matrices`，其余逐字上传。理由：GameMaker 生成的 HLSL9 写 `mul(transpose(gm_Matrices[i]), v)`，即 `Mᵀ·v`；要得到 `WVP·v`，需上传 `transpose(WVP)`（列主序）。
- **uniform 表来源。** 解析 fxc `/Fc` 清单的 «Registers» 表（`_gm_Matrices c29 20` 即基址 + 跨度寄存器数）与 `defconst`（类型/形状），得到每个 uniform 的 stage/寄存器/类型；运行时据此上传，无需 d3dx9 反射。
- **GMS 2.3+ 仅存声明的问题。** 新式 Windows data.win 的 `hlsl9_*` 槽只有声明前言（函数体以 GLSL/编译后的 HLSL11 形式另存），无 `main` 入口。工具**跳过**这类 shader（记日志、计数），运行时对该 index 回落内置着色器；**半个 shader** 的源码若只有声明，则把该半当缺失处理。

**可用性判定（避免 varying 不匹配）**

一个 sidecar shader 只有 `HAS_VS|HAS_PS` 两半齐备且 `vs`/`ps` 句柄非 NULL 时才可用（`x360ShaderUsable`）；否则**整体回落内置着色器**。只绑定半套会造成写/读 varying 不一致。

**内置块上传**

用户的 `gm_Matrices`（VI 矩阵块，含 WVP）与像素侧 `gm_FogColour` / `gm_PS_FogEnabled` 由运行时按名字查找并上传（对齐 GameMaker HLSL9 前言）。内部绘制（表面合成、`surfaceCopy`、像素回写重绘）一律强制内置着色器，绘制结束再恢复到活动用户 shader。

**真机证据（devkit `192.168.2.9`）**

`-DBS_X360_SHADER_SELFTEST`（`scripts/xbox360/build.sh --shader-selftest`）把一对确定性测试 shader（`src/xbox360/shaders/selftest/selftest.{v,f}.hlsl`：VS 读 `transpose(gm_Matrices[4])`，PS `outColor` 乘 `gm_BaseTexture`）经该工具编译为 `data.x360shaders`，部署到 data.win 同目录，开机跑一次自检。KDNet 实测：

```
ShaderSelfTest: sidecar shaders=1, using index 0
ShaderSelfTest: shader_get_uniform("outColor") = 1
ShaderSelfTest: shader_get_sampler_index("gm_BaseTexture") = 0
ShaderSelfTest: center (expect 0,255,0,255) = (0,255,0,255)
ShaderSelfTest: PASS
```

即**机制已真机验证**：sidecar 加载 + D3D 着色器创建、`shader_get_uniform`/`shader_get_sampler_index` 查表、`shader_set_uniform_f` 上传、走用户 shader 的 draw、以及转置 `gm_Matrices` 的落点（红底上画绿即证明矩阵与 uniform 都到位）——全部在 devkit 上经像素读回证实。

**未验证 / 限制（如实记录）**

- **真实标题的 SHDR 重编译路径未端到端复现。** 现有目标 data.win 无法触发：DELTARUNE ch1 无 `SHDR`；ch2 的 `hlsl9_*` 为仅声明（被跳过）。自检用的是合成 shader 对，非真实标题 shader。
- **浮点矩阵经 GML `shader_set_uniform_f_array` 的打包**（`float4x4` 连续 16 float）未在真机复现；自检只覆盖了 1 寄存器 uniform。
- **多 stage 覆盖语义**（未纹理绘制时 stage>0 的回退）未在真机复现。
- sidecar 文件名固定为 `data.x360shaders`（经 runner 文件系统相对加载），**未纳入打包/部署自动化**。

---

## 5. 纹理与 TPAG 策略（实施结果）

- **页级、惰性上传。** 每个 `TXTR` 页在首次被引用时解码为 RGBA8，在端口中就地转换为 `0xAARRGGBB` 32 位字，`CreateTexture(A8R8G8B8, 1 level)` → `LockRect` → `XGTileTextureLevel` → `SetTexture`。**已真机验证**。
- **线性源字节序。** `ImageDecoder_decodeToRgba`（`src/image/image_decoder.c`，PNG / GameMaker 自定义 QOI / BZip2+QOI）输出 R,G,B,A 字节；`XGTileTextureLevel`/`XGUntileTextureLevel` 的 32bpp 线性数据按 `0xAARRGGBB` **字**解释。端口做一次就地字节置换（`x360RgbaBytesToArgbWords`）。红爱心/白字颜色正确即反证了该排布（此前探针直接把 RGBA 当作字上传，彩色纹理会串通道）。
- **解码复用公共实现。** `gm2022_5` 标志由 `DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0)` 决定。原始目标（DELTARUNE ch1）的 `data.win` 共 **9 个 TXTR 页**；本阶段实际按需解码的是**页 7**（2048×2048），其余页未解码。2026-10-17 的 Chapter 5 目标为 **83 个 TXTR 页**（日志 `83 texture pages`），按需解码。
- **TPAG 子矩形 → UV**（与公共 GL 后端同式，`src/gl/gl_renderer.c` L1230–1233）：
  ```
  u0 = sourceX / texW            v0 = sourceY / texH
  u1 = (sourceX + sourceWidth) / texW
  v1 = (sourceY + sourceHeight) / texH
  ```
  **已真机验证**：真实 `data.win` TPAG 1 为 `src=(2,2 155x14)`，页 256×64，得到 `UV=(0.0078,0.0313)-(0.6133,0.2500)`。
- **绘制尺寸用 `targetWidth/Height`**（bounding 矩形内的绘制尺寸），而不是 `sourceWidth/Height`（纹理采样尺寸）——与 GL 后端一致。
- **tiling 旗标**由 `x360TileFlags()` 用 `XGIsPackedTexture`/`XGIsBorderTexture` 计算。
- **待办。** 外部 `.yytex`（`externalPath`）路径已按 GL 语义读取 runner 的文件系统，但未在本阶段复现；mip/packed/border 变体未在真实资源上出现。

### 5.1 GPU 纹理压缩：DXT + `data.x360tex` sidecar（2026-10-17）

**动机。** Chapter 5 的 83 个 TXTR 页里，单次运行实际加载了 18 页、约 **260 MB** RGBA8；`data.win` 本身还常驻约 167 MB。Xbox 360 只有 512 MB 统一内存，这已经接近上限。2026-10-17 的长运行日志里，页 31/32/36/45/47/60/79 在加载后期 `failed to decode`；`ImageDecoder` 使用裸 `malloc`，失败时返回 NULL，**疑与内存压力有关**（未单独取证）。

**方案。** 静态 TXTR 页优先使用 Xenon 原生 DXT：

- **无 alpha 的页 → `D3DFMT_DXT1`**（4bpp，8:1）。
- **有 alpha 的页 → `D3DFMT_DXT5`**（8bpp，4:1）。
- **透明像素颜色清理（alpha bleed）**：GameMaker 的 TXTR 页会把完全透明区域填成 chroma 色（本作大量页面是纯绿 `(0,255,0)`）。DXT 颜色块会把透明 texel 的 RGB 一起编码，蓝色精灵边缘因此出现绿色伪影。压缩前对每个 4×4 块统计可见 texel 的平均色，把块内 `alpha==0` 的 texel RGB 改为该平均色；相邻的空块继承邻居平均色，避免双线性采样把 chroma 色拉进边缘。alpha 本身不改。
- **运行时压缩**：解码 RGBA → alpha bleed → `x360RgbaBytesToArgbWords` → `XGCompressSurface(..., D3DFMT_LIN_DXT1/5, ..., D3DFMT_LIN_A8R8G8B8, ...)` → `CreateTexture(D3DFMT_DXT1/5)` → `LockRect` → `XGTileTextureLevel`。
- **离线 sidecar `data.x360tex`**：如果存在且与当前 `data.win` 匹配（`gameID` + 文件大小），直接上传其中的线性 DXT/ARGB 数据，跳过解码和压缩。
- **回退链**：sidecar 缺页/损坏 → 运行时解码 + `XGCompressSurface` → 失败或尺寸不是 4×4 对齐 → RGBA8。动态页（`createSpriteFromSurface`）、surface resolve 纹理、`drawTextUI` 调试字体保持 RGBA8。
- **sidecar 格式**（大端，version 3）：36 字节头（magic `BSX3TEX\0`、version、pageCount、tableCount、gameID、dataWinSize、flags）+ `tableCount × 24` 字节页表（pageId、format、width、height、dataOffset、dataSize）+ 数据块；flags bit 0 = complete。v3 表示 DXT 数据已做透明像素颜色清理；v2 会被拒绝并重新生成（避免继续使用带绿色伪影的旧缓存），v1 头为 32 字节、无 complete 标志，仍可读取但会触发重新生成。
- **构建开关**：`BS_X360_TEXTURE_AUTOGEN` 默认 **ON**（`scripts/xbox360/build.sh --no-texture-autogen` 可关闭）；`--dump-textures` → `BS_X360_DUMP_TEXTURES`（边加载边写，devkit 调试用，自动关闭 autogen）。
- **首次启动自动生成（默认）**：`main()` 在 `Runner_create` 之后、`Runner_initFirstRoom` 之前调用 `X360Renderer_generateTextureSidecar`。sidecar 缺失或 `complete=0` 时，逐页调用 `x360EnsurePage`（解码 → 压缩 → 写入 sidecar），每页上传后立即释放纹理并复位页表，避免 83 页同时常驻；全部尝试完后写 `complete=1` 并重新加载 sidecar。第二次启动看到 `complete=1` 直接复用，不再生成。
- **加载界面进度与跳过**：生成期间调用 `X360Overlay_drawTextureProgress`，显示 `Compressing textures... (n/83)`、百分比条、内存和 `Hold START to skip`。按住 START 会停止剩余页，sidecar 以 `complete=0` 关闭（下次启动继续生成），当前启动用部分 sidecar，缺页走运行时压缩。START 在释放前会从游戏输入中屏蔽，避免第一帧误触发菜单。
- **写盘位置/零售限制**：sidecar 通过端口 `FileSystem` 写到 `save` 区（devkit 上默认是 `data.win` 同目录，即可写的 `game:\`）。零售机 `game:\` 只读，`X360TextureDump_create` 会失败并回退运行时压缩；零售部署应预先生成 sidecar 并随标题打包，或改用可写 XContent/cache 路径（存档策略仍未实现）。

**为什么不用 PS3 的调色板方案。** PS3 端口实际是 **8bpp 索引 + 256×N CLUT + palette 片段着色器**（`src/ps3/ps3_textures.c`、`paletted.fcg`），不是 DXT。Xbox 360 也能做（`D3DFMT_L8` + `D3DFMT_A8R8G8B8` CLUT + `ps_3_0` 依赖纹理读取），但会：

1. 强制索引纹理 point sampling，破坏 `gpu_set_texfilter` / `texture_set_interpolation`；
2. 每次 TPAG 调色板行变化都要打断批处理；
3. 用户 shader 采样 `gm_BaseTexture` 会拿到索引而不是颜色。

DXT 由硬件透明解压，内置/用户 shader、线性过滤和批处理都不受影响，因此本端口选 DXT。调色板仅作为“DXT 画质不达标且该页不走用户 shader”的备选，未实施。

代码核对（Chapter 5）：`SHDR` 有 66 个 shader，运行时会调用 `shader_set`（`build/ch5_600_listen.log` 有 `shd_*` 回落 warning）；`gml_GlobalScript_texture_set_interpolation` 在全局初始化中执行，`Runner_create` 用 `optn.info & 0x2` 初始化 `renderer->texFilter`（该 `data.win` 的 `info=0x38400204`，初始为 nearest）。因此线性过滤和用户 shader 采样必须保持可用，调色板方案会破坏这两点。

**真机证据（devkit `192.168.2.9`，Chapter 5）**

- 运行时压缩（无 sidecar）：页 5 → DXT1 32 KB、页 39 → DXT5 4096 KB、页 80 → DXT5 1024 KB；`3 texture page(s) resident, 5.0 MB`；600 帧稳态 33.38 ms/帧（与压缩前一致）。
- sidecar 生成：`--dump-textures` 运行 600 帧后 `X360Tex: texture dump closed (3 pages)`，`data.x360tex` 5,277,672 字节；正常构建日志 `X360Tex: loaded "data.x360tex"`，三页均 `from sidecar`，稳态 33.38 ms/帧。
- 扩展到菜单：dump 运行加载 12 页（含此前解码失败的页 31），正常构建 12/12 页从 sidecar 读取；截图 `build/dxt_play0.png`、`build/dxt_play1.png`、`build/dxt_menu_sidecar2.png` 与 RGBA 版本观感一致（文字边缘、渐变背景无明显块伪影）。**这是代表性场景，不是逐像素/全量页面验收。**
- 首次启动自动生成（默认构建）：首次启动读到不完整 sidecar（`41/41, complete=0`），逐页生成；加载界面显示 `Compressing textures... (30/83)`、36% 和 `Hold START to skip`（`build/autogen_overlay.png`）。按住 START 后生成在 41/83 停止，`texture dump closed (41 pages, complete=0)`、`sidecar generation skipped (41/83 pages)`，游戏继续启动，缺页（如页 80）走运行时压缩（`build/autogen_skipped.png`）。不跳过时完成 83 页：`texture dump closed (83 pages, complete=1)`、`loaded (83/83, complete=1)`；第二次启动没有生成日志，20 秒内到达 `First room`。带加载界面的生成约 3.5 分钟（本次页 30 在 15 s、页 73 在 180 s），无界面 dump 版本约 2.5 分钟；差异主要来自每页 Present/截图。
- 2026-10-17 绿色伪影修复（v3）：页 39 源图有 52,856 个 alpha=0 纯绿 texel；旧 v2 sidecar 解码后 104,195 个蓝色可见像素中有 249 个变成纯绿。加入 alpha bleed 后重新生成 v3（83/83, complete=1，163.3 s），同口径纯绿只剩 0 个，剩余 70 个是 `(107,178,57)` 这类蓝绿可见像素同块混合的暗绿。详见 `docs/xbox360-progress.md` 第 19 节。

**未验证/限制**

- DXT 对像素画、文字、软 alpha 的块伪影需要逐场景评审；目前只看了 logo、intro、存档菜单和部分角色页。已加入透明像素颜色清理（v3 sidecar），用于消除绿底页在蓝色/深色精灵边缘产生的绿色伪影；仍需在角色、战斗和 UI 场景逐页复测。
- sidecar 目前由 devkit 的 dump 构建生成（需要跑过相应内容），不是主机侧独立工具；sidecar 未再做磁盘压缩，体积约等于 DXT 数据。
- 多 mip / packed / border 变体未在真实资源上出现；DXT 路径只创建 1 level。
- 运行时 `XGCompressSurface` 的一次性耗时尚未单独量测（600 帧总平均受首次房间加载影响）。

---

## 6. 渲染目标与 EDRAM 策略（已实现，部分真机验证）

这是当前最不确定的部分，端口采用如下模型：

- **一个 GML surface = 一张 main-memory 可锁定 resolve 纹理；EDRAM render target 按需创建并缓存。**
  - `surface_create` 只创建 resolve 纹理（普通 `CreateTexture`，可 `LockRect`），**不**在这里分配 EDRAM RT。这样 `surface_create` 不会因为 EDRAM 满而失败，`surface_exists` 也保持 true。
  - 绑定某个 surface 时，如果它还没有 RT，才 `CreateRenderTarget(w,h,A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL)`。**RT 不在 unbind 时释放**，而是保留在缓存里供下次绑定直接复用，避免正常游戏在每次 surface 切换时反复创建/释放。
  - EDRAM 压力下按 LRU 淘汰：分配失败时，从非当前、非应用 surface 的缓存 RT 中淘汰最久未使用的那个（内容已在 resolve 纹理里），然后重试；应用 surface 的 RT 只在最后一步淘汰，并在下次绑定应用 surface 时重建 + restore。
  - 因此正常路径的 EDRAM 占用是：back buffer 1280×720（约 3.5 MB）+ 应用 surface（640×480，约 1.2 MB）+ 少量缓存 RT。`room_dw_fcastle_orange_gauntlet` 的 2000×120 / 2080×120（约 1 MB）不会触发淘汰。
- **单张 RT 放不下时使用 predicated tiling（2026-10-17 实现并真机验证）。**
  - 如果 full-size RT 在 LRU 淘汰后仍创建失败，改为创建一张 **tile 大小** 的 RT（当前最大 1024×1024，4 MB），把 surface 的屏幕空间切成 32×32 对齐的 tile（最多 15 个），在首次 draw/clear 前 `BeginTiling`，在 unbind/Resolve 时 `EndTiling` 把各 tile 合成到 full-size resolve 纹理。
  - 已验证 2048×2048 surface：full-size RT（16 MB）放不下，自动退化为 1024×1024 RT + 4 tiles；clear + 1024×1024 绿色矩形读回正确（`TilingSelfTest: PASS`）。
  - **限制**：tiled surface 按“每次绑定重画”处理，不把 resolve 纹理 restore 回 tile RT；tiling bracket 内忽略 viewport/scissor 变更（D3D 控制 per-tile viewport）；`surface_set_pixels`/`surface_upload_pixels` 在 tiled surface 上返回 false；tile 数超过 15 或 tile RT 仍放不下时记录错误并返回 false。纹理页应尽量在进入 bracket 前已驻留；首次使用新页的懒上传发生在 bracket 内属于未完全验证路径。
- **采样必须经 Resolve。** Xenon 不能直接从 RT 采样；`drawSurface`、应用 surface 合成、`surfaceGetTexture` 等都采样 resolve 纹理。采样前若该 surface 正是当前 RT，先 `Resolve(D3DRESOLVE_RENDERTARGET0, NULL, tex, ...)`。
- **Present 后 EDRAM 不保留。** 因此：
  - 目标切换（unbind）时先 `Resolve`，把内容固化到 resolve 纹理；
  - `Present` 时对当前 RT `Resolve`，并把所有 surface 标记 `rtStale`；
  - 下一帧重新绑定某个 stale surface 时，重建 RT（如果已释放）并用 resolve 纹理**重绘**回去（restore）后再让游戏绘制，从而保住 GameMaker 的「surface 跨帧持久」语义。
- **合成与清屏。** `endFrameEnd` 绑定 back buffer 后先 `Clear` 黑（EDRAM 内容不确定，须自清），再按字母框把应用 surface 画上去。
- **深度缓冲不用。** 设备创建时 `EnableAutoDepthStencil=FALSE`，节省 1280×720 的 EDRAM 占用（2D 渲染不需要 z）。

**真机证据**：`--renderer-selftest` 自检在 devkit 上 `PASS`：
```
RendererSelfTest: createSurface -> 0 (exists=1)
RendererSelfTest: after clear   (expect 255,0,0,255) = (255,0,0,255)
RendererSelfTest: after setPixels (expect 0,255,0,255) = (0,255,0,255)
RendererSelfTest: createSpriteFromSurface -> 1093
RendererSelfTest: PASS
```

> 2026-10-17 在 Chapter 5 目标上重跑 `--renderer-selftest`：`createSurface -> 0`、clear/setPixels 读回精确、`createSpriteFromSurface -> 8536`、`PASS`（`build/ch5_selftest_listen.log`，构建目录产物，不提交）。

**未实现 / 风险**：

- **predicated tiling 的语义限制**：tiling 已实现并覆盖“单张 RT 大于 EDRAM”的情况，但 tiled surface 目前按每次绑定重画处理（不 restore 上一帧内容），且 bracket 内的 viewport/scissor 变更被忽略、`surface_set_pixels` 不支持。若某 surface 需要跨绑定持久内容或频繁 setPixels，仍应避免让它落到 tiling 路径。单张 RT 的 tile 数上限为 15；4096×4096 这类需要 >15 个 1024×1024 tile 的 RT 仍会失败并记录 `CreateRenderTarget` 错误。
- **潜在认证警告**：实测标题**启动和退出时各一次**记录「minimum frame buffer of 1280×720」警告（`build/listen.log`、`build/ch5_600_listen.log` 均可见）；back buffer 已是 1280×720，疑与应用 surface（640×480）作为 RT 有关，待定位。
- **2026-10-17 复测**：`--renderer-selftest` 新增 `EdramStress`，连续创建并绑定 8 张 2000×120 / 2080×120 surface，`EdramStress: PASS (8 surfaces)`，无 `CreateRenderTarget` 失败。实际走到 `room_dw_fcastle_orange_gauntlet` 并继续向左（`room_dw_fcastle_top_ascent`）时，日志中的 surface 25/26/45/46（2000×120、2000×200、2080×120）全部创建成功，没有白屏和 EDRAM 错误。
- **2026-10-17 tiling 自检**：`--renderer-selftest` 新增 `TilingSelfTest`，创建 2048×2048 surface（full-size RT 16 MB 放不下），自动使用 1024×1024 RT + 4 tiles；clear 红色后画 1024×1024 绿色矩形，读回 `(512,512)=(0,255,0,255)`、`(1536,512)=(255,0,0,255)`，`TilingSelfTest: PASS`。日志：`X360Render: surface 2048x2048 uses predicated tiling (1024x1024 RT, 4 tiles)`。
- **2026-10-17 LRU 缓存**：RT 改为按需创建 + LRU 淘汰，不再在每次 unbind 时释放；overlay 增加 `RTs: n (evict n)` 统计，正常游戏可观察缓存命中/淘汰次数。

---

## 7. Present 与帧生命周期

端口 `main()` 自有帧序（对齐 `src/loop.c` 与 PS2/PS3 端口）：

```
frameStart = nowNanos()
runner->deltaTime = frameStart - lastFrameStart
Runner_drawPre(runner, fbW, fbH)          // 画到应用 surface
Runner_beginFrame(runner, gameW, gameH, winW, winH, fbW, fbH)
Runner_drawViews(runner, gameW, gameH, debug)
renderer->vtable->endFrameInit(renderer)
Runner_drawPost(runner, fbW, fbH)         // back buffer
renderer->vtable->endFrameEnd(renderer)   // 应用 surface → back buffer 合成
Runner_drawGUI(runner, fbW, fbH, gameW, gameH)  // back buffer
if (runner->pendingRoom == -1)
    X360Renderer_present(renderer)        // Present
Runner_handlePendingRoomChange(runner)
sleepUntil(frameStart + 1e9 / Runner_getEffectiveGameSpeed(runner))
```

- Present 参数为 `NULL`；呈现同步由 Present 内部完成，不手动 `SynchronizeToPresentationInterval`（`_dx_IDirect3DDevice9_present_graphics.htm`）。
- 1280×720 为 HD 模式下的最小帧缓冲。
- **帧率限制**：Present 只同步到 60 Hz VBlank（`synchronous_and_asynchronous_swaps_on_xbox_360.htm`）。端口在帧末按 `Runner_getEffectiveGameSpeed(runner)`（房间速度 / `game_set_speed` / data.win 默认）用 `Sleep` 节流；`runner->deltaTime` 在帧首按实际墙钟计算，供 `delta_time`、物理和淡入淡出使用。
- **房间切换**：`runner->pendingRoom != -1` 时**不 Present**，与 `src/loop.c` 和 PS2/PS3 端口一致；屏幕上保留上一帧（转场黑场），等 `Runner_handlePendingRoomChange` 加载完新房间后，下一帧再绘制并 Present。否则会把旧房间的最后一帧在转场后闪一下。
- **实测（2026-10-17，Chapter 5，devkit 192.168.2.9）**：`Ran 600 frame(s) in 25312.79 ms (42.19 ms/frame)`，其中 `Steady-state: 479 frame(s) in 15991.75 ms (33.39 ms/frame)` ≈ 29.95 fps。总平均包含首次房间/纹理加载（约 5 s），不应与稳态混淆。旧的 17.6 ms/帧是**未节流的 60 Hz 路径**，已不可比。

---

## 8. 未决问题与停止条件

| 问题 | 处理 |
| --- | --- |
| EDRAM 分配与 predicated tiling | **已实现并验证**：RT 按需创建 + LRU 缓存，单张 full-size RT 放不下时退化为 ≤15 tiles 的 predicated tiling；2048×2048 / 4 tiles 真机 `TilingSelfTest: PASS`。仍未覆盖：>15 tiles 的 RT、tiled surface 跨绑定持久内容、tiled surface 的 `surface_set_pixels` |
| 用户脚本 shader（`hlsl9_*`）映射到 Xenon | **已定方案并实施（步骤 4.3）**：主机侧 `fxc` 离线预编译 → `data.x360shaders` sidecar → 运行时加载；机制已真机验证，见第 4.2 节。剩余未复现项：真实标题 SHDR 端到端重编译、`f_array` 矩阵打包、多 stage 覆盖 |
| 用户 shader sidecar 的打包/部署 | 待确认；文件名固定 `data.x360shaders`，未纳入打包自动化 |
| Chapter 5 的 66 个 `hlsl9_*` 全部是 declarations-only | 2026-10-17 实测：`precompile_shaders.py --datawin` 对 66 个 shader 全部判定无入口点，无法生成 sidecar；运行时逐 shader 回落内置 shader（日志可见 `shd_prophecy_legend` 等 warning）。需等 GMS 提供可编译源码或实现 GLSL/HLSL11 转换后再解决 |
| `f_array` 的 `float3[]` 打包 | 仍按 `ceil(count/4)` 上传，未按 fxc 的 `regCount`/类型展开；`float4x4` 路径正常。待有真实用例时修 |
| 平铺的可见区域裁剪 | `drawSpriteTiled` 未移植 GL 的 AABB 裁剪；大房间 tiled 背景会生成离屏 quad。性能项，规划步骤 4.4 |
| DXT 纹理画质 | 已实现 DXT1/DXT5 + `data.x360tex` sidecar，真机验证 logo、intro、存档菜单；像素画/文字/软 alpha 的块伪影需逐场景评审，未做全量 83 页逐像素对比 |
| 纹理 sidecar 生成 | `--texture-autogen` 首次启动逐页生成并写 `complete=1`，第二次启动复用（真机 83 页约 2.5 分钟）；`--dump-textures` 仍可边加载边写。零售 `game:\` 只读时回退运行时压缩。主机侧独立工具未做；sidecar 未做磁盘压缩（完整 83 页约 197 MB） |
| 调色板/低 bpp 纹理（GMS 1.x） | 未实现；PS3 式 8bpp 索引 + CLUT 方案评估过，因线性过滤/用户 shader/批处理兼容性未采用；DXT 已覆盖当前目标 |
| `d3dx9` 运行时 API | 未依赖（自建常量布局，仅用 `fxc` 预编译与 XGraphics） |
| 性能与内存峰值 | 已记录 Chapter 5 稳态 33.39 ms/帧（30 fps 节流后）；规划步骤 4.4 在真机上量测后再优化 |
| 标题启动/退出的 1280×720 认证警告 | 待定位（疑与应用 surface 作为 RT）；启动和退出各出现一次 |

**通用停止条件**：任何需要修改 `src/renderer.h`、`src/gl*`、`src/runner.c`、根 `CMakeLists.txt` 或公共 CI 的方案，一律停止并另行审批。本阶段未触发。

---

## 9. 证据索引

- 实施进度与真机证据：`docs/xbox360-progress.md` 第 8、11、14、16 节。
- 2026-10-17 Chapter 5 运行日志：`build/ch5_600_listen.log`（600 帧，稳态 33.39 ms/帧）、`build/ch5_listen.log`（启动/房间切换）、`build/ch5_selftest_listen.log`（renderer selftest PASS）。截图：`build/ch5_play0.png`、`build/ch5_capture.png`、`build/ch5_text_crop.png`（像素对齐放大裁切）。
- 2026-10-17 DXT 纹理压缩：运行时/sidecar 日志见第 5.1 节；截图 `build/dxt_play0.png`、`build/dxt_play1.png`、`build/dxt_menu_sidecar2.png`；完整记录见 `docs/xbox360-progress.md` 第 16 节。实现：`src/xbox360/xbox360_textures.{c,h}`；生成开关 `scripts/xbox360/build.sh --dump-textures`（`BS_X360_DUMP_TEXTURES`）。
- 2026-10-17 EDRAM/白屏修复：`EdramStress` 自检与 `room_dw_fcastle_orange_gauntlet` 复测见 `docs/xbox360-progress.md` 第 17 节；截图 `build/edram_walk1.png`、`build/edram_walk2.png`。
- 2026-10-17 predicated tiling / LRU RT 缓存：`TilingSelfTest: PASS`（2048×2048，1024×1024 RT，4 tiles），`EdramStress: PASS`；实现见 `x360SetupTiling`、`x360BeginTilingIfNeeded`、`x360ResolveCurrentSurface`、`x360CreateRTWithEviction`；overlay 统计 `RTs: n (evict n)`。详见 `docs/xbox360-progress.md` 第 18 节。
- 能力与依赖清单：`docs/xbox360-capability-inventory.md` B.2。
- 构建与探针用法：`scripts/xbox360/README.md`。
- 端口渲染器实现：`src/xbox360/xbox360_renderer.{c,h}`；设备：`src/xbox360/xbox360_d3d_device.{c,h}`；着色器：`src/xbox360/shaders/renderer_{vs,ps}.hlsl`；着色器编译：`scripts/xbox360/compile_shaders.py`。
- 用户 shader 离线预编译：工具 `scripts/xbox360/precompile_shaders.py`；sidecar 运行时 `src/xbox360/xbox360_shadersets.{c,h}`；自检 shader `src/xbox360/shaders/selftest/selftest.{v,f}.hlsl`（见第 4.2 节）。
- 渲染器自检：`src/xbox360/main.c` 的 `bs_x360_rendererSelftest`（`-DBS_X360_RENDERER_SELFTEST`）。
- 用户 shader 自检：`X360Renderer_runShaderSelftest`（`src/xbox360/xbox360_renderer.c`，`-DBS_X360_SHADER_SELFTEST` / `build.sh --shader-selftest`）。
- 探针实现：`src/xbox360/main_d3d_probe.c`、`src/xbox360/shaders/probe_{vs,ps}.hlsl`。
- XDK 文档（快照 21256.17）：`fixed_function_pipeline.htm`、`_dx_Direct3D9_For_Xenon.htm`、`_dx_IDirect3D9_createdevice_graphics.htm`、`_dx_IDirect3DDevice9_present_graphics.htm`、`_dx_d3drenderstatetype_graphics.htm`（`D3DRS_HALFPIXELOFFSET`）、`synchronous_and_asynchronous_swaps_on_xbox_360.htm`、`Sleep.htm`、`_dx_IDirect3DDevice9_setviewport_graphics.htm`、`_dx_IDirect3DDevice9_settexture_graphics.htm`、`_dx_IDirect3DDevice9_setsamplerstate_graphics.htm`、`_dx_IDirect3DDevice9_createtexture_graphics.htm`、`_dx_IDirect3DDevice9_createrendertarget_graphics.htm`、`_dx_IDirect3DDevice9_Resolve_graphics.htm`、`_dx_IDirect3DDevice9_createvertexdeclaration_graphics.htm`、`_dx_IDirect3DDevice9_drawprimitiveup_graphics.htm`、`_dx_IDirect3DTexture9_lockrect_graphics.htm`、`XGTileTextureLevel.htm`、`XGUntileTextureLevel.htm`、`XGGetGpuFormat.htm`、`XGIsBorderTexture.htm`、`XGIsPackedTexture.htm`、`xenon_texture_storage.htm`、`texture_and_render_target_layouts.htm`、`predicated_tiling.htm`、`xenon_basic_predicated_tiling.htm`、`graphics_tools_fxc.htm`。

---

## 10. 2026-10-17 修复记录与真机复测

### 10.1 2D 像素对齐（已实现，真机截图待逐像素对比）

**现象**：画面整体看起来偏移约半个像素，点采样时精灵/文字边缘会采到相邻纹素，出现接缝或抖动。

**原因**：Xenon D3D9 默认 `D3DRS_HALFPIXELOFFSET = FALSE`，即 D3D1–9 的像素中心约定（像素中心在整数坐标）。GameMaker 的 `Matrix4f_viewProjection` 按 OpenGL/D3D10 约定（像素中心在半整数坐标）编写；同一个 quad 在默认约定下会在纹素边界上采样。

**修复**：`x360Init` 设置 `D3DRS_HALFPIXELOFFSET = TRUE`，由硬件对每个顶点施加等效 `(-0.5, -0.5)` 的 post-viewport 偏移，恢复 GL/D3D10 约定。依据：`_dx_d3drenderstatetype_graphics.htm`。修复后的 Chapter 5 文字放大裁切见 `build/ch5_text_crop.png`（2× 最近邻放大，笔画边缘干净，没有半像素错位）。

**注意**：应用 surface 是 640×480，最终合成到 960×540（1.5×）再放进 1280×720 的字母框；非整数缩放本身会让部分像素变成 1 px、部分 2 px。这是 GameMaker 在 1280×720 窗口下的既有行为，不是半像素偏移；若要完全等宽像素，需要改变应用 surface 的缩放策略（未做）。

### 10.2 帧率限制（已实现，真机稳态验证）

**现象**：DELTARUNE 正常锁 30 fps，端口里所有操作约快一倍。

**原因**：`Present` 同步到 60 Hz VBlank（`synchronous_and_asynchronous_swaps_on_xbox_360.htm`），而 `main()` 没有按房间速度节流；同时 `runner->deltaTime` 从未赋值，`delta_time` 为 0。

**修复**：`main()` 每帧开始用 `nowNanos()` 计算 `runner->deltaTime`；帧末按 `Runner_getEffectiveGameSpeed(runner)`（房间速度 / `game_set_speed` / data.win 默认）用 `Sleep` 等到 `frameStart + 1e9/speed`。`Sleep` 只有毫秒粒度，故先睡到 deadline 前 1 ms，再用 `Sleep(0)` 让出剩余时间。依据：`Sleep.htm`、`synchronous_and_asynchronous_swaps_on_xbox_360.htm`。

**真机证据**（Chapter 5，devkit 192.168.2.9，`--frames=600`）：

```
Ran 600 frame(s) in 25312.79 ms (42.19 ms/frame)
Steady-state: 479 frame(s) in 15991.75 ms (33.39 ms/frame)
```

稳态 33.39 ms/帧 ≈ 29.95 fps。总平均包含首次房间/纹理加载（约 5 s），不能当作稳态帧时间。该测量在保存/传说画面完成；战斗或高负载场景尚未复测。

### 10.3 房间切换（已实现，逐帧视觉复测待做）

**现象**：黑场转场后，旧房间的最后一帧会闪一下，然后才切到新房间。

**原因**：`main()` 在 `Runner_handlePendingRoomChange` 之前无条件 Present。房间切换挂起时，刚绘制的是旧房间的最后一帧；Present 把它送到屏幕，下一帧才加载新房间。

**修复**：仅当 `runner->pendingRoom == -1` 时调用 `X360Renderer_present()`，然后执行 `Runner_handlePendingRoomChange()`。这与 `src/loop.c`、PS2/PS3 端口的顺序一致：转场期间保留上一张已呈现的帧（黑场），新房间加载完成后的下一帧再 Present。依据：`_dx_IDirect3DDevice9_present_graphics.htm`（Present 后 EDRAM 内容不保留，不能靠重复 Present 保内容）。

**验证**：Chapter 5 启动、`room_legend -> PLACE_LOGO` 等房间切换在真机跑通（`build/ch5_600_listen.log`）；“转场期间不闪旧帧”需要逐帧截图/录像确认，尚未做。

### 10.4 同批修正的已知缺陷

| 缺陷 | 修复 |
| --- | --- |
| `drawTiledPart` UV 用 `targetX/Y + srcX/Y` | 改为 `sourceX/Y + srcX/Y`（与 `drawSpritePart` 和 `Renderer_nineSliceAdjustForTiledPart` 的约定一致） |
| `x360ApplyBlendState` 每次内部绘制后把 `SEPARATEALPHABLENDENABLE` 重置为 FALSE | 增加 `separateAlphaBlend` 影子状态；`gpu_set_blendmode_ext` 设置后由 `x360ApplyBlendState` 保留 |
| `gpuSetTexFilter` 只设 sampler 0 | 遍历 stage 0..7 设置 MIN/MAG/MIP/ADDRESS |
| `gm_Matrices` 固定上传 20 个寄存器 | 使用 fxc 报告的 `regCount`（上限 20） |
| `textureSetStage(0, 0)` 绑 NULL | stage 0 也回退到 1×1 白纹理 |
| `x360RestoreSurface` 不设视口/不关剪刀 | 恢复前显式设全 surface 视口并禁用 scissor |
| `backBuffer` 未 Release | `x360Destroy` 释放 |

### 10.5 证据缺口与未复测项

- **渲染器 surface/EDRAM 自检**：2026-10-17 已在 Chapter 5 上重跑 `--renderer-selftest`，`PASS`；日志 `build/ch5_selftest_listen.log`。§6 引用的 1093 是 2026-10-16 的 ch1 记录；Chapter 5 的 `createSpriteFromSurface` 返回 8536（索引不同，属正常）。
- **音频自检**：`--audio-selftest` 的 PASS 日志同样未归档。
- **像素对齐**：已按 SDK 文档修复，但还没有“修复前/修复后”同场景逐像素对比图。
- **房间切换**：代码顺序已对齐公共 runner，但没有转场逐帧录像确认。
- **Chapter 5 用户 shader**：66 个 `hlsl9_*` 全部是 declarations-only，无法生成 sidecar；`shd_prophecy_legend` 等已回落内置 shader（日志 warning）。这不是本次修复能解决的，属于 GMS 2.3+ 源码形态限制。
- **Chapter 5 的 shader 替换扩展**：日志显示 `Runner: Extension init script 'shader_replace_simple_init' not found, skipping`，标题目录里的 `shader_replace_simple_x64.dll` 未被端口加载；依赖该扩展的画面效果未验证。
- **旧帧时间数据**：`docs/xbox360-progress.md` 里的 17.25/17.63 ms/帧是未节流的 60 Hz 路径，与本次 33.39 ms/帧不可直接比较。

---

## 11. 端口自有加载界面与调试叠加层（2026-10-17）

本节记录 PS2 端口同款的加载界面和调试叠加层在 360 上的实现。它们**不属于公共 `RendererVtable`**，只使用端口已有的 `drawTextUI`、`drawRectangle`、`clearScreen` 和 `X360Renderer_present`。

### 11.1 为什么需要拆分 init

加载界面必须覆盖 `data.win` 解析阶段，而解析发生在 `Runner_create` 之前。原 `x360Init` 同时创建设备和纹理页表，无法在解析前调用。现在拆成：

- `x360InitDevice`：D3D 设备、back buffer、顶点声明、内置 VS/PS、1×1 白纹理、批处理缓冲、默认 render state、identity 矩阵。幂等。
- `x360InitData`：`dataWin` 指针、`data.x360shaders` sidecar、纹理页表、按 `renderer->texFilter` 重新应用 sampler filter。
- 端口入口在 `DataWin_parse` 前调用 `X360Renderer_initDevice`；`Runner_create` 的 vtable `init` 再完成 `x360InitData`。

### 11.2 叠加层绘制入口

`X360Renderer_beginOverlay` / `X360Renderer_endOverlay`（端口 API，不在公共 vtable）：

- begin：flush → 绑定 back buffer → 全屏 viewport → 关剪刀 → 1280×720 正交投影 → 强制内置 VS/PS → alpha test 关、颜色写全开、fog 0 → 打开 `SRC_ALPHA/INV_SRC_ALPHA` 混合 → sampler 0..7 临时 LINEAR。
- end：flush → 恢复游戏 blend / alpha test / color write / 当前 shader / 投影 / fog → 恢复 sampler filter。
- 约定：overlay 是 `X360Renderer_present` 前最后一次绘制；end 不恢复原 render target。

这样即使用户 shader 正在生效，叠加层也不会被它的矩阵/采样逻辑影响。

### 11.3 加载界面

`src/xbox360/xbox360_overlay.c` 的 `X360Overlay_statusScreenCallback` 挂到 `DataWinParserOptions.progressCallback`：

- 标题 `Butterscotch360-Next`、游戏名、状态文字、左下角构建信息。
- 进度条、百分比、`Loading XXXX... (n/total)`、`Memory: used/total MB`。
- 左上角按 chunk 记录 `sounds / sprites / backgrounds / fonts / objects / rooms / code entries / textures`。
- 错误界面红色文字，停留 5 秒后退出。

### 11.4 调试叠加层

- 控制器 0 的 **LB+RB** 边沿切换，`X360Input_consumeOverlayToggle()` 消费，不进入游戏按键状态。
- 三档：stats → stats+profiler → off；默认关闭，`BS_X360_DEBUG_OVERLAY` 构建时默认打开。
- 字段：Room、Tick/Step/Draw/Audio、Free（`GlobalMemoryStatus`，标注 approx）、Room Speed、Textures loaded/total、Surfaces allocated/total、Instances、Structs。
- 8 方向黑色描边；profiler 档位在未启用 `ENABLE_VM_GML_PROFILER` 时显示 disabled 提示。

### 11.5 状态与证据

**已真机验证**（Chapter 5，devkit `192.168.2.9`）：加载界面覆盖解析、进度条/chunk 统计/内存、各初始化状态界面、叠加层绘制与三档切换、关闭后画面干净。**LB+RB 的实体手柄同时按下路径尚未复测**（XBDM 注入不能稳定保证两键同时按下）。截图与日志索引见 `docs/xbox360-progress.md` 第 15 节。

**限制**：叠加层与游戏自带左上角文字会重叠（仅靠描边保证可读）；profiler 启用路径未验证；`GlobalMemoryStatus` 不是精确动态内存计数；叠加层自身开销未单独量测；每个 chunk 都 Present，未做节流。
