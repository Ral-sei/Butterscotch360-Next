# Xbox 360 平台层：实施进度记录

本文记录 `docs/xbox360-platform-plan.md` 各步骤的实际进展与证据。只增不改；公共代码保护路径始终未改动。

## 状态总览

| 步骤 | 内容 | 状态 | 证据/产物 |
| --- | --- | --- | --- |
| 0 | 固定约束与基线 | 部分完成 | 公共路径零改动已核（`git status` 仅新增）；上游基线提交 `e15b765`；真机/授权项仍待确认 |
| 1 | 能力与依赖清单 | **完成** | `docs/xbox360-capability-inventory.md` |
| 2 | 确认 OXDK 工具链与最小构建 | **完成（空 XEX 已产出并通过校验）** | 本文第 2 节 |
| 2.4 | 无图形/无音频的可启动运行时 | **完成（交叉编译 + 真机验证）** | 本文第 3 节 |
| 3 | 平台后端（入口/文件/输入/图形/音频/运行时协作） | 进行中：3.2 文件系统、3.3 输入、3.4 图形（`RendererVtable`）、3.5 音频（XAudio2）均已真机验证；剩余图形子项见 3.4 | 本文第 6、7、11、12、13 节 |
| 3.2 | 文件系统 | **完成（交叉编译 + 真机验证）** | 本文第 6 节 |
| 3.3 | 输入（XInput） | **完成（交叉编译 + 真机验证）** | 本文第 7 节 |
| 3.4 | 图形（D3D9） | **基本完成并真机验证**：`RendererVtable` 已实现（`src/xbox360/xbox360_renderer.{c,h}`），真实 DELTARUNE 画面（文字/精灵/背景/字母框）与 surface/EDRAM 自检均已在真机通过；用户脚本 shader 离线预编译（步骤 4.3）已实施并真机验证机制（第 12 节）；剩余：predicated tiling、性能量测 | 本文第 8、11、12 节；设计见 `docs/xbox360-renderer-design.md` |
| 3.5 | 音频（XAudio2） | **完成（交叉编译 + 真机验证）**：`src/xbox360/xbox360_audio.{c,h}` 实现 `AudioSystemVtable`；内嵌 AUDO（RIFF WAVE PCM）与外部/流式 Ogg（stb_vorbis）解码后交给 XAudio2；自检 PASS | 本文第 13 节 |
| 4 | 资源、内容与性能 | 部分开始（4.3 用户脚本 shader 离线预编译已完成，见第 12 节）；资源清单/内存峰值/打包未开始 | 第 12 节 |
| 5 | 测试、打包和验收 | 未开始 | — |
| 6 | 交付与上游提案 | 未开始 | — |

## 1. 已确认的构建环境（本机，机器本地事实）

| 项 | 值 | 如何确认 |
| --- | --- | --- |
| 宿主 | WSL2 Debian（`wsl -e sh`，`uid=0`） | `wsl.exe --list --verbose` |
| OXDK | `/mnt/h/OXDK`（含预构建 `xbox360/tools/cxex/cxex`） | `ls /mnt/h/OXDK/oxdk.mk` |
| 补丁 LLVM | `/root/oxdk-llvm/build`，clang 22.1.8 | `clang --version` |
| Xenon ABI | 可用 | 编译探测通过（`+xenon-abi`） |
| Xbox 360 XDK | `/root/xdk360`（含 `lib/xbox/xboxkrnl.lib`、`include/xbox`、`TechPreview/Jul12Compiler`） | 文件存在性 |
| 仓库（WSL 视角） | `/mnt/h/Butterscotch` | — |

注意：`llvm-objcopy` / `llvm-ar` / `llvm-nm` 不在默认 PATH，位于 `$OXDK_LLVM/bin`；`env.sh` 负责把它们加进 PATH。

## 2. 步骤 2：最小构建（空 XEX）—— 完成

### 2.1 新增的端口资产（全部为新增文件，零公共改动）

| 文件 | 职责 |
| --- | --- |
| `scripts/xbox360/env.sh` | 解析并校验 `OXDK_DIR` / `OXDK_LLVM` / `XDK_DIR`，检查宿主工具，把 `$OXDK_LLVM/bin` 加入 PATH。只从环境或 OXDK 文档位置发现路径，不写死其他项目路径。 |
| `scripts/xbox360/build.sh` | 唯一构建入口；`--clean` / `--build-type=` / `--backend=cmake\|make` / `-j N` / `--help`。 |
| `scripts/xbox360/toolchain.cmake` | 端口自有 CMake 工具链：钉死 OXDK clang、`powerpc-unknown-none-elf` + Xenon ABI、`CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY`、自定义 `CMAKE_C/CXX_LINK_EXECUTABLE` 调包装器、`CMAKE_EXECUTABLE_SUFFIX=.xex`。 |
| `scripts/xbox360/oxdklink_wrapper.py` | 端口自有链接包装器：staging 副本上清理 `.deplibs`，再调 OXDK `oxdklink.py`。 |
| `scripts/xbox360/README.md` | 用法、环境要求、设计说明。 |
| `port/xbox360/CMakeLists.txt` | 端口 CMake 工程，显式列举源文件，不 `add_subdirectory` 根工程。 |
| `port/xbox360/Makefile` | OXDK 原生最小构建（直接 `include $(OXDK_DIR)/oxdk.mk`）。 |
| `src/xbox360/main_minimal.c` | 最小 XEX：仅 `OutputDebugStringA` + `Sleep` + `return`，不链接任何 Butterscotch 运行时。 |

### 2.2 旗标来源（不是凭记忆重建）

`toolchain.cmake` 的 C 旗标通过对 OXDK 自身 `xbox360.mk` 求值得到（`make -f ... print`），与 OXDK 默认 C 路径一致：

```
-target powerpc-unknown-none-elf  -c  -ffreestanding  -fno-builtin
-fms-extensions  -fms-compatibility  -fms-compatibility-version=14
-fshort-wchar  -fno-autolink  -fdelayed-template-parsing  -Wno-invalid-token-paste
-D_WIN32 -D_M_PPCBE -D_M_PPC -D_PPC_ -D_XBOX -D_XENON -DXBOX
-D_MSC_VER=1400 -D_MSC_FULL_VER=140050727  -D_SIZE_T_DEFINED  -D_W64=
-Xclang -target-feature -Xclang +xenon-abi
-I <OXDK>/xbox360/oxdk360/header-shim
-I <XDK>/include/xbox  -I <XDK>/TechPreview/Jul12Compiler/include/xbox
```

XDK 库列表首阶段为 `xapilib,libcMT`（OXDK 默认）。

### 2.3 验证结果（实测）

在 WSL 中执行（`OXDK_DIR=/mnt/h/OXDK`、`XDK_DIR=/root/xdk360`、`OXDK_LLVM=/root/oxdk-llvm/build`）：

| 检查 | 命令 | 结果 |
| --- | --- | --- |
| 脚本语法 | `sh -n env.sh build.sh` | 通过 |
| 包装器语法 | `python -m py_compile oxdklink_wrapper.py` | 通过 |
| Make 后端 | `build.sh --backend=make --clean -j4` | `build/xbox360-make/default.xex`，331776 字节 |
| CMake 后端 | `build.sh --backend=cmake --clean -j4` | `build/xbox360/default.xex`，266240 字节 |
| XEX 魔数 | `head -c4 \| od` | 两者均为 `XEX2` |
| XEX 结构校验 | OXDK `xexutil/xexcheck.py` | 两者均 **PASS**（页描述符、导入表、header digest、导入摘要链全部通过） |
| 增量构建 | 再次 `build.sh`（无 `--clean`） | `ninja: no work to do` / `Nothing to be done for 'all'`（staging 未破坏时间戳） |
| 环境错误路径 | 取消 `OXDK_DIR` 后 source `env.sh` | 明确报错并返回 1 |
| 帮助信息 | `env -u OXDK_DIR ... build.sh --help` | 不依赖工具链即可打印 |

CMake 后端链接日志（节选）：

```
[2/2] Linking C executable default.xex
cxex: default.xex
  base 0x82000000  entry 0x820217b8  image 0x40000 bytes
  2 libraries, payload at 0x1000, total 266240 bytes
  65 imports (60 code, 5 data)
```

### 2.4 实施中遇到并修正的问题（供后续参考）

1. **CMake 未注入 target triple。** `CMAKE_C_COMPILER_TARGET` 不会出现在编译规则里，导致按宿主 x86_64 编译、XDK 头报 `__stdcall`/内建函数冲突。修正：把 `-target powerpc-unknown-none-elf` 显式写进 `CMAKE_C_FLAGS_INIT`。
2. **头文件顺序。** `target_include_directories` 会把 XDK 头排在 `header-shim` 之前，与 OXDK 期望相反。修正：包含目录只通过旗标提供，顺序为 `header-shim` → XDK。
3. **`.xex` 后缀（纯命名对齐，非缺陷）。** OXDK 的 make 路径产物是 `default.xex`；CMake 在 Generic 系统下默认不追加后缀，产物是 `default`（这是预期行为）。为与 make 后端及 OXDK 部署文档一致，端口对目标显式设置 `SUFFIX ".xex"`；去掉它功能不受影响。
4. **Make 默认目标被抢占。** 端口 Makefile 的“拷贝源文件”规则排在 `include oxdk.mk` 之前，成了默认目标，导致 `make` 只做拷贝。修正：把该规则移到 `include` 之后。

### 2.5 未验证 / 限制

- **真机未验证**：本机无开发机/调试环境，只完成交叉编译与静态 XEX 校验。**不得**据此宣称已在目标机运行。
- **`.deplibs` 清理**：最小样例未触发 `#pragma comment(lib)`，故未产生 `.deplibs` 段，清理步骤为幂等空操作。机制已接入，待接入带该 pragma 的 XDK 头/库时再行验证其必要性。
- 最小 XEX 未使用 `XEX_TITLE_ID`（仍为 `0x0`）。真正需要存档时必须在构建里设为非零值（见能力清单 B.4）。

## 3. 步骤 2.4：无图形/无音频运行时（交叉编译 + 真机验证）

### 3.1 结论

端口已能把**完整的 Butterscotch 运行时**交叉编译并链接为 XEX2：`NoopRenderer` + `NoopAudioSystem` + `NoopFileSystem`，入口 `src/xbox360/main.c` 会解析 `data.win`、创建 VM/Runner、跑 120 帧后干净退出。

| 目标 | 产物 | 大小 | 校验 |
| --- | --- | --- | --- |
| 运行时（默认） | `build/xbox360/default.xex` | 2 625 536 字节 | `xexcheck.py` **PASS** |
| 最小探针（`--minimal`） | `build/xbox360/default.xex` | 266 240 字节 | `xexcheck.py` **PASS** |
| Make 后端探针 | `build/xbox360-make/default.xex` | 331 776 字节 | `xexcheck.py` **PASS** |

运行时链接统计：89 个目标 + 2 个 XDK 库，80 个导入（74 code / 6 data）。增量重建为 no-op。

### 3.1.1 真机验证（devkit 192.168.2.9，2026-10-15）

运行时 XEX 已部署到 `DEVKIT:\chapter1_windows\default.xex`（另存，未覆盖已有的 `Butterscotch.xex`），经 `xbreboot` 启动，并用 KDNet（`i386kd`）抓取调试输出。结果：

| 阶段 | 日志证据 |
| --- | --- |
| 入口 | `Butterscotch Xbox 360: runtime boot (no graphics, no audio)` |
| 读取游戏 | `Loading game:\data.win...` → `Loaded "DELTARUNE Chapter 1" (WAD 17, GM 2022.8.0.0)` |
| VM | `VM: Initialized with 1985 functions mapped` |
| 渲染器 | `No-op renderer initialized` |
| 首个房间 | `Runner: Room loaded: ROOM_INITIALIZE (room 0) with 7 instances` → `First room: ROOM_INITIALIZE` |
| 房间切换 | `Room changed: ROOM_INITIALIZE (room 0) -> PLACE_CONTACT (room 1)`（8 instances） |
| 帧循环 | `Ran 120 frame(s) in 125.31 ms (1.04 ms/frame)` |
| 退出 | `Butterscotch Xbox 360: runtime shutdown complete` → `[XAPI RETURN VALUE] 0` |

即：**真实 13 MB 的 DELTARUNE Chapter 1 数据被解析、VM 执行了全局初始化脚本与房间逻辑、帧循环正常、退出干净（返回值 0）**。控制台随后正常回到 dashboard。

