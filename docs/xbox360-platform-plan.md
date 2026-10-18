# Xbox 360 平台层新增规划

## 目标

为 Butterscotch 建立独立的 Xbox 360 平台层实施路线，覆盖 XDK 工具链、启动与主循环、输入、图形、音频、文件访问、日志、打包和验证。本文是实施规划，不表示当前主仓库已经包含可构建的 Xbox 360 目标。

## 不可突破的代码边界

**不得修改公共代码。** Xbox 360 适配不得要求修改或以条件编译污染现有公共运行时。具体保护范围包括：

- `src/` 根目录的公共实现和头文件，包括 `runner.*`、`vm.*`、`renderer.h`、`file_system.h`、`audio_system.h`、`platformdefs.h`、`loop.*`、`common.h` 等。
- `src/backends/`、`src/gl*`、`src/audio/`、`src/physics/` 等既有共享实现。
- 现有 `CMakeLists.txt`、`Makefile`、公共 CI、vcpkg 清单和既有平台代码。

任何平台必须新增的函数、类型或 `#ifdef PLATFORM_XBOX360`，都不能以“只影响 Xbox 360”为理由加进以上文件。不得为 Xbox 360 修改通用接口或改变其他平台行为。

允许的工作范围优先限定在：

- 新增独立规划、设计、构建和移植文档。
- 新增 Xbox 360 专属目录，例如独立端口仓库中的 `src/xbox360/`、`cmake/xbox360/`、`toolchain/`、`scripts/`。
- 如必须使用上游公共仓库，可先提交纯新增文件的实验分支；但如果纳入该仓库的构建需要编辑根 `CMakeLists.txt` 或公共 CI，应停止并改用独立端口仓库/外部构建驱动，不得自行扩大修改范围。
- 对上游的补丁只能作为独立、可审阅的补丁文件保存；不能直接改写或覆盖公共源码。是否接受任何上游补丁须另行审批。

## 现状与方案原则

当前 CMake 通过 `PLATFORM` 选择 `src/${PLATFORM}/*.c`，公共源码默认由 `src/*.c` 汇入目标；但 Xbox 360 尚未列为平台，现有配置还包含桌面图形/音频依赖。增加目标通常会触及根 `CMakeLists.txt`。这与本任务的公共代码禁改要求冲突，因此本规划不把“直接加一个平台分支”作为默认路径。

推荐先用 `H:\OXDK` 做独立端口构建验证。OXDK 的 Xbox 360 应用构建入口是 GNU Make（项目 Makefile 包含 `oxdk.mk`，由 `xbox360/xbox360.mk` 设置 Xenon 目标、链接 XDK 库并封装 XEX2）；OXDK 使用 CMake + Ninja 构建其 Xenon ABI 补丁版 LLVM/Clang 编译器，这不等同于提供一个可供本项目直接 `cmake -DCMAKE_TOOLCHAIN_FILE=...` 使用的 Xbox 360 CMake 工具链。应在 Butterscotch 端口专属 Makefile 中显式枚举需要的公共源文件与 `src/xbox360/` 平台源文件，不生成或改写公共构建文件。若该源文件清单随上游变化，构建应明确失败或由维护者更新清单，不能通过修改公共 CMake 偷渡集成。

OXDK 文档将其 Xbox 360 目标描述为面向 RGH 或 devkit 的 XEX2。它不是微软 XDK 的替代品：使用者需自行提供合法可用的 Xbox 360 XDK；OXDK README 指明主机开发环境为 macOS/Linux，工具链依赖带 Xenon ABI 补丁的 Clang。当前工作站是 Windows，因此在本机直接运行 OXDK shell/scripts 不应视作已验证方案；需使用受支持的 Linux/macOS 主机、WSL（若逐项验证路径/工具兼容）或独立构建机。不得因仓库在 `H:\OXDK` 就推断 Windows 原生命令行受支持。

