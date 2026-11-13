# 构建规格（给 AI 代理）—— 2026-11-12

> **读者是 AI 编码代理，不是人。** 所以这里不写死路径、不给现成脚本，
> 只给**契约、不变量、验证判据和陷阱**。脚本由你自己生成 —— 你比我更
> 清楚目标机器上的目录布局。
>
> 目标产物：一个可 `dlopen` 的 `libisaac.so`（arm64）和可选 APK。
> 预期使用者：在 **x86_64 Linux** 上构建，产出装到 arm64 Android 设备。

---

> **遇到问题先查 [`PITFALLS.md`](PITFALLS.md)。** 那里有全部踩过的坑，
> 按代价排序，含构建期、ABI、分配器、Android 平台、诊断方法五类。

## 零、先把这五条事实读进去

它们决定了你该怎么生成脚本。**其中第 1 条是整个流程最反直觉的地方。**

| # | 事实 | 后果 |
|---|---|---|
| 1 | `gen_all.py --image-base` **只支持 0x30000000 和 Vita 的 0x98000000** | 必须先生成 0x30000000，再**文本平移**到 0x3C000000 |
| 2 | 本项目**纯 C**，产物不依赖 libc++ | 交叉编译只需 libc，省掉 `libc++_shared.so` |
| 3 | Termux 上的 clang **就是 NDK 的 clang**（`Target: aarch64-unknown-linux-android24`） | 原生产物 == 交叉编译产物，可等价复现 |
| 4 | `gen/*.c` 直接 `-I` 上游 `recomp/runtime` 的 `guest.h` | 本项目是 **GPL-2.0-or-later** 衍生物 |
| 5 | 还剩 **20 个未翻译桩**（全是 MMX/SSE 指令） | 构建"成功"不等于 0 个桩；别拿这个当失败判据 |

---

## 一、平台映射：原生 → 交叉编译

原始构建在 Termux（arm64 原生）完成，`clang` 不带 `--target` 也能出 arm64。
你在 x86_64 上必须显式指定，否则会产出 x86 目标文件，链接时才发现。

### 等价关系

```
Termux 原生:   clang <flags> -c x.c        （隐式 target = aarch64-...-android24）
x86 交叉:      clang --target=aarch64-linux-android24 <flags> -c x.c
```

`aarch64-linux-android24` 里的 `24` 是 **min API level**。产物会标注
`for Android 24`。APK 的 `minSdkVersion` 是 26，两者不冲突（24 < 26 向后兼容）。

### 需要准备的交叉工具链

**方案 A：Android NDK（推荐）**

```
NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang
```

NDK 的 clang 自带 sysroot，不需要 `--sysroot`。参考版本 **r27 ~ r30**
（原产物由 r30 生成；r25+ 应该都可以，r30 是已验证的）。

**方案 B：系统 clang + 自备 sysroot**

需要显式 `--sysroot=<NDK>/toolchains/llvm/prebuilt/linux-x86_64/sysroot`，
且要保证 API level 与 `--target` 的 `24` 一致。**不推荐**，坑多。

### 交叉编译时不会出现、但你要主动处理的一件事

原产物里有一条 RUNPATH：

```
RUNPATH  /data/data/com.termux/files/usr/bin/../../usr/lib
```

那是 Termux 的 clang 自动加的（libc++ 搜索路径残留）。你用 NDK clang
时不会出现。但**验证阶段应该确认产物里没有奇怪的绝对路径**：

```bash
readelf -d libisaac.so | grep -i runpath     # 期望：无输出
```

有的话链接时加 `-Wl,--disable-new-dtags -Wl,-rpath,''` 或直接不管
（Android 不读 RUNPATH，但干净一点好）。

---

## 一·五、一页速查（AI 先读这个，再读下面的契约）

九步能跑通的顺序。每步都有可检查的产物，**产物不对就别往下走**。