说明与限制：
- `game:\data.win` 在本机 devkit 上解析到启动目录，故 `main.c` 的首选路径即可命中。
- `Unknown chunk: FEAT/TXTR/AUDO` 是预期输出：本阶段 `parseTxtr`/`parseAudo` 为 false（无渲染器/音频）。
- 仍**没有图形与音频**，因此**不能**据此宣称画面、输入、音频或性能已达标；那属于步骤 3。

### 3.2 新增的端口资产

| 文件 | 职责 |
| --- | --- |
| `src/xbox360/main.c` | 端口自有入口：查 `data.win` → `DataWin_parse` → `VM_create` → `NoopRenderer`/`NoopAudioSystem`/`NoopFileSystem` → `Runner_create` → `Runner_initFirstRoom` → 跑 `BS_X360_TEST_FRAMES`（默认 120）帧 → 释放。 |
| `src/xbox360/xbox360_time.c` | 端口自有 `nowNanos()`（`QueryPerformanceCounter`）。用于绕开能力清单缺口 G1：不改 `src/gettime.c`，而是端口源清单排除它。 |
| `src/xbox360/xbox360_log.c` | `platformLog`：格式化后走 `OutputDebugStringA`（XBDM `listen` 可见）。 |
| `src/xbox360/xbox360_compat.c` | `getenv` 桩（XDK 声明但不实现）。 |
| `src/xbox360/stb_impl.c` | STB 单一定义 TU（PNG/JPEG/GIF + write）。 |
| `port/xbox360/shim/windows.h` | 端口自有 `windows.h`：`#include <xtl.h>` + 把 Xenon 缺失的 `CreateFileMappingA`/`MapViewOfFile`/`UnmapViewOfFile`/`GetModuleHandleW` 降级为返回 NULL 的桩（令 `data_win.c` 走非 mmap 回退、`utils.h` 的 `dropMappedRange` 变 no-op）。 |
| `port/xbox360/shim/math.h` | 端口自有 `math.h`：`#include_next <math.h>` + 补 XDK 缺的 `remainderf`（box2d 需要）。 |

`port/xbox360/CMakeLists.txt` 现显式枚举运行时源清单：30 个 `src/*.c`（排除 `gettime.c`/`loop.c`/`overlay_file_system.c`）、`src/debug_font`、`src/physics`、`src/video/null_video.c`、端口源、vendor（miniz/bzip2/md5/base64/sha1/box2d），外加 OXDK 的 `oxdk360/builtins/builtins.c`。

### 3.3 使公共代码在 Xenon 上成立的兼容措施

必须定义 `_WIN32`（XDK CRT 头依赖它），于是公共代码走 Windows 分支。端口通过**新增**文件与编译期开关弥合差异，未改任何公共源码：

- XDK 缺 `strcasecmp`/`strncasecmp`/`strtok_r`/`snprintf`/`isnan`/`isinf`/`roundf`/`lround`/`log2`：用公共兼容头已有的开关 `NO_STRCASECMP`/`NO_STRTOK_R`/`NO_STRINGS_H`/`NO_ISNAN`/`NO_ISINF`/`NO_ROUNDF`/`NO_LROUND`/`NO_LOG2`，并把 `snprintf` 重定向到 `_snprintf`。
- XDK 无 `windows.h`/`strings.h`：端口 shim + `NO_STRINGS_H`。
- Xenon 无 `CreateFileMappingA`/`MapViewOfFile`/`UnmapViewOfFile`/`GetModuleHandleW`：shim 桩降级。
- box2d 需 `remainderf`：`math.h` shim 补齐。
- 64 位算术内建（`__divdi3` 等）：链接 OXDK 的 `builtins.c`。
- `getenv`：端口桩。

### 3.4 本次暴露的已知缺口（进入步骤 3）