`H:\wiliwili` 提供了一个已验证可行的 CMake 驱动范例：它的 `library/borealis/library/cmake/toolchain.cmake` 在 `PLATFORM_XBOX360` 分支里把 `CMAKE_C/CXX_COMPILER` 指向 OXDK 的 patched clang、设置 `powerpc-unknown-none-elf` 目标、`CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY`，并用自定义的 `CMAKE_C/CXX_LINK_EXECUTABLE` 调 OXDK 的 `oxdklink.py` 产出 `.xex`。这说明 CMake 工具链 + 自定义链接规则这条路可行，可作为 Butterscotch 端口的参考；但它是 wiliwili 自己的脚本与工具链，Butterscotch 必须编写并维护自己的一套（见“Butterscotch 独立构建脚本”一节），不能直接复用或 source wiliwili 的脚本、路径或缓存。

建议先阅读并逐项对照：`src/ps2/`、`src/ps3/`、`src/vita/`、`src/switch/` 的平台入口，`src/runner.h`、`src/renderer.h`、`src/file_system.h`、`src/audio_system.h`、`src/platformdefs.h` 的现有契约，以及 `CMakeLists.txt` 的平台选择与链接区段。这里只借鉴架构和行为，不复制其平台专属代码作为 Xbox 360 API 依据。

## 分阶段实施步骤

### 0. 固定约束与基线

1. 确认本次工作的仓库边界、目标 Xbox 360 XDK 版本、授权/合规要求，以及允许使用的开发机、真机或模拟环境。不要将公开社区工具链假定为官方 XDK。
2. 记录上游基准提交、工作树状态和当前可通过的桌面/主机测试。
3. 将“公共路径零改动”写进 PR 检查清单。工作树变更检查必须显示所有源码改动都在批准的 Xbox 360 专属路径；基线文档本身可单独新增。
4. 若目标构建必须改动受保护路径，停止实施，提交阻塞说明和备选架构供维护者决策。

### 1. 建立能力与依赖清单

逐项盘点公共运行时需要的平台能力，并记录“现有接口、Xbox 360 后端负责人、依赖/库、失败与降级策略、验证方式”：

- 程序入口、初始化/退出、帧循环与平台事件/系统退出通知。
- 显示模式、D3D9 设备与资源生命周期、纹理/调色板/着色器、present 与垂直同步。
- XInput 手柄数量、按键/摇杆映射、断连和重连语义；键盘/鼠标若非目标功能应明确不支持。
- 游戏数据与配置文件的位置、只读内容和可写用户数据的分离、存储设备选择/权限/空间不足处理。
- 音频设备、混音格式、缓冲与延迟、设备不可用时的行为。
- 单调计时、睡眠/节流、日志输出、崩溃和调试输出。
- 内存/对齐、字节序、链接库、可执行文件格式、内容布局与部署方式。

每项都要引用现有接口定义和 XDK 文档页；未经官方资料或目标 SDK 头文件确认的函数、库名、常量和路径仅标为待确认，不写成既定事实。

### 2. 确认 OXDK 工具链与最小构建

