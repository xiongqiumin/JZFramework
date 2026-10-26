# FDB-v3 文本模式跑批（JZAgent / C++）

上游 `fdb_v3_upstream`（NTU 的 *Full-Duplex-Bench-v3*，语音 agent 多步工具调用 benchmark）的
`cascaded_agent.py` 是这条链路：

```
User Audio → Silero VAD → Whisper STT → LLM(tool calling) → OpenAI TTS → Agent Audio
```

本目录**只保留中间"文本进、文本出"那一段**，用 JZAgent 承担 LLM + 工具自驱循环：

```
用户文本 → JZAgent(12 个工具) → mock API → 文本回复 → result_<provider>.json（上游评测直接吃）
```

去掉的是语音侧：LiveKit 房间、VAD、STT、TTS、音频时序。**"工具调用是否正确"这个被测评的东西没变** ——
12 个工具的名称/描述/参数、system prompt、mock API 返回值全部照抄上游。

## 文件

| 文件 | 作用 |
|---|---|
| `fdb_v3_text.cpp` | **C++ / JZAgent 版**跑批器（主力） |
| `CMakeLists.txt` | 目标 `fdb_v3_text`，产物 `build/test/bin/<CFG>/fdb_v3_text.exe` |
| `text_agent.py` | Python 版 core（复用上游 `mock_apis.py`，便于快速改协议/对比） |
| `run_text_benchmark.py` | Python 版跑批器（并发，字段与 C++ 版一致） |
| `out/` | 结果（`out/<provider>/<example_id>/result_<provider>.json`） |

## 编译 & 运行

```powershell
cmake --build build --config Debug --target fdb_v3_text

# ① 真实数据目录（推荐）：100 个样本，每样本一份 metadata.json
build\test\bin\Debug\fdb_v3_text.exe `
  --data-dir  C:\Users\7052\Desktop\tmp\fdb_v3_data_released `
  --modelUrl  http://192.168.111.123:8000/v1/chat/completions `
  --modelName ugreen-ai-model --modelToken <key> `
  --provider  cpp_text --limit 5        # 或 --ids travel_01 / --ids travel_01_62a885d5...

# ② 仓库里的场景表（无音频时用）：结果写 <out>/<provider>/<scenario_id>/
build\test\bin\Debug\fdb_v3_text.exe --benchmark <...>/benchmark_data_v2.json --provider cpp_text --out test/agent/fdb_v3/out
```

⚠ **两种输入源不一样，别混**：

| | `--data-dir`（真实数据） | `--benchmark`（仓库场景表） |
|---|---|---|
| 单位 | 100 个样本目录 `{scenario_id}_{speaker_id}/` | 100 个 scenario |
| 对话轮数 | **1 轮**（就是那通录音里用户真说的那句） | **2 轮**（多一句寒暄热身，100/100 都这样） |
| 期望工具调用 | 相同 | 相同（与 metadata 完全一致，已核对） |
| 结果落点 | **写回样本目录** `result_<provider>.json`（与既有 `result_cascade.json` 等同位置） | `<out>/<provider>/<scenario_id>/` |

`--provider` 同时决定文件名与评测的 `--provider`（别覆盖别人已有的 `result_*.json`）。
串行跑（一个场景一个 JZAgent，跑完销毁再起下一个），真实数据约 1~2s/条、场景表约 3~5s/条。

## 产物字段（与上游 `result_{provider}.json` 对齐）

上游 `evaluate_tool_calls.py` / `evaluate_pass_rate.py` 用 `rglob("result_<provider>.json")` 扫目录，
按 `example_id` 对回 benchmark 场景，只读三个字段：

| 字段 | 本程序怎么填 |
|---|---|
| `actual_tool_calls` | `[{function, args, timestamp_start, timestamp_end, duration, result}]`（时间戳是绝对 unix 秒，同上游 `log_tool_call`） |
| `transcript` | 各回合最终回复拼成一段（上游是整段输出音频的 ASR 全文） |
| `asr_chunks` | `[]`（文本模式无音频；`turn_take_success` 会退化用 `transcript` 非空判定） |