```bash
# ---- 前置：装好并验证 NDK ----
export ANDROID_NDK_HOME=<你的 NDK 路径>
export TOOLBIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$(ls $ANDROID_NDK_HOME/toolchains/llvm/prebuilt/ | head -1)/bin"
export CC="$TOOLBIN/clang"
"$CC" --version | grep -q android || echo "✗ NDK 的 clang 不对劲，先解决这个"

# ---- 1. 脱壳 ----
python3 tools/steamstub_unpack.py isaac-ng.exe -o work/isaac-ng.exe.unpacked
# 判据: 8,650,240 字节 + sha256 31846486...

# ---- 2. 生成 C（约 13 分钟）----
export REPENTOGXM_WORKDIR="$PWD/work"
export REPENTOGXM_PE="$PWD/work/isaac-ng.exe.unpacked"
cd <上游 repentogxm>/recomp
python3 gen_all.py --image-base 0x30000000 --outdir "$REPENTOGXM_WORKDIR/gen"
# 判据: 192 个 guest_*.c，合计 4,669,714 行

# ---- 3. 基址平移 ★ 最容易漏 ----
cd <本包>
python3 tools/shift_base.py --from 0x30000000 --to 0x3C000000 --apply --dir "$PWD"
# 判据: 约 32 万处改动 / 210 个文件，含 host_*.c 和 jni_bridge.c

# ---- 4. Lua（33 个 .o）----
# 见 S4.2

# ---- 5. gl4es（libGL.a，必须 -DSTATICLIB=ON）----
# 见 S4.1

# ---- 6. 编译 192 个翻译单元 ----
mkdir -p obj
for f in work/gen/guest_*.c; do
  "$CC" --target=aarch64-linux-android24 -O2 -fPIC \
     -DGUEST_TRACE_FUNCTIONS=1 -DGUEST_CHECKED_MEMORY=1 \
     -I <上游>/recomp/runtime -c "$f" -o "obj/$(basename "$f" .c).o" || break
done
# 判据: 192/192 成功

# ---- 7. 编译宿主层 ----
for f in source/host_*.c source/minimal_kage.c source/cpu_seam.c \
         source/missing_stubs.c source/gl4es_bridge_gen.c; do
  "$CC" --target=aarch64-linux-android24 -O2 -fPIC -Dfprintf=host_fprintf \
     -I <上游>/recomp/runtime -c "$f" -o "obj/$(basename "$f" .c).o" || break
done
# jni_bridge.c 单独编，不加 -Dfprintf 宏

# ---- 8. 链接 ----
"$CC" --target=aarch64-linux-android24 -shared obj/*.o luapic/*.o \
  -o libisaac.so \
  -Wl,-soname,libisaac.so -Wl,--version-script=source/libisaac.map \
  -lm -ldl -lpthread \
  <gl4es>/build/lib/libGL.a
# 判据: 约 74 MB，导出符号恰好 8 个，NEEDED 只有 libm/libdl/libc

# ---- 9. 验证 ----
readelf -sW --dyn-syms libisaac.so | awk '$5=="GLOBAL" && $7!="UND"' | wc -l   # 8
readelf -d libisaac.so | grep -c NEEDED                                        # 3
```

跑起来后 `[boot]` 段出现 `✓ image loaded base=0x3c000000` 才算成功。

**卡住时按顺序怀疑**：
1. `PE load failed` → 第 3 步基址没平移，或平移漏了 `host_*.c`
2. 链接报 `undefined symbol` → 第 4/5 步依赖没编全，或 `libGL.a` 路径不对
3. 导出符号远多于 8 个 → version-script 没生效
4. 运行时崩在启动 → 看 `[probe]` 段打印的低 4GB 占用

---

## 二、阶段契约

每个阶段写着**输入 / 输出 / 通过判据**。判据都是可机器检查的。

### S1 脱壳

```
输入  isaac-ng.exe（SteamStub Variant 3.1 加壳）
工具  tools/steamstub_unpack.py（纯 Python，无依赖）
输出  isaac-ng.exe.unpacked
判据  8,650,240 字节
      sha256 = 31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404
```

**判据不匹配就停。** 不是 v1.7.9b (J835) 的话，后面 15,071 个函数全部对不上。
上游的 `gen_all.py` 内部还有 `body_sha256` / `order_sha256` / `sites_sha256`
三重校验，会替你拒绝错误的 PE。

### S2 生成 C 代码