1. 在受支持的 macOS/Linux 构建主机上先运行 `H:\OXDK\oxdk doctor`（Windows 下从已配置的 POSIX shell/WSL 中用实际挂载路径执行），确认诊断脚本能找到补丁版 LLVM、Xbox 360 XDK 和必需工具。若诊断显示缺失，按 OXDK 自身 `README.md`、`docs/xbox360.md` 操作，不在 Butterscotch 公共配置里硬编码环境路径。
2. 若没有现成编译器，OXDK 的 `./oxdk build-llvm` 使用 CMake + Ninja 构建带 Xenon ABI patch 的 LLVM/Clang；注意该步骤下载/构建 LLVM，需要较大磁盘空间。编译器必须启用 Xenon ABI，不能用普通 PowerPC SysV Clang 替代。
3. 配置 `XDK_DIR` 指向解包后的微软 Xbox 360 XDK 根目录，并确认存在 `lib/xbox/xboxkrnl.lib`。XDK 不由 OXDK 分发；不得提交 XDK 文件或假定仓库已包含授权 SDK。
4. 先分别构建并运行 `H:\OXDK\xbox360\samples\abitest`、`d3dclear` 和 `title` 示例（在 OXDK POSIX shell 内使用相应路径）。ABI 测试通过且 XEX 在获准的 RGH/devkit 环境验证后，再做 Butterscotch 的无图形/无音频最小链接。
5. 新增只属于端口的 Makefile，设置 `OXDK_DIR`、`OXDK_TARGET=xbox360`、`SRCS`、`XEX_TITLE`、`XDK_LIBS`，然后 include `$(OXDK_DIR)/oxdk.mk`。根据实际代码依赖逐步增加库；不能照抄样例库列表后就认为依赖已核实。产物目标为 XEX2，不是桌面 ELF。
6. 将 Makefile、环境检查、源码清单、构建/部署脚本与依赖版本记录在端口专属路径；不把 XDK 路径写入公共配置，不提交 SDK 二进制或未经许可的 SDK 内容。
7. 构建驱动采用 Butterscotch 自己的 CMake 工具链 + 链接包装器（参考 wiliwili 的可行做法，见下文“参考实现”与“Butterscotch 独立构建脚本”两节）。工具链文件、包装器和脚本都放在端口专属路径，调用 OXDK 已验证的编译、链接与 XEX 封装链；分别测试 CMake configure、build、clean 和增量构建。不得修改根 `CMakeLists.txt`，也不得复用 wiliwili 的脚本或路径。

## 参考实现：wiliwili 的 OXDK Xbox 360 构建链

`H:\wiliwili` 已用 OXDK 在 Linux/WSL 上成功构建出 XEX。它的做法可作为 Butterscotch 的参考，但以下文件都属于 wiliwili，只能阅读借鉴，不得被 Butterscotch 直接调用或复制成运行时依赖：

- `scripts/build_xbox360_linux.sh`：原生 Linux 入口。设置 `REPO`、`BUILD_DIR`、`OXDK_DIR`、`XDK_DIR`、`OXDK_LLVM`，导出 `OXDK_ROOT`、`OXDK360_DIR`、`OXDK360_CACHE`、`PATH`，处理 XDK 路径含空格的问题（在 build 目录里建符号链接），然后 `cmake -DPLATFORM_XBOX360=ON` 配置并用 Ninja 构建。
- `scripts/build_xbox360_wsl.sh`：WSL 下的增量构建包装，只做环境变量默认值与 `cmake --build`。
- `scripts/build_xbox360_local.sh`：机器本地路径版本，额外校验 `cmake/ninja/python3/make` 与依赖路径，并在检测到来自其他机器的 `CMakeCache.txt` 时删除 build 目录重新配置。
- `scripts/build_xbox360_libcxx.sh`：用 OXDK 的 `libcxx-lib` Makefile 为 Xenon 目标构建 `libcxx-xenon.a`，编译参数通过 `scripts/print_cxxflags.mk` 从 OXDK 的 `xbox360.mk` 取，保证头文件与编译半库匹配。
- `scripts/print_cxxflags.mk`：include OXDK 的 `xbox360.mk` 并打印 `OXDK360_CXXFLAGS`。
- `scripts/oxdklink_xbox360.py`：链接包装器。把每个 `.o/.a/.obj` 复制进 staging 目录，在副本上 `llvm-objcopy --remove-section=.deplibs`（清除 clang MS 兼容模式写入的 `#pragma comment(lib,...)` 元数据），对 `libxbox360_runtime.a` 额外 strip 掉 OXDK `builtins.c` 里的占位 RTTI vtable，然后调用 `oxdklink.py`；原地 strip 会导致每次全量重编，所以必须在副本上做。
- `cmake/strip_deplibs.cmake`：CMake 版的 `.deplibs` 清理（含拆包/重组静态归档），与上面的 wrapper 二选一。
- `library/borealis/library/cmake/toolchain.cmake`（`PLATFORM_XBOX360` 分支）：真正驱动 CMake 交叉编译与链接的工具链逻辑，见下节要点。
- `scripts/x360-ld-fake.sh`、`scripts/x360-pkg-config.sh`：为 FFmpeg/mpv 的 `configure` 提供假链接器与精简 `pkg-config`。Butterscotch 首阶段不接 FFmpeg，暂不需要。
- `scripts/build_nanovg_d3d9_xbox360.sh`：第三方 D3D9 后端构建示例。

