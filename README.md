# Project Maya · AC922

这是 [mw00/project-maya](https://github.com/mw00/project-maya) 的 IBM AC922 / POWER9 / 六卡 V100 实验 fork。保留上游模型、服务和文档，并加入针对本机的 NUMA、专家搬运与预填充状态复用改动。

**截至 2026-11-07（北京时间）：在线服务仍使用保留的 v1.1.53 配置与二进制。下列性能数字来自 v1.1.53 基线上的独立实验，不是本仓库合并版 v1.1.56 的测速结果。提交、推送本仓库不等于升级在线服务。**

## 机器与模型

| 项目 | 实测机器 |
|---|---|
| 服务器 / 架构 | IBM AC922 / ppc64le |
| CPU | 双路 POWER9，每路 22 核；44 个物理核、176 个逻辑线程（SMT4） |
| 内存 | Linux 可见约 221 GiB；这是系统报告容量，不是可用内存 |
| GPU | 6 × Tesla V100-SXM2-16GB，合计 96 GiB |
| CPU NUMA 节点 | 0、8；GPU 0–2 对应节点 0，GPU 3–5 对应节点 8 |
| TP2 分组 | 0/1、2/3、4/5；2/3 跨 CPU NUMA 节点 |
| 系统 / 驱动 / 编译器 | Debian 12；NVIDIA 550.54.15；CUDA 12.4、GCC 12.2；目标 sm_70 |
| 模型 | DeepSeek-V4.1-Flash-1017-Maya-GGUF / Maya-S |
| 共同参数 | TP2、262144 上下文容量、4096 预填充分块、22 个 CPU 专家线程、512 MiB Engram 行缓存 |

CPU 的 176 个逻辑线程不等于 Maya 使用 176 个专家线程。GPU 内存 NUMA 节点不作为 CPU 内存分配节点。

## 当前线上配置与实验配置

| 项目 | 当前线上恢复配置 | 最近已完成的 down-slice 实验 |
|---|---|---|
| 源码基础 | v1.1.53 + 早期 AC922 修复 | v1.1.53 + 累积实验补丁 |
| 独立二进制 | `stage-cache-20261105` | `downslice-20261107` |
| 模型 / TP / 分块 / 缓存 | 上表共同参数 | 与线上相同的模型和这些参数 |
| 专家存储 | 文件映射，`--mmap-experts on` | 相同 |
| NUMA 专家搬运池 | `DS4_NUMA_STAGE=2`、`DS4_STAGE_DIV=2`，本地半池 | 相同 |
| Engram 直接读取 | 关闭 | 仅预填充开启，解码沿用原路径 |
| 相邻双分块专家复用 | 未启用 | 开启 |
| 第二份状态 GPU 驻留 / 临时区生命周期复用 | 未启用 | 开启；放不下的状态回主机 |
| 专家热页映射复制 | 未启用 | 开启，检查失败回退原读取 |
| TP2 down 按量化块裁剪 | 未启用 | A/B 唯一切换项 |
| 提前执行常驻专家 | 未启用 | 关闭 |
| `DS4_COPY_PIN` / `DS4_AR2` / `DS4_FLOW_DOWN_PART` | 全部为 `0` | 全部为 `0` |

线上服务使用 GPU 0–5，端口 8080，视觉关闭；默认 `temperature=1`、`top_p=1`、`reasoning_effort=high`。

路径已替换成占位值的配置示例：

- [当前线上参数](configs/ac922/online-v1.1.53.example.json)。实际线上原文件仍单独保留，示例不覆盖它。
- [实验功能参数](configs/ac922/experimental-v1.1.53.example.json)。该文件描述旧实验二进制的功能开关，去除了逐请求控制文件、日志导出和诊断设置，**不是完整复现实验的驱动脚本，也不是已验证的新版本部署配置**。

实验开启项是 `DS4_TP_PAIR=1`、`DS4_PAIR_GPU_STATE=1`、`DS4_PAIR_ALIAS_STATE=1`、`DS4_PAIR_HOT_MAP=1`、`DS4_PAIR_DOWN_SLICE=1`、`DS4_ENGRAM_DIRECT_PREFILL=1`。`DS4_PAIR_EARLY_VRAM=0` 保持关闭。全局 `DS4_FLOW_DOWN_PART` 与两分块路径的原有互斥保护保留；本 fork 的 `DS4_PAIR_DOWN_SLICE` 在配对路径内单独裁剪。

POWER9 上曾发生 ATS/UVM 相关内存注册故障。本 fork 保留禁止专家大块 `cudaHostRegister` 的保护，搬运池使用 `cudaHostAlloc`；不把 `DS4_COPY_PIN=1` 当作本机提速配置。

## 已测优化结果

以下各轮都有自己的基线。**不同轮次的百分比不能相加或连乘，也不能据此推算相对当前线上版的整体收益。** 吞吐采用调和平均；解码小幅差异仍需结合逐轮波动判断。

| 单独测试项 | 负载 | 关闭 → 开启，tok/s | 变化 | 同轮缓存解码 |
|---|---|---:|---:|---:|
| 预填充临时区生命周期复用，供第二份状态使用 | 65384 tokens 热预填充 | 1064.80 → 1150.35 | +8.03% | 29.65 → 29.30，−1.18%，每组 8 轮 |
| 已驻留专家文件页直接复制到原搬运池 | 65384 tokens 热预填充 | 1128.30 → 1200.88 | +6.43% | 29.39 → 29.08，−1.06%，每组 8 轮 |
| Engram 预填充 4 KiB 对齐直接读取 | 261600 输入 + 512 输出 | 573.51 → 958.77 | +67.18% | 26.52 → 27.50，+3.69%，每组 8 轮 |
| TP2 down 只搬各卡需要的量化块 | 65384 tokens 热预填充 | 1208.29 → 1375.30 | +13.82% | 28.71 → 28.91，+0.71%，每组 12 轮 |
| 等待搬运时提前计算常驻专家 | 65384 tokens 热预填充 | 1052.57 → 1054.50 | +0.18% | 28.78 → 28.94，+0.55%，每组 8 轮；后续保持关闭 |

前三轮都早于 down-slice；262K 的 Engram 实验没有开启后来新增的 down-slice。262K 轮未清空 OS 页缓存，且仍有物理读盘，不能称作严格冷盘或全内存热跑。Engram 直接读让该轮首字前实际读盘从 369.32 GB 降到 44.73 GB，首字时间从 456.52 秒降到 273.10 秒。解码本身不走新增的直接读路径。

### 最近一轮：TP2 down 按需复制

同一模型进程、同一实验二进制，在请求边界仅切换 `DS4_PAIR_DOWN_SLICE`；顺序为 off/on、on/off、on/off、off/on。固定两分块复用、GPU 状态/alias、专家热页路径、Engram 直接读、4096 分块和 512 MiB 缓存。每卡 8 个专家读取线程；CPU 专家线程仍为 22。

| 输入长度 | 关闭预填充 tok/s | 开启预填充 tok/s | 变化 | 首字时间：关闭 → 开启 |
|---|---:|---:|---:|---:|
| 82 | 46.19 | 46.22 | +0.06% | 1.783 → 1.782 秒 |
| 2660，单分块 | 413.34 | 410.91 | −0.59% | 6.448 → 6.484 秒 |
| 9667，三分块 | 637.08 | 752.89 | +18.18% | 15.191 → 12.860 秒 |
| 65384，热预填充 | 1208.29 | 1375.30 | +13.82% | 54.181 → 47.614 秒 |

- 每个输入每组 4 次主测；首次请求和单独的详细计时轮不进入主均值。82/2660 不进入新增配对裁剪路径，用于回归检查。
- 完整请求 `cache_n=0`，没有跳过提示预填充；“热”指文件/页缓存条件。64K 后每次接 3 轮缓存解码，每组共 12 轮、每轮固定输出 512 tokens；复用 65383 个提示 token。短提示输出 128 tokens。
- 解码 28.71 → 28.91 tok/s 只说明本轮均值，不能当作稳定的 +0.71% 收益。逐轮解码关闭范围 26.5–30.2，开启范围 27.0–30.0 tok/s。
- 详细计时：down 提交的 H2D 字节约 584.71 → 292.56 GB，热页实际 memcpy 字节约 987.25 → 695.41 GB。是提交/复制计数，不是硬件总线采样或物理磁盘流量；逻辑读取范围仍可能覆盖整个矩阵。
- 38 次采样的常驻专家槽总数均为 6463；配对状态 GPU 容量逐卡均为 `[320, 274, 320, 274, 274, 320]` MiB。保留原搬运池槽和 GPU down scratch，没有把节省的搬运量当成额外显存配额。
- 101 个完整请求完成，62/62 事实探针通过；4 种提示的首步 logits 回归通过。事实检索和首步 logits 不代表全面质量评测或逐 token 完全一致。
- 本轮最长实际输入为 65384；262144 是容量设置。**新增 down-slice 尚未做 262K 满长、真正并发、取消或模型级故障注入测试。**

机器可读的吞吐样本和版本标识见 [测量摘要](docs/ac922/measurements-20261107.json)。这些数字来自本机实验，不是 Strata-AC922、RustingStrata 或其他上游仓库的收益移植承诺。

## 合并版验证范围

本 fork 将基于 `37b1e848df0de7b6934d11059b4339b4ea53c8d2`（v1.1.53）的 12 个本地源码修改/新增文件，三方合并到 `85c07264282cc78fec35a7d5b8eeaa10aa315deb`（v1.1.56），并保留 fork 中随后加入的 `78c450a23cf40e99ef0dd7204ca88572e1c469ed` 服务退出修复。

- 合并版 C++/CUDA 源码已在 AC922 上独立完整构建；CUDA 12.4 / sm_70 / POWER9。未替换在线二进制。
- 9 组 CPU 组件测试通过，覆盖专家队列、块裁剪、边界保护、并发及 Engram 直接读/失败回退。
- 在 `85c0726` 合并版上完成的接口与 lane parity 环境测试共 110 项：107 通过、3 项因缺少可选 SDK 跳过。
- 本次发布包保留 `78c450a` 退出修复后，接口、lane parity 和退出流程共 125 项测试：121 通过，4 项跳过（3 项可选 SDK、1 项 Windows 专属测试）；三组已收录的本地 CPU 回归也再次通过。详见 [发布验证](docs/ac922/validation-20261107.json)。
- **没有在合并版上重跑完整模型性能测试**，本页旧实验数字不代表合并版速度。
- 上游 v1.1.55 起改变了 `DS4_AI_RUN`、AR2、快速链路环形缓冲、DSpark 放置、加载交错读取和提示增量处理的默认行为。下轮 A/B 需显式固定 `DS4_AI_RUN`、`DS4_AR2`、`DS4_RING_FAST_LINK`、`DS4_DRAFT_SPREAD`、`DS4_SPEC_HEAD`、`STRATA_DS4_LOAD_INTERLEAVE`、`MAYA_PROMPT_MEMO`，再逐项测试。
- 本机默认 Latin-1 locale 会令新版服务部分日志输出失败；测试进程使用 `python -X utf8` 后通过。使用新版服务时同样指定 UTF-8，无须更改系统 locale。

可在 Linux 上运行已收录的三组本地回归测试，不需要模型或 GPU：

```bash
python3 tests/ac922/run.py
```

## 保留与回退

提交本仓库不覆盖以下本机文件。日期、哈希和实验标签用于辨认实际测试对象：

| 对象 | SHA256 |
|---|---|
| 当前线上/回退二进制 `stage-cache-20261105` | `73f27db082d90e28652fa530113a2c97719a526f7168f80d5e29c58717d26093` |
| down-slice 完整模型测试二进制 | `341da7b6bbe214f1d382860036d2da31a4491bc5d0f96bab70478d1c570beede` |
| 独立构建的 v1.1.56 合并二进制 | `26c914fac602abb37e3174843507d0d10dbb27de079ff1444c43aaae5ae38683` |
| 原正式配置 | `c864dfc673bfd11108844a77cf86caaef307a918418f72ce414ef76262f20167` |

原正式配置、当前实际启动配置、原二进制、实验源码/二进制和逐轮日志均保留在原机器的独立目录。仓库只发布源码、配置示例和非敏感测量摘要，不包含模型权重、二进制、服务凭据或对话日志。

回退时应先等待在途请求结束，再停止实验服务，并用**保留的旧启动配置、旧服务端源码和旧二进制整套启动**；不能只把新的配置指向旧二进制，或用新版默认值冒充原配置。核验模型、TP2、4096、512 MiB、版本和健康状态。当前线上尚未切换，因此此次发布无需执行回退。

---

## Upstream README


<p align="center">English | <a href="README.ja.md">日本語</a> | <a href="README.zh-CN.md">简体中文</a> | <a href="README.pt-BR.md">Português</a></p>

<p align="center"><img src="assets/maya-logo.png" width="128" alt="Maya logo"></p>

<h1 align="center">Project Maya</h1>

<p align="center"><b>GLM-5.3-Flash (321 B), DeepSeek-V4.1-Flash-1017 (748 B) and MiniMax-M3.1-Flash (428 B) on your own GPU</b><br>
private and fast · one NVIDIA GPU or up to 16 · AMD and Windows · chat, pictures, OpenAI- and
Anthropic-compatible API · several requests at once</p>

<p align="center">
<a href="https://huggingface.co/peasantsmith/GLM-5.3-Flash-Maya-GGUF"><img src="https://raw.githubusercontent.com/mw00/project-maya/badges/glm.svg" alt="GLM-5.3-Flash Maya downloads"></a>
<a href="https://huggingface.co/peasantsmith/GLM-5.3-Flash-Maya-M-Derisked-IQ2_S-GGUF"><img src="https://raw.githubusercontent.com/mw00/project-maya/badges/glm-m-derisked.svg" alt="GLM-5.3-Flash Maya-M-Derisked downloads"></a>
<a href="https://huggingface.co/peasantsmith/DeepSeek-V4.1-Flash-1017-Maya-GGUF"><img src="https://raw.githubusercontent.com/mw00/project-maya/badges/deepseek.svg" alt="DeepSeek-V4.1-Flash Maya downloads"></a>
<a href="https://huggingface.co/peasantsmith/MiniMax-M3.1-Flash-Maya-GGUF"><img src="https://raw.githubusercontent.com/mw00/project-maya/badges/minimax.svg" alt="MiniMax-M3.1-Flash Maya downloads"></a>
</p>

<p align="center">Created and led by <b>Peasant Smith</b> (<a href="https://github.com/mw00">@mw00</a>), co-maintained
with <b><a href="https://github.com/needmorevram">@needmorevram</a></b></p>

<p align="center"><a href="https://github.com/sponsors/mw00"><img src="https://img.shields.io/badge/Sponsor_on_GitHub-ea4aaa?logo=githubsponsors&logoColor=white&style=for-the-badge" alt="Sponsor on GitHub" height="40"></a>
<a href="https://buymeacoffee.com/peasantsmith"><img src="https://img.shields.io/badge/Buy_me_a_coffee-FFDD00?logo=buymeacoffee&logoColor=black&style=for-the-badge" alt="Buy Me A Coffee" height="32"></a><br>
<sub>Project Maya is free. If it runs well on your PC, sponsoring it pays for the test hardware and the work on it.</sub></p>

Models this size usually need a datacenter. Maya runs three of them on the hardware you already have:
**GLM-5.3-Flash**, **DeepSeek-V4.1-Flash-1017** and **MiniMax-M3.1-Flash**. Each model has an engine built for it. It
keeps the experts your conversation uses most on the GPU, the next ones in RAM and the rest on your NVMe SSD, and moves
them as you chat. It measures your GPUs, CPU, RAM and SSD and tunes itself to them. Nothing leaves your machine.

- **Fast:** up to 149 tokens/s on two RTX 5090s and an RTX 4090, 124 on two H100s, 118 on four RTX 4090s, 81 on one
  RTX PRO 6000, 30+ on a single RTX 5090. [Real numbers from real users](#how-fast-is-it).
- **Close to the original:** Maya's own quants of GLM-5.3-Flash keep 97.7-99.2% of the FP8 model's zero-shot
  accuracy; DeepSeek V4.1 · Maya-S picks the same next token as the original 92.6% of the time, MiniMax M3.1 · Maya-S
  87.6% (the average over its categories).
- **Plugs into your tools:** OpenAI- and Anthropic-compatible API with tool calls, ready for coding agents.
- **Several requests at once:** up to 8 answered together with GLM-5.3-Flash, DeepSeek-V4.1-Flash and
  MiniMax-M3.1-Flash - for agents that run several sessions, or several people on one PC.
- **Everything in the box:** a chat and a live monitor in the browser, pictures on every model, thinking levels, a
  Models page that downloads, loads and deletes models, a Turbo button, a one-command setup.

## Quick start

```sh
git clone https://github.com/mw00/project-maya.git && cd project-maya
./setup.sh
```

The setup asks which model you want, downloads it, builds everything once and **starts the model**. Then open
`http://127.0.0.1:8080`. Windows: `START-MAYA.bat` instead of `./setup.sh`. More in [Install](#install).

All documentation: [docs/README.md](docs/README.md)

## <a name="the-models"></a><a name="deepseek-v41-flash-1017-new"></a><a name="minimax-m31-flash-new"></a>Pick your model

Every Maya quant is named after its model, then its tier - XS, S, M or L, by how many bits its experts keep. The setup
recommends one for your PC; to pick one yourself, give its setup id: `./setup.sh --model glm-maya-s24`.

| Model | Maya quant · setup id | Size | Made for |
| --- | --- | ---: | --- |
| **[GLM-5.3-Flash](https://huggingface.co/zai-org/GLM-5.3-Flash)**<br><sub>zai-org · MIT · 321 B, 18 B active · up to 1M context</sub> | **Maya-S** (default) · `glm-maya-s` | 96.5 GB | PCs with a smaller memory pool (RAM and VRAM together) |
| | Maya-S24 · `glm-maya-s24` | 94.7 GB | cards of 24 GB or less: ~1.5 GB more of the card for experts, decode +14% |
| | Maya-XS · `glm-maya-xs` | 73.4 GB | PCs with 64 GB of RAM and a 24 GB card, at 128K context |
| | Maya-M · `glm-maya-m` | 116 GB | a bigger memory pool; calibrated toward tool calls and front-end code |
| | Maya-M-Derisked · `glm-maya-m-derisked` | 116 GB | Maya-M with a weight edit by [Blackfrost_AI](https://x.com/Blackfrost_AI) that reduces blanket refusals; experimental |
| | Maya-L · `glm-maya-l` | 156.3 GB | the biggest memory pools; the closest to the original |
| **[DeepSeek-V4.1-Flash-1017](https://huggingface.co/deepseek-ai/DeepSeek-V4.1-Flash-1017)**<br><sub>DeepSeek · MIT · 748 B</sub> | **Maya-S** (default) · `deepseek-maya-s` | 340 GB | any amount of memory: what VRAM and RAM don't hold is read from the SSD |
| | Maya-XS (v2) · `deepseek-maya-xs` | 277 GB | the whole model in a 128 GB PC's memory (the Engram tables stay on the SSD); v2 is about 12% closer to the original and about 20% faster at writing than v1 - running the setup again offers the update |
| **[MiniMax-M3.1-Flash](https://huggingface.co/MiniMaxAI/MiniMax-M3.1-Flash)**<br><sub>MiniMax · [MiniMax Community License](LICENSE.minimax-m31) · 428 B, 23 B active · up to 1M context</sub> | Maya-XS · `minimax-maya-xs` | 93 GB | the whole model in a 128 GB PC's memory |
| | Maya-S · `minimax-maya-s` | 122 GB | closer to the original |

All of them run on NVIDIA from the V100 / RTX 20 on, and on AMD and Windows (see
[What you need](#what-you-need)). DeepSeek's sizes include its Engram tables. The setup also offers unsloth's GGUF and
LibertAIDAI's EXL3 quants of DeepSeek, orcarouter's abliterated DeepSeek, Mia-AiLab's EXL3 quant of MiniMax,
turboderp's EXL3 quants of GLM, Qwen-3.8-Flash-Next (CYBER-FROST) as its Q5_K_M GGUF and Qwen-3.8-Flash-Next
itself as unsloth's UD-IQ2_XS GGUF, both with pictures
([its settings](docs/SETTINGS.md#qwen-38-flash-next-cyber-frost)). **Each model's files, drafters, pictures and options:
[docs/MODELS.md](docs/MODELS.md).**

## Quality

Maya's quants are made from each model's released weights and measured against the original:

- **GLM-5.3 · Maya quants** keep 97.7-99.2% of the FP8 model's zero-shot accuracy and pick its next token 83-90% of
  the time (Maya-L: 99.2%, 90%). How they're made and measured: [MAYA-S](bench/results/MAYA-S.md),
  [MAYA-S24](bench/results/MAYA-S24.md), [MAYA-M](bench/results/MAYA-M.md), [MAYA-L](bench/results/MAYA-L.md).
- **DeepSeek V4.1 · Maya-S** picks the same next token as the original 92.6% of the time.
- **MiniMax M3.1 · Maya-S** picks it 87.6% of the time (the average over its categories).

More on each model's card on [Hugging Face](https://huggingface.co/peasantsmith).

## How fast is it?

Real machines measured by our users, most with `./maya.sh --bench` (decode: three greedy 256-token answers; prefill:
an 8K-token prompt unless the row says otherwise). Share yours: `--bench`, then `--report`, in a
[GitHub issue](https://github.com/mw00/project-maya/issues).

<details><summary><b>GLM-5.3-Flash speed benchmarks</b></summary>

| Machine | Model | Decode (writing the answer) | Prefill (reading your prompt) | By |
| --- | --- | ---: | ---: | --- |
| **2x RTX 5090 32 GB + RTX 4090 48 GB** (PCIe 5.0 x8, 5.0 x8, 4.0 x4), Core i7-13700K, 188 GB RAM | Maya-S / Maya-M-Derisked (DFlash2), 32K context | **148.6 / 123.4 tokens/s** | 2605 / 2341 tokens/s | @prismaillya17 (v1.1.26, #498) |
| **2x H100 80 GB** (PCIe 5.0), Xeon Platinum 8488C, 504 GB RAM | Maya-L / Maya-M-Derisked, 1M context | **122.4 / 123.7 tokens/s** | 2099 / 1986 tokens/s | @Molyleaf (v1.1.11, #258, #259) |
| **4x RTX 4090 24 GB** (PCIe 4.0 x16), Xeon w7-3445, 62 GB RAM | Maya-S24 | **118 tokens/s** | 4129 tokens/s | @0xPreDa (v1.0.31, #89) |
| **8x RTX 3090 24 GB** (PCIe 4.0, six x16 and two x8), EPYC 7402P, 126 GB RAM | EXL3-4.05bpw (DFlash2), 1M context | **117.4 tokens/s** | 1470 tokens/s | @MarkMakers (v1.1.15, #414) |
| **4x RTX 3090 24 GB** | Maya-S24 / Maya-M (DFlash2), 256K context | **100.8 / 86.5 tokens/s** | 979 / 907 tokens/s | @Whamp (v1.1.19, #349) |
| **2x A100 80 GB** (PCIe 4.0 x16), EPYC 7742 (a VM, 25 threads), 161 GB RAM | Maya-M, 128K context | **91.9 tokens/s** | - | @driona-will-fluff (v1.1.19, #326) |
| **RTX PRO 6000 Blackwell 96 GB**, Ryzen 7 9800X3D, 61 GB RAM | Maya-M, 256K context | **81.0 tokens/s** | 3102 tokens/s | @yundddd (v1.1.7, #172) |
| **4x Tesla V100 32 GB** (PCIe 3.0 x16), 2x Xeon E5-2630 v4, 126-504 GB RAM, SATA SSD | Maya-S / Maya-M / Maya-L, 32K context | **69.7 / 67.2 / 43.3 tokens/s** | 730 / 713 / 627 tokens/s | @HansStrupat (v1.1.36, #327, #340, #653) |
| **4x Tesla V100-SXM2 32 GB** (NVLink 2.0) | Maya-S, 32K context | 68.4 tokens/s | 549 tokens/s | @jeremiahlangner (v1.1.18, #333) |
| **1x H100 80 GB** (PCIe 5.0 x16), Xeon Platinum 8488C, 504 GB RAM | Maya-M-Derisked, 1M context | 61.4 tokens/s | 2014 tokens/s | @Molyleaf (v1.1.15, #260) |
| **2x NVIDIA CMP 170HX 64 GB** (PCIe Gen2 x4 each), 2x Xeon E5-2690 v4, 91 GB RAM | Maya-M, every expert in VRAM | 61.3 tokens/s | 514 tokens/s | @ZackO2o (v1.0.6, #22) |
| **2x RTX A6000 48 GB** (PCIe 4.0 x8), Core i9-13900K, 128 GB RAM, SATA SSD | Maya-S / Maya-M / Maya-L, 131K-token prompts | 60.7 / 50.9 / 32.1 tokens/s | 1726 / 1629 / 1298 tokens/s (131K) | @everson (v1.1.20, #371) |
| **2x RX 7900 XTX 24 GB** (PCIe 4.0 x4 each) + **Radeon 8060S** helper (Ryzen AI Max+ 395, 128 GB) | Maya-M, up to 512K context | 56.0 tokens/s (50.1 after a 456K-token prompt) | 907 tokens/s (114K) | @pepuscz (v1.1.25 + #454-#457, #458) |
| **2x RTX 5880 Ada 45 GB** (PCIe 4.0 x16), Xeon Gold 5420+, 393 GB RAM | Maya-L, 256K context | 54.5 tokens/s | 1401 tokens/s | @vjsyong (v1.1.4, #149) |
| **2x NVIDIA CMP 170HX 40 GB** (PCIe Gen2 x16 each), Xeon w3-2423, 128 GB DDR5 | Maya-S, 128K context | 54.3 tokens/s | 831 tokens/s | @nvsofts (v1.1.15, #263) |
| **2x RTX 3090 24 GB** (PCIe 4.0 x16, NVLink, power-capped at 252 W), Threadripper PRO 3975WX, 512 GB RAM | Maya-M / Maya-L, 128K context | 54.1 / 40.3 tokens/s | 936 / 850 tokens/s | @ksim63-gif (v1.1.23, #404) |
| **2x RTX 3090 24 GB** (PCIe 4.0 x16, power-capped at 225 W), Threadripper 3970X, 128 GB RAM | Maya-S24 | 53.0 tokens/s | 885 tokens/s | @briepala (v1.1.2, #135) |
| **3x Tesla V100 32 GB** (PCIe 3.0 x16), 2x Xeon E5-2696 v3, 128 GB DDR3L, Windows 10 | Maya-S, 128K context | 52.9 tokens/s | 616 tokens/s | @noahark (v1.1.20, #346) |
| **RTX PRO 6000 Blackwell 96 GB** (PCIe 5.0 x16), Ryzen 9 9950X3D, 253 GB RAM, Windows 11 | Maya-L, 512K context | 48.8 tokens/s | 3810 tokens/s | @Andrey0710 (v1.1.43, #730) |
| **4x Tesla V100 32 GB** (PCIe 3.0 x16), 24 CPU cores, a 100 GiB RAM limit | Maya-L, 32K context | 48.3 tokens/s (after `--calibrate`) | 499 tokens/s | @areslp (v1.1.3, [#130](https://github.com/mw00/project-maya/issues/130)) |
| **2x Radeon AI PRO R9700 32 GB** (gfx1201, PCIe 4.0 x16 each, 240 W), EPYC 7313, 256 GB RAM | Maya-M-Derisked, 256K context | 44.4 tokens/s | 703 tokens/s | @knoodrake (v1.1.28, #509) |
| **4x RTX 3090 24 GB** (PCIe 4.0 x8 behind one switch), Xeon Platinum 8260M, 376 GB RAM | Maya-L, 1M context | 44.2 tokens/s (tuned) | 698 tokens/s | @foureight84 (v1.1.17, #302) |
| **2x Radeon AI PRO R9700 32 GB** (gfx1201, PCIe 4.0 x8 each), Ryzen 5 3600, 64 GB RAM | Maya-S24 / Maya-M-Derisked, 256K context | 44.1 / 24.9 tokens/s | 704 / 617 tokens/s | @gegnep (v1.1.20, #395) |
| **2x RTX 3090 24 GB** (PCIe 4.0 x16, power-capped at 250 W), EPYC 7452, 256 GB RAM | Maya-M / Maya-L, 128K context | 44.0 / 30.2 tokens/s | 860 / 801 tokens/s | @orochifj (v1.1.4, #165) |
| **8x AMD Instinct MI50 32 GB** (gfx906, PCIe), EPYC 7K62, 112 GB RAM | Maya-L, 128K context | 43.3 tokens/s | 288 tokens/s | @abluesheep (v1.1.52, #830) |
| **RTX PRO 6000 Blackwell 96 GB** (PCIe 5.0 x16), Ryzen 9 9950X3D, 253 GB RAM, Windows 11 | Maya-L, a coding-agent session grown from 265K to 482K tokens | 42.4 tokens/s (median of 51 answers) | - | @Andrey0710 (v1.1.53, #873) |
| **2x RTX 4090 24 GB** (PCIe 3.0 + 4.0 x16), 2x Xeon Silver 4309Y, 188 GB RAM | Maya-S24 | 40.5 tokens/s | 1271 tokens/s | @fuad00 (v1.1.0, #122) |
| **RTX PRO 6000 Blackwell 96 GB** (PCIe 4.0 x16), Ryzen 5 5600, 92 GB DDR4 | Maya-L, 1M context | 39.6 tokens/s | 2708 tokens/s (3246 warm) | @QueTensors (v1.1.28, #532) |
| **4x Tesla T10 15 GB** (PCIe 3.0 x16), Xeon E5-2680 v4, 63 GB RAM | Maya-S24, 100K context | 38.4 tokens/s | 393 tokens/s | @zeallag (v1.1.16, #291) |
| **2x Tesla V100 32 GB** (PCIe 3.0 x8 each), Ryzen 9 7945HX3D, 96 GB RAM | Maya-M / Maya-L | 33.8 / 13.3 tokens/s | 490 / 372 tokens/s | @nkhaooat (v1.1.11, #208) |
| **RTX 3090 24 GB + RTX 3060 12 GB** (PCIe 4.0 x16), EPYC 7702P (8-channel DDR4), 141 GB RAM (a VM) | Maya-M, 128K context | 33.7 tokens/s | 682 tokens/s | @erikash (v1.1.4, #157) |
| **AMD Radeon 8065S** (Ryzen AI Max+ PRO 495, 192 GB, 160 GB of it for the GPU), Windows 11 | Maya-S, 32K context | 31.5 tokens/s | 342 tokens/s | @tobitege (v1.1.37, #663) |
| **RTX 5090 32 GB**, Ryzen 9 7950X, 96 GB DDR5 | Maya-M | 30+ tokens/s | ~2000 tokens/s | @siganos (#43) |
| **2x RTX 3090 24 GB** (PCIe 4.0 x16, NVLink), Threadripper PRO 3955WX, 247 GB RAM | EXL3-3.05bpw, 128K context | 29.2 tokens/s | - | @catonooka (v1.1.9, #187) |
| **2x Tesla P40 24 GB** (Pascal, PCIe 3.0 x16), EPYC 7302P (a VM), 184 GB RAM | Maya-S24 / Maya-L | 28.0 / 16.9 tokens/s | 202 / 194 tokens/s | @Jw88888 (v1.1.11 + #214, #212) |
| **RX 7900 XTX 24 GB + Radeon 8060S** helper (Ryzen AI Max+ 395, 128 GB) | Maya-S24, 114K-token prompts | 27.5 tokens/s | 836 tokens/s (114K) | @pepuscz (v1.1.17, #311) |
| **1x RTX 3090 24 GB** (PCIe 4.0 x16), EPYC 7702P (8-channel DDR4), 141 GB RAM (a VM) | Maya-M, 128K context | 27.0 tokens/s | 1069 tokens/s | @erikash (v1.1.4, #157) |
| **RTX 4080 SUPER 32 GB** (a 32 GB modded card), Threadripper 3970X, 157 GB RAM, SATA SSD | Maya-S24, 262K context | 26.7 tokens/s | 1552 tokens/s | @Micro-ATP (v1.1.11, #200) |
| **3x Tesla V100 16 GB** (PCIe 3.0 x16), 2x Xeon E5-2690 v4, 62 GB RAM | Maya-S, 128K context | 26.6 tokens/s | 497 tokens/s | @saatvik-viniak (v1.1.11, #211) |
| **4x RTX 3080 20 GB**, 64 GB DDR4 | Maya-M, 512K context | 26.3 tokens/s (a day of use) | - | @CoolJohn-lab (#104) |
| **1x RTX 3090 24 GB** (power-capped at 225 W), Threadripper 3970X, 128 GB RAM | Maya-S24 | 26.0 tokens/s | 913 tokens/s | @briepala (v1.1.2, #135) |
| **1x L40S 48 GB** (vGPU), Xeon Platinum 8580 (a VM, 32 vCPUs), 504 GB RAM | UD-Q4_K_XL / UD-Q5_K_XL / Q8_0, 512K context | 26.0 / 22.3 / 17.8 tokens/s | 1987 / 1877 / 1750 tokens/s (185K) | @talisp (v1.1.32, #577) |
| **RTX 4090 24 GB** (PCIe 4.0 x16), 2x Xeon Silver 4310, 251 GB RAM | Maya-S24, 32K context | 25.7 tokens/s | 1963 tokens/s | @oscarlius (v1.1.24, #420) |
| **RTX PRO 4000 Blackwell 24 GB**, Core i9-13900K, 128 GB DDR5 | Maya-S24 | 24.7 tokens/s | 1146 tokens/s | @morrisfamily (v1.1.7, #174) |
| **2x Tesla V100 16 GB** (PCIe 3.0 x16), 2x Xeon E5-2696 v4, 128 GB DDR3-1866 | Maya-M, 256K context | 24.6 tokens/s (normal sampling) | 1092 tokens/s (12K) | @c200762620-max (v1.1.49, #790) |
| **RTX 5090 32 GB** (PCIe 5.0 x16), Core i9-13900K, 64 GB RAM, Windows 11 | Maya-S, 128K context | 23.0 tokens/s | 2173 tokens/s | @gianbing (v1.1.3, #139) |
| **4x Tesla T4 16 GB**, Xeon E5-2660 v2, 220 GB RAM | Maya-S | 22.6 tokens/s | 416 tokens/s | @ksanislo (v1.0.16, #36) |
| **2x RTX 2080 Ti 22 GB** (modded, NVLink, PCIe 3.0 x16, 250 W), Xeon E5-2686 v4, 247 GB RAM, SATA SSD | Maya-M, 1M context | 22.0 tokens/s | 533 tokens/s | @blackxwhite88 (v1.1.36, #675) |
| **Radeon AI PRO R9700 32 GB** (gfx1201), Core i5-14400F, 128 GB RAM | Maya-S | 21.4 tokens/s | - | @potatopote-debug (v1.1.12, #213) |
| **1x NVIDIA CMP 170HX 64 GB** (PCIe Gen2 x16), Core i7-9700K, 32 GB RAM | Maya-S24, 32K context | 20.6 tokens/s | 797 tokens/s | @Kanorin-chan (v1.1.8, #281) |
| **2x Quadro RTX 5000 16 GB** (Turing, PCIe 3.0 x16 each, 200 W), Xeon W-2133, 94 GB RAM | Maya-S, 128K context | 18.8 tokens/s | 494 tokens/s | @arpleasa (v1.1.39, #707) |
| **AMD Radeon 8060S** (Ryzen AI Max+ 395, 128 GB, 96 GB of it for the GPU), Windows 11 | Maya-S | 18.3 tokens/s | 320 tokens/s | @LIN-LAB-AI (v1.0.30, #102) |
| **RTX 3090 Ti 24 GB** (PCIe 3.0 x16, a VM), EPYC 7302P, 240 GB RAM | Maya-L, 32K context | 18.2 tokens/s (tuned, #177) | 791 tokens/s | @Jw88888 (v1.1.7, #177) |
| **3x Tesla P100 16 GB** (Pascal; PCIe 3.0 x8, 3.0 x8, 2.0 x4), Ryzen 5 2600, 64 GB RAM | Maya-S24, 64K context | 17.9 tokens/s | 105 tokens/s | @maxlawrie (v1.1.28 + #479, #504) |
| **RTX 4090 24 GB**, Ryzen 7 9700X, 64 GB RAM | Maya-S24, 32K context | 16.6 tokens/s | 1320 tokens/s | @MagikEh (#274) |
| **RTX 5070 12 GB** (PCIe 4.0 x16), EPYC 7302 (4-channel DDR4), 126 GB RAM | Maya-S24, 64K context | 16.6 tokens/s | 323 tokens/s | @hugou74130 (v1.1.45, #755) |
| **NVIDIA DGX Spark** (GB10, 121 GB unified memory) | Maya-S, 128K context | 16.5 tokens/s | ~730 tokens/s (6K prompt) | @ericoh88 (v1.1.15, #282) |
| **RTX 4090 24 GB**, Ryzen 9 9950X3D, 192 GB DDR5, Windows 11 | Maya-L, 128K context | 16.4 tokens/s | 1371 tokens/s | @npc97 (v1.0.24, #63) |
| **RTX 5090 Laptop 24 GB**, Core Ultra 9 275HX, 64 GB RAM, Windows 11 (a laptop) | Maya-S24 | 14.5 tokens/s | 994 tokens/s | @klvnblst (v1.0.30, #67) |
| **2x RTX 4060 Ti 16 GB** (PCIe 3.0 x8 each), Core i7-10700K, 128 GB DDR4, SATA SSD | UD-IQ3_XXS, 128K context | 13.0 tokens/s | 385 tokens/s | @oisee (v1.1.42, #723) |

</details>

<details><summary><b>DeepSeek-V4.1-Flash speed benchmarks</b></summary>

| Machine | Model | Decode (writing the answer) | Prefill (reading your prompt) | By |
| --- | --- | ---: | ---: | --- |
| **8x RTX 5090 32 GB**, Xeon Gold 6530, 1 TB RAM | Maya-S, 512K context, `--parallel 4` | **184.2 tokens/s** | 2510 tokens/s | @etevrfed34-commits (v1.1.34, #585) |
| **10x RTX 3090 24 GB** (tensor parallel 2, PCIe 4.0 x4 each, 200 W), Xeon (24 threads), 64 GB RAM | EXL3 2.9 bpw, 1M context | **97.0 tokens/s** | 2390 tokens/s (128K) | @DoktorMincs (v1.1.28, #494) |
| **RTX 6000D 85 GB** (PCIe 5.0 x16), Threadripper PRO 9965WX, 251 GB RAM | EXL3 2.9 bpw, 512K context | **89.3 tokens/s** | 1687 tokens/s (2836 at 33K) | @Ross-Yang-94 (v1.1.25, #423) |
| **2x A100-SXM4 80 GB**, Xeon Platinum 8360Y, 1 TB RAM | Maya-S, 1M context | **87.4 tokens/s** | 2084 tokens/s | @SunYong0821 (v1.1.34, #445, #594) |
| **2x RX 7900 XTX 24 GB** (tensor parallel 2, PCIe 4.0 x4 each) + **Radeon 8060S** helper (Ryzen AI Max+ 395, 128 GB), the model on a USB4 NVMe | Maya-XS (v2), 512K context, `DS4_INDEX_TC=1` | **82.5 tokens/s** | 1587 tokens/s (130K; 663 at 8K) | @pepuscz (v1.1.50 + #801, #797) |
| **RTX PRO 6000 Blackwell 96 GB** (PCIe 5.0 x16), Ryzen 9 9950X3D, 253 GB RAM, Windows 11 | Maya-S, 256K context | 72.3 tokens/s | 1757 tokens/s | @Andrey0710 (v1.1.43, #737) |
| **2x RTX PRO 5000 Blackwell 72 GB**, Ryzen 9 9950X3D, 249 GB DDR5 | Maya-S, 512K context | 68-71 tokens/s (prose), up to 162 (code), tuned | ~3700 tokens/s (42K) | @realsquall1980-art (v1.1.28, #424) |
| **4x RTX 4090 24 GB** (PCIe 4.0 x16), 2x Xeon Platinum 8380, 251 GB RAM, SATA SSD | Maya-S, 256K context | **67.5 tokens/s** | 1087 tokens/s (400 at 2K) | @oscarlius (v1.1.51) |
| **1x A100-SXM4 80 GB**, Xeon Platinum 8360Y, 1 TB RAM | Maya-S, 1M context | 60.2 tokens/s | 241 tokens/s | @SunYong0821 (v1.1.34, #594) |
| **4x AMD Instinct MI100 32 GB** (gfx908, PCIe 4.0 x16 each), EPYC 7402, 252 GB RAM | Maya-XS, 32K context | 51.8 tokens/s | 172 tokens/s | @nsaxploit (v1.1.22, #396) |
| **4x RTX 3090 24 GB** (PCIe 3.0, SM clocks capped at 1500 MHz), Threadripper 1950X, 126 GB RAM | Maya-S, 256K context | 50-53 tokens/s (code), 40-43 (prose) | 947 tokens/s (10K), 1954 (90K) | @mgabor3141 (v1.1.26, #448) |
| **2x NVIDIA A40 48 GB** (PCIe 4.0 x16), 2x Xeon Gold 6330, 256 GB RAM, Windows Server 2022 | Maya-S, 256K context | 48.9 tokens/s (tuned) | ~660 tokens/s (3K) | @yjyz1011 (v1.1.28, #508) |
| **2x RTX PRO 6000 Blackwell 96 GB** (tensor parallel 2, PCIe 5.0), Core Ultra 7 270K Plus, 188 GB DDR5 | UD-Q8_K_XL, 1M context, DSpark | 48.3 tokens/s | 1245 tokens/s (3360 at 48K) | v1.1.51, #819 |
| **RTX 4090 24 GB** (PCIe 4.0 x8), Ryzen 9 9950X3D, 192 GB DDR5 | Maya-XS, 64K context | 44.1 tokens/s | 539 tokens/s | @naeq (v1.1.27, #503) |
| **4x Tesla V100 32 GB** (PCIe 3.0 x16), 2x Xeon E5-2630 v4, 504 GB RAM | Maya-S / UD-Q4_K_XL / UD-Q8_K_XL, 64K context | 44.0 / 28.6 / 29.7 tokens/s | 592 / 236 / 233 tokens/s | @HansStrupat (v1.1.49, #785) |
| **1x L40S 48 GB** (vGPU), Xeon Platinum 8580 (a VM, 32 vCPUs), 504 GB RAM | UD-Q8_K_XL, 256K context | 43.2 tokens/s | 1862 tokens/s (174K) | @talisp (v1.1.32, #577) |
| **AMD Radeon 8065S** (Ryzen AI Max+ PRO 495, 192 GB, 160 GB of it for the GPU), Windows 11 | orcarouter abliterated Mix-128G, 256K context, DSpark | 43.2 tokens/s | 453 tokens/s | @tobitege (v1.1.37, #663) |
| **2x RX 7900 XTX 24 GB** (tensor parallel 2, PCIe 4.0 x4 each) + **Radeon 8060S** helper (Ryzen AI Max+ 395, 128 GB), the model on a USB4 NVMe | Maya-S, 512K context, `DS4_INDEX_TC=1` | 41.7 tokens/s (49.4 after a 130K-token prompt, cached) | 1378 tokens/s (130K; 478 at 8K) | @pepuscz (v1.1.48 + #801, #797) |
| **4x RTX 3090 24 GB** (PCIe 4.0 x16), Threadripper PRO 3975WX, 502 GB RAM | UD-Q8_K_XL, 256K context | 37.1 tokens/s | 596 tokens/s | @bandito1 (v1.1.50, #766) |
| **4x RX 7900 XT 20 GB** (gfx1100, `--tp 4`, tuned), 2x Xeon E5-2699C v4 (a Dell T7910), 1 TB DDR4-2400 | UD-Q4_K_XL | 35.0 tokens/s | - | @Cass67 (v1.1.32, #566) |
| **2x RTX 3090 24 GB** (PCIe 4.0 x8, 270 W), Core Ultra 7 265K, 192 GB DDR5 | Maya-S, 1M context, tuned | 33.3 tokens/s | 467 tokens/s | @Jellepepe (v1.1.35, #614) |
| **2x Tesla V100 16 GB** (PCIe 3.0 x16), 2x Xeon E5-2696 v4, 128 GB DDR3-1866 | Maya-XS (v2), 256K context, DSpark | 32.0 tokens/s | 420 tokens/s | @c200762620-max (v1.1.51, #815) |
| **3x Tesla V100 32 GB** (PCIe 3.0 x16), 2x Xeon E5-2696 v3, 128 GB DDR3L, Windows 10 | Maya-S, 64K context | 23-26 tokens/s (long answers) | 377 tokens/s | @noahark (v1.1.26, #450) |
| **2x RTX 2080 Ti 22 GB** (modded, PCIe 3.0 x16, power-capped at 250 W), 2x Xeon E5-2696 v4, 94 GB RAM | Maya-XS, 128K context | 23.1 tokens/s | 317 tokens/s | @NorwinYu (v1.1.36, #659) |
| **2x RTX 4090 24 GB** (PCIe 3.0 + 4.0 x16), 2x Xeon Silver 4309Y, 188 GB RAM | Maya-XS, 64K context | 21.5 tokens/s | 573 tokens/s | @fuad00 (v1.1.22, #393) |
| **RTX 4000 Ada 20 GB**, Xeon w7-3455, 255 GB DDR5, Windows 11 | Maya-XS, 256K context | 21.1 tokens/s | 518 tokens/s (20K) | @pietroMastro92 (v1.1.29, #534) |
| **2x Tesla V100 32 GB** (PCIe 3.0 x8 each), Ryzen 9 5900XT, 128 GB DDR4-3200 | Maya-S, 64K context | 20.8 tokens/s | 344 tokens/s | @pirosap (v1.1.34, #586) |
| **RX 7900 XTX 24 GB** (gfx1100, PCIe 4.0 x16), Core i7-14700KF, 63 GB RAM, ROCm 10.2 | Maya-XS, 64K context, DSpark | 18.4 tokens/s | 395 tokens/s (146 at 2K) | @Cayaf89 (v1.1.46, [#757](https://github.com/mw00/project-maya/issues/757)) |

</details>

<details><summary><b>MiniMax-M3.1-Flash speed benchmarks</b></summary>

| Machine | Model | Decode (writing the answer) | Prefill (reading your prompt) | By |
| --- | --- | ---: | ---: | --- |
| **RTX PRO 6000 Blackwell 96 GB** (PCIe 5.0 x16), Ryzen 9 9950X3D, 253 GB RAM, Windows 11 | Maya-S, 256K context, DSpark | **48.6 tokens/s** | 757 tokens/s | @Andrey0710 (v1.1.43, #738) |
| **RTX 3090 24 GB + 8x RTX 5060 Ti 16 GB** (PCIe 3.0 x8 each), 183 GB RAM | EXL3 3.5 bpw, tensor parallel 4 + DSpark, 8K context | 38.2 tokens/s (code), 32.4 (prose) | - | @needmorevram (#477) |
| **1x L40S 48 GB** (vGPU), Xeon Platinum 8580 (a VM, 32 vCPUs), 504 GB RAM | Maya-S / EXL3 3.55 bpw, 256K context | 23.4 / 16.7 tokens/s | 332 / 273 tokens/s (158K) | @talisp (v1.1.32, #577) |
| **2x RTX 3090 24 GB** (power-capped at 230 W), EPYC 7543 (a VM, 32 vCPUs), 224 GB RAM | Maya-S, 200K context, DSpark | 22.0 tokens/s (code), 18.4 (prose) | 163 tokens/s (10K) | @noonghunna (v1.1.30, #549) |
| **AMD Radeon 8065S** (Ryzen AI Max+ PRO 495, 192 GB, 160 GB of it for the GPU), Windows 11 | Maya-S, 128K context, DSpark | 21.0 tokens/s | 139 tokens/s | @tobitege (v1.1.37, #663) |
| **2x Tesla V100 16 GB** (PCIe 3.0 x16), 2x Xeon E5-2696 v4, 128 GB DDR3-1866 | Maya-XS, 256K context, DSpark | 14.5 tokens/s | 254 tokens/s | @c200762620-max (v1.1.51, #816) |

</details>

A token is about ¾ of a word. The speed holds as a conversation grows, and the first answers after a start are the
slowest while the caches fill with what you use.

## Using it

- **In the browser:** chat (kept in your browser, with search, pins and folders, per-chat instructions and thinking
  level, edit and regenerate, HTML previews), a live **Monitor** of the model and your hardware (down to what fills
  each GPU's memory), a context size and port you can change in Settings, and **Models**: every model set up on this
  machine, switching to another (Maya starts again with it) and each one's engine settings (context, GPUs, drafting,
  images, memory), saved into its `maya-<model>.json`.
- **Models page:** the catalog lists every Maya model with its size. Download one with a progress bar, load it, unload
  the running one or delete one - no command line needed.
- **Turbo:** one button beside the model's parameters sets the fastest settings for the model (greedy sampling, no
  thinking, expert skipping, the fastest drafting). It trades some quality for speed; switching it off restores your
  settings exactly.
- **Saved:** the sidebar's Saved line adds up what the model's own API (DeepSeek's, Z.ai's or MiniMax's) would have
  charged for the same tokens - the prompt Maya reuses from its cache at the API's cached price, DeepSeek's peak hours
  at its peak price - kept per model in `token-savings.json`; its prices can be changed in **Settings › Savings**.
- **Your apps and coding agents:** an OpenAI-compatible API at `http://127.0.0.1:8080/v1`, and Anthropic's at
  `http://127.0.0.1:8080/v1/messages` (Claude Code: `ANTHROPIC_BASE_URL=http://127.0.0.1:8080`). Tool calls are
  streamed as the model writes them, and a long prompt's reading progress too when a request asks for it
  (`"return_progress": true`). A web page served from another address can call it too once you allow its address
  (`"cors_origins"`, [docs/SETUP.md](docs/SETUP.md#web-pages-on-another-address)).
- **Responses API:** `/v1/responses` supports text/image conversations, streaming, function tool calls,
  schema-validated output, background tasks with cancellation/replay, and optional shared in-memory history
  (`store:true`, `previous_response_id`). Responses are not stored by default. Options and limits:
  [docs/RESPONSES.md](docs/RESPONSES.md).
- **Model API:** `/api/tags`, `/api/ps`, `/api/show`, `/api/chat`, `/api/generate`, `/api/pull` and `/api/delete` list,
  load, download and delete models from any app or script. A request for a model that is not loaded swaps to it, and
  `keep_alive` unloads it when idle. `/models/load`, `/models/unload` and `/models/sse` are on by default too
  (`"model_management": false` turns them off).
- **Thinking:** Off, Low, High (the default) or Max, in Settings or per request, and a **thinking budget** field that
  sets how many tokens the model may think before it answers.
- **Loop guard:** on by default - an agent that repeats the same tool call with the same result is steered off it.
- **Pictures:** attach one in the chat, or send image parts through either API. Every model reads them (setup details per
  model: [docs/MODELS.md](docs/MODELS.md)). Videos too, for the models that read them.
- **Files:** attach any file in the chat. Text and code go in as they are; **PDFs** as their text, or a scanned one's
  first pages as pictures; Word, Excel, PowerPoint, OpenDocument, EPUB and RTF files as their text. Anything Maya can't
  read says why. The same works through the APIs: OpenAI `file` / `input_file` parts and Anthropic `document` blocks
  (sent inline). poppler, when installed (`sudo apt install poppler-utils`), reads PDFs a little better; it is optional.
- **MCP tools:** list MCP servers in the config (`"mcp_servers"`; Claude Desktop's `mcpServers` block works as it is)
  and the web chat's model can call their tools. A switch in the chat's **+** menu turns them on and off. What it
  does: [serve/mcp.py](serve/mcp.py).
- **Docker:** there is no official image. [docs/DOCKER.md](docs/DOCKER.md) is a recipe to build one from a release.
- **From another device:** `./maya.sh --setup --host 0.0.0.0 --api-key <secret>`. Always set a key.
- **Tuning:** `./maya.sh --calibrate` tunes the engine to your hardware in 10-15 minutes. Every setting:
  [docs/SETTINGS.md](docs/SETTINGS.md).
- <a name="deepseek-engram-prompt-read-ahead"></a><a name="deepseek-conversations-kept"></a><a name="deepseek-loading-the-experts"></a>**Each model's own options** - GLM-5.3-Flash's several requests at once (`--parallel`), long contexts and DFlash2
  drafter; DeepSeek's kept conversations, expert loading and Engram read-ahead; MiniMax's thinking levels:
  [docs/MODELS.md](docs/MODELS.md).

## What you need

| | |
| --- | --- |
| **GPU** | NVIDIA, compute capability 7.0 or newer (V100, RTX 20 and newer): one, or up to 16 that share the model. More VRAM is faster. Pascal (Tesla P40 / P100, GTX 10) is experimental, not validated yet. **AMD:** RX 7900 XT / XTX, Radeon AI PRO R9700 / RX 9070, Strix Halo and Gorgon Halo (Radeon 8060S / 8065S); RX 6800 / 6900 and Radeon PRO V620 / W6800 not validated yet; GLM-5.3-Flash and DeepSeek-V4.1-Flash-1017 read pictures there too - [docs/AMD_MAYA.md](docs/AMD_MAYA.md) |
| **RAM** | 32 GB runs it. More RAM keeps more experts close and is faster |
| **Disk** | **A fast NVMe SSD** with room for the model (~75 GB for GLM-5.3 · Maya-XS, ~100 GB for GLM-5.3 · Maya-S, ~160 GB for GLM-5.3 · Maya-L, ~350 GB for DeepSeek V4.1 · Maya-S, ~470 GB for unsloth's Q8 GGUF of it, ~95 GB for MiniMax M3.1 · Maya-XS, ~200 GB for MiniMax-M3.1-Flash's EXL3 quant) and its picture encoder (1.1 GB). Not a hard disk |
| **System** | Linux (x86-64; a CPU with AVX2 is best), the NVIDIA driver, CUDA 12.x (CUDA 13 works for RTX 20 and newer, not for the V100 or Pascal), g++, Python 3.10+. Windows 10/11: [docs/WINDOWS.md](docs/WINDOWS.md). Not WSL2. AMD: ROCm 7 |

The setup checks all of this, prints the exact command for anything missing, and installs nothing system-wide.

## Install

Run the two commands in [Quick start](#quick-start) and answer a few questions (Enter takes the recommended choice).
The setup shows every download before it starts it, builds everything once (20-40 minutes plus the download) and
**starts the model**. Then open `http://127.0.0.1:8080`. Ctrl+C stops it, and `./setup.sh` starts it again right away.

- **Windows:** `START-MAYA.bat` instead of `./setup.sh` - see [docs/WINDOWS.md](docs/WINDOWS.md).
- **AMD:** [docs/AMD_MAYA.md](docs/AMD_MAYA.md).
- **Mac with Apple Silicon (experimental, GLM-5.3-Flash only):** `./maya.sh --backend metal` (on a Mac, `./maya.sh`
  picks Metal by itself) - see [docs/METAL.md](docs/METAL.md). Built and unit-tested on GitHub's Macs, not yet tested
  by us with the real model (we have no Mac): if you run it, please tell us how it went in an issue.
- **Intel Arc:** not supported by Maya's engine; a community report of the Maya-XS DeepSeek GGUFs on llama.cpp's SYCL backend: [docs/INTEL_ARC.md](docs/INTEL_ARC.md).
- **Updating:** the page's **Settings › About and updates**, or `git pull` then `./setup.sh`. The model isn't downloaded again.
- **Model files downloaded another way** (a download manager, another PC, a Hugging Face cache): `./maya.sh --import-model FOLDER`
  finds Maya's files in FOLDER by size and checksum, whatever they are called, and moves them into place (`--copy` when
  FOLDER is on another drive; the originals stay). Files that do not match are left alone.
- **Every option** (GPUs, context, a model folder you already have, remote access, ...): [docs/SETUP.md](docs/SETUP.md).

> **The start takes a few minutes:** the engine pins most of the free RAM (all but about 6 GB) for its expert tier,
> so other programs get little RAM while Maya runs.

## Something went wrong?

Run **`./maya.sh --report`** (Windows: `START-MAYA.bat --report`) right after a slow answer or an error, and attach the
`maya-report.txt` it writes to a [GitHub issue](https://github.com/mw00/project-maya/issues). It holds your hardware,
your setup and the engine's speed lines; nothing is sent anywhere by itself. The engine's own log is
`maya-<model>.log` in the Maya folder.

- **The build stops** ("nvcc ... cannot build for these GPUs", "unsupported GNU version", or `cuda_profiler_api.h` /
  `nvtx3/nvToolsExt.h` missing): [docs/SETUP.md](docs/SETUP.md#if-the-build-fails) has the fix for each.
- **Slow, with the SSD busy all the time:** not enough free RAM for the experts. Close programs or add RAM.
- **Windows: answers suddenly several times slower:** the card is too full and Windows moves GPU memory to RAM. Leave
  more of it free: `START-MAYA.bat --setup --vram-reserve-mib 2048`
  ([the expert pool's size](docs/SETTINGS.md#the-expert-pools-size)).
- **Windows on a Strix Halo / Gorgon Halo: other programs run out of memory, or the setup warns about the page
  file:** set the page file to the size the setup prints ([docs/AMD_MAYA.md](docs/AMD_MAYA.md)).
- **The download stopped:** run `./maya.sh` again; it continues where it stopped.
- **Port 8080 is in use:** Maya is already running, or `--port 8081` starts another one. Settings > Port moves Maya to
  another port for good (it starts again there).

## Support

Maya is free and open source, and it always will be. It exists so that anyone can run the biggest open models at home,
on hardware they already own. Sponsors make that possible:

- **More hardware runs well.** Every release is tested on our own NVIDIA and AMD machines before it ships. Macs are so
  far tested only on GitHub's virtual Macs. The current goal is 50 monthly sponsors, enough for a real Mac and newer
  GPUs.
- **New models sooner.** Every new model and quant takes many hours of GPU time to build and check.
- **Faster fixes** when something breaks on your setup.

If you enjoy running big models on your own machine, a small monthly sponsorship is the best way to keep Maya growing.
Thank you!

<p align="center"><a href="https://github.com/sponsors/mw00"><img src="https://img.shields.io/badge/Sponsor_on_GitHub-ea4aaa?logo=githubsponsors&logoColor=white&style=for-the-badge" alt="Sponsor on GitHub" height="40"></a>
<a href="https://buymeacoffee.com/peasantsmith"><img src="https://img.shields.io/badge/Buy_me_a_coffee-FFDD00?logo=buymeacoffee&logoColor=black&style=for-the-badge" alt="Buy Me A Coffee" height="32"></a></p>

Speed reports, bug reports and pull requests are welcome: [CONTRIBUTING.md](CONTRIBUTING.md) says what helps most.

## Credits and license

- **People:** Project Maya is created and led by Peasant Smith ([@mw00](https://github.com/mw00),
  [X](https://x.com/PeasantSmith)), who owns the project, its name and the Maya models.
  [@needmorevram](https://github.com/needmorevram) ([X](https://x.com/needmorevram)) is a co-maintainer and
  contributed major features: DFlash2 drafting, EXL3 support, KV streaming,
  conversation checkpoints, the layer-split search, the expert profile and the CPU expert kernels, and for
  `maya-ds4` the 3.5-bit EXL3 option, tensor parallel over 4 / 8 GPUs and the threaded expert-cache swaps (#276). Thanks also to everyone who sent code and speed reports
  ([contributors](https://github.com/mw00/project-maya/graphs/contributors)).
- **Built on [ggml / llama.cpp](https://github.com/ggml-org/llama.cpp)** (MIT License, Copyright (c) 2023-2026 The ggml
  authors): the quantization formats, the CPU dot products and the prompt path's MMQ kernels, built from a pinned
  commit (`third_party/ggml/LICENSE`).
- **EXL3 support** follows [exllamav3](https://github.com/turboderp-org/exllamav3)'s format, codebooks, CPU expert
  kernel and tensor-core decoding of a tile (MIT License, Copyright (c) 2025 Turboderp; `third_party/exllamav3/LICENSE`). **The DFlash2 drafter's
  forward** follows [beellama.cpp](https://github.com/Anbeeld/beellama.cpp)'s (MIT License, Copyright (c) 2023-2026
  The ggml authors, Anbeeld; `third_party/beellama.cpp/LICENSE`).
- The models: [GLM-5.3-Flash](https://huggingface.co/zai-org/GLM-5.3-Flash) by zai-org (MIT); Maya's quants and the
  image encoder file are made from zai-org's released weights and keep its license.
  [DeepSeek-V4.1-Flash-1017](https://huggingface.co/deepseek-ai/DeepSeek-V4.1-Flash-1017) by DeepSeek (MIT); Maya's
  quant, its Engram tables and the drafter are made from DeepSeek's released weights and keep its license; the vision
  tower comes from DeepSeek's checkpoint, the GGUF option from
  [unsloth's GGUFs](https://huggingface.co/unsloth/DeepSeek-V4.1-Flash-1017-GGUF), the EXL3 options are
  LibertAIDAI's ([2.9](https://huggingface.co/LibertAIDAI/DeepSeek-V4.1-Flash-1017-EXL3-2.9bpw) and
  [3.5 bpw](https://huggingface.co/LibertAIDAI/DeepSeek-V4.1-Flash-1017-EXL3-3.5bpw)). The optional DFlash2 drafter
  ([GLM-5.3-Flash-DFlash2-GGUF](https://huggingface.co/Anbeeld/GLM-5.3-Flash-DFlash2-GGUF) by Anbeeld) is CC BY-NC-ND
  4.0, non-commercial use only, and is downloaded only when you choose it.
- The dashboard's font: Outfit (SIL Open Font License 1.1, `serve/web/fonts/OFL.txt`); its icons:
  [Lucide](https://lucide.dev) (ISC License, `serve/web/LICENSE.lucide.txt`); its UI library:
  [Preact](https://preactjs.com) (MIT License) with [htm](https://github.com/developit/htm) (Apache License 2.0),
  bundled in `serve/web/preact.js` with their notices.
- Maya is open source under the [MIT License](LICENSE); the notices in [LICENSE](LICENSE) and `third_party/` stay
  with every copy.
