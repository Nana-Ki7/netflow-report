# 教学：从取数到出图，一次搞懂

这份文档按「你亲手做一遍」的顺序写。每节课都包含：要做什么 → 为什么这么做 → 动手改一个地方 → 检查是否懂了。

配套代码在 `export-rust/`（Rust 取数）和 `cpp/`（C++ 画图）。

---

## 第 0 课：先看清楚数据长什么样

写代码之前先看一眼接口返回什么，比什么都重要。

`hnu_query` 里跟校园网流量有关的函数一共就五个（都在 `src/netflow/` 下）：

| 函数 | 拿到什么 | 单位 |
| --- | --- | --- |
| `netflow::get_order(&token)` | **全部历史月份的账单列表**：`time`("2026-03")、`upload_usage`、`download_usage`、`over_usage`、`should_pay` | 字节 / GB / 元 |
| `netflow::get_month_detail(&token, year, month)` | 某个月的明细：总/上传/下载 + 按应用分类的 `items` | KB |
| `netflow::get_day_detail(&token, y, m, d)` | 某一天的明细 | KB |
| `netflow::get_this_month_info(&token)` | 本月用量、剩余免费额度 | GB，带字符串单位 |
| `netflow::get_overdue_payment(&token)` | 欠费金额 | 元 |

**结论：做「按月」的图，首选 `get_order`。** 它一次请求就把所有月份给你了，字段还最全。
`get_month_detail` 要循环调用（一个月一次请求），慢，但能拿到「哪个 App 最费流量」。两个都实现了，你可以对比着看。

⚠️ 第一个坑：**单位不统一**。账单接口是字节，明细接口是 KB，本月信息是 GB。所以代码里有两个常量：

```rust
const BYTES_PER_GB: f64 = 1024.0 * 1024.0 * 1024.0;  // 账单用（字节）
const KB_PER_GB: f64 = 1024.0 * 1024.0;               // 明细用（KB）
```

把「换算」和「取数」分开，是这种数据管道里最容易踩坑的地方。

**动手改**：把 `export-rust/src/main.rs` 里的 `detail.total` 换成 `detail.total / 1024.0`（只按 MB 算），跑一遍看数字差多少倍。

---

## 第 1 课：Rust 侧的登录链路（令牌接力）

校园网的接口不能直接调，得先证明「你是谁」。湖大用的是 CAS 统一身份认证，完整链路是两跳：

```rust
// 第 1 跳：学号 + 密码 → CAS 令牌
let cas_token = CasToken::acquire_by_login(&stu_id, &password).await?;

// 第 2 跳：CAS 令牌 → 校园网流量系统的令牌
let netflow_token = NetflowToken::acquire_by_cas_login(&cas_token).await?;

// 之后所有请求都带着这个 netflow_token
let orders = netflow::get_order(&netflow_token).await?;
```

三个细节：

1. **为什么不直接用账号密码调流量接口**：因为流量系统信任的是「CAS 签发的票据」，不是你的密码。密码只在第一跳用一次，之后流通的都是令牌。这也是为什么这套东西**必须在校园网/VPN 内**——令牌签发只在校内网可达。
2. **为什么凭据从环境变量读**：

   ```rust
   let stu_id = env::var("HNU_STU_ID").unwrap_or_default();
   ```

   如果写成 `let stu_id = "2023xxxxxx";`，你哪天手一抖 `git push`，密码就永久留在仓库历史里了。**这条规则没有例外。**

3. **`?` 和 `match` 的区别**：`?` 是「出错就往上抛」。这里我们不想抛，想告诉用户到底哪一步挂了，所以用 `match + fail(...)` 打了人话的错误信息。

**动手改**：删掉 `CasToken::acquire_by_login` 外面那圈 `match`，改成 `?`，看看编译错误长什么样，理解 `main` 返回类型和 `?` 的关系。

---

## 第 2 课：为什么中间要夹一层 CSV

你可能会想：Rust 里直接把图生成了不就完了？

因为：**分层以后，每一层都能单独调试。**

- 登录挂了？那是第 1 层的事，跟画图无关。
- 图不理想？把 CSV 存下来，反复改 C++ 代码就行，**不用再登录一次**，也不受校园网限制。
- 你甚至可以拿 Excel 打开这个 CSV 检查数字对不对（顺便验证你的换算是准的）。

技术上的理由也很实际：Rust 的绘图生态对新手门槛高，C++ 这边用「输出 HTML」的画法连第三方库都不用装。

CSV 列名就是两层的接口契约：

```csv
month,total_gb,upload_gb,download_gb,over_usage_gb,should_pay_yuan
2025-10,62.40,18.20,44.20,0.00,0.00
```

**练一练**：往 CSV 里手动加一行假数据（比如 `2026-10,999.00,...`），重新跑 C++，看柱子是不是立刻变了。这一步能让你确信「数据驱动」是怎么working的。

---

## 第 3 课：C++ 读文件——按列名，而不是按下标

新手最容易写成这样：