wiliwili 工具链文件的关键做法（值得移植到 Butterscotch 自己的工具链）：

- 在任何探测之前把编译器钉死为 `$OXDK_LLVM/bin/clang(++)`，目标 `powerpc-unknown-none-elf`，避免 CMake 回退到宿主 GCC。
- `CMAKE_TRY_COMPILE_TARGET_TYPE = STATIC_LIBRARY`，因为 Xenon 没有宿主 CRT 启动对象，默认的“链接一个可执行文件”探测会失败。
- 覆盖 `CMAKE_C_LINK_EXECUTABLE` / `CMAKE_CXX_LINK_EXECUTABLE`，改成调用 `oxdklink` 包装器：`--objcopy .../llvm-objcopy --oxdklink .../oxdklink.py --stage <build>/.oxdklink/inputs -o <TARGET> -T .../oxdk360/title.ld --xdk $XDK_DIR --libs <XDK 库> <OBJECTS> -- <LINK_LIBRARIES>`，并设 `CMAKE_EXECUTABLE_SUFFIX ".xex"`。
- 通过 `--` 分隔符把目标文件与归档分开传给 `oxdklink`，由它区分自身选项与库。
- XDK 库列表示例：`xapilib,d3d9,d3dx9,xgraphics,xaudio2,xmcore,oldnames,libcMT,xnet`；Butterscotch 要按实际用到的 API 逐个确认，不能照抄。
- 统一公共编译旗标：`--target=powerpc-unknown-none-elf -m32 -ffreestanding -fno-builtin -fms-extensions -fms-compatibility -fshort-wchar -fno-autolink -D_WIN32 -D_M_PPCBE -D_M_PPC -D_PPC_ -D_XBOX -D_XENON -DXBOX -D_SIZE_T_DEFINED`，并 `-U__linux__ -U__unix__ -U__unix` 去掉宿主预定义宏，否则头文件会落到宿主的 glibc。
- 需要 Xenon 调用约定时加 `-Xclang -target-feature -Xclang +xenon-abi`。

## Butterscotch 独立构建脚本（不复用 wiliwili）

**原则：Butterscotch 必须有自己的脚本、工具链文件和链接包装器，路径与逻辑都独立于 wiliwili。** 不得 `source`/`include` wiliwili 的脚本，不得指向 `H:\wiliwili` 下的任何文件，也不得复用其 `build-xbox360-*` 目录或缓存。wiliwili 仅作为阅读参考。新增文件都只属于 Xbox 360 端口路径（例如端口仓库的 `scripts/xbox360/`、`cmake/xbox360/`），不进入公共路径。

建议 Butterscotch 自己维护的文件（名称可调整，职责必须齐全）：