另外多写了 `turns / usage / llm_elapsed_s / total_elapsed_s / mode:"cpp_text"`，评测脚本不读，排查用。

## 跑上游评测

```powershell
cd C:\Users\7052\Desktop\tmp\fdb_v3_upstream
$env:PYTHONIOENCODING='utf-8'       # 上游脚本 print emoji，GBK 控制台会 UnicodeEncodeError

python evaluate_tool_calls.py --benchmark benchmark_data_v2.json `
  --results-dir <repo>\test\agent\fdb_v3\out\cpp_text --provider cpp_text --output <...>\eval.json
python evaluate_pass_rate.py  --benchmark benchmark_data_v2.json `
  --results-dir <repo>\test\agent\fdb_v3\out\cpp_text --provider cpp_text --output <...>\pass.json
```

## 已知口径问题（不是本链路引入的）

1. **不带 `--use-llm` 时 `Argument Acc` 恒为 0%**：上游的 fallback 是 `exact_match_args`
   （只做小写/下划线归一）。而 benchmark 的期望值是"人听音频后写的规范值"，模型输出是逐字的：
   - `date`: 期望 `July 15` ← 模型 `2026-07-15`
   - `doc_number`: 期望 `P9-9-9-90011` ← 模型 `P-8-8-9-9-0-0-1-1`（用户就是一个字符一个字符念的）
   这类等价性只有走 gpt-4o judge（`--use-llm`）才判得对。语音链路里也一样，不是文本化造成的。
2. **`--use-llm` 需要 `pip install openai` + 一个 OpenAI key**。想用内网 vllm 当 judge 的话：
   上游 judge 里模型名写死 `gpt-4o`，而该 vllm 只认 `ugreen-ai-model`（`model gpt-4o` → 404），
   需要在中间放一个把 `model` 字段改写的代理，再把 `OPENAI_BASE_URL` 指过去。
3. **延迟类指标无意义**：`analyze_tool_latency.py` 读的是音频时序（`latency` / `perceived_total_latency`），
   文本模式没有 → 该报告为空，属预期。
4. `state_rollback_test` 场景在语音链路里靠多轮打断/回滚触发，文本模式等价性弱，看工具调用即可。

## 本次实测（2026-10-26，ugreen-ai-model @ vllm，c++ 版全量 100 条）

**A. 真实数据目录（`--data-dir`，100 个样本，单轮）** ← 与官方口径一致
- 100/100 `completed`，约 2 分钟（1~2s/样本）。
- `evaluate_tool_calls.py`（无 --use-llm）：turn-take 100%，**Tool Selection F1 95.2%**，Argument 52.8%。
  分域：finance 100% / travel 98.3% / ecommerce 98.3% / housing 84.7%。
- `evaluate_pass_rate.py`（严格二值）：**46%**（wrong_tools 12 / wrong_arguments 42）。

**B. 仓库场景表（`--benchmark`，2 轮 dialogue）**
- **Tool Selection F1 87.8%**、Argument 49.7%、严格 pass **43%**（wrong_tools 23 / wrong_arguments 34）。
- 比 A 低 7 个点：多出来的那句寒暄让模型先回应寒暄，部分场景因此没在该回合调用工具。

下面是 B 的细项（A 的失分形态与之类似，`wrong_arguments` 占大头仍是 exact-match 口径问题）：

- 跑批：100/100 `completed`，156 次工具调用，64.6 万 token，约 5 分钟（串行）。
- 上游 `evaluate_tool_calls.py`（**无 --use-llm**）：turn-take 100%，**Tool Selection F1 87.8%**，
  Argument 49.7%。分域：finance 98.7% / travel 95.9% / ecommerce 90.1% / **housing 66.5%**。
