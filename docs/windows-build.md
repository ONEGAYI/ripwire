# ripwire Windows 原生构建说明

产物：`D:\CODE\Project\_ForExplore\ripwire.exe`（57MB，全静态，仅依赖系统 DLL）。
版本：`ripwire 0.6.1 (Release, Clang 19.1.5, emit=std::format+fputs, built_from=30f14a274+dirty)`。

## 工具链（为什么是这套组合）

- **编译器：VS 2022 自带 Clang 19.1.5**，以 GNU target + MinGW sysroot 混合使用
  （`--target=x86_64-w64-mingw32 --sysroot=C:/mingw64`）。
- **头文件与库：`C:\mingw64`（MinGW-Builds GCC 15.2 的运行时）**——只借头库，不用它的编译器。
- **为什么不用 GCC 15.2 直编**：MinGW-Builds（win32 线程模型）生成的代码在
  ripwire 的多线程解析池中稳定崩溃（`ts_parser_parse_string` 序言 `vmovdqa` 段错误；
  单线程/`-O0`/禁向量化/`force_align_arg_pointer` 均不能救），同代码 WSL 与 Clang 下完全正常，
  判定为该工具链的代码生成/运行时缺陷。独立最小复刻（std::thread + tree-sitter）复现不出，
  仅在 ripwire 完整管线中触发。
- **为什么不用 MSVC 直编**：源码无条件使用 `__builtin_trap`/`open_memstream`/`pthread.h`
  等 POSIX/GCC 专属面（README 声称支持 MSVC 19.36+ 与实际不符），MSVC 路径补丁面更大。

## 重建步骤

```bash
cd /d/CODE/Project/_ForExplore/ripwire
CLANG="/c/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/x64/bin"
CC="$CLANG/clang.exe" CXX="$CLANG/clang++.exe" cmake -S . -B build-clang -G "MinGW Makefiles" \
  -DCMAKE_BUILD_TYPE=Release -DRIPWIRE_NATIVE=OFF -DRIPWIRE_LTO=OFF \
  -DCMAKE_C_FLAGS="--target=x86_64-w64-mingw32 --sysroot=C:/mingw64" \
  -DCMAKE_CXX_FLAGS="--target=x86_64-w64-mingw32 --sysroot=C:/mingw64" \
  -DCMAKE_CXX_STANDARD_LIBRARIES="-static -lws2_32" \
  -DCMAKE_EXE_LINKER_FLAGS="-static -Wl,--stack,16777216"
cmake --build build-clang -j 14
```

- `-static`：消除 libstdc++-6.dll 等运行库依赖（PATH 里 Git for Windows 自带的旧版
  libstdc++-6.dll 会劫持加载并报 `STATUS_ENTRYPOINT_NOT_FOUND`）。
- `--stack,16777216`：16MB 栈（Windows 默认 2MB，Linux 8MB；大文件解析需要）。
- `-lws2_32`：MCP `--listen` 的 winsock 依赖；必须放在链接命令**尾部**
  （`CMAKE_EXE_LINKER_FLAGS` 会被放在对象文件之前而失效）。

## 补丁清单（相对上游 30f14a27）

核心是新增 `src/infra/posixcompat.h`（全部 Windows 兼容层，glibc 路径零开销转发）：

| 兼容项 | Windows 实现 |
|---|---|
| `rwMemstreamOpen/Close` | `open_memstream` 的 tmpfile 读回替身（fseek-end/ftell 语义与 glibc 一致）；20 处调用点改为显式 open/close 对 |
| `rwRealpath` | `_fullpath`（不解析符号链接） |
| `pread` | `ReadFile` + `OVERLAPPED`（真定位读，不移动文件指针） |
| `lstat`/`S_ISLNK`/`O_NOFOLLOW` | 弱化为普通 stat（符号链接防护失效，见「已知弱化」） |
| `getline` | fgetc 增长缓冲实现 |
| socket 桥（`--listen`） | winsock2 + WSAStartup 自动初始化 + `INVALID_SOCKET→-1` 转译 |
| `flock` | `LockFileEx` 全文件范围（定价目录跨进程锁，功能性等价） |
| `setenv`/`mkdir`/`chmod`/`localtime_r`/`fsync` | CRT 等价函数映射 |
| `fcntl`(F_GETFL/F_SETFL) | 恒成功 stub（Windows 描述符恒阻塞，语义正确） |
| `rwRenameReplace` | `MoveFileEx` + 降级链（见下） |
| `runCommandCapture` | `--run-trace` 的 CreateProcess 重写（见下） |