- `scripts/xbox360/env.sh`：只负责设置与校验环境（`OXDK_DIR`、`XDK_DIR`、`OXDK_LLVM`、`OXDK360_DIR`、`OXDK_ROOT`、`PATH`），检查 `cmake/ninja/python3/make` 与依赖路径，缺少即报错退出。路径全部来自环境或本仓库，绝不写死 wiliwili 的路径。
- `scripts/xbox360/build.sh`：唯一构建入口。调用 `env.sh` 后，用本端口自己的 CMake 工程配置并构建，输出 `.xex`。支持 `--clean`、并行度、`BUILD_TYPE`。
- `scripts/xbox360/toolchain.cmake`（或 `cmake/xbox360/toolchain.cmake`）：Butterscotch 自己的 CMake 工具链，落实上节列出的编译器、`try_compile`、链接规则、后缀与公共旗标。它属于端口工程，不是公共 `CMakeLists.txt`。
- `scripts/xbox360/oxdklink_wrapper.py`：Butterscotch 自己的 `oxdklink` 包装器（staging + `.deplibs` 清理），独立实现，不 import wiliwili 的脚本。
- 端口 CMake 工程（例如 `port/xbox360/CMakeLists.txt`）：显式枚举需要的公共 `src/*.c`、`src/xbox360/*.c` 与依赖，不 `add_subdirectory` 根工程，不修改根 `CMakeLists.txt`。
- `scripts/xbox360/README.md`：记录用法、环境要求、已验证范围与与 wiliwili 的差异。

与 wiliwili 的差异（Butterscotch 首阶段更简单）：

- Butterscotch 主体是 C，不需要 wiliwili 的 libc++ 头文件/运行库那一整套；先不引入 `libcxx-xenon.a` 与 `libcxx_runtime.cpp`。
- 不接 FFmpeg/mpv，因此暂不需要 `x360-ld-fake.sh`、`x360-pkg-config.sh` 那类第三方 `configure` 辅助脚本。
- 优先让无图形/无音频的最小 XEX 先跑通，再逐步加 D3D9、XInput、音频与文件系统，脚本按需扩展。

验证独立性的检查项：

- 在只读、不可访问 wiliwili 的环境里，Butterscotch 的脚本仍能配置并构建。
- 全仓库搜索确认没有对 `wiliwili`、`borealis`、`cbffmpeg`、`bili360` 等外部路径的引用。
- 公共路径（`src/*.c`、公共头、根 `CMakeLists.txt`、公共 CI）无改动。

### 3. 实现 Xbox 360 专属后端

按依赖顺序实现，每一步均只新增平台专属文件：

1. **入口与生命周期：** 初始化日志/计时/系统服务，建立 Runner，处理启动失败、退出通知和资源释放；保证重复退出安全。
2. **文件系统：** 先支持只读游戏包/`data.win` 装载，再定义配置、存档或缓存的可写策略。把路径转换集中在 Xbox 360 专属文件中；不要假设开发机路径与零售机路径相同。遇到用户存储/内容 API 时，按内容所有权、用户选择和权限要求设计，不能把任意文件写入当作存档方案。
3. **输入：** 以 XInput 状态轮询实现控制器读取，明确用户索引、未连接/未激活时的处理、按键边沿、摇杆死区和断连时释放状态。映射到现有 Runner/键盘游戏输入契约，不改公共映射代码。
4. **图形：** 建立 Xbox 360 D3D9 专属渲染器或经验证的兼容实现；实现公共 Renderer 契约所需能力。逐项审计 Windows D3D9 假设、固定功能管线、着色器、纹理/调色板、渲染目标、状态重建和 Present 生命周期。性能优化先确保帧正确，再量测。
5. **音频：** 先让游戏可在无声模式启动，再接入经 XDK 头文件和文档确认的音频 API；测试格式转换、缓冲队列、延迟、长时间播放和退出清理。若本阶段暂不支持音频，需明确报告限制，不能链接桌面音频后端冒充支持。
6. **运行时协作：** 接入时间、sleep、日志、窗口尺寸/帧呈现等已有平台钩子。所有新增平台专用状态和辅助 API 留在 Xbox 360 专属目录。

### 4. 资源、内容与性能