```cpp
// ❌ 脆弱：以后 CSV 多加一列，全崩
row.total = std::stod(cells[1]);
row.upload = std::stod(cells[2]);
```

正确做法是先读表头，建立「列名 → 下标」的映射，再按名字取值（见 `cpp/main.cpp` 的 `readCsv`）：

```cpp
for (size_t i = 0; i < cells.size(); ++i) {
    column[trim(cells[i])] = i;   // "total_gb" -> 1
}
// 取值时：
const auto it = column.find("total_gb");
if (it == column.end()) return "";   // 这列不存在，返回空串
```

顺带解决了两个问题：

- **列的增减不会崩**：以后多一列 `avg_daily_gb`，老代码照常跑。
- **缺失列用 `-1` 标记**：`upload = toDouble(cell("upload_gb"), -1.0)`。这样 C++ 就知道「这条线上没有数据」，不会傻乎乎画一条全 0 的线。

**动手改**：在 `MonthData` 里加一个 `double avg_daily = -1.0;`，从 CSV 的 `avg_daily_gb` 列读进来，然后在终端表格里打印出来。改完你会发现——只要 CSV 里没有那列，程序一点事都没有。

---

## 第 4 课：两种画法，各有用处

### 4.1 终端 ASCII 柱状图（复杂度的下限）

```cpp
const int bars = std::max(1, (int)std::lround(row.total / max_total * 40));
std::cout << row.month << " | " << std::string(bars, '#') << " " << row.total;
```

核心就一句：**把数值归一化成 0~40 之间的字符个数**。
好处：不用任何库、SSH 里也能看、启动瞬间。调数据和验证换算时，它比 HTML 快得多。

### 4.2 生成 HTML + ECharts（颜值上限）

思路是「C++ 不画图，C++ 只生产画图指令」：

```
C++ 读 CSV → 拼一段 JSON 数组 → 塞进 HTML 模板 → 浏览器渲染
```

关键片段：

```cpp
std::vector<std::string> series;
series.push_back("{ name: '总流量', type: 'bar', data: [" + joinNumbers(rows, &MonthData::total) + "] }");
```

注意 `&MonthData::total` 这个写法——**指向成员的指针**。它让 `joinNumbers` 一个函数就能处理任意一个字段，不用为每个字段写一遍循环：

```cpp
std::string joinNumbers(const std::vector<MonthData>& rows, double MonthData::*field) {
    ...
    out << rows[i].*field;   // 语法有点怪，但意思就是"取这个字段"
}
```

想加新线？在 `writeHtml` 里 push 一个 series 就行。`type` 可选 `'bar'`（柱）/ `'line'`（线）/ `'scatter'`（散点）。

**动手改**：把总流量那根柱子的 `type: 'bar'` 改成 `'line'`，再打开 HTML 看看区别。

---

## 第 5 课：用智谱 GLM 当私教

`glm/ask_glm.py` 是一个**只用标准库**的 GLM 调用示例（不 pip install 任何东西）。

```bash
export GLM_API_KEY=你的key
python3 glm/ask_glm.py explain                 # 让它读你的 CSV，讲趋势 + 出练习题
python3 glm/ask_glm.py review cpp/main.cpp     # 让它批改你的 C++
python3 glm/ask_glm.py ask "为什么用 CSV 不用 JSON"
```

### 请求长什么样

```python
payload = {"model": "glm-4-flash", "messages": [...], "stream": False}
headers = {"Content-Type": "application/json", "Authorization": f"Bearer {API_KEY}"}
```

- **base_url**：`https://open.bigmodel.cn/api/paas/v4/chat/completions`。这是 OpenAI 兼容格式，所以你会觉得眼熟——换成别家模型时，改 `GLM_BASE_URL` 和模型名基本就能跑。
- **模型名**：`glm-4-flash` 便宜快，适合日常问答；`glm-4-plus` 更强更贵；`glm-4-air` 居中。教学场景 `flash` 就够。
- **回复取法**：`data["choices"][0]["message"]["content"]`。

### 新手最容易忽略的两件事

1. **限流（HTTP 429）**：免费/低额度账号很容易撞上。脚本里的处理是「指数退避」——等 1 秒、2 秒、4 秒再重试：

   ```python
   if error.code in (429, 500, 502, 503) and attempt < retries - 1:
       time.sleep(2 ** attempt)
       continue
   ```

   千万不要写成「失败就 while True 猛冲」，那只会让账号被限得更久。

2. **Key 绝不进代码**：`API_KEY = os.environ.get("GLM_API_KEY", "")`。硬编码 key 的脚本一旦分享出去，别人就能拿你的额度刷。

### 怎么问才有效

同一个模型，问法差别巨大。对比：

- ❌「这段代码有什么问题」→ 它只能泛泛而谈。
- ✅「指出 `readCsv` 里哪几行会在 CSV 有空字段时崩掉，并给出修改后的代码」→ 它会真去找空字段的风险点。

脚本里的 `review` 模板就是这么写的：**先列问题（带行号）→ 再给改好的片段 → 最后说一条写得好的地方**。最后那条不是客套，它能防止你只看到缺点而失去动力，也顺便告诉你「哪些写法是值得保持的」。