源码级修改（除 include 外的最小改动）：

- `quality.h`：缓存目录探测加 Windows 分支（`%TEMP%\ripwire`）；权限位 0700 恒等检查
  在 Windows 下跳过（MinGW stat 的 mode 位是模拟值）。
- `ingest_cache.h`：tmp→final 替换改用 `rwRenameReplace`——Windows 禁止 rename 到
  「被本进程读句柄持有」的文件（POSIX 允许），降级链 remove+rename → CopyFile 覆写
  （非原子，缓存 checksum trailer 自愈兜底）。
- `infra/jsonesc.h`：**宿主 shell 适配层**（原 `popenTrimmed` 私有补丁的推广）：
  - `rw::popenForHost(cmd, mode)` —— 全部 17 处 popen 站点（popenTrimmed/gitCommandLines/
    git 历史走读/crossref cat-file/gitoracle 补丁走读/docparse markitdown/ingest 忽略探测/
    clone/prcontext/planlint/quality 探针）的统一入口；Windows 分支把 `2>/dev/null`、
    `>/dev/null`、`</dev/null` 重定向逐个替换为 `NUL`（cmd.exe 打不开 `\dev\null`，会打印
    "The system cannot find the path specified." **并中止整条命令**——旧版只补了 popenTrimmed
    一处，其余裸 popen 站点每次探测一行噪音且 git 输出整体丢失）。POSIX 分支原样转发。
  - `shSingleQuote` 的 `_WIN32` 分支 —— cmd 引号转义层：双引号包裹（cmd 视引号内
    `& | < > ^` 空格为字面量，子进程 CRT 剥引号；单引号两层都不剥，git 会收到字面引号路径）。
  - 残留弱点：cmd 在双引号内仍展开 `%VAR%`（命令行模式下未定义变量保持字面量）；
    路径/refs/日期不含 `%`，不设防。
- `gitmine.h`/`quality.h`：两处 `| tail -1` 在 `_WIN32` 下改为省略管道、读端取末行
  （cmd.exe 无 tail）；`githarden.h` 的 fsmonitor 探测依赖 POSIX `printf`+`$?` 复合命令，
  Windows 下诚实降级为 Unset。
- `infra/posixcompat.h` 的 `rwRealpath`：`_fullpath` 输出归一化为 `/` 分隔符——反斜杠
  形式的绝对路径上浮到 gitmine 的 GitPathOffset 尾部匹配（全管线正斜杠拼写）时永远失败，
  churn 整体降级为 "no churn evidence"。Win32/CRT 文件 API 两形皆收，归一化无副作用。
- `ingest_crawl.h` 的目录走读：`p.string()` → `p.generic_string()`——Windows 原生拼写
  产 `.\src\x.h`，与 git 输出 `src/x.h` 的 join 精确匹配失败（POSIX 下两写法字节相同，
  零变化；单文件根分支沿用用户拼写，不变）。
- `verbs_change.h`：`--run-trace` 执行体加 `#ifdef _WIN32` 的 CreateProcess 实现。
- `main.cpp`/`mcpserver.h`：`sys/wait.h`、`sys/socket.h` 等 POSIX 头的处理与 socket 调用替换。

## 已知弱化（实验性构建，验收口径）

1. **符号链接防护失效**：pathguard 的 TOCTOU 防护（lstat/S_ISLNK/O_NOFOLLOW/ELOOP）
   弱化为「视为普通文件」。NTFS 符号链接场景的安全检查不生效。
2. **`--run-trace` 超时杀进程只杀直系进程**（POSIX 版杀整个进程组）；命令按 `cmd.exe /c`
   解释（POSIX 版是 `sh -c`），shell 方言不同。