1. 列出每个游戏资源的来源、生命周期、主存/图形内存需求和最大尺寸，验证目标配置下的峰值内存。
2. 记录 Xbox 360 大端环境带来的边界：文件格式字段必须沿用公共解析器既有语义，不可未经证据对公共解析器改字节序；在平台端处理已证实需要的平台数据转换。
3. 明确着色器编译/预编译流程、产物版本和资源打包规则；工具产物应可由干净构建重现。
4. 以真实目标设备或获准的调试环境测帧时间、内存峰值、启动时长、音频延迟和长时间稳定性；将数据、场景与构建配置一起归档。

### 5. 测试、打包和验收

- **静态边界：** PR diff 不修改任何保护路径；检查新增文件路径、宏定义和公共头文件引用，拒绝公共头/API/行为变更。
- **构建：** 使用记录版本的 XDK 做干净构建、重复构建和链接检查；检查 XEX、资源包及依赖清单。
- **桌面回归：** Xbox 360 专属新增必须不改变桌面、PS2、PS3、Vita、Switch 的构建和测试结果。若为独立端口，验证其基准上游提交仍可复现。
- **输入：** 多控制器、未连接/无线未激活、运行中断连/重连、摇杆中心与边缘、按键按下/释放不丢失。
- **文件：** 缺失/损坏游戏文件、只读资源、无可写存储、用户取消选择、空间不足与异常退出恢复。
- **图形/运行：** 启动到退出、连续场景、分辨率/画面比例、透明度/调色板、纹理边界、呈现同步、反复初始化/退出。
- **音频：** 静音或无设备降级、连续播放、停止/切场景、长时间运行及退出资源回收。
- **分发：** 只分发许可允许的自有代码和资源；说明运行环境、内容放置方式、版本与已知限制，不分发 XDK 文件。

## 推荐交付顺序

1. 独立端口仓库/分支的最小工具链探测、Butterscotch 自己的 `env.sh`/`build.sh`/`toolchain.cmake`/`oxdklink_wrapper.py`，以及空 XEX。
2. 可启动、可读入测试游戏数据的无图形/无音频版本。
3. 控制器、文件系统和退出生命周期。
4. D3D9 渲染器与代表性画面回归。
5. 音频、性能优化、打包和开发机部署自动化。
6. 经过真机验证后再决定是否发布、是否提上游集成提案；任何需要修改公共代码的提案均需单独授权和审查，不属于本计划的默认批准范围。

## 风险与停止条件

- **公共接口缺少平台钩子：** 停止并记录具体接口缺口；不得在公共头文件中临时添加 Xbox 360 特例。
- **构建入口无法隔离：** 使用外部端口构建/独立仓库；不得擅改根构建系统。
- **SDK/API 版本不确定：** 以安装的 XDK 头文件、库和对应版本文档复核；文档快照与目标 SDK 不一致时标记版本差异。
- **存储、用户资料或认证策略不清：** 不实现写存档或在线功能，直到需求和平台政策得到确认。
- **没有真机/调试环境：** 可完成静态审查和交叉编译，但不得宣称输入、渲染、音频或性能已通过目标机验证。
- **新增实现导致公共行为变化：** 立即停止合并并定位影响；不通过回滚他人改动处理，先由维护者审查差异。

## XDK 文档依据

本规划检索并读取了本地 Xbox 360 XDK 文档快照 21256.17。API 具体声明、需求项和语义必须在实现时继续核对目标 XDK 版本。

- `XInputGetState.htm`：声明 `DWORD XInputGetState(DWORD dwUserIndex, PXINPUT_STATE pState)`；返回 `ERROR_SUCCESS` 或 `ERROR_DEVICE_NOT_CONNECTED` 等错误；文档说明无线控制器在系统启动时不一定处于激活状态。Requirements 指向 `Xtl.h` 和 `Xapilib.lib`。
- `_dx_Direct3D9_For_Xenon.htm`：说明 Xbox 360 Direct3D 9 与 Windows 版本存在差异，包括非 COM、无固定功能管线和 EDRAM 架构；不能把桌面 D3D9 当作二进制/行为兼容依据。
- `_dx_IDirect3DDevice9_present_graphics.htm`：说明 Present 参数应为 `NULL`、Present 内部调用同步呈现间隔，并提示调用后不能假设 EDRAM 内容保留；Requirements 为 `d3d9.h` 和 `d3d9.lib`。
- `XNotifyGetNext.htm`：通知读取 API 的签名及队列语义；Requirements 为 `xam.h` 和 `xapilibw.lib`。是否需要此 API 取决于平台生命周期/通知实现，不代表所有通知都必须接入。
- `system_storage_using.htm`、`developing_with_Xbox_360_unified_storage.htm`：本地存储介质通过 XContent API 管理、在线存储使用 XStorage API；实现前须读全文及关联页，不能仅凭桌面文件 API 推断主机存档行为。