- 上游 `evaluate_pass_rate.py`（严格二值）：**43%**（wrong_tools 23 / wrong_arguments 34）。
  travel_identity 只有 4% —— 全被 `date`/`doc_number` 的 exact-match 卡掉（见上面口径问题 1）。
- 工具选择的失分形态（23 条）：多调 `search_products` 11 次（问"推荐"就搜，`add_to_cart` 前多搜一遍）、
  漏调 `calculate_commute` 6 / `search_apartments` 4（模型反问预算，见下）。
- ⚠ `search_apartments(city, bedrooms, max_price)` 在上游没有默认值 ⇒ schema 里 `max_price` 是必填，
  但 benchmark 期望调用里常常只给 city+bedrooms。模型于是反问"预算多少"而不调用 → 判漏调。
  这是上游 schema 与标注之间的摩擦，**我们保持与上游一致**（不擅自把 max_price 改可选，否则就比参考实现更容易了）。

## 汇总表（对齐论文/README 那张表）

`make_table.py` 把结果目录里所有 `result_<provider>.json` 批量跑上游评测，输出：

```
模型 | Tool Sel↑ | Arg Acc↑ | Resp Qual↑ | Pass@1↑ | Take-turn↑ | Latency↓ | N
```

```powershell
# 不开 judge：Resp Qual 空、Arg Acc 被 exact-match 压低
python make_table.py --results-dir C:\Users\7052\Desktop\tmp\fdb_v3_data_released ^
                     --min-count 50 --out out\table.md

# 开 judge（Resp Qual / 语义 Arg Acc）：需 pip install openai + 一个能应答 gpt-4o 名字的端点
python judge_proxy.py --listen 18100 --target http://192.168.111.123:8000/v1/chat/completions ^
                      --model ugreen-ai-model --api-key <key>          # 另开一个窗口常驻
$env:OPENAI_BASE_URL='http://127.0.0.1:18100/v1'; $env:OPENAI_API_KEY='<key>'
python make_table.py --providers cpp_text --use-llm --out out\table_cpp_text.md
```

`judge_proxy.py` 只做一件事：上游 judge 把模型名写死 `gpt-4o`，而本地 vllm 只认 `ugreen-ai-model`，
代理把 body 的 `model` 改写后转发（纯标准库，非流式）。

结果（cpp_text = 本目录 C++/JZAgent 文本链路，100 样本）：

| 模型 | Tool Sel | Arg Acc | Resp Qual | Pass@1 | Take-turn | Latency |
|---|---|---|---|---|---|---|
| ugreen-ai-model（文本链路，judge 开） | **0.952** | 0.843 | 0.950 | **0.810** | 1.000 | -（无音频） |
| 同上（judge 关，exact-match） | 0.952 | 0.528 | - | 0.460 | 1.000 | - |

⚠ 三条对比红线：
1. **judge 就是被测模型自己**（内网只有一个端点）—— Resp Qual / 语义 Arg Acc 存在自评偏差，
   只能证明"表和列能跑出来"，不能当论文数字横向比。
2. **文本链路天然优于语音链路**：没有 ASR 错词、没有打断、没有端到端延迟预算 ⇒ Tool Sel/Pass 偏高是预期的
   （论文里 GPT-Realtime 是 0.876/0.600，Cascaded 0.803/0.450）。
3. **Latency 列永远是 "-"**：它是"用户说完 → agent 出声"的音频时序，文本模式不存在这个量。

## Python 版 vs C++ 版

- 两版对应关系一致（同样的 12 个工具、同样的 system prompt、同样的 mock 返回值、同样的输出字段）。
  实测同一 3 条场景，两版给出的工具与参数完全一致。
- Python 版直接 `import` 上游 `mock_apis.py`（不复制）；C++ 版把那份假数据**照抄**进
  `fdb_v3_text.cpp::mockApi()`，**上游改了 mock 数据要同步改这里**。
- Python 版支持并发（`--workers`）；C++ 版串行（单 agent 单事件循环，简单可靠）。