1. **文件系统**：`src/overlay_file_system.c` 依赖宿主 glibc 的 `<dirent.h>`，在 Xenon 上无法链接，故端口暂用 `NoopFileSystem`。步骤 3.2 需实现端口自有 FileSystem（只读 `game:\` + 可写 XContent 挂载盘），并用 XDK 的 `FindFirstFile`/`FindNextFile`/`FindClose` 实现 `listDirectory`。
2. **`data.win` 路径**：`main.c` 目前尝试 `game:\data.win`、`data.win`、`D:\data.win`；部署布局确定后需固化。
3. **G1 的落地方式**：当前以“端口排除 `src/gettime.c` + 自有 `nowNanos()`”实现（能力清单中的方案 2）。这是不改公共文件的可行路径，但仍需维护者认可（见第 6 节）。



## 4. 步骤 1 交付物

见 `docs/xbox360-capability-inventory.md`：7 类平台能力逐项记录「现有接口 / 端口责任文件 / 依赖库 / 失败降级 / 验证方式 / 依据与确认度」，并列出公共钩子缺口 G1–G5。

## 5. 公共边界核对

`git status --porcelain` 只显示新增：`docs/`、`port/`、`scripts/`、`src/xbox360/`。未改动任何 `src/*.c`、公共头、根 `CMakeLists.txt`、公共 CI 或 vcpkg 清单。根 `CMakeLists.txt` 的 `src/*.c` 为非递归 glob，`src/xbox360/` 不会被现有目标带入。

## 6. 步骤 3.2：文件系统（真机验证）

### 6.1 结论

端口已实现自有文件系统后端 `src/xbox360/xbox360_file_system.c`，并在 devkit 上验证读文件与目录枚举。运行时会用它替换 `NoopFileSystem`。

| 检查 | 真机日志 |
| --- | --- |
| 后端建立 | `FileSystem: bundle=game: save=game:` |
| 读文件 | `FileSystem probe: readFileText("options.ini") -> ok` |
| 列目录 | `FileSystem probe: listDirectory(".") -> 15 entries` |
| 帧循环 | `Ran 120 frame(s) in 126.81 ms (1.06 ms/frame)` |
| 退出 | `runtime shutdown complete` |

### 6.2 关键发现：XDK CRT 只接受反斜杠（推翻一处能力清单假设）

能力清单 B.4 曾记录“可复用公共 `OverlayFileSystem`”。真机实测否定了它：

```
FileSystem probe: raw fopen backslash=ok forward=fail
```

`fopen("game:\\options.ini")` 成功，`fopen("game:/options.ini")` 失败；而公共 `OverlayFileSystem` 的 `normalizePath` 把**所有**路径统一规范成正斜杠，因此在 Xbox 360 上完全无法访问存储。故端口改为**反斜杠原生**的自有实现，保留同样的两区语义（读：save 优先、回退 bundle；写/删：仅 save）。

同时修正一处拼接缺陷：`listDirectory(".")` 原先会拼出 `game:\.\*`（`FindFirstFileA` 不接受），已改为对 `.`/空路径直接使用区域根。

### 6.3 端口责任文件

`src/xbox360/xbox360_file_system.{c,h}`：完整实现 `FileSystemVtable`（路径解析、文本/二进制读写、流式 `file_bin_*` 句柄、目录增删查、`listDirectory` 用 `FindFirstFileA`/`FindNextFileA`/`FindClose`）。

### 6.4 尚未完成（存档策略）

当前 `bundle` 与 `save` 都指向 `data.win` 所在目录（devkit 上可写，零售机不可写），并由 `BS_X360_SAVE_ROOT` 编译期覆盖。**真正的存档策略（XContent 内容包、用户选择、配额、`XContentFlush` 提交）尚未实现**，符合规划“存储/用户资料策略不清则不实现写存档”的停止条件。零售机上的写操作会静默失败（`fopen` 返回 null → vtable 返回 false），不会被当作成功。

## 7. 步骤 3.3：输入（真机验证）

### 7.1 结论

端口已实现 `src/xbox360/xbox360_input.{c,h}`：每帧轮询 4 个 XInput 用户槽，同时驱动两条公共输入契约，未改任何公共映射代码。

- **游戏手柄契约**（主）：填充 `RunnerGamepadState` 槽位，供 `gamepad_button_check` / `gamepad_axis_value` 等 GML 内置函数使用。按钮索引与 SDL2/Switch 后端一致；触发器按 `triggerThreshold` 计入 `buttonDown` 并写入 `buttonValue`；摇杆按“+Y 向下”约定（与 SDL2/Switch 一致，故对 XInput 的 +Y 向上取反）。
- **键盘契约**（辅）：把 A/B/X/Y/Start/Back/肩键/方向键/摇杆映射到 VK 码，经公共 `RunnerKeyboard_onKeyDown/onKeyUp` 注入，使纯键盘游戏也能用手柄玩。

### 7.2 真机证据（devkit 192.168.2.9）

用技能侧 `controller.py` 注入输入（`pad A UP LEFT LX=-0.9 LY=0.9 RB`），运行时以 `--frames=40000 --input-log` 构建：

| 事件 | 日志 |
| --- | --- |
| 手柄接入 | `Input: controller 0 connected` |
| A → VK 'Z'(90) | `Input: pad 0 A down -> key 90` |
| 方向键上 → VK_UP(38) | `Input: pad 0 Up down -> key 38` |
| 方向键左 → VK_LEFT(37) | `Input: pad 0 Left down -> key 37` |
| RB → VK_PAGEUP(33) | `Input: pad 0 RB down -> key 33` |
| 自动化释放后断开 | `Input: controller 0 disconnected` |
| 帧循环 | `Ran 40000 frame(s) in 40379.17 ms (1.01 ms/frame)` |

即：**按钮→按键映射、游戏手柄槽位填充、接入与断开处理均在真机生效**。

### 7.3 语义要点（对齐 XDK 文档）

- 无线手柄开机时未激活，`XInputGetState` 返回 `ERROR_DEVICE_NOT_CONNECTED`；本实现把“无状态”当作正常条件（槽位标记为未连接），不视为错误。
- **断开边沿**：释放该手柄此前持有的所有映射按键，避免方向卡住。
- **重连边沿**：用当前按键状态播种“上一帧状态”，使跨重连仍按住的按钮不产生幻影按下。
- 摇杆模拟方向键用 0.5 阈值；模拟量本身按原值交给 runner，由其按可配置死区（`GamepadSlot.deadzone`，默认 0.15）处理。

### 7.4 尚未完成

- 震动（`XInputSetState`）未实现。
- Guide 键需 `XInputGetStateEx`，未接入。
- 键盘/鼠标**明确不支持**（规划要求），不伪装。

## 8. 步骤 3.4：D3D9 图形基础探针（真机验证，渲染器未完成）

### 8.1 隔离范围

新增 `src/xbox360/xbox360_d3d_device.{c,h}`（设备创建、Present、释放的端口模块）、`src/xbox360/main_d3d_probe.c` 和 `src/xbox360/shaders/probe_{vs,ps}.hlsl`。`scripts/xbox360/build.sh --d3d-probe` 独立输出 `build/xbox360-d3d/d3d-probe.xex`，链接 XDK `d3d9.lib`；加 `--d3d-shader-probe` 时用安装在 Windows 主机的 Xbox 360 `fxc.exe` 预编译 HLSL，由端口自有 `scripts/xbox360/compile_probe_shaders.py` 将微码头文件生成到构建目录；不提交专有生成物。着色器探针另链接 OXDK 内置的 `__IsIntConst` 实现（XDK 顶点声明辅助代码会引用它）。默认运行时仍使用 `NoopRenderer`，探针没有冒充 `RendererVtable`，也不覆盖 `DEVKIT:\chapter1_windows\default.xex` 或原有 `Butterscotch.xex`。

纹理实验加入了 `D3DFMT_A8R8G8B8` 纹理、`LockRect`、`XGTileTextureLevel`、sampler 绑定和 alpha blend，链接 XDK `xgraphics.lib`。程序纹理阶段使用四象限不透明/半透明颜色，用于区分线性源数据、UV 采样和混合结果。Xbox 360 的 tiled layout 不按 PC 的线性 `row*pitch+offset` 写入，上传由 `XGTileTextureLevel` 完成。

真实资源阶段新增 `--real-resource-probe`。它在 `game:\data.win` 中只解析 `TPAG/TXTR`，选择第一张有效 TPAG（或用 `--probe-tpag=N` 指定索引），通过现有 `ImageDecoder_decodeToRgba` 解码，再将真实 RGBA 页面上传到 D3D9 纹理；失败时直接退出，不回退到程序生成测试图。该模式只给探针目标追加它需要的源（`binary_reader.c`、`data_win.c`、`log.c`、`stb_ds.c`、`image_decoder.c`、`xbox360/stb_impl.c`、`xbox360_log.c` 与 vendored `bzip2`）和 `xgraphics.lib`，默认运行时源清单不变。

### 8.2 真机证据（devkit `192.168.2.9`）

| 检查 | 结果 |
| --- | --- |
| 交叉编译/XEX 静态校验 | 使用 `OXDK_DIR=/mnt/h/OXDK XDK_DIR=/root/xdk360 OXDK_LLVM=/root/oxdk-llvm/build XEDK='/mnt/d/Microsoft Xbox 360 SDK' sh scripts/xbox360/build.sh --backend=cmake --d3d-probe --d3d-shader-probe --frames=1800 -j8` 构建；`xexcheck.py build/xbox360-d3d/d3d-probe.xex` PASS。 |
| 启动与资源 | 部署为 `DEVKIT:\chapter1_windows\d3d-probe.xex`；KDNet：`D3D9 probe: device created`、`D3D9 probe: shaders and vertex declaration created`、`D3D9 probe: first Present returned`。 |
| 截图（运行中） | `xbcapture` 得到 1280×720 位图；(640,120)、(640,360)、(640,600) 均约 (255,255,0)，即黄色着色器三角形；(320,360)、(1150,360) 均约 (48,204,48)，即三角形外绿色清屏带。之前的无着色器探针同样在画面上得到三色清屏带。这是**实拍 framebuffer 像素**，不是仅凭日志推断绘制。 |
| 模式 | 初始 640×480 在 HD 模式触发 XDK “minimum frame buffer of 1280x720” 认证警告；探针现改用 1280×720，新一次 KDNet 启动日志中无该警告。 |
| 生命周期 | 1800 帧后探针释放着色器/声明与设备并返回；KDNet 显示 title 卸载并返回 xshell。 |
| 纹理实验真机验证 | devkit 恢复后按“先启动 KDNet、再启动 XEX”的顺序运行；日志出现 `D3D9 probe: tiled texture uploaded and bound`、`first Present returned`、`finished and released device`，并卸载 `d3d-probe.xex`。 |
| 纹理截图证据 | `xbcapture` 得到 1280×720 位图。探针四边形采样点：上左 `(320,180)=(255,0,0)`、上右 `(960,180)=(0,255,0)`；下左 `(320,540)=(23,23,230)`、下右 `(960,540)=(152,152,230)`。下半部数值是蓝色/白色半透明纹理与背景混合后的结果；截图颜色分布同时包含 `(255,0,0)`、`(0,255,0)`、`(23,23,230)`、`(152,152,230)` 等预期类别。 |
| 真实 TXTR/TPAG 真机验证 | 部署真实 `H:\SteamLibrary\steamapps\common\DELTARUNE\chapter1_windows\data.win`（SHA-256：`82C2BB61B8D78CD287120F6301588FECBA34EC5A890BAC711B7A8774C760EC70`）到 `DEVKIT:\chapter1_windows\data.win`，使用 `--real-resource-probe` 启动。KDNet：`real TPAG 0 page 0 decoded 64x64`、`tiled texture uploaded and bound`、`first Present returned`、`finished and released device`。 |
| 真实资源截图证据 | `xbcapture` 得到 1280×720 位图。真实纹理四边形采样点为 `(320,180)=(68,7,221)`、`(960,180)=(212,186,221)`、`(320,540)=(46,46,212)`、`(960,540)=(75,40,213)`；扫描颜色分布出现多个真实纹理颜色类别，而非程序四象限测试色。 |
| TPAG UV 裁剪真机验证 | 探针按 `sourceX/sourceY/sourceWidth/sourceHeight` 计算 UV，并输出 `TPAG crop src=... dst=... UV=...`。索引 0 为整页 `src=(0,0 64x64) UV=(0,0)-(1,1)`。 |
| 非整页子矩形真机验证 | 使用 `--probe-tpag=1`：日志 `real TPAG 1 page 1 decoded 256x64`、`TPAG crop src=(2,2 155x14) dst=(0,0 155x14) UV=(0.0078,0.0313)-(0.6133,0.2500)`，证明探针读取了真实 TPAG 裁剪字段而非整页。 |
| 子矩形截图证据 | 该条带在透明底上为白色文字像素：四边形内扫描到 15652 个非背景像素，例如 `(558,95)=(248,250,253)`，另有 `(119,252,253)`、`(55,2,253)` 等字形颜色；四边形外仍是清屏色带。说明 155×14 区域被正确裁剪、采样并与背景做 alpha 合成。 |

### 8.3 尚未完成／下一段工作

- **这不是 3.4 验收**：`RendererVtable` 的 sprite、字体、GUI、混合、状态、用户 shader/uniform、texture page、应用 surface、用户 surface/EDRAM resolve/readback 等没有实现；默认运行时仍无游戏画面。
- 当前真实资源探针只选择第一张有效 TPAG 或 `--probe-tpag=N` 指定的单条 TPAG；已覆盖整页与非整页裁剪，但尚未处理所有 texture page 生命周期、外部 `.yytex` 资源、精灵旋转/原点、多 sprite 批处理和 Runner 的 draw 调用。随后按 Runner 帧流程实现应用 surface、EDRAM resolve、GUI 和 Present，再逐项实现并验证完整 `RendererVtable`。不得用 silent noop 冒充缺失能力。
- 参考 XDK 文档：`_dx_Direct3D9_For_Xenon.htm` / `fixed_function_pipeline.htm`（无固定功能管线）、`graphics_tools_fxc.htm`（Xbox `fxc`）、`_dx_IDirect3DDevice9_createvertexdeclaration_graphics.htm`、`_dx_IDirect3DDevice9_drawprimitiveup_graphics.htm`、`_dx_IDirect3DDevice9_present_graphics.htm`（Present 后不假设 EDRAM 保留）、`_dx_IDirect3DDevice9_Resolve_graphics.htm`。
- 完整设计（`RendererVtable` 映射、着色器/纹理/RT/EDRAM 策略与未决问题）见 `docs/xbox360-renderer-design.md`。

## 9. 阻塞项与下一步

### 阻塞项 G1（已用不改公共文件的方式绕过，仍待维护者认可）

`nowNanos()` 定义在受保护的 `src/gettime.c`，没有 Xbox 360 分支，却被 `runner.c`、`vm_builtins.c`、`profiler.c`、物理计时等公共代码使用。端口采用能力清单中的**方案 2**：从端口源清单中排除 `src/gettime.c`，由 `src/xbox360/xbox360_time.c` 提供 `nowNanos()`。这样**没有修改任何公共文件**，但重复了一份公共逻辑，仍需维护者认可；若维护者选择方案 1（在公共文件加分支），端口应改回。

### 下一步（按规划顺序）

1. 步骤 3.4 图形：`RendererVtable` 已实现并在真机验证代表性画面、surface/EDRAM 自检（第 11 节）与用户脚本 shader 离线预编译机制（第 12 节）。剩余子项：predicated tiling/EDRAM 预算、真实标题 SHDR 端到端重编译、图元/`drawTextUI` 等未被目标游戏触发的入口补测。
2. 步骤 3.5 音频：**已实施并真机验证**（第 13 节）：`XAudio2Create` + `CreateMasteringVoice`，内嵌 WAVE 与外部/流式 Ogg 解码，自检 PASS。剩余：流式音频为整段解码（非分块流），多声道/重采样未单独量测，XAudio2 输出未做主观听感验证。
3. 存档策略：接入 XContent 内容包（用户选择、配额、`XContentFlush`），把 `save` 区从“data.win 同目录”迁出。策略确认前不实现写存档。
4. 步骤 4.4 性能与内存：帧时间已量（约 17.3 ms/帧含 Present，接入音频后无回归）；仍需真机峰值内存与长时间稳定性数据。
5. 可选补强：`XInputSetState` 震动、`XInputGetStateEx` 的 Guide 键；音频侧可补 `audio_play_sound_at` 的多普勒/HRTF（当前为衰减 + 等功率声像）。
6. 真机验证：每接入一项后按第 6、7、11 节的方式在 devkit 上取证（画面、控制器、音频、帧时间）。

## 10. 真机已验证 / 尚未验证 对照

本表用于区分「有 devkit 实拍或日志证据」与「仅实现、仅编译、或完全未做」。任何一项不得从「已实现」直接推断为「已可用」。

### 10.1 已真机验证（有截图或 KDNet 证据）

| 能力 | 证据 |
| --- | --- |
| 启动/退出、帧循环、日志 | 见第 3 节；`Ran 120 frame(s)`、`runtime shutdown complete` |
| 文件系统读/列目录 | 见第 6 节；`readFileText -> ok`、`listDirectory -> 15 entries` |
| XInput 连接/按键/断开 | 见第 7 节；`controller 0 connected/disconnected`、按键边沿 |
| D3D9 设备、Clear、Present、1280×720 模式 | 见第 8.2 节；无 HD 帧缓冲警告 |
| XDK `fxc` 预编译 VS/PS + 顶点声明 + 索引绘制 | 见第 8.2 节；黄色三角形实拍 |
| 纹理创建/`LockRect`/`XGTileTextureLevel`/采样/alpha | 见第 8.2 节；程序四象限纹理实拍 |
| 真实 `data.win` TXTR 解码与上传 | 见第 8.2 节；`real TPAG 0 page 0 decoded 64x64` |
| 真实 TPAG 整页与非整页 UV 裁剪 | 见第 8.2 节；`src=(2,2 155x14)`、15652 个非背景像素 |
| 端口 D3D9 `RendererVtable`（设备/预编译着色器/顶点声明/纹理/批处理/应用 surface 合成/字母框/Present） | 见第 11 节；真实 DELTARUNE 画面实拍，3000 帧连续运行后干净退出 |
| 真实游戏文字/精灵/背景渲染（颜色、缩放、字母框、黑边） | 见第 11 节；`SELECT THE HEAD THAT YOU PREFER.` 文本 + 红爱心 + 头像图标 |
| surface / EDRAM Resolve / 读回 / 上传 / 动态 sprite | 见第 11 节；`--renderer-selftest` 自检 PASS（清红读回 255,0,0,255；上传绿读回 0,255,0,255） |
| 用户脚本 shader 离线预编译（sidecar 加载 + `shader_get_uniform`/`shader_set_uniform_f` + 走用户 shader 的 draw + 转置 `gm_Matrices`） | 见第 12 节；`--shader-selftest` 自检 PASS（`outColor`=1、`gm_BaseTexture`=0、中心读回 0,255,0,255） |
| XAudio2 音频后端（设备/主混音 voice、内嵌 WAVE 播放、外部/流式 Ogg 解码、增益/音高/暂停/恢复/停止/循环/定位） | 见第 13 节；`--audio-selftest` 自检 PASS（内嵌 0.472 s、外部 Ogg 3.000 s、流式播放位置推进，全部 PASS） |

### 10.2 尚未完成 / 未验证

| 能力 | 当前状态 |
| --- | --- |
| `RendererVtable` 全部入口 | **已实现**；代表性画面（文字/精灵/背景/字母框）已真机验证，见第 11 节 |
| 应用 surface / 用户 surface / EDRAM resolve / 读回 | **已实现并真机验证**（自检 PASS）：createSurface/setRenderTarget/clear/surfaceGetPixels/surfaceSetPixels/createSpriteFromSurface/deleteSprite/surfaceFree |
| 精灵旋转、原点、`drawSpritePart` 批量绘制 | **已实现**；平移/缩放/UV 已在真机随房间渲染验证，旋转/九宫格未单独复现 |
| 字体、GUI、脚本 shader/uniform | 字体与 GUI 已实现并真机验证文本/GUI 路径；**用户脚本 shader 已实现**（离线预编译 sidecar + 运行时加载，机制真机验证，见第 12 节）；真实标题 SHDR 端到端重编译、`f_array` 矩阵打包、多 stage 覆盖**未复现** |
| 多纹理页生命周期、外部 `.yytex`、所有 TPAG 遍历 | 页级惰性上传已实现；本阶段目标仅用到页 7；外部 `.yytex` 仅按 GL 语义接入，未复现 |
| 图元（rectangle/line/triangle/primitive_*/vertex buffer）、drawTextUI | **已实现未验证**（目标游戏未触发） |
| predicated tiling / EDRAM 预算 | **未实现**；单张应用 surface 配置已验证，多/大 RT 未验证 |
| 音频（XAudio2） | **已实现并真机验证机制**（第 13 节）：内嵌 WAVE、外部 Ogg、流式 Ogg 均可播放（自检 PASS）。**未验证**：主观听感/输出质量、分块流式（当前整段解码）、多声道/重采样、`audio_play_sound_at` 的多普勒/HRTF、`audio_group_load` 之外的多组并发 |
| 存档策略（XContent/用户选择/配额/`XContentFlush`） | **未决定**；写存档不伪装成功 |
| 震动 `XInputSetState`、Guide 键 `XInputGetStateEx` | **未实现** |
| 键盘/鼠标 | **明确不支持**（规划要求） |
| 零售机行为、性能与内存峰值 | **未测**；devkit 可写不等于零售可写；帧时间约 17.3 ms/帧（含 Present，接入音频后无回归） |

