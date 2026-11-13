# 分发政策

> 使用本项目前请先读 [`DISCLAIMER.md`](DISCLAIMER.md)：本包不含游戏本体，
> 无任何担保，且使用修改版客户端可能违反游戏 EULA —— 风险由使用者承担。

## 发布什么

- **源码**：本仓库 / 本压缩包。
- **不发布任何构建产物**：没有 `.so`、没有 APK、没有生成的 `guest_####.c`、
  没有目标文件。

## 绝不发布的内容

游戏本身的素材，版权属于发行方（Edmund McMillen / Nicalis）：

- `resources/packed/*.a` —— 22 个归档，约 1.0 GiB
- `isaac-ng.exe` 及 `isaac-ng.exe.unpacked`
- 存档、密钥、创意工坊内容

任何构建本项目的人都必须自备正版 PC 版
《以撒的结合：忏悔》v1.7.9b (J835)。构建管线期望的确切大小和 SHA-256
见 `BUILD_SPEC_FOR_AGENTS.md`。没有那些资源，程序无法启动。

另外也不发布：

- `gen/` —— 192 个生成单元，4,669,714 行，约 213 MB。GPL 要求的是
  **生成它的手段**（生成器 + 你自己的 PE），不是生成出来的结果。
  上游 `.gitignore` 同样排除 `guest_####.c`，理由相同。
- `obj*/`、`luapic/`、构建输出、日志、转储、截图。

## 再分发时的许可义务

本项目是 **GPL-2.0-or-later**。若你分发二进制（`libisaac.so` 或 APK），
必须同时提供完整的对应源码。

有两个第三方义务容易被忽略：

- **OpenAL Soft 1.23.1 是 LGPL-2.1。** 分发它需要动态链接（或提供可重新
  链接的说明）、附上完整 LGPL 文本、并给出获取其源码的方式。最省事的做法
  是从 https://github.com/kcat/openal-soft 自己编 1.23.1，而不是复用某个
  第三方 APK 里那份 —— 那种来源通常不明。
- **`tools/steamstub_unpack.py` 里的 SteamStub 脱壳**是按公开文档独立实现的。
  算法上参考过 Steamless（atom0s/Steamless，CC BY-NC-ND 4.0），
  但没有复制代码。再分发该脚本时请保留这条说明。

只发源码 —— 本压缩包就是如此 —— 不产生任何额外负担。

## 发布构建之前的检查

1. 按 `BUILD_SPEC_FOR_AGENTS.md` 的判据验证产物：**导出符号恰好 8 个**和
   **NEEDED 不含 libc++** 在 `S7 链接` 阶段那张表里；**`base=0x3c000000`**
   在第三节（运行期验证）里。
2. 确认压缩包里没有匹配上面排除清单的文件。`gen/` 以及任何
   `.a` / `.exe` / `.so` / `.apk` 都应缺席。
3. 附上你所发布内容的 SHA-256。

## 本包内代码的来源

- 宿主层（`host_*.c`、`jni_bridge.c`、`libisaac.map`、
  `gl4es_bridge_gen.c`）：为本移植编写。
- `gen_extra/*.c`：对少数生成函数的手工修正版本。
- `runtime_patched/guest.c`、`guest.h`、`guest_pe.h`：上游 repentogxm 的文件，
  经本移植修改（`guest.c` 的日志已英文化并加了诊断）。
- `tools/*.py`：为本移植编写，除上面提到的 SteamStub 算法说明。
- `apk/src/*.java`、`apk/build_apk.sh`：为本移植编写。