```
输入  isaac-ng.exe.unpacked
工具  上游 repentogxm 的 recomp/gen_all.py
      （环境变量驱动：REPENTOGXM_WORKDIR / REPENTOGXM_PE）
输出  <workdir>/gen/guest_0000.c … guest_0191.c + guest_table.c
判据  192 个文件，合计 4,669,714 行 C
      耗时约 13 分钟（8 核）
```

用 `--image-base 0x30000000`。**不要试图传 0x3C000000** —— 见事实 1。

### S3 基址平移 ★

```
输入  S2 的 gen/、gen_extra/、host_*.c、jni_bridge.c
工具  shift_base.py --from 0x30000000 --to 0x3C000000 --apply --dir <build>
判据  改动约 32 万处 / 210 个文件
      文件里不再出现 0x3[0-9a-f]XXXXXX 形式的镜像内地址
```

**为什么必须做**：Android app 进程里 ART 占住低 4 GiB 的一段，guest 镜像
必须落在编译期硬编码的基址上，否则运行时
`PE load failed: cannot allocate fixed guest image`。

**为什么不能改生成器**：新基址要重算
`COVERAGE_CROSS_ROOT_CONTRACTS_BY_IMAGE_BASE` 里的覆盖率契约哈希，代价很高。

**为什么必须包含 `host_*.c`**：宿主层也硬编码了镜像地址（资源表探针、
xinput 的 xprobe 等）。只平移 `gen/` 会留下不一致。

平移安全性：只改数值落在 `[0x30000000, 0x30000000+0x0085F000)` 的十六进制
字面量。区间外一个字节不碰 —— 因为那 8.6 MB 被 guest 镜像独占。

### S4 依赖库（三个，都要 arm64）

| 库 | 许可 | 要求 |
|---|---|---|
| **gl4es 1.1.7** | MIT | 静态库 `libGL.a`，**必须 `-DSTATICLIB=ON`** |
| **Lua 5.3.3** | MIT | 逐文件编成 `luapic/*.o`，**排除 `lua.c` / `luac.c`**。应为 **33 个 .o** |
| **OpenAL Soft 1.23.1** | **LGPL-2.1** | 只有音频需要。⚠ 分发需动态链接 + 附 LGPL 全文 + 提供源码 |

#### S4.0 装 NDK（这是最容易卡住的一步）

任选一种：

```bash
# 方式 A：已有 Android Studio —— SDK Manager → SDK Tools → 勾选 NDK (Side by side)
#         装到 $ANDROID_SDK_ROOT/ndk/<version>/

# 方式 B：命令行（需先装 cmdline-tools）
sdkmanager --install "ndk;27.0.12077973"

# 方式 C：直接下载官方 zip（developer.android.com/ndk/downloads）
unzip android-ndk-r27*-linux.zip -d ~/
```

装完**必须验证**（这一步不要跳过）：

```bash
export ANDROID_NDK_HOME=~/android-ndk-r27   # 换成你的实际路径
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" --version
# 期望：clang version 18.x 或 19.x，Target: aarch64-unknown-linux-android
```

`prebuilt/linux-x86_64` 是 Linux/macOS 的路径。**目录名不是这个的话**
（例如 macOS 上是 `darwin-x86_64`），用下面这条确认真实名字：

```bash
ls "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/"
```

后面所有 `clang` 命令都用这个绝对路径，或者先：

```bash
export CC="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/<你查到的>/bin/clang"
```

> 版本说明：原产物由 **r30** 生成。r27+ 应该都可以。
> 低于 r25 的 NDK 其 clang 可能不支持某些 C11 语法，不推荐。

#### S4.1 gl4es（产出 libGL.a）

**`-DSTATICLIB=ON` 是硬性要求** —— 没有它 CMake 会走 `add_library(GL SHARED)`
分支，产出 `.so` 而不是 `.a`，后面链接会失败。

```bash
git clone https://github.com/ptitSeb/gl4es.git
cd gl4es && git checkout v1.1.7

cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$CC" \
  -DANDROID=ON \
  -DUSE_ANDROID_LOG=ON \
  -DSTATICLIB=ON \
  -DNOX11=ON \
  -DNO_GBM=ON

cmake --build build -j"$(nproc)"
```