OXDK 项目内的工具链依据（这是本地 OXDK 仓库文档，不属于微软 XDK API 文档）：

- `H:\OXDK\README.md`、`H:\OXDK\docs\xbox360.md`：说明它支持 Xbox 360 XEX2，要求自备 XDK，并列出宿主平台、编译器准备、示例和部署方式。
- `H:\OXDK\oxdk.mk`、`H:\OXDK\xbox360\xbox360.mk`：OXDK 提供的 Makefile 构建入口；定义 Xenon 目标、XDK headers/libraries 与 XEX 链接/封装所需的编译器旗标、库与步骤。项目也可以像 wiliwili 那样改用 CMake 工具链驱动同一套 OXDK 编译/链接/封装链（见“参考实现”一节）。
- `H:\OXDK\xbox360\tools\oxdklink\oxdklink.py`：OXDK 的两段式链接器，负责把目标文件与 XDK 导入 thunk 链接并封装成 XEX；`xbox360/oxdk360/title.ld` 是对应链接脚本。
- `H:\OXDK\scripts\build-llvm.sh`：其中 CMake/Ninja 用于构建 LLVM 工具链（当前脚本固定 LLVM tag）；不是 Butterscotch Xbox 360 应用的 CMake preset。
- `H:\OXDK\scripts\doctor.sh`：可检查补丁编译器 Xenon ABI 能力与 `XDK_DIR` 结构。

wiliwili 的构建脚本依据（这是 `H:\wiliwili` 仓库的内容，仅作参考，不属于本项目，也不得被复用）：

- `H:\wiliwili\scripts\build_xbox360_linux.sh`、`build_xbox360_wsl.sh`、`build_xbox360_local.sh`：CMake + Ninja 驱动的 Xbox 360 构建入口与环境变量设置。
- `H:\wiliwili\scripts\oxdklink_xbox360.py`、`H:\wiliwili\cmake\strip_deplibs.cmake`：链接前清除 `.deplibs` 元数据的两种实现。
- `H:\wiliwili\library\borealis\library\cmake\toolchain.cmake`：`PLATFORM_XBOX360` 分支的 CMake 工具链与自定义链接规则。
- `H:\wiliwili\scripts\build_xbox360_libcxx.sh`、`print_cxxflags.mk`：为 Xenon 构建 libc++ 静态库（Butterscotch 首阶段不需要）。

文档位置：`C:\Users\lbx50\.config\opencode\skills\xbox360-sdk-doc\xbox360docs\`。该快照为版本化参考，不代替目标 XDK 安装中的头文件、库和版本核对。

## 完成定义

- 已有可复现的独立 Xbox 360 构建与部署说明，SDK 依赖及版本明确。
- 构建脚本属于 Butterscotch 自己（独立 `env.sh`/`build.sh`/工具链/链接包装器），不 source 或复用 wiliwili 的脚本与路径。
- 平台能力清单中的每项都有实现状态、限制、测试证据和责任文件。
- 公共代码保护路径无改动；如果实现依赖公共代码变更，则状态为“阻塞/待审批”，不得标成完成。
- 构建、功能测试、真机覆盖范围和未验证事项如实记录；没有将交叉编译成功表述为真机验收。