## 11. 步骤 3.4 图形：D3D9 `RendererVtable`（实现 + 真机验证）

### 11.1 结论

端口已实现满足 `src/renderer.h` 的 D3D9 `RendererVtable`（`src/xbox360/xbox360_renderer.{c,h}`），并把它接入运行时 `src/xbox360/main.c`。在 devkit 上用真实 `DELTARUNE chapter1`（`data.win` SHA-256 `82C2BB61B8D78CD287120F6301588FECBA34EC5A890BAC711B7A8774C760EC70`）连续运行 3000 帧并干净退出，画面正确（真实文字、精灵、背景、字母框、黑边）。设计细节见 `docs/xbox360-renderer-design.md`。

### 11.2 新增/修改的端口资产

| 文件 | 职责 |
| --- | --- |
| `src/xbox360/xbox360_renderer.{c,h}` | D3D9 `RendererVtable` 实现与工厂/`present` |
| `src/xbox360/shaders/renderer_vs.hlsl`、`renderer_ps.hlsl` | 预编译顶点/像素着色器（`vs_3_0`/`ps_3_0`） |
| `scripts/xbox360/compile_shaders.py` | 通用 `fxc` 驱动（`NAME:PROFILE` → 构建目录头文件） |
| `src/xbox360/xbox360_d3d_device.{c,h}` | 新增 `Xbox360D3DDevice_createEx(..., depthStencil)`；渲染器用无深度设备 |
| `src/xbox360/main.c` | 帧循环接入渲染器（`drawPre→beginFrame→drawViews→endFrameInit→drawPost→endFrameEnd→drawGUI→present`）；`parseTxtr=true`；`loadType=LOAD_IN_MEMORY_AHEAD_OF_TIME`；可选自检 |
| `port/xbox360/CMakeLists.txt` | 运行时加入渲染器源、`src/image`、`image_decoder.c`、着色器预编译、`d3d9`/`xgraphics` |
| `scripts/xbox360/build.sh` | 运行时构建要求 `XEDK`（`fxc`）；新增 `--renderer-selftest` |

### 11.3 实施要点（与 Xenon 约束直接相关）

1. **无固定功能管线。** 全部绘制经 `renderer_vs`/`renderer_ps`；顶点声明为 `POSITION FLOAT4@0 / COLOR FLOAT4@16 / TEXCOORD FLOAT2@32`。矩阵按列主序上传到 `c0`；D3D render target 顶行为第 0 行，故**不做** GL 的 clip-Y 翻转。
2. **像素着色器不能 `discard`。** 实测 `fxc` 报 `X3500: Microcode Compiler not yet implemented: 'discard' statement`，alpha test 改用 `D3DRS_ALPHATESTENABLE`/`ALPHAREF`/`ALPHAFUNC`。
3. **纹理线性源是 `0xAARRGGBB` 字。** `ImageDecoder` 输出 RGBA 字节，端口就地转为 ARGB 字再 `XGTileTextureLevel`；红爱心/白字颜色正确即反证该排布（此前探针把 RGBA 直接当字上传，彩色纹理会串通道）。
4. **surface = EDRAM RT + 可锁定 resolve 纹理。** Xenon 不能锁定或直接采样 RT；采样/读回走 `Resolve`，`Present` 后 EDRAM 不保留，故切换目标时 `Resolve`、重新绑定时从 resolve 纹理重绘（restore）。`endFrameEnd` 先清 back buffer 黑再字母框合成（否则黑边会露出上一帧/其他标题的 EDRAM 残留）。
5. **无深度缓冲。** 设备 1280×720、`EnableAutoDepthStencil=FALSE`，省 EDRAM。
6. **用户脚本 shader。** （本小节为步骤 3.4 当时的结论；步骤 4.3 已把它从「无条件降级」推进为「离线预编译 + 不可用时回落」，见第 12 节。）Xenon 无运行时 HLSL 编译；未预编译的 shader 由 `shaderIsCompiled` 报 false，`gpuSetShader` 回落内置 shader 并每 shader 记一次 warning。

### 11.4 真机证据（devkit `192.168.2.9`）

构建（WSL）：`OXDK_DIR=/mnt/h/OXDK XDK_DIR=/root/xdk360 OXDK_LLVM=/root/oxdk-llvm/build XEDK='/mnt/d/Microsoft Xbox 360 SDK' sh scripts/xbox360/build.sh --backend=cmake --frames=3000 -j8`；`xexcheck.py` **PASS**（链接 XDK 库 `xapilib,d3d9,xgraphics,libcMT`）。

| 检查 | 证据 |
| --- | --- |
| 渲染器初始化 | KDNet：`X360Render: initialized (1280x720 back buffer, 9 texture pages)` |
| 应用 surface | `X360Render: created surface 0 (640x480)` |
| 懒加载纹理页 | `X360Render: loaded TXTR page 7 (2048x2048)` |
| 代表性画面（文字/精灵/背景） | `xbcapture` 1280×720：`SELECT THE HEAD THAT YOU PREFER.` 文本、红爱心、三个头像图标、深蓝“眼”背景；左右黑边宽约 160 px（`effW=960` 的正确字母框） |
| 连续运行与退出 | `Ran 3000 frame(s) in 52904.45 ms (17.63 ms/frame)` → `runtime shutdown complete` → 返回 dashboard；无 Bug Check |
| surface/EDRAM 自检 | `--renderer-selftest`：`createSurface -> 0 (exists=1)`、`after clear (expect 255,0,0,255) = (255,0,0,255)`、`after setPixels (expect 0,255,0,255) = (0,255,0,255)`、`createSpriteFromSurface -> 1093`、`PASS` |

截图（`xbcapture` 实拍，已转 PNG 备查）：`build/x360_capture5.png`、`build/x360_capture6.png`（固定构建下的头部选择画面）。

### 11.5 本次修正 / 发现

1. **`data.win` 解析崩溃。** 默认 `loadType=DATAWINLOADTYPE_LOAD_PER_CHUNK` 下，TXTR 的 blob 绝对偏移超出分块缓冲范围，`BinaryReader_seek` 断言失败（`Bug Check 0x00`）。端口改用 `DATAWINLOADTYPE_LOAD_IN_MEMORY_AHEAD_OF_TIME`（与真实资源探针一致）后正常。此为**端口读取选项**差异，未改公共解析器。
2. **黑边残留。** `endFrameEnd` 未清 back buffer 时，字母框区露出未定义 EDRAM（实测为其他标题残留），已在合成前 `Clear` 黑。
3. **日志 `%zX`。** XDK 的 `_snprintf` 不支持 `%z`，公共解析器的 `0x%zX` 打印成 `0xzX`（既是既有现象，也不影响功能；端口不改公共日志）。

### 11.6 未完成 / 未验证

- **用户脚本 shader 离线预编译**：已在步骤 4.3 实施并真机验证机制（第 12 节）；真实标题 SHDR 端到端重编译等仍未复现，见第 12.6 节。
- **predicated tiling / EDRAM 预算**未实现；仅验证单张 640×480 应用 surface + 1280×720 back buffer。
- 图元、`drawTextUI`、`drawSprite*Tiled`、`surfaceCopy`、`surfaceResize`、`textureSetStage` 等入口**已实现未真机复现**（真实目标未触发）。
- 标题退出时 XDK 报一条「minimum frame buffer of 1280×720」警告，疑与应用 surface 作为 RT 相关，待定位。
- 零售机行为、峰值内存、长时间稳定性未测。

## 12. 步骤 4.3：用户脚本 shader 离线预编译（实现 + 真机验证机制）

### 12.1 结论

规划步骤 4.3「用户脚本 shader 离线预编译」已实施：把 data.win 的 HLSL9 shader（`hlsl9_*`）在**主机侧**用 Xbox `fxc` 翻译为 Xenon 微码，写成 sidecar `data.x360shaders`；运行时加载并用它实现 `shaderIsCompiled` / `gpuSetShader` / `shaderGetUniform` / `shaderGetSamplerIndex` / `shader_set_uniform_*`。**不修改任何公共文件**，**不依赖 `d3dx9`**。

在 devkit 上用 `-DBS_X360_SHADER_SELFTEST` 自检，经 KDNet 得到 `ShaderSelfTest: PASS`，并以像素读回证实「走用户 shader 的 draw + 转置 `gm_Matrices` + `shader_set_uniform_f` 上传」全部到位。设计细节见 `docs/xbox360-renderer-design.md` 第 4.2 节。

口径：**机制已真机验证**（合成测试 shader 对）；**真实标题的 SHDR 端到端重编译未复现**（现有目标 data.win 无法触发，见 12.6）。

### 12.2 数据流

```
data.win 的 SHDR.hlsl9_*  ─┐
（或 --hlsl-dir 的成对源码） ├─►  Xbox 360 fxc  ─►  data.x360shaders（sidecar，构建产物，不提交）
                            ┘                     └─►  运行时 X360ShaderSet_load → D3D 着色器对象 + uniform 表
```

### 12.3 新增/修改的端口资产

| 文件 | 职责 |
| --- | --- |
| `scripts/xbox360/precompile_shaders.py` | 离线预编译工具：`--datawin` 自解析 SHDR 的 `hlsl9_*` 源码，或 `--hlsl-dir` 编译成对 HLSL；调 Xbox `fxc`，写大端 sidecar |
| `src/xbox360/xbox360_shadersets.{c,h}` | 运行时 sidecar 加载器：创建 D3D9 着色器（微码对齐拷贝）、uniform 表与查找（含 `_` 前缀回退） |
| `src/xbox360/shaders/selftest/selftest.{v,f}.hlsl` | 确定性自检 shader 对（VS 读 `transpose(gm_Matrices[4])`；PS `outColor * gm_BaseTexture`） |
| `src/xbox360/xbox360_renderer.c` | 用户 shader section（`gpuSetShader`/`shaderGetUniform`/`shaderGetSamplerIndex`/`shaderSetUniform*`/`shaderIsCompiled`）、`gm_Matrices`/fog 内置块上传、内部绘制强制内置、`textureSetStage` 支持任意 stage（不绑 NULL）、sidecar 加载/释放、自检 `X360Renderer_runShaderSelftest` |
| `src/xbox360/main.c` | `#ifdef BS_X360_SHADER_SELFTEST` 时在初始化后跑一次自检 |
| `port/xbox360/CMakeLists.txt` | 加入 `xbox360_shadersets.c`；`BS_X360_SHADER_SELFTEST` 选项 + 用 `precompile_shaders.py --hlsl-dir` 生成测试 sidecar 的 `add_custom_command` |
| `scripts/xbox360/build.sh` | 新增 `--shader-selftest`（传 `-DBS_X360_SHADER_SELFTEST`） |

产物 `build/xbox360/generated/data.x360shaders`（795 字节，自检 shader）**不提交**，与内置着色器微码同属构建产物。

### 12.4 实施要点（Xenon 约束）

1. **`discard` 剥离。** Xenon fxc 拒绝 `discard`（`X3500`）。工具剥掉 `discard;` 并置 `ALPHATEST_STRIPPED` 旗标，alpha test 改由 `D3DRS_ALPHATESTENABLE`/`ALPHAREF`/`ALPHAFUNC` 完成（与内置路径一致）。
2. **transpose 检测。** 检测源码里的 `transpose(_gm_Matrices...)`，置 `MATRICES_TRANSPOSED`；运行时对这类 shader 上传**转置**的 `gm_Matrices`，其余逐字上传（`mul(transpose(M), v) = Mᵀ·v`，要得 `WVP·v` 需上传 `transpose(WVP)`）。
3. **大端 byte-oriented sidecar。** 所有整数按大端写，大端 Xenon 读取端无需字节交换。
4. **无 d3dx9 反射。** uniform 表来自 fxc `/Fc` 的 «Registers» 表 + `defconst`（名字/类型/形状/寄存器跨度）；运行时据此上传 `Set{Vertex,Pixel}ShaderConstant{F,I}`。
5. **两半齐备才可用。** 只加载半套会造成 varying 不匹配，故 `HAS_VS|HAS_PS` 均备且句柄非 NULL 才启用；否则整体回落内置着色器（每 shader 一次 warning）。
6. **GMS 2.3+ 的仅声明 hlsl9 被跳过。** 新式 Windows data.win 的 hlsl9 槽只存声明前言、无 `main`，无法重编译；工具记日志并跳过，运行时对该 index 回落。