`-DANDROID=ON` 本身会自动带上 `-DNOX11 -DNO_GBM -DDEFAULT_ES=2`，
所以额外写 `-DNOX11` / `-DNO_GBM` 是冗余的 —— 写上无害，省得 AI 反复确认。

**验证**：

```bash
ls -l build/lib/libGL.a        # 应该存在，约 7 MB
```
路径可能是 `build/lib/libGL.a`，也可能是仓库根的 `lib/libGL.a`，
取决于 gl4es 版本。**找不到就用 `find . -name libGL.a`**。

#### S4.2 Lua（产出 luapic/*.o）

必须是**未修改**的官方 lua-5.3.3 源码树：

```bash
curl -LO https://www.lua.org/ftp/lua-5.3.3.tar.gz
tar xf lua-5.3.3.tar.gz
LUA_SRC="$PWD/lua-5.3.3/src"

mkdir -p build/luapic
for f in "$LUA_SRC"/*.c; do
  case "$(basename "$f")" in
    lua.c|luac.c) continue ;;   # 这两个是命令行入口，不要
  esac
  "$CC" -O2 -fPIC -c "$f" -I "$LUA_SRC" -o "build/luapic/$(basename "$f" .c).o"
done
```

**验证**：`ls build/luapic/*.o | wc -l` 应为 **33**。

#### S4.3 OpenAL Soft（可选，只有要声音才需要）

如果不编这个，链接时会缺 `libopenal.so`，音频静音但游戏能跑
（`host_openal.c` 里有"真 OpenAL 不可用"的分支）。

```bash
git clone --branch 1.23.1 https://github.com/kcat/openal-soft.git
# 用 NDK 的 CMake toolchain 编 arm64-v8a，产物 libopenal.so
```

⚠ **LGPL-2.1**：如果你分发 APK，必须动态链接这一份（不要 `-static` 并进去）、
附带 LGPL 全文、并提供源码获取方式。从第三方 APK 里抠一个 `libopenal.so`
来用是**不行的** —— 来源不明，无法履行 LGPL 义务。

### S5 编译翻译单元

```
输入  gen/guest_*.c（S3 平移后）
标志  -O2 -fPIC -DGUEST_TRACE_FUNCTIONS=1 -DGUEST_CHECKED_MEMORY=1
      -I <upstream>/recomp/runtime
输出  obj_pic/guest_XXXX.o × 192
判据  192/192 成功
```

**`-fPIC` 不能省。** 共享库要求全局数据走 GOT（`adrp/ldr`），
PC 相对直取会报：
```
relocation R_AARCH64_ADR_PREL_PG_HI21 cannot be used against symbol '...'
recompile with -fPIC
```

**增量判据必须同时看两个时间戳** —— `.c` 和 `guest.h`。
只比 `.c` 会出现「头文件里是新逻辑、目标文件里是旧逻辑」的静默不一致。

### S6 编译宿主层

```
输入  host_*.c / minimal_kage.c / cpu_seam.c / missing_stubs.c /
      gl4es_bridge_gen.c / jni_bridge.c
标志  -O2 -fPIC -Dfprintf=host_fprintf  -I <runtime>  -I source
      jni_bridge.c 单独编（不加 -Dfprintf 宏）
      host_lua.c 另需  -I <lua-5.3.3/src 的路径>
输出  obj_pic/*.o
```

**有两个头文件要准备好。** `host_path.h` 是本项目自己写的，随包发在
`source/` 下 —— 加 `-I source`（或拷到源文件旁边）。`kage_pc_backend.h` 来自
上游 `recomp/runtime`，和 `guest.h` 同一个位置，不在本包里。没有指向真正
上游 checkout 的 `-I <runtime>`，`minimal_kage.c` 编不过。

`-Dfprintf=host_fprintf` 的作用：让 stderr 日志能落进 `inproc.log`
（app 进程里看不到 stderr）。`jni_bridge.c` 不加这个宏 —— 它自己的
`JLOG` 宏已经写好，加了会写两遍。