3. **git 探测的 cmd.exe 方言**（大部分已修复，见补丁清单的宿主 shell 适配层）：双引号层落地后
   `git -C <含空格路径>` 的探测正常。残留：① 双引号内 `%VAR%` 仍被 cmd 展开（见上）；② 依赖
   POSIX 复合命令的探测（fsmonitor 的 `printf`+`$?`）降级为无配置，不冒充；③
   `quality.h` 的 `git archive | tar -x` 管道依赖 PATH 上的 tar（Win10 1803+ 自带 bsdtar）。
4. **缓存覆写的极端窗口非原子**（见上，checksum 自愈）。
5. **未经上游回归套件验证**（612 个 gate 脚本为 POSIX shell 系）。冒烟验证范围：
   `--version`、默认 map、`--for` 排名（与 WSL 版字节级对比一致）、缓存冷/暖、
   `--run-trace`（执行与退出码解码）、MCP stdio initialize 握手。

## 踩坑速查表

按症状查。每行：症状 → 根因 → 解法。诊断过程的完整叙事见上文「工具链」节。

| 症状 | 根因 | 解法 |
|---|---|---|
| MSVC 编译报 `__builtin_trap`/`open_memstream`/`pthread.h` 不存在 | 源码无条件使用 POSIX/GCC 专属 API，README 的「OS: Any、MSVC 19.36+」与实际不符 | 弃 MSVC；走 Clang+MinGW 混合。**教训：先 grep POSIX 面（`unistd.h`/`dirent.h`/`__builtin_`/glibc 函数）再信文档的平台声明** |
| 编译过但运行即 `exit=127` 或零输出 | PATH 里 Git for Windows 自带的旧版 `libstdc++-6.dll` 抢先加载，缺 GCC 15 符号，`STATUS_ENTRYPOINT_NOT_FOUND`（cmd 下 exit 码 `-1073741511` = `0xC0000139`） | 链接加 `-static` 根除运行库依赖；诊断用 `cmd /c` 跑拿真实退出码，`where libstdc++-6.dll` 找劫持源 |
| 链接报 `undefined reference to __imp_send/recv/socket…` 且 `-lws2_32` 明明加了 | `CMAKE_EXE_LINKER_FLAGS` 把 `-l` 放在对象文件**之前**，ld 从左到右解析时无未定义符号需求而丢弃 | 用 `CMAKE_CXX_STANDARD_LIBRARIES`（追加到链接命令**尾部**） |
| 大仓多线程解析稳定段错误，崩点固定在 `ts_parser_parse_string` 序言（`vmovdqa %ymm0, 0x20(%rsp)`） | MinGW-Builds GCC 15.2（win32 线程模型）代码生成/运行时缺陷；单线程不崩、`-O0` 不崩、独立最小复刻（`std::thread`+tree-sitter）复现不出 | 换编译器：VS 自带 Clang 以 `--target=x86_64-w64-mingw32 --sysroot=C:/mingw64` 借 MinGW 头库。**教训：同代码在 WSL 正常 + 最小复刻不复现 = 工具链问题，早换别恋战** |
| 变量名 `near`/`far`/`interface` 编译报 `expected unqualified-id before '='` | windows.h 的 16 位时代段限定符宏（`#define near`，无守卫） | posixcompat.h 在 `#include <windows.h>` 后 `#undef near/far/interface` |
| `open_memstream` 替身的输出永远为空 | `FILE*` 的 `fclose` 无法挂钩，tmpfile 数据在调用方读 buf 前已丢 | 放弃透明替换：调用方 `fflush+fclose` 改为显式 `rwMemstreamClose(m,&buf,&sz)` 配对（20 处）；大小用 `fseek(END)+ftell`，与 glibc「最大写入偏移」语义一致 |
| 缓存每次报 `cannot write` / `cannot replace` | ① 缓存目录探测走 `/tmp` 字面路径；② MinGW `stat` 的权限位是模拟值，`0700` 恒等检查必失败；③ Windows 禁止 rename 到被本进程读句柄持有的文件（POSIX 允许） | ① `%TEMP%\ripwire` 分支；② Windows 跳过权限位检查（ACL 保证私有）；③ `MoveFileEx(REPLACE)` + 降级链 remove+rename → `CopyFile` 覆写（checksum trailer 自愈） |
| 每条 git 探测打印 `The system cannot find the path specified.` | cmd.exe 打不开 `\dev\null`：打印该行**并中止整条命令**。旧版只在 `popenTrimmed` 一处做了 `2>/dev/null → 2>NUL`，其余 16 处裸 popen 站点（gitCommandLines、历史走读、cat-file、markitdown、忽略探测等）漏网——每次探测一行噪音，且该次 git 输出整体丢失（非仅噪音） | `rw::popenForHost`（infra/jsonesc.h）统一替换所有 null-设备重定向；新增 popen 站点一律走它，不再裸调 |
| `git -C <路径>` 类探测在路径含空格时行为异常，或 git 层整体无输出（churn 全缺） | `shSingleQuote` 产出 POSIX 单引号转义；cmd.exe 与子进程 CRT 都不剥单引号，git 收到字面 `'路径'`，`-C` 即 `cannot change to` 失败（且单引号内 `& | < > ^` 对 cmd 仍是元字符） | `shSingleQuote` 的 `_WIN32` 分支：双引号包裹 + `\"` 转义（即本表旧版所注「深度修复需 cmd 引号转义层」的落地） |
| 探测正常但 churn/co-change 全缺，`window="… (no churn evidence)"`，`--rank-by=churn` 退化为 pagerank | join 两端拼写不一致：`_fullpath`（rwRealpath）产 `\`、`std::filesystem::path::string()`（走读）产 `.\x\y`，而 git 输出与全管线比对说 `/` 的 `x/y`——GitPathOffset 的精确尾部匹配永远失败 | `rwRealpath` 归一化 `/`（posixcompat.h）+ 走读改 `generic_string()`（ingest_crawl.h）；POSIX 字节不变 |
| 大文件解析段错误但参数全部合法 | PE 默认栈 2MB，Linux 默认 8MB（此条修复的是真实风险，虽然主崩溃另有其因） | 链接 `-Wl,--stack,16777216` |
| `realpath`/`getline`/`setenv`/`flock`/`pread`/`fcntl` 等编译报未声明 | MinGW-Builds 的 CRT 比 MSYS2 精简（连 `realpath` 都没有） | 全部在 posixcompat.h 提供（见补丁清单）；`pread` 用 `ReadFile+OVERLAPPED`（真定位读），`flock` 用 `LockFileEx` 全文件范围 |
| 后台构建中途无声死亡、产物残缺 | 构建命令管道接了 `head -40`：head 读满即关管道，make 收 SIGPIPE 自杀 | **长构建输出重定向到文件**（`cmd > log 2>&1`）再 grep/分段看；同理避免 `tail -N` 截掉编译错误详情 |
| `ripwire . --for=...` 报 `root path does not exist: <第二个词>` | 参数经 git-bash → wsl.exe 转发时引号被打碎，多词参数被拆成多个位置参数 | 引号敏感的命令写成脚本文件执行，不内联在 `wsl -e bash -lc '...'` 里 |
| `git -C <路径>` 类探测在路径含空格时行为异常 | `shSingleQuote` 产出 POSIX 单引号转义，cmd.exe 引号语义不同 | 已知弱化，接受降级（地图/查询不受影响，仅 churn 注记缺失）；深度修复需 cmd 引号转义层 |

## 踩坑速查表之外的两条流程教训

- **判定性实验优于猜测**：崩溃排查里「-O0 全编」「单线程化」「独立最小复刻」三个便宜实验各排除一大类假设，比连续读代码猜快得多。
- **止损线要预设**：MinGW 工具链调试在第 4 轮无效修复后就该切换（实际拖到第 7 轮）。同类问题的止损判据：同代码他平台正常 + 最小复刻不复现 + 两个优化级别行为一致 → 换工具链。

## 性能参考（QuotaTray 仓库，同机）


| 形态 | `--for` 查询耗时 |
|---|---|
| Windows 原生 exe | ~1.7 s（冷/暖同；大头为目录爬取 + git 探测子进程） |
| WSL ext4 内 | ~1.1–1.3 s |
| WSL 跨 9P 访问 D: | ~4.7–5.0 s |