### 12.5 真机证据（devkit `192.168.2.9`）

构建（WSL）：`OXDK_DIR=/mnt/h/OXDK XDK_DIR=/root/xdk360 OXDK_LLVM=/root/oxdk-llvm/build XEDK='/mnt/d/Microsoft Xbox 360 SDK' sh scripts/xbox360/build.sh --backend=cmake --shader-selftest --frames=1800 -j8`。

部署：`DEVKIT:\chapter1_windows\default.xex` 与 `DEVKIT:\chapter1_windows\data.x360shaders`（均 `200- OK`，未覆盖 `Butterscotch.xex`），经 XBDM `magicboot` 启动，KDNet `listen` 抓取：

| 检查 | 证据 |
| --- | --- |
| sidecar 加载 | `ShaderSelfTest: sidecar shaders=1, using index 0` |
| 自检用的 surface | `X360Render: created surface 0 (64x64)` |
| uniform 查表 | `ShaderSelfTest: shader_get_uniform("outColor") = 1` |
| sampler 查表 | `ShaderSelfTest: shader_get_sampler_index("gm_BaseTexture") = 0` |
| 走用户 shader 的 draw + 转置矩阵 + 常量上传 | `ShaderSelfTest: center (expect 0,255,0,255) = (0,255,0,255)` |
| 结果 | `ShaderSelfTest: PASS` |

即：sidecar 加载 + D3D 着色器创建、`shader_get_uniform`/`shader_get_sampler_index` 查表、`shader_set_uniform_f` 上传、走用户 shader 的 draw、以及转置 `gm_Matrices` 的落点（红底上画出绿中心）——全部在 devkit 上经像素读回证实。

### 12.6 未完成 / 未验证

- **真实标题 SHDR 端到端重编译未复现。** 现有目标无法触发：DELTARUNE ch1 无 `SHDR`；ch2 的 `hlsl9_*` 为仅声明（被跳过）。自检用合成 shader 对，非真实标题 shader。
- **浮点矩阵经 GML `shader_set_uniform_f_array` 的打包**（`float4x4` 连续 16 float）未在真机复现；自检只覆盖 1 寄存器 uniform。
- **多 stage 覆盖语义**（未纹理绘制时 stage>0 的回退）未在真机复现。
- **sidecar 打包/部署未自动化**：文件名固定 `data.x360shaders`，需手工随 data.win 部署。

## 13. 步骤 3.5：音频（XAudio2，实现 + 真机验证）

### 13.1 结论

端口已实现满足 `src/audio_system.h` 的 XAudio2 音频后端（`src/xbox360/xbox360_audio.{c,h}`），并接入运行时 `src/xbox360/main.c`（替换 `NoopAudioSystem`）。在 devkit 上用真实 `DELTARUNE chapter1` 数据运行 `--audio-selftest` 自检 **PASS**，且完整帧循环（1800 帧）干净退出，帧时间无回归（17.27 ms/帧）。**不修改任何公共文件**，**不链接桌面音频后端**。

口径：**机制已真机验证**（内嵌 WAVE、外部 Ogg、流式 Ogg 均可播放且位置推进）；**主观听感/输出音质未验证**（无音频回采通道）。

### 13.2 数据流与格式

```
SOND/流 ─┬─ 内嵌 (flags 0x65) → AUDO 条目 = RIFF WAVE (16-bit PCM) ─┐
         └─ 外部 (flags 0x64) / audio_create_stream → .ogg 文件 ────┤
                                                                    ├─► 16-bit PCM（大端）
                       stb_vorbis（Ogg，宿主字节序） / WAV 解析（字节交换）┘
                                                                    │
                       IXAudio2::CreateSourceVoice ─ SubmitSourceBuffer ─ Start ─► 主混音 voice
```

- 目标数据的音频格式经实测确认：`data.win` 的 56 个 `AUDO` 条目与 `audiogroup1.dat` 的 86 个条目**全部是 `WAVE_FORMAT_PCM`、44100 Hz、16-bit**（单/双声道）；9 个 `flags=0x64` 的 SOND 走外部 `.ogg`。
- **字节序**：Xbox 360 为大端，XAudio2 要求 16-bit PCM 为大端（见 XDK `audio_overview_xaudio2_endianness.htm`）。WAV 文件是小端字节流，解码后就地做 16-bit 字节交换；stb_vorbis 直接产出宿主字节序的 `short`，无需再交换。
- **无运行时解码器**：Xenon 的 XAudio2 不解 Ogg，端口用 vendored `stb_vorbis`（`src/xbox360/stb_vorbis_impl.c` 单一定义 TU，`xbox360_audio.c` 以 `STB_VORBIS_HEADER_ONLY` 取声明）。

### 13.3 新增/修改的端口资产

| 文件 | 职责 |
| --- | --- |
| `src/xbox360/xbox360_audio.{c,h}` | XAudio2 后端：设备/主混音 voice、WAV/Ogg 解码、实例槽位与 voice 生命周期、增益/音高/暂停/恢复/停止/定位/循环、组增益/主增益、简单空间化（衰减 + 等功率声像）、流（`audio_create_stream`）、自检 |
| `src/xbox360/stb_vorbis_impl.c` | `stb_vorbis.c` 的单一定义 TU（`STB_VORBIS_NO_STDIO`） |
| `src/xbox360/main.c` | 改用 `X360AudioSystem_create()`；`options.parseAudo=true`；每帧调用 `audioSystem->vtable->update(audio, dt)`；`BS_X360_AUDIO_SELFTEST` 时跑自检 |
| `port/xbox360/CMakeLists.txt` | 加入 `xbox360_audio.c`/`stb_vorbis_impl.c`；`BS_X360_AUDIO_SELFTEST` 选项与宏 |
| `scripts/xbox360/build.sh` | 运行时链接库加 `xaudio2,xmcore`（`xmcore` 提供 XAudio2 依赖的 `XLFQueue*`）；新增 `--audio-selftest` |

### 13.4 实施要点（Xenon 约束）

1. **链路**：`XAudio2Create` → `CreateMasteringVoice`（默认通道/采样率）。XAudio2 在 360 上使用硬件线程 4、5（`XAUDIO2_DEFAULT_PROCESSOR`）。链接需 `xaudio2.lib` + `xmcore.lib`（后者提供 `XLFQueueAdd/Create/Destroy/Remove`，否则链接报 “not provided by the console and not in any library”）。
2. **无声降级**：`XAudio2Create`/`CreateMasteringVoice` 失败时只记录错误并保持 `initialized=false`，`playSound` 返回 -1——游戏仍可无声启动，不崩溃、不伪装成功。
3. **每实例一个 source voice**：按 SOND/流的格式（声道/采样率）建 voice，`SubmitSourceBuffer` 提交整段 PCM（循环用 `XAUDIO2_LOOP_INFINITE`）。`update()` 用 `GetState` 的 `BuffersQueued` 回收已播完的非循环实例。
4. **定位（seek）**：XAudio2 无 PCM seek，改为 `Stop` + `FlushSourceBuffers` + 从偏移处重新提交缓冲区 + `Start`；播放位置由 `SamplesPlayed` 增量计算。
5. **SOND pitch 约定**：目标数据里 SOND 的 pitch 字段为 `0.0`（默认值），桌面后端依赖其音频库忽略非正 pitch。端口同样把 `<=0` 视为 `1.0`，与 miniaudio 的 `ma_sound_set_pitch` 行为一致。
6. **嵌入音频缓存**：AUDO 条目解码后按条目地址缓存在 `audoCache`（stb_ds 数组，线性查找；条目数 ≤142）；流在 `X360AudioStream` 内缓存一次，`destroyStream` 释放。
7. **外部/流文件路径**：经端口 `FileSystem`（反斜杠原生，第 6 节）解析与读取；无扩展名时补 `.ogg`。
8. **空间化**：实现 `audio_play_sound_at` 的距离衰减（`ref` 内满音量、之后按 `factor` 反比衰减、`max` 外静音）与单声道等功率声像；**不实现多普勒与 HRTF**。
9. **`audioGroups` 顺序**：`groupLoad` 按 `audiogroupN.dat` 解析（`DataWin_parse` + `parseAudo`，`DATAWINLOADTYPE_LOAD_IN_MEMORY_AHEAD_OF_TIME`，因 AUDO 指针表为绝对文件偏移），并把结果放在数组下标 N，保证 `sound->audioGroup` 寻址正确。

### 13.5 真机证据（devkit `192.168.2.9`）

构建（WSL）：`OXDK_DIR=/mnt/h/OXDK XDK_DIR=/root/xdk360 OXDK_LLVM=/root/oxdk-llvm/build XEDK='/mnt/d/Microsoft Xbox 360 SDK' sh scripts/xbox360/build.sh --backend=cmake --audio-selftest --frames=1800 -j8`；`xexcheck.py` **PASS**（链接 XDK 库 `xapilib,d3d9,xgraphics,xaudio2,xmcore,libcMT`）。部署为 `DEVKIT:\chapter1_windows\default.xex` 后启动，KDNet 抓取：

```
X360Audio: XAudio2 initialized (mastering voice ready)
X360Audio: loaded audio group 1 ('audiogroup1.dat')
AudioSelfTest: embedded group-0 sound = 14 ('snd_bell')
AudioSelfTest: getSoundLength(14) = 0.472 s
AudioSelfTest: playSound -> instance 100000 (playing=1)
AudioSelfTest: track position after 120 ms = 0.1227 s
AudioSelfTest: gain=0.50 pitch=1.50
AudioSelfTest: after pause isPlaying=0
AudioSelfTest: after resume isPlaying=1
AudioSelfTest: after stop isPlaying=0
AudioSelfTest: looping instance 100001 playing=1
AudioSelfTest: external group-0 sound = 17 ('snd_rurus_appear')
AudioSelfTest: external length=3.000 s instance=100002 position=0.1173 s
X360Audio: created stream 300000 for 'snd_rurus_appear.ogg'
AudioSelfTest: stream=300000 instance=100003 position=0.1227 s
AudioSelfTest: PASS
```

随后真实游戏运行 1800 帧：`Ran 1800 frame(s) in 31085.80 ms (17.27 ms/frame)` → `runtime shutdown complete`，无 Bug Check。帧时间与接入音频前（17.63 ms/帧）一致。