**`gl4es_bridge_gen.c` 是自动生成的** —— 约 2300 个 GL 入口点对 75 种参数签名，
由 `tools/gen_gl_bridge.py` 写出。准确数字记在文件自己的头部注释里；要看就看
那里，不要相信以字节为单位的大小 —— 重新生成一次就变了。需要重新生成时用
同一个脚本。它内置构建期闸门：签名表里没有 `'f'`/`'d'` 就报错退出。这个闸门
是为了防一个真实事故 —— 见第四节陷阱 1。

### S7 链接

```
输入  obj_pic/*.o + luapic/*.o + libGL.a
标志  -shared -Wl,-soname,libisaac.so -Wl,--version-script=libisaac.map
      -lm -ldl -lpthread
输出  libisaac.so
判据  约 74 MB
      导出符号恰好 8 个，且全部形如 Java_com_isaacport_launcher_*
      readelf -d 里 NEEDED 只有 libm.so / libdl.so / libc.so（无 libc++）
```

**`--version-script` 绝对不能省。** 不加会导出 **31,104 个符号**，
`dlopen` 进 ART 进程后符号空间和运行时混在一起：查找变慢、可能撞名。
`libisaac.map` 是通配 `Java_com_isaacport_launcher_*`，其余全 local。

**上面三条判据描述的是进程内构建** —— 真正的 `-shared` JNI 库，`dlopen`
进 App 进程。另一种受支持的形态是**子进程构建**：同一份目标码按 PIE 可执行
文件链接，命名为 `lib/arm64-v8a/libisaac.so`，让包管理器解压出来、Java 端
`execve` 它。这种形态**没有**任何 `Java_com_*` 符号，且会导出上万个名字，
因为它是一个程序、不是 JNI 库 —— 拿上面“恰好 8 个导出符号”去卡它是误判。
区分方法：读 ELF 头，有 `PT_INTERP` 段就是可执行文件。`apk/build_apk.sh`
第 1 步做的就是这个判断，并打印出它正在打包哪种形态。

### S8 APK（可选）

```
输入  libisaac.so + apk/src/*.java
工具  apk/build_apk.sh（ANDROID_JAR / R8_JAR / ELF 三个环境变量）
判据  APK 结构检查通过，arm64-v8a/libisaac.so 存在
```

`android.jar` / `r8.jar` 属于 Android SDK，**不要从本项目取，自己装**。

### S9 资源（不进 APK）

```
设备路径  /storage/emulated/0/Android/media/com.isaacport.launcher/files/game/
需要的     resources/packed/*.a      约 1.0 GiB（22 个归档）
           isaac-ng.exe.unpacked
           data/options.ini          分辨率从它读
```

**游戏资源一律不分发。** 见 `DISTRIBUTION.md` 的政策说明。

---

## 三、运行期验证

跑起来后，`[boot]` 段应该长这样：

```
=== The Binding of Isaac: Repentance - Android arm64 (repentogxm) ===
layer entries implemented: 370 (with overlap, 366 of 413 imports covered)
  KERNEL32 76  CRT 85  Lua 67  USER32 59  GL 11  Audio 24
  Steam 11  stdio 4  file 9  time 8  sys 16
✓ image loaded base=0x3c000000
[gl4es] ✓ GL_VERSION  = 2.1 gl4es wrapper 1.1.7
[gl4es] ✓ GL_RENDERER = GL4ES using <your GPU>
```

> 日志标签（`[drawstat]` `[texfinal]` `[alphachk]` `[heap]` …）保持英文原名，
> 它们是诊断的检索键。**全部运行时日志字符串均为英文**（2026-11-12 已统一），
> 便于自动化解析。源码注释仍为中文 —— 那是排查过程的记录，不影响产物。

**`base=0x3c000000` 是平移正确的唯一可靠信号。** 不是这个值 → 回 S3。
`0x30000000` 会在 ART 占用的区间上失败。

---

## 四、陷阱清单（按踩坑代价排序）

### 1. GL 桥的浮点参数被当整数传 —— 已修，但你要知道为什么有闸门

签名解析 `kind_of()` 曾拿**含变量名的完整声明**（如 `"GLfloat r"`）去和
类型名（`"GLfloat"`）比较，永不相等，于是 16 种签名全退化成 `'i'`。
后果：275 个 GL 入口的 `float` 参数全按整数从 x86 栈上取。
`glClearColor(0,0,0,1)` 变成乱值 → 切房间出红色块。

