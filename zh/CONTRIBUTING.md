# 参与与反馈

原作者已不再开发本项目，但仍在测试并把发现记进这份归档。这也决定了什么东西
对这里最有价值：**一个可复现的观察，比一个修复有用得多。** 因为修复没人能替你
验证，而观察可以被写进文档。

开 issue 之前请先读这一节。这个仓库可能收到的问题，有一半是关于下面那两个
已经记录在案的缺陷。

---

## 报告问题

最有用的东西是**能被反复读取的证据**。这个项目因为"只看了一半日志就下结论"
浪费过好几个小时——其中有三轮在追一个所谓的"第七条贴图路径"，最后发现是
返回地址被读错了。所以：

### 请附上这些

**1. 启动段。** `inproc.log` 的前 20 行左右，包含：

```
[boot] T+   0.0 ms  main entered
[boot] PE path = ...
[boot] image loaded base=0x...
```

如果完全没有 `[boot] PE path =` 这一行，说明你的构建早于 `v0.1.1-alpha` 里的
修复——先用那个 tag 重新构建。

**2. mode 行**，来自 `app.log`：

```
>> mode: in-process .so (direct Surface)
>> mode: child process (...)
```

这一行比看起来重要。child 模式通过 socket 回传画面，按设计被限制在约 20 fps；
进程内模式是直接 `eglSwapBuffers`，跑 60。**没有这一行的性能报告无法解读。**
游戏目录里存在 `exec` 文件会强制 child 模式——如果不是你自己放的，删掉它。

**3. 崩溃段**（如果崩了）。`[SEGV]` 那一段，包含 `fault_addr`，以及紧挨着它的
`[heap]` 行。

**4. 设备信息**：型号、Android 版本、GPU，以及游戏目录在内置存储还是 `/sdcard`。
`/sdcard` 上的 `fopen` 实测比内置存储慢 61 倍，这一项会改变结论。

**5. 复现步骤**：你做了什么，期望看到什么。

日志位置：

```
Android/media/com.isaacport.launcher/files/game/inproc.log
Android/media/com.isaacport.launcher/files/game/app.log
```

### 不用报的东西

下面两个都是已知、已定位、未修复的。报"HUD 显示 µ"没有增量；报"**什么时候**
开始显示 µ"才有。

**缺陷 1 —— 部分 UI 图集上传为空。** 所有 HUD 计数都渲染成 `µ`，主菜单列表空白。
字体图集被创建成一张 512x128 的空 `ImageSolid`，此后再没被填充；
`glTexSubImage2D` 调用次数为零。根因未找到。动手之前请读
`STARTUP_AND_ARCHIVES.md` §2.2——那里有一条具体线索（`.fnt` 语言不一致，以及
正确的 512x128 PNG 落在解不开的加密直存块里）。

**缺陷 2 —— 密集房间掉帧。** 游戏完全不用 VBO，用 client-side 顶点数组，所以
驱动在每次 draw 时都要从 CPU 内存重新读取并转换顶点。一次离屏 draw 实测
879 µs，而应该只要 20-50 µs。gl4es 里的修法已经找到，从未实施。试过
`LIBGL_BATCH` 批处理：一帧发出 48 万次 malloc/free 配对，游戏在 80 帧内 abort。

### 有一种崩溃不是你的错

如果你的构建死在 `fault_addr=0x87e7fff0`，那是既有的越界写，不是你引入的。
它位于分配器自己报告的已占用区边界下方 16 字节，触发条件是**任何足以改变内存
布局的改动**——重打包归档、加诊断代码，任何东西。四种配置及其结果列在
`STARTUP_AND_ARCHIVES.md` §3.3。**先修它；其他一切都卡在它上面。**

---

## 提交修复

**许可证：GPL-2.0。** 本归档携带的是 `GNU GENERAL PUBLIC LICENSE Version 2,
June 1991`，没有 "or later"。贡献按同样条款接收。如果你 fork 并再分发，请保留
许可证和声明——这是法律要求，不是客气。

**守住构建契约。** 链接之后，`libisaac.so` 必须满足：

```
readelf -sW --dyn-syms libisaac.so | awk '$5=="GLOBAL" && $7!="UND"' | sort -u | wc -l
    -> 8

readelf -d libisaac.so | grep -c NEEDED
    -> 3                       （libm、libdl、libc —— 不含 libc++）

readelf -d libisaac.so | grep -i runpath
    -> 无输出
```

`tools/build_and_check.sh` 会编译宿主层、链接、检查上面四条加 PE 路径，任一不符
就拒绝推送。它还会去掉 Termux clang 注入的 RUNPATH，因为
`BUILD_SPEC_FOR_AGENTS.md` 要求没有。

**那份文档漏掉的两个构建依赖**，都是第一次真正运行脚本时才发现的：

- `host_lua.c` 会 include **Lua 5.3.3** 的 `lua.h`、`lauxlib.h`、`lualib.h`。
  这些既不在 runtime 目录里，也不在本归档里。把 `ISAAC_LUA` 指向一份
  `lua-5.3.3/src` 检出，否则构建停在那里。
- Termux clang 会给每次链接写入一条指向 `$PREFIX/../usr/lib` 的 RUNPATH。在这里
  无害——NEEDED 只有 `libm`/`libdl`/`libc`，都先从系统路径解析——但它违反契约，
  而且把一条游戏进程读不到的路径塞进了库里。链接后
  `patchelf --remove-rpath` 可以清掉，导出符号和 NEEDED 不变
  （已核实：改前改后都是 8 和 3）。

**说你测到了什么，不要说你期待什么。** 如果声称帧率改善，请给出阶段
（update / render / other）、样本数、中位数和 95 分位帧时间。一个裸的"FPS"
数字掩盖了"稳定 60"和"60 但有一次 6 秒卡顿"的区别。

**说你没测什么。** 一台设备、一块 GPU、没有一局跑到结局——这就是本归档的状态，
诚实的贡献应保持同样标准。

---

## 接手这个项目

按这个顺序读，每一份回答前一份没有回答的问题：

| | 文件 | 为什么 |
|---|---|---|
| 1 | `PROJECT.md` | 这是什么、架构、现在处于什么状态 |
| 2 | `PITFALLS.md` | 每一个已经付过代价的坑；动构建之前先读 |
| 3 | `LIMITATIONS.md` | 什么已验证、什么未验证，每一条都附了自查命令 |
| 4 | `STARTUP_AND_ARCHIVES.md` | 归档格式，以及挡住启动优化的那个越界写 |
| 5 | `BUILD_SPEC_FOR_AGENTS.md` | 逐阶段的构建流程与通过判据 |

**价值最高的一件事是找到 `STARTUP_AND_ARCHIVES.md` §3.3 里那个越界写。**
让启动快一倍的重新打包方案已经写好、对 13,282 个条目逐字节验证过、真机实测
8 秒——它只被那个 bug 挡着。

这份归档**不知道**的事，你不必重新踩一遍：

- 字体图集为什么被创建成空的、此后再没填充
- 宿主层的 mod 修复（已写、已单测、从未发布）在真机上是否可行
- 游戏为什么对归档发出 22,522 次 `fopen` 且启动之间没有任何缓存
- `x87` 用 `double`（而非 80 位）建模，是否改变了任何可观察行为

---

## 提问

没有支持承诺。如果你问的问题 `LIMITATIONS.md` 已经回答过，预期收到的是一个
章节指引，而不是重新讲一遍。