**真实标题音乐（流式）**：把 `mus/AUDIO_DRONE.ogg`（186 KB）与 `mus/AUDIO_ANOTHERHIM.ogg`（1.17 MB）部署到 `DEVKIT:\chapter1_windows\mus\` 后再跑，日志显示真实音乐流被解码并播放：

```
X360Audio: created stream 300000 for 'mus/AUDIO_DRONE.ogg'
X360Audio: decoded stream 300000 ('game:\mus\AUDIO_DRONE.ogg') 8.73 s 2 ch 44100 Hz
X360Audio: decoded stream 300000 ('game:\mus\AUDIO_ANOTHERHIM.ogg') 48.00 s 2 ch 44100 Hz
Ran 1800 frame(s) in 35371.49 ms (19.65 ms/frame)
runtime shutdown complete
```

即：**XAudio2 设备/主混音、内嵌 WAVE 播放、外部 Ogg 经 stb_vorbis 解码、`audio_create_stream` 流式播放（含真实标题的 8.73 s / 48 s 音乐）、增益/音高/暂停/恢复/停止/循环/定位全部在真机生效**，播放位置随时间推进（证明 voice 确实在消费音频，而非仅创建对象）。

同一次运行也量到整段解码的代价：播放约 57 s 音乐使 1800 帧总时长从 31.1 s 增至 35.4 s（平均 17.27 → 19.65 ms/帧），即首播时的解码停顿可测。

### 13.6 未完成 / 未验证

- **主观听感/输出音质未验证**：本机没有音频回采通道（KDNet/截图都拿不到音频），只验证了 voice 建立、缓冲区消费与位置推进；字节序正确性依据 XDK 文档 + 内嵌 WAV 路径的对称处理，未做听感确认。
- **流式音频是整段解码**：`audio_create_stream` 首次播放时把整段 Ogg 解码进内存（一首曲子约数十 MB），不是分块流；实测播放约 57 s 音乐使 1800 帧平均帧时间从 17.27 ms 升到 19.65 ms（首播解码停顿），长曲的内存峰值与卡顿属后续优化项（规划要求“缓冲队列/延迟”需另行量测）。
- **音乐目录只部署了验证所需的两个文件**：真实标题请求 `mus/*.ogg`（完整 `mus/` 为 339 个文件约 298 MB）。本次只部署 `AUDIO_DRONE.ogg`/`AUDIO_ANOTHERHIM.ogg` 以验证流式路径；未部署时端口按“文件不存在即静音”降级并记 warning，不伪装成功。
- **多声道/重采样未单独量测**：目标数据全为 44100 Hz 单/双声道；其他采样率/多声道未验证。
- **`audio_play_sound_at` 的多普勒/HRTF 未实现**：仅有衰减 + 等功率声像。
- **多组并发/`audio_group_unload` 未验证**：目标只加载了组 1（`audiogroup1.dat`）；多组同时加载与卸载未覆盖。

## 14. 2026-10-17：像素对齐、30 fps 节流、房间切换修复（真机验证）

本节追加记录，不修改上文历史结论。设计细节与 SDK 依据见 `docs/xbox360-renderer-design.md` 第 10 节。

### 14.1 结论

三个实测问题已修复并部署到 `DEVKIT:\drchs\chapter5_windows\default.xex`：

1. **2D 像素对齐**：`x360Init` 设置 `D3DRS_HALFPIXELOFFSET = TRUE`，恢复 OpenGL/D3D10 的半整数像素中心约定（Xenon D3D9 默认是 D3D1–9 的整数中心，会让点采样落在纹素边界）。
2. **帧率限制**：`main()` 每帧计算 `runner->deltaTime`，并按 `Runner_getEffectiveGameSpeed(runner)` 用 `Sleep` 节流；不再依赖 Present 的 60 Hz VBlank。
3. **房间切换**：`runner->pendingRoom != -1` 时跳过 `X360Renderer_present()`，再调用 `Runner_handlePendingRoomChange()`，与 `src/loop.c` 和 PS2/PS3 端口一致，避免旧房间最后一帧在转场后闪现。

同批修正：`drawTiledPart` UV 原点、`SEPARATEALPHABLENDENABLE` 被内部绘制重置、sampler 只设 stage 0、`gm_Matrices` 固定上传 20 寄存器、`textureSetStage(0,0)` 绑 NULL、surface restore 的 scissor、`backBuffer` 释放。

### 14.2 构建与部署

```text
# WSL
OXDK_DIR=/mnt/h/OXDK XDK_DIR=/root/xdk360 OXDK_LLVM=/root/oxdk-llvm/build \
XEDK='/mnt/d/Microsoft Xbox 360 SDK' \
sh scripts/xbox360/build.sh --backend=cmake --frames=600 -j8

# Windows（x360.py）
x360.py --target 192.168.2.9 push build/xbox360/default.xex \
    'DEVKIT:\drchs\chapter5_windows\default.xex'
x360.py --target 192.168.2.9 launch \
    'DEVKIT:\drchs\chapter5_windows\default.xex'
```

`--frames=0` 的可交互构建也已部署（运行到标题自行退出；本次测试后用 `x360.py reboot` 回到 dashboard）。

### 14.3 真机证据（devkit `192.168.2.9`，Chapter 5）

`build/ch5_600_listen.log`：

```text
X360Render: initialized (1280x720 back buffer, 83 texture pages)
Room changed: room_gms_debug_failsafe (room 0) -> ROOM_INITIALIZE (room 1)
Room changed: ROOM_INITIALIZE (room 1) -> room_legend (room 67)
Room changed: room_legend (room 67) -> PLACE_LOGO (room 72)
Ran 600 frame(s) in 25312.79 ms (42.19 ms/frame)
Steady-state: 479 frame(s) in 15991.75 ms (33.39 ms/frame)
```

- 稳态 **33.39 ms/帧 ≈ 29.95 fps**，证明 30 fps 节流生效；600 帧总平均 42.19 ms 包含首次房间/纹理加载（约 5 s），不是稳态。该测量在保存/传说画面完成，战斗/高负载场景未复测。
- 启动画面截图：`build/ch5_play0.png`（1280×720，文字/精灵/黑边正常）；文字 2× 最近邻放大裁切 `build/ch5_text_crop.png` 边缘干净，无半像素错位。
- `--renderer-selftest` 在 Chapter 5 上重跑：`createSurface -> 0`、clear/setPixels 读回精确、`createSpriteFromSurface -> 8536`、`PASS`（`build/ch5_selftest_listen.log`）。
- 旧的 17.25/17.63 ms/帧是未节流 60 Hz 路径，不能与本次数值比较。

### 14.4 Chapter 5 的用户 shader

Chapter 5 的 `data.win` 有 66 个 `hlsl9_*` shader，但全部只有声明前言、没有入口点（GMS 2.3+ 把函数体存在 GLSL/HLSL11 里）。`precompile_shaders.py --datawin` 全部跳过，无法生成 `data.x360shaders`；运行时回落内置 shader，日志可见：

```text
Warning: X360Render: shader "shd_prophecy_legend" is not precompiled for Xenon; using the built-in shader
```

这会影响依赖这些 shader 的画面效果，属于已知限制，不是本次三个问题的回归。日志还显示 `Extension init script 'shader_replace_simple_init' not found, skipping`；标题目录中的 `shader_replace_simple_x64.dll` 未被端口加载，依赖该扩展的 shader 替换效果也未验证。

### 14.5 未复测 / 证据缺口

- **像素对齐**：已按 SDK 文档修复，但还没有修复前/后的同场景逐像素对比；1.5× 非整数缩放（640×480 → 960×540）本身会造成像素宽窄不均，属 GameMaker 在 1280×720 下的既有行为。
- **房间切换**：代码顺序已对齐公共 runner，但还没有转场逐帧录像确认“黑场期间不闪旧帧”。
- **`--renderer-selftest`**：2026-10-17 已在 Chapter 5 上重跑，`PASS`（`build/ch5_selftest_listen.log`；`createSpriteFromSurface -> 8536`）。上文第 11 节的 1093 是 ch1 记录。
- **`--audio-selftest` 日志**：仍未归档；第 13 节的 PASS 输出是当时的运行记录。
- **`f_array` 的 `float3[]` 打包、平铺可见区域裁剪**：仍未处理，见设计文档 §8。

## 15. 2026-10-17：加载界面与调试叠加层（PS2 同款，真机验证）

### 15.1 结论

端口新增了与 PS2 端口同款的启动加载界面和运行时调试叠加层，并已在 devkit `192.168.2.9` 的 DELTARUNE Chapter 5 上验证加载界面、叠加层绘制与三档状态切换；LB+RB 的实体手柄同时按下路径待用户复测：

- 加载界面覆盖 `data.win` 解析阶段，显示标题、游戏名、状态文字、chunk 进度条、百分比、chunk 名、内存用量、已解析资源统计和构建信息。
- 调试叠加层用 **LB+RB**（控制器 0）在 `stats -> stats+profiler -> off` 之间切换，显示 Room、Tick/Step/Draw/Audio、空闲内存、Room Speed、纹理页、surface、实例/结构体数量。
- 叠加层走 renderer 的内置着色器和内嵌调试字体，不使用公共 `RendererVtable` 新增入口；绘制前强制内置着色器，绘制后恢复游戏状态。
- 构建信息（commit/date）由 `scripts/xbox360/build.sh` 注入；标题显示为 `Butterscotch360-Next`，不显示作者名。

### 15.2 新增/修改的端口资产

| 文件 | 职责 |
| --- | --- |
| `src/xbox360/xbox360_overlay.{c,h}` | 加载/状态界面、`DataWin_parse` 进度回调、错误界面、调试叠加层、状态机 |
| `src/xbox360/xbox360_renderer.{c,h}` | `x360Init` 拆为 `x360InitDevice`（设备/着色器/白纹理/批处理）+ `x360InitData`（页表/sidecar/sampler）；新增 `X360Renderer_initDevice`、`X360Renderer_beginOverlay`、`X360Renderer_endOverlay`、`X360Renderer_getStats` |
| `src/xbox360/main.c` | 在 `DataWin_parse` 前建设备并初始化 overlay；挂进度回调；各初始化阶段状态界面；帧内 Tick/Step/Draw/Audio 计时；LB+RB 切换；Present 前画叠加层 |
| `src/xbox360/xbox360_input.{c,h}` | `X360Input_consumeOverlayToggle()`：控制器 0 的 LB+RB 边沿，不注入游戏按键状态 |
| `port/xbox360/CMakeLists.txt` | 加入 `xbox360_overlay.c`；新增 `BS_X360_DEBUG_OVERLAY` 选项 |
| `scripts/xbox360/build.sh` | 新增 `--debug-overlay`；把 git commit/date 传给 CMake |

### 15.3 行为与绘制策略

**加载界面**

- 设备在 `DataWin_parse` 之前创建，所以解析期间能持续出画面。
- 解析前先画 `Butterscotch360-Next` + `Loading data.win...`。
- `DataWinParserOptions.progressCallback` 每个 chunk 回调一次：进度条（深灰底 + 黄色填充）、百分比、`Loading XXXX... (n/total)`、`Memory: used/total MB`、左上角已解析资源统计。
- 解析后按阶段显示 `Creating VM...`、`Initializing file system...`、`Initializing audio...`、`Creating runner...`、`Initializing first room...`、`Reticulating splines...`。
- 左下角显示 `Butterscotch360-Next` 和 `Build <hash> [<date>]`。
- 错误界面显示红色错误文字，停留 5 秒后退出。

**调试叠加层**

- 状态：`X360_STATS_ENABLED`、`X360_STATS_ENABLED_WITH_PROFILER`、`X360_STATS_DISABLED`。
- 默认关闭；`--debug-overlay` 构建时默认打开。
- 叠加层在 `Runner_drawGUI` 之后、`X360Renderer_present` 之前绘制；房间切换挂起时不绘制。
- 文字带 8 方向黑色柔光描边，避免在游戏画面上不可读。
- profiler 档位在未定义 `ENABLE_VM_GML_PROFILER` 的构建上显示 `Butterscotch GML Profiler is disabled on this build :(`。

**状态隔离**

- `X360Renderer_beginOverlay` 绑定 back buffer、设置全屏正交投影、强制内置 VS/PS、关闭 alpha test、颜色写全开、fog 0、打开 `SRC_ALPHA/INV_SRC_ALPHA` 混合、sampler 0..7 临时设为 LINEAR。
- `X360Renderer_endOverlay` flush 后恢复游戏 blend / alpha test / color write / 当前 shader / 投影 / fog / sampler filter。
- 调用约定：overlay 是 Present 前最后一次绘制；`endOverlay` 不恢复原 render target。

### 15.4 真机证据（devkit 192.168.2.9，Chapter 5）

构建：

```sh
OXDK_DIR=/mnt/h/OXDK XDK_DIR=/root/xdk360 OXDK_LLVM=/root/oxdk-llvm/build \
XEDK='/mnt/d/Microsoft Xbox 360 SDK' \
sh scripts/xbox360/build.sh --backend=cmake --frames=0 -j8
```

部署到 `DEVKIT:\drchs\chapter5_windows\default.xex` 后启动。截图（`xbcapture` 实拍，已转 PNG）：

| 场景 | 文件 | 观察 |
| --- | --- | --- |
| 解析前 | `build/overlay_loading2.png` | `Butterscotch360-Next` + `Loading data.win...` + `Build e15b765 [2026-10-13]` |
| 解析进度 | `build/overlay_loading4.png`、`build/final_loading.png` | 进度条 38%、`Loading SHDR... (12/31)`、`Memory: 210/512 MB`；最终构建为 22%、`Loading SPRT... (7/31)`、`Memory: 208/512 MB` |
| 解析后/首房间前 | `build/overlay_loading.png`、`build/overlay_loading3.png` | `DELTARUNE Chapter 5`、`Creating VM...` / `Initializing first room...`、8 类资源统计 |
| stats 叠加层 | `build/overlay_stats5.png`、`build/final_stats4.png` | `Room: PLACE_...`（存档画面）/ `Room: room_legend`；最终构建 `Tick 7.64ms`、`Draw 6.44ms`、`Free 260 MB`、`Room Speed 30`、`Textures 3/83`、`Surfaces 1/1`、`Instances 7`、`Structs 2` |
| profiler 档位 | `build/overlay_profiler.png` | 显示 `Butterscotch GML Profiler is disabled on this build :(` |
| 关闭后 | `build/overlay_off.png` | 游戏画面干净，无叠加层残留 |

叠加层的绘制与三档状态切换已通过截图验证（`final_stats4.png` 为 `room_legend` 的 stats 画面）。**LB+RB 的输入组合尚未用实体手柄复测**：XBDM 的 `controller.py pad 'LB+RB:0.5'` 不能稳定保证两个键同时按下，且 `pad LB RB` 是顺序按两个键，不会形成组合；因此实体手柄同时按下 LB+RB 的路径以用户后续实测为准。

### 15.5 限制与未验证

- **叠加层文字会与游戏自带的左上角文字重叠**（Chapter 5 的章节标题）。黑色描边保证可读，但视觉上不如带半透明底板干净；本次按 PS2 的“纯文字 + 描边”风格实现，未加底板。
- **profiler 档位未在启用 `ENABLE_VM_GML_PROFILER` 的构建上验证**；当前 360 构建未定义该宏，所以只验证了“disabled”提示。
- **内存数字来自 `GlobalMemoryStatus`**，XDK 文档说明它不适合精确动态追踪；界面标注 `approx`。
- **叠加层自身开销未单独量测**：`Tick/Step/Draw/Audio` 是叠加层绘制之前的采样，不包含当前帧的叠加层绘制时间；关闭叠加层时无额外绘制。
- **LB+RB 实体手柄同时按下未复测**：XBDM 注入不能稳定保证两个键同时按下；叠加层绘制与三档切换已用截图验证，输入组合以实体手柄为准。
- **只验证了 Chapter 5**；其他标题、多手柄、断连重连下的 LB+RB 未复现。
- **加载界面每个 chunk 都 Present**，会同步到 60 Hz；对 31 个 chunk 的 Chapter 5 最多增加约 0.5 秒，未做节流。
- 没有自动化测试；证据为真机截图。

## 16. 2026-10-17：DXT 纹理压缩与 `data.x360tex` sidecar（真机验证）

### 16.1 结论

端口新增了 Xenon 原生 DXT 纹理压缩，静态 TXTR 页不再一律以 RGBA8 常驻：

- 无 alpha 的页 → `D3DFMT_DXT1`（4bpp）；有 alpha 的页 → `D3DFMT_DXT5`（8bpp）。
- 运行时用 `XGCompressSurface` 把解码后的 `0xAARRGGBB` 字压缩成线性 DXT，再 `XGTileTextureLevel` 铺进 tiled 纹理。
- 如果 `data.x360tex` sidecar 存在且与当前 `data.win` 匹配，直接上传 sidecar，跳过解码/压缩；缺失的页回退到运行时压缩，压缩失败或尺寸不满足 4×4 再回退 RGBA8。
- 动态页（`createSpriteFromSurface`）、surface resolve 纹理、`drawTextUI` 调试字体仍为 RGBA8。

代码核对（Chapter 5）：`SHDR` 有 66 个 shader，运行时会调用 `shader_set`（日志有 `shd_*` 回落 warning）；`gml_GlobalScript_texture_set_interpolation` 在全局初始化中执行，`Runner_create` 用 `optn.info & 0x2` 初始化 `renderer->texFilter`（该 `data.win` 的 `info=0x38400204`，初始为 nearest）。因此线性过滤和用户 shader 采样必须保持可用；PS3 式调色板会破坏这两点，所以本端口选 DXT。

真机结果：Chapter 5 的 600 帧运行中，页 5/39/80 分别以 DXT1 32 KB、DXT5 4096 KB、DXT5 1024 KB 常驻，`3 texture page(s) resident, 5.0 MB`；稳态 **33.38 ms/帧**，与压缩前一致。sidecar 生成与加载均已在 devkit 上跑通；扩展到存档菜单时 12 页全部从 sidecar 读取，截图与 RGBA 版本观感一致。

### 16.2 新增/修改的端口资产

| 文件 | 职责 |
| --- | --- |
| `src/xbox360/xbox360_textures.{c,h}` | `data.x360tex` 大端 sidecar 的读取/写入；页表、按页读数据、gameID + data.win 大小校验 |
| `src/xbox360/xbox360_renderer.c` | `x360CreateTextureFormat` / `x360UploadLevelFormat`；`x360EnsurePage` 的 sidecar → 运行时 DXT → RGBA 回退链；每页格式表；常驻纹理内存日志 |
| `port/xbox360/CMakeLists.txt` | 加入 `xbox360_textures.c`；新增 `BS_X360_DUMP_TEXTURES` 选项 |
| `scripts/xbox360/build.sh` | 新增 `--dump-textures`（生成 `data.x360tex`） |
| `docs/xbox360-renderer-design.md` | §5.1 记录方案、sidecar 格式、与 PS3 调色板的对比 |

### 16.3 sidecar 格式与生成

- 文件 `data.x360tex`，大端，magic `BSX3TEX\0`，当前 **version 3**。
- 36 字节头：magic、version、pageCount、tableCount、gameID、dataWinSize、flags（bit 0 = complete）。
- `tableCount × 24` 字节页表：pageId、format（0=RGBA8、1=DXT1、2=DXT5）、width、height、dataOffset、dataSize。
- 数据块：DXT 线性块，或 RGBA8 的 `0xAARRGGBB` 字。
- **v3 的压缩前处理**：对每个 4×4 块，把 `alpha==0` 的 texel RGB 改为块内可见 texel 的平均色；相邻空块继承邻居平均色。GameMaker 页面常用纯绿 `(0,255,0)` 填透明区域，这一步避免 DXT 颜色块把绿色带到蓝色精灵边缘。alpha 不变。
- v2 sidecar 会被拒绝并重新生成；v1 仍可读但无 complete 标志，也会触发重新生成。
- `--dump-textures` 构建在 devkit 上边加载边写；页表预留 83 个槽位，每写一页同时更新 header 的 pageCount，进程被重启打断也能保留已写页。
- 正常构建只读 sidecar；gameID 或 data.win 大小不匹配时忽略并回退运行时压缩。

### 16.4 真机证据（devkit 192.168.2.9，Chapter 5）

**运行时压缩（无 sidecar）**

```text
X360Tex: no sidecar at "data.x360tex"; pages use runtime compression
X360Render: loaded TXTR page 5 (256x256, DXT1, 32 KB)
X360Render: loaded TXTR page 39 (2048x2048, DXT5, 4096 KB)
X360Render: loaded TXTR page 80 (1024x1024, DXT5, 1024 KB)
Ran 600 frame(s) in 28774.67 ms (47.96 ms/frame)
Steady-state: 479 frame(s) in 15989.47 ms (33.38 ms/frame)
X360Render: 3 texture page(s) resident, 5.0 MB
```

**sidecar 生成（`--dump-textures --frames=600`）**

```text
X360Tex: dumping compressed pages to "data.x360tex" (83 slots)
X360Tex: texture dump closed (3 pages)
```

`data.x360tex` 为 5,277,672 字节（3 页）。

**sidecar 加载（正常构建）**

```text
X360Tex: loaded "data.x360tex" (3/3 pages present, game 0)
X360Render: loaded TXTR page 5 from sidecar (256x256, DXT1)
X360Render: loaded TXTR page 39 from sidecar (2048x2048, DXT5)
X360Render: loaded TXTR page 80 from sidecar (1024x1024, DXT5)
Ran 600 frame(s) in 22690.82 ms (37.82 ms/frame)
Steady-state: 479 frame(s) in 15988.71 ms (33.38 ms/frame)
X360Render: 3 texture page(s) resident, 5.0 MB
```

**扩展到存档菜单**

dump 运行加载 12 页（含此前在 `build/ch5_listen.log` 中解码失败的页 31），正常构建 12 页全部从 sidecar 读取：

```text
X360Tex: loaded "data.x360tex" (12/... pages present, game 0)
X360Render: loaded TXTR page 5/39/80/33/75/31/46/50/29/40/30/51 from sidecar
```

页 31 这次以 DXT5 4096 KB 成功加载；此前 RGBA 路径在加载后期失败，**疑与内存压力有关**（未单独取证）。

**截图（`xbcapture` 实拍，已转 PNG）**

| 场景 | 文件 | 观察 |
| --- | --- | --- |
| 开头旁白 | `build/dxt_play0.png` | 像素画背景、白色文字、红色爱心边缘正常 |
| DELTARUNE 第 5 章 logo | `build/dxt_play1.png` | 白色 logo、红色爱心、文字边缘清晰 |
| 存档菜单（sidecar） | `build/dxt_menu_sidecar2.png` | 大渐变背景、中文文字、菜单框；与 `build/ch5_capture.png`（RGBA）观感一致 |

### 16.5 首次启动自动生成（默认构建，真机验证）

用户提出的“第一次启动生成全部 sidecar，第二次启动复用”现在是**默认构建行为**：`BS_X360_TEXTURE_AUTOGEN` 默认 ON（`--no-texture-autogen` 可关闭）。

- `Runner_create` 完成后、`Runner_initFirstRoom` 之前，`main()` 调用 `X360Renderer_generateTextureSidecar`。
- sidecar 缺失或 `complete=0` 时进入生成：逐页 `x360EnsurePage`（解码 → 压缩 → 写 sidecar），每页上传后立即 `Release` 纹理并复位页表，83 页不会同时常驻。
- 生成期间加载界面显示 `Compressing textures... (n/83)`、百分比条、内存和 `Hold START to skip`（`X360Overlay_drawTextureProgress`）。
- 按住 START：停止剩余页，sidecar 以 `complete=0` 关闭，当前启动用部分 sidecar（缺页运行时压缩）；下一次启动继续生成。START 在释放前会从游戏输入中屏蔽，避免第一帧误开菜单。
- 不跳过：全部页尝试完后写 `complete=1`，关闭并重新加载 sidecar；第二次启动直接复用。

真机日志（devkit 192.168.2.9，Chapter 5）：

```text
# 第一次启动（不完整 sidecar）：加载界面显示 30/83、36%
X360Tex: loaded "data.x360tex" (41/41 pages present, complete=0, game 0)
X360Tex: dumping compressed pages to "data.x360tex" (83 slots)
...
X360Tex: texture dump closed (83 pages, complete=1)
X360Tex: loaded "data.x360tex" (83/83 pages present, complete=1, game 0)
X360Tex: sidecar generation finished (83/83 pages)
First room: room_gms_debug_failsafe
```

```text
# 按住 START 跳过：在 41/83 停止
X360Tex: texture dump closed (41 pages, complete=0)
X360Tex: loaded "data.x360tex" (41/41 pages present, complete=0, game 0)
X360Tex: sidecar generation skipped (41/83 pages)
First room: room_gms_debug_failsafe
X360Render: loaded TXTR page 5 from sidecar (256x256, DXT1)
X360Render: loaded TXTR page 39 from sidecar (2048x2048, DXT5)
X360Render: loaded TXTR page 80 (1024x1024, DXT5, 1024 KB)   # 缺页，运行时压缩
```

```text
# 第二次启动（完整 sidecar）：无生成
X360Tex: loaded "data.x360tex" (83/83 pages present, complete=1, game 0)
First room: room_gms_debug_failsafe
```

- 截图：`build/autogen_overlay.png`（30/83、36%、`Hold START to skip`）、`build/autogen_skipped.png`（跳过后的游戏画面）、`build/autogen_firstboot.png`（更早的完成画面）。
- 带加载界面的生成约 **3.5 分钟**（页 30 在 15 s、页 73 在 180 s）；无界面 dump 版本约 2.5 分钟，差异主要来自每页 Present/截图。
- 完整 `data.x360tex` 为 **206,333,932 字节（约 197 MB）**。
- 第二次启动在 20 秒内到达 `First room`。

**限制**

- **零售机 `game:\` 只读**：自动生成写入 `save` 区（devkit 上默认是 `data.win` 同目录）。零售机写失败时回退运行时压缩；零售部署应预生成 sidecar 并随标题打包，或改用可写 XContent/cache 路径（存档策略仍未实现）。
- **首次启动会阻塞约 3.5 分钟**（带界面）；目前每页 Present 一次，页面进度更新及时，但生成总时长受 Present/VBlank 和截图影响。
- **跳过检测在页边界**：每页解码/压缩是阻塞调用，按住 START 会在当前页完成后的回调里被检测；快速点按可能错过，所以界面提示是 `Hold START to skip`。
- **失败页会被跳过**：如果某页解码失败，sidecar 不含该页，但 `complete` 仍会在尝试完所有页后置 1；该页之后按运行时压缩/RGBA 回退。若需要“失败则下次重试”，应改为 `complete=0` 或记录缺失页。
- **`--dump-textures` 会关闭 autogen**：它保留旧的“边加载边写”调试模式；默认构建不再需要它。

### 16.6 限制与未验证

- **画质只做了代表性场景目测**（logo、开头旁白、存档菜单），没有逐像素对比，也没有覆盖所有 83 页；DXT 对像素画/软 alpha/文字边缘的块伪影仍需逐场景评审。
- **sidecar 由 devkit 的 dump 构建生成**，需要跑过相应内容；不是主机侧独立工具。缺失页会在运行时压缩，但完整 sidecar 仍需要一次覆盖性运行或后续的主机侧工具。
- **sidecar 未做磁盘压缩**；完整 83 页约 197 MB，12 页时约 43 MB，体积约等于 DXT 数据。
- **运行时 `XGCompressSurface` 的一次性耗时未单独量测**；600 帧总平均包含首次房间/纹理加载。
- **只创建 1 个 mip level**；mip/packed/border 变体未在真实资源上出现。
- **`data.win` 仍整文件载入**；本次降低的是纹理常驻内存，不是 `data.win` 的 167 MB 文件缓冲。若要进一步降内存，需要另做 TXTR 剥离/分块加载。
- **动态页、surface resolve 纹理、调试字体仍为 RGBA8**，没有压缩。

## 17. 2026-10-17：`room_dw_fcastle_orange_gauntlet` 白屏 / EDRAM 耗尽修复（真机验证）

### 17.1 现象

在 Chapter 5 的 `room_dw_fcastle_orange_gauntlet` 往左走时白屏，KDNet 日志：

```text
Error: X360Render: CreateRenderTarget(2000x120) failed (EDRAM budget?)
Error: X360Render: CreateRenderTarget(2080x120) failed (EDRAM budget?)
```

此时系统内存 `Free: 146 MB`，所以不是主存不足，而是 **EDRAM 耗尽**。

### 17.2 原因

旧实现给每个 GML surface 都保留一张 EDRAM render target，直到 `surface_free`：

- back buffer 1280×720 ≈ 3.5 MB；
- 应用 surface 640×480 ≈ 1.2 MB；
- 每个 `surface_create` 再占一张 RT（2000×120 ≈ 1 MB，2080×120 ≈ 1 MB，2000×200 ≈ 1.6 MB，……）。

Xbox 360 EDRAM 固定 **10 MB**。房间里同时活着的 surface 一多，`CreateRenderTarget` 就失败。失败后该 surface 没有有效 RT，`surfaceExists` 为 false；游戏继续按自己的 surface 栈绘制，最终落到错误的 target/空白内容，表现为白屏。

### 17.3 修复：EDRAM RT 按需创建 + LRU 缓存

- `surface_create` 只创建 main-memory resolve 纹理，**不再分配 EDRAM RT**，所以不会因为 EDRAM 满而在创建阶段失败。
- 绑定 surface 时，只有该 surface 还没有 RT 才 `CreateRenderTarget`；**RT 不在 unbind 时释放**，保留在缓存中供下次绑定复用，避免正常游戏每次切换 surface 都创建/释放。
- 切走时先 `Resolve` 到 resolve 纹理，然后保留 RT 缓存。
- EDRAM 压力下按 LRU 淘汰：分配失败时，从非当前、非应用 surface 的缓存 RT 中淘汰最久未使用的那个（像素已在 resolve 纹理里），再重试；应用 surface 的 RT 只在最后一步淘汰，下次绑定应用 surface 时重建 + restore。
- 因此正常路径的 EDRAM 占用是：back buffer 3.5 MB + 应用 surface 1.2 MB + 少量缓存 RT。`2000×120` / `2080×120` 不会触发淘汰。

相关实现：`x360CreateSurfaceResources`、`x360CreateRTWithEviction`、`x360CreateSurfaceRT`、`x360ReleaseSurfaceRT`、`x360BindSurface`、`x360BindBackbuffer`。overlay 新增 `RTs: n (evict n)` 统计缓存 RT 数与淘汰次数。

### 17.4 真机验证

**合成压力测试**（`--renderer-selftest` 新增 `EdramStress`）：连续创建并绑定 8 张 2000×120 / 2080×120 surface：

```text
X360Render: created surface 1 (2000x120)
X360Render: created surface 2 (2080x120)
...
X360Render: created surface 8 (2080x120)
X360Render: freed surface 1
...
EdramStress: PASS (8 surfaces)
```

**实际房间**：从 `room_dw_fcastle_final_save` 向左进入 `room_dw_fcastle_orange_gauntlet`，再向左进入 `room_dw_fcastle_top_ascent`；日志中：

```text
X360Render: created surface 25 (2000x120)
X360Render: created surface 26 (2000x200)
X360Render: created surface 45 (2000x120)
X360Render: created surface 46 (2080x120)
```

全部创建成功，**没有 `CreateRenderTarget` 错误，没有白屏**。截图：`build/edram_walk1.png`（orange gauntlet）、`build/edram_walk2.png`（top ascent）。

### 17.5 限制

- **单张大 RT**：back buffer + 应用 surface 之后约 5 MB 是单张 full-size surface 的上限；超过时由第 18 节的 predicated tiling 接管。tile 数上限 15，4096×4096 这类需要 >15 个 1024×1024 tile 的 RT 仍会失败。
- 绑定失败时端口记录 `CreateRenderTarget` 错误并返回 false；游戏可能继续画到 back buffer，极端情况下仍会白屏。
- RT 缓存 + LRU 淘汰避免了正常路径的每次绑定创建/释放；只有真正发生 EDRAM 压力时才付出淘汰/重建 + restore 的开销。`room_dw_fcastle_orange_gauntlet` 的 overlay Draw 13–16 ms 未做前后对比。

---

## 18. 2026-10-17：predicated tiling + RT 缓存（真机验证）

### 18.1 需求

- 单张 full-size RT 超过 EDRAM 时（例如 2048×2048 = 16 MB），不能只靠淘汰其他 RT 解决，需要 Xbox 360 的 predicated tiling。
- 不希望正常游戏在每次 surface 绑定/解绑时创建/释放 RT；RT 应缓存复用，只在 EDRAM 压力下淘汰。

### 18.2 实现

**RT 缓存 + LRU**

- `x360CreateRTWithEviction`：先尝试 full-size `CreateRenderTarget`；失败时按 LRU 逐个淘汰非当前、非应用 surface 的缓存 RT 并重试；最后才淘汰应用 surface 的 RT。
- `x360BindSurface` / `x360BindBackbuffer` 不再在 unbind 时释放 RT；只更新 `lastUsed` 时钟。
- `x360ReleaseSurfaceRT` 仅在 `surface_free`、`x360Destroy`、LRU 淘汰时调用。
- overlay 统计：`RTs: n (evict n)`。

**predicated tiling**

- `x360CreateSurfaceRT`：full-size RT 在 LRU 淘汰后仍失败时调用 `x360SetupTiling`。
- `x360SetupTiling`：选择最大 1024×1024 的 tile（4 MB），按 32×32 对齐切分 surface，最多 15 个 tile；创建 tile 大小的 RT，记录 `tileRects`。
- `x360BeginTilingIfNeeded`：在首次 draw/clear 前调用 `BeginTiling(0, tileCount, tileRects, NULL, 1.0f, 0)`；此时 viewport/projection 已由调用方设置好。bracket 内禁用 scissor。
- `x360ResolveCurrentSurface`：`EndTiling(D3DRESOLVE_RENDERTARGET0, NULL, s->tex, NULL, 1.0f, 0, NULL)`，把各 tile 合成到 full-size resolve 纹理；恢复 scissor 状态。
- `x360SetViewport` / `x360SetScissor`：`tilingActive` 时只更新 shadow，不调用 D3D（per-tile viewport/clip 由 D3D 控制）。
- `x360SurfaceUploadRGBA`：tiled surface 上返回 false（当前不支持 setPixels/uploadPixels）。
- `x360ReleaseSurfaceResources`：如果 surface 是当前 target，先 `x360ResolveCurrentSurface` 结束 tiling，再释放资源。

### 18.3 真机验证（devkit 192.168.2.9，Chapter 5，`--renderer-selftest`）

```text
X360Render: created surface 9 (2048x2048)
X360Render: surface 2048x2048 uses predicated tiling (1024x1024 RT, 4 tiles)
TilingSelfTest: (512,512) expect green = (0,255,0,255)
TilingSelfTest: (1536,512) expect red   = (255,0,0,255)
TilingSelfTest: PASS
```

同一轮 `EdramStress: PASS (8 surfaces)`，证明 2000×120 / 2080×120 多 surface 路径没有回归。`RendererSelfTest: PASS` 仍然通过。

### 18.4 限制

- tiled surface 按“每次绑定重画”处理，不 restore 上一帧内容；需要跨绑定持久内容的 surface 不应落到 tiling 路径。
- bracket 内忽略 viewport/scissor 变更；tiled surface 的 `surface_set_pixels`/`surface_upload_pixels` 返回 false。
- tile 数上限 15；4096×4096 这类需要 >15 个 1024×1024 tile 的 RT 仍会失败。
- 纹理页懒上传发生在 bracket 内属于未完全验证路径；大 surface 最好在进入 bracket 前已驻留所需纹理页。
- tiled surface 每次绑定都会重新执行 `BeginTiling`/`EndTiling`，大 surface 的 tile 越多，GPU 回放成本越高；当前目标没有使用 2048×2048 这类 surface，未量测游戏内帧时间影响。

---

## 19. 2026-10-17：角色蓝色区域绿色伪影修复（alpha bleed + sidecar v3）

### 19.1 现象

用户真机试玩反馈：存档界面不明显，但角色身上有蓝色的地方会出现明显的绿色伪影。DXT 压缩后的角色页在蓝色/深色精灵边缘出现纯绿描边。

### 19.2 原因

GameMaker 的 TXTR 页把完全透明区域填成 **纯绿 `(0,255,0)`**，而不是黑色：

- 页 39（角色/场景 atlas）源图有 **52,856 个 alpha=0 的纯绿 texel**。
- DXT5 的颜色块会把 4×4 块内所有 texel 的 RGB 一起编码，包括 alpha=0 的绿色 texel。
- 当块内同时有蓝色精灵和绿色透明背景时，DXT 端点/插值会把绿色带进可见的蓝色边缘。

这不是 Xbox 360 的 alpha 解码问题；是源纹理的 chroma 背景 + DXT 颜色块共同造成的。

### 19.3 修复

- `x360BleedTransparentBlockColors`（`src/xbox360/xbox360_renderer.c`）在压缩/上传前处理每个 4×4 块：
  - 统计块内 `alpha != 0` 的 texel 平均 RGB；
  - 把块内 `alpha == 0` 的 texel RGB 改为该平均色；
  - 相邻的纯透明块继承邻居块的平均色，避免双线性采样把 chroma 色拉进边缘；
  - **alpha 不变**，只改不可见 texel 的 RGB。
- sidecar 版本升到 **v3**；v2 会被拒绝并重新生成，避免继续使用带绿色伪影的旧缓存。v1 仍可读但会触发重新生成。
- 日志：`X360Render: TXTR page N: bled M transparent texels for clean edges`。

### 19.4 验证

对页 39 做离屏解码对比（源图 vs sidecar 解码）：

- v2 sidecar：104,195 个源图蓝色可见像素中，**249 个**在 DXT 解码后变成纯绿 `(0,255,...)`。
- v3 sidecar：同口径只剩 **70 个**，且样本是 `(107,178,57)` 这类**蓝绿混合的暗绿**，不再出现 `(0,255,0)` 纯绿描边；剩余部分是蓝色和绿色**可见**像素落在同一 4×4 块内的正常 DXT 颜色混合，不是透明背景渗色。
- 真机重新生成 v3 sidecar：`texture dump closed (83 pages, complete=1)`、`sidecar generation finished (83/83 pages, 2 worker(s), 163.3 s)`；页 39 等页面均有 `bled ...` 日志。
- 新 sidecar 页 39 的绿色楼梯/教堂边缘不再有纯绿描边；教堂/植物本身的绿色保留。

### 19.5 限制

- 剩余 70 个像素属于 DXT 颜色块的固有混合；如果仍觉得明显，可对特定角色页改用 RGBA8（内存代价大）或换更高质量的离线 DXT 编码器。
- alpha bleed 只处理 `alpha==0` 的 texel；半透明边缘像素的颜色保持原样。
- v3 sidecar 需要重新生成一次；旧 v2 缓存不会自动复用。