修法是先剥变量名再匹配；数组形参（C 里退化为指针）一律按 `'i'` 传。
**闸门会拦住回归。别为了让它通过而放宽闸门。**

### 2. 主动调 GL 的探针会把进程搞崩

踩过两次，现场都是 `std::terminate`：
- `glGetProgramiv` / `glGetActiveUniform` 探针 → 崩在 `glDeleteProgram`
- 抓帧路径里加 `glBindFramebuffer` → 同上

**规律：读 guest 内存安全；调 GL 不安全。** 任何主动调 GL 的探针，
必须先在本地（子进程模式）跑通，再推到真机。

### 3. `access()` 在 Android FUSE 上撒谎

`/storage/emulated` 走 FUSE，`access(F_OK)` 经常返回 `EACCES`，**即使
`fopen` 完全正常**。这个坑让两个诊断开关长期"文件在、代码在、永不生效"。

**所有文件开关必须用 `fopen` 探测。**

### 4. 分发函数里写裸 `return` 会吞掉 GL 调用

`host_gl_dynamic` 中间 `return 0` 会跳过后面的 `gl_bridge_wrapper`，
等于这次 GL 调用没发生。加探针时尤其容易犯。

### 5. 进程内模式禁 `dup2` / `chdir` / `abort()` / 信号处理

app 进程里，进程内模式和 ART 共享一切。这四个会伤运行时。
子进程模式（`game/exec` 开关）随便折腾。

### 6. 静态指令数 ≠ 性能

`objdump` 数出 -9.1% 指令数，A/B 实跑差异 **0.2%**（噪声内）。
任何性能结论必须有运行时数据。

### 7. 用 CPU 侧读数判断纹理好坏会把所有 FBO 附件判成坏图

游戏用**透明画布**（`rep stosd` 铺 `0x00000000`）建离屏 RT，之后用 FBO
往里渲染。CPU 侧采样一块 GPU 渲染目标，读到零是必然的。
`[texfinal]` 报的 "14.3% 全零"里混着这批**正常**的离屏画布。

### 8. 共用暂存池：别 "按指针重放" 补传纹理

12 个纹理 id 可能共用同一块 ptr。按 ptr 重放 = 把最后一张图复制给池上
每个 id → 串图。曾经做过这个"补传"方案，实测导致初始房间全是彩色贴图，
保留但**默认关闭**。

---

## 五、构建"成功"之后仍然存在的两个缺陷

不要把这两条当成构建失败 —— 它们是已知的、上游还没解决的问题。

### A. 部分 UI 贴图为空

表现：HUD 数字全渲染成同一个 `µ`，主菜单列表空白。

已排除 6 种（都有证据）：
- 数据层：本地 Python 独立实现 PNG 解码，与游戏解码结果**逐字节一致**
  （`bulletatlas.png` 1024x512：65062 == 65062 个非零字节）
- 格式普通（bd=8, ct=6, 非隔行）
- 不是 premultiply 清的（alpha 也是 0）
- 不是共用池串图
- 不是大小写（`.fnt` 引用 `TeamMeatFont16_0.png`，磁盘是小写，12 个里
  9 个不符；加大小写回退后**命中计数 = 0** → 说明不是从散文件读的）
- 不是 `glTexSubImage2D`（调用 **0 次**）

**根因未找到。** 当前线索：字体图集的上传返回地址是 `0x5b0a13`，但它
**不在** `SetTexelData` 的 6 个已知调用点里（6 个中有 4 个计数为 0）。
说明存在第七条还没插探针的路径。

### B. 实体密集房间掉帧

实测模型（两个数据点拟合）：

```
帧时间 ≈ 3.21 ms + 0.88 ms × 每帧离屏 RT 的 draw 次数
```

每次离屏 draw **879 μs**，正常 GLES draw 应 20-50 μs。

**根因已定位**：
- 日志证明游戏**零 VBO**（7800 帧内无一次 `glGenBuffers`/`glBufferData`）
- 用 client-side vertex array：`glVertexAttribPointer` 全程只调 **1 次**
- gl4es 在 ES2 + shader 路径下把 guest 指针**原样**交给驱动
  （`src/gl/drawing.c:427-433`），无中间层