**练一练**：故意把 `cpp/main.cpp` 里的 `if (column.count("total_gb") == 0)` 这行删掉，然后让 GLM 批改，看它能不能发现「CSV 没有 total_gb 列时会静默读到 0」。

---

## 第 6 课：练习题

1. **（入门）** 终端柱状图现在是按「总流量」排序画的。改成按月份排序，并让 `2025-12`（最高那个月）在终端里用 `*` 而不是 `#` 画。
2. **（入门）** 给 HTML 图表加一条虚线，标出「12 个月平均流量」。
3. **（进阶）** `export-rust` 的 `detail` 模式里，把每个月 `items` 里排名第一的应用名也打到 CSV 的最后一列；C++ 侧把它显示在终端表格里，并在 HTML 上用 ECharts 的 `tooltip` 显示出来。
4. **（进阶）** 给 `netflow-chart` 加一个 `--top N` 参数，只画流量最高的 N 个月。参数解析怎么写才不会越写越乱？
5. **（挑战）** 把 `get_month_detail` 的结果缓存到本地文件，第二次运行时如果已经查过这个月就直接读缓存。什么时候该让缓存失效？
6. **（挑战）** 你发现 `should_pay_yuan` 全是 0，但学校确实收过费。写出三种可能的原因和对应的验证方法。

> 提示：第 3 题的关键是「Rust 侧多 `writeln!` 一列」，C++ 侧什么都不用改（因为它按列名读）。
> 这正是第 2 课讲的分层带来的好处——**改一层不牵连另一层**。

---

## 第 7 课：第二种图 —— 流量到底花在哪了

第 0~6 课画的都是「每个月用了多少」。但「**分布**」是另一个问题：
同样是 80 GB，是看视频用掉的，还是下游戏用掉的？这要看**明细接口**。

### 7.1 数据源换了：从「账单」到「明细」

|  | `order`（账单） | `dist`（明细） |
| --- | --- | --- |
| 请求次数 | 1 次拿全部历史 | 每个月 1 次 |
| 粒度 | 月 | 月 + 应用 |
| 有金额 | 有 | 没有 |
| 能画分布 | ❌ | ✅ |

明细接口（`netflow::get_month_detail`）返回的 `items` 里，每一项长这样：

```rust
DetailItem {
    app: "/网络游戏/steam平台",   // 分类路径
    total: 703434.19,            // KB
    download: 678507.29,
    upload: 24926.9,
    percentage: 0.37,            // 占当月比例
}
```

注意 `app` 是**斜杠分隔的层级路径**，不是扁平的名字。这一条直接决定了后面怎么画。

### 7.2 Rust 侧：把 items 摊平成 CSV

`export_distribution()` 就干一件亊：每个月查一次，把 items 一行一个打进 CSV。两个小细节：

- `dist N` 里的 `N` 是「往前推几个月」，1 就是只查当月；
- 个别分类名理论上可能带英文逗号，会把 CSV 列数冲乱，所以写出去之前把 `,` 换成全角 `，`。

### 7.3 C++ 侧：怎么知道该画哪种图

两种 CSV 的表头不一样，程序**读第一行非注释内容**判断：

```cpp
return trimmed.find("app") != std::string::npos;   // 表头里有 app 就是分布数据
```

一行代码换来「一个可执行文件吃两种数据」。若拆成两个程序，编译命令、CMake、教程都得写两遍——
**判断成本低于维护成本时，就别拆。**

### 7.4 层级路径怎么变成两张图

`/网络游戏/steam平台` 按 `/` 切开是 `{网络游戏, steam平台}`。于是：

- **饼图**：只看第一段（一级分类），把「网络游戏」下所有应用加起来。回答「大类之间的比例」。
- **条形图**：看完整路径（具体应用），排前 15。回答「哪个具体应用最狠」。

同一份数据，问的问题不同，图就不同。**先想清楚要回答哪个问题，再选图型**，别反过来。

再补两个实现细节：

- 横向条形图的类目是**从下往上**画的，所以要先排序再 `std::reverse`，不然最大的那根会跑到最底下；
- `std::setw` 数的是**字节**，一个汉字占 3 字节但显示 2 列，直接用会歪。所以写了 `displayWidth()` 按「显示宽度」补空格。

**练一练**：把 `topCategory()` 改成返回前两段（`网络游戏/steam平台`），饼图会变成什么样？先猜，再改，再用 GLM 问问你猜得对不对。

---

## 附录：我按什么顺序调试这套东西

1. 先 `./netflow-chart data/sample_monthly.csv`（和 `data/sample_distribution.csv`）—— 确认画图层两种图都是好的。
2. 再 `cargo run --release -- order | head` —— 确认取数层能出数据（**别急着存文件**，先看内容对不对）。
3. 数字明显不对时，先怀疑**单位**（字节 / KB / GB），再看字段名是不是搞混了（`upload` 和 `download` 特别好搞混）。
4. 图不满意？把 CSV 复制一份当固定输入，反复改 C++，不再碰网络。