- 于是驱动每次 draw 都从 CPU 内存重读顶点并转换

**改法明确但未实施**：gl4es 的 `ToBuffer()`（`src/gl/gl4es.c:707`）支持
interleaved 布局，正好匹配 Isaac 的 `vec2 Position / vec2 UV / vec4 Color`。
它被 `glstate->vao->locked==1` 门住（只有游戏调 `glLockArraysEXT` 才触发，
Isaac 不调）。在 `drawing.c` 分发前加一条路径，ES2 + shader + client array
时无条件走 VBO 缓存，用 `len_indices()` 的返回顶点数 +
(ptr, stride, len, 内容 hash) 判断是否需重传。

---

## 六、本地环境的硬限制（别浪费时间）

如果你在 x86 上想先本地验证，注意原项目的本地问题**不一定适用于你**
（原项目是在 Termux + pbuffer 上跑的）。但下面这些结论已确认：

1. 本地跑不到字体加载（7800 帧都没有字体图集上传）
2. 本地抓帧永远白屏（pbuffer 上 gl4es 的 FBO 链路不正常）
3. 本地测不出掉帧（60 fps 封顶）
4. 起不了 app（Android 12+ 权限限制，必须手动点图标）

**所以：帧率、画面、性能的结论必须来自真机。**
本地能做的：编译验证、纯只读探针、`TERMINATE_SELFTEST=N` 自测。

`TERMINATE_SELFTEST=N` 会在第 N 次 GL 调用时强制走 terminate 取证路径。
改崩溃路径代码前跑一次，`sig=11` 必须为 0。

---

## 七、体积与耗时参考（用于判断构建是否正常）

| 项 | 值 |
|---|---|
| PE 脱壳后 | 8,650,240 字节 |
| gen/ | 192 文件 / 4,669,714 行 / 约 213 MB |
| 翻译耗时 | 约 13 分钟（8 核） |
| 编译 192 单元 | 约 6-8 分钟 |
| gen_all 覆盖率 | 14,920 成功 / 109 未支持 / 42 失败（共 15,071） |
| 未翻译桩 | **20 个**（全是 MMX/SSE：pabsd, pcmpeqd, pshufb, vpcmpeqb, pmaddubsw, movq, pxor, movd, stmxcsr, popfd） |
| libisaac.so | 约 74 MB |
| 导出符号 | 8 个 JNI（未加 version-script 时 31,104 个） |
| 镜像大小常量 | 0x0085F000 |
| 导入覆盖 | 366 / 413 |
| 真机帧率 | 最好情况 58-60 fps（Adreno 830 / Android 17）；带 2026-11-12 诊断探针的构建实测中位 56.9 fps——见 `LIMITATIONS.md` 第 3 节 |
| 启动 | 约 18.7 s |

那 20 个未翻译桩**被引用的代码路径存在**（有的被引 1-3 次），但正常游戏
流程不触发。若你的构建在某些特殊场景崩溃，先怀疑踩到了其中某个桩 ——
它会 `guest_fault(c, <addr>, "untranslated: <指令>")`，日志里能直接看到指令名。

---

## 八、许可（写进你的产物说明）

| 组件 | 许可 |
|---|---|
| 本项目（host 层 + gen 产物） | **GPL-2.0-or-later** |
| 上游 repentogxm 工具链 + `guest.h`/`guest.c` | GPL-2.0-or-later |
| gl4es | MIT |
| Lua 5.3.3 | MIT |
| OpenAL Soft 1.23.1 | **LGPL-2.1**（分发需动态链接 + 附全文 + 提供源码） |
| SteamStub 脱壳算法 | 参照 Steamless（CC BY-NC-ND 4.0）自行实现，未复制代码 |

GPL 义务的由来：`gen/*.c` 编译时 `-I` 上游 `recomp/runtime`，直接使用其
`guest.h`（CPU 模型、地址映射、调用约定全在里面）。这是衍生关系，不是"参考"。

**发源码就没有额外负担。** 发二进制则必须同时提供完整源码。
