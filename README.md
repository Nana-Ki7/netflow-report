# 校园网流量 → 图表（Rust 取数 + C++ 画图）

把 [hnu_query](https://github.com/qnxg/hnu_query) 里的**校园网流量**数据抠出来，再画成能看的图。支持两种图：

| 你想看什么 | 数据口径 | Rust 模式 | 出来的图 |
| --- | --- | --- | --- |
| 每个月用了多少（趋势） | 按月汇总 | `order` | 柱状图 + 上传/下载折线 |
| 流量都花在哪了（分布） | 按月 + 按应用 | `dist` | 一级分类饼图 + 应用 Top15 条形图 |

C++ 那个程序会**自己看 CSV 表头**判断该画哪种，你不用手动切。

一句话记住整条链路：

```
hnu_query（Rust 库）→ 取数 → CSV（中间层）→ C++ 读文件 → 图（终端 + HTML）
```

Rust 只负责「拿到数据」，C++ 只负责「把数据画出来」，中间用 CSV 隔开。
好处是：C++ 那边不需要懂登录、不需要联网、不需要校园网——**你可以在任何地方反复调图表代码**。

---

## 目录

```
netflow-report/
├── export-rust/          # 第一层：登录 + 取数，输出按月 CSV
│   ├── Cargo.toml
│   └── src/main.rs
├── cpp/                  # 第二层：读 CSV + 画图（这一层是 C++ 的主场）
│   ├── main.cpp
│   ├── CMakeLists.txt
│   └── data/
│       ├── sample_monthly.csv        # 编的示例数据：按月汇总
│       └── sample_distribution.csv   # 编的示例数据：按应用分布
├── web/                  # 第三层：网页版（浏览器登录 + 出图）
│   ├── Cargo.toml
│   ├── src/main.rs
│   └── static/
│       ├── index.html          # 登录框 + KPI 卡 + ECharts 图表
│       ├── echarts.min.js      # ECharts 本体，本地内嵌（不依赖 CDN）
│       ├── brands.js           # 品牌图标（Simple Icons，离线内嵌）
│       └── demo.json           # 免登录示例模式用的数据
├── glm/                  # 让智谱 GLM 当私教的小脚本
│   └── ask_glm.py
└── TUTORIAL.md           # 逐步讲解 + 练习题，从这开始看
```

---

## 跑起来

### 第 1 步：先把 C++ 这边跑通（不需要校园网）

```bash
cd cpp
g++ -std=c++17 -O2 -Wall -o netflow-chart main.cpp

./netflow-chart data/sample_monthly.csv        # 月度趋势图
./netflow-chart data/sample_distribution.csv   # 应用分布图
```

终端会打出 ASCII 图，同时生成 `chart.html`，双击用浏览器打开就是正经的图。

> `data/sample_monthly.csv` 里的数字是我编的，只为让你先看到效果。

### 第 2 步：换真实数据（要在校园网内，或者连上学校 VPN）

```bash
# 装 Rust（需要 1.85 或更高，因为 hnu_query 用了 edition 2024）
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh

cd export-rust
export HNU_STU_ID=你的学号
export HNU_PASSWORD=你的个人门户密码
cargo run --release -- order > ../cpp/data/netflow_monthly.csv
```

`order` 模式只发**一次**请求，就能拿到全部历史月份（还带超额流量和应缴费用），图的信息量最大。

想按月看**总量趋势**（不带应用分类），用逐月明细模式：

```bash
cargo run --release -- detail 12 > ../cpp/data/netflow_monthly.csv
```

想按应用看**流量分布**（到底哪个 App 最费流量），用 `dist` 模式：

```bash
cargo run --release -- dist 1 > ../cpp/data/netflow_distribution.csv   # 最近 1 个月
cargo run --release -- dist 3 > ../cpp/data/netflow_distribution.csv   # 最近 3 个月，图里每个月一块
```

### 第 3 步：画你自己的图

```bash
cd ../cpp
./netflow-chart data/netflow_monthly.csv my-chart.html
```

---

## 网页版（带网页登录）

不想跟 CSV、命令行打交道的话，`web/` 里有个网页版：**在浏览器上填学号密码，登录和取数都在服务端完成，页面直接出图。**

```bash
cd web
cargo run --release
# 然后打开 http://127.0.0.1:8080
```

- 前端就一个 HTML（`web/static/index.html`），图用 ECharts 画，渲染器选的 **SVG**——矢量、放大不糊。
- **页面完全离线**：ECharts 本体和品牌图标都直接编译进二进制（走 `/echarts.min.js`、`/brands.js`），一个 CDN 都不碰——校园网里外都能开。
- 后端 `web/src/main.rs` 复用同一个 `hnu_query`：`POST /api/data` 收学号密码 → 登录 → 取账单（可选按应用分布）→ 回 JSON。
- **凭据只在这一次请求的内存里过一遍**，用完即弃：不落盘、不写日志、不进响应。
- 页面上还有个「**先看示例数据**」按钮，走 `GET /api/demo`，吐的是 `cpp/data` 那两份样例 CSV 生成的演示数据。
- 界面是仪表盘式的：顶上一排 KPI 卡（累计 / 月均 / 超套餐月数 / 累计应缴 / 峰值月），下面趋势、分布、Top15 三张图；分布和 Top15 里的应用图标走本地品牌表，没收录的分类回退 emoji。
  没进校园网、手上没凭据也能先把界面和图表看一遍——它不碰登录，也不读任何真实用量。
- 每张图标题旁有「**存 SVG**」，点一下就把当前这张图存下来；SVG 是矢量，放大不糊，直接塞文档、推文都行。
- 和命令行版一样，登录必须身处**校园网或学校 VPN** 内。

接口就这几个：

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/` | 页面本体 |
| GET | `/echarts.min.js` | 内嵌的 ECharts 本体 |
| GET | `/brands.js` | 内嵌的品牌图标表 |
| POST | `/api/data` | 入参 `{stu_id, password, dist_months}` → 出参 `{ok, data:{monthly, distribution}}` |
| GET | `/api/demo` | 免登录的示例数据，结构与 `/api/data` 的 `data` 一致 |

`dist_months` 是「按应用分布」取最近几个月，`0` 表示不取——这部分要逐月发请求，会慢一点。

---

## 数据长这样

CSV 的列名就是「取数层」和「画图层」之间的契约：

| 列名 | 含义 | 单位 |
| --- | --- | --- |
| `month` | 月份，如 `2026-03` | — |
| `total_gb` | 总流量（上传 + 下载） | GB |
| `upload_gb` | 上行流量 | GB |
| `download_gb` | 下行流量 | GB |
| `over_usage_gb` | 超出套餐的流量 | GB |
| `should_pay_yuan` | 应缴费用 | 元 |

C++ 那边是**按列名**找列的：多一列少一列都不会崩，缺的列就不画那条线。
所以你以后想加「日均流量」「最大单日」之类的列，只要在 Rust 里多打一列就行。

`dist` 模式（分布图）用的是另一张表：

| 列名 | 含义 | 单位 |
| --- | --- | --- |
| `month` | 月份，如 `2026-09` | — |
| `app` | 应用分类路径，如 `/网络游戏/steam平台` | — |
| `total_gb` | 该应用当月总流量 | GB |
| `download_gb` | 下载流量 | GB |
| `upload_gb` | 上传流量 | GB |
| `percentage` | 占当月总流量的比例 | 0~1 小数 |

`app` 是接口给的**层级路径**，所以程序能顺手把它拆开：饼图按第一段合并（一级分类，如「网络游戏」），条形图看具体应用。
两张表的表头不一样，C++ 靠这个自动认出场。

---

## 凭据怎么放（重要）

- 学号、密码**只从环境变量读**，源码里没有任何真实凭据。
- 别把密码写进 `Cargo.toml`、别写进脚本、别 commit 上去。`.env` 记得进 `.gitignore`。
- GLM 的 API Key 同理，见 `glm/ask_glm.py`。

---

## 常见问题

**`cargo` 编译报 `edition 2024` 相关错误**
Rust 版本太旧。`rustup update stable`，或者用 `rustup toolchain install 1.85` 以上。

**登录一直失败**
按顺序排查：① 是不是在校园网/VPN 内（先 `ping cas.hnu.edu.cn` 试试）；② 密码是不是最近改过；③ 账号是不是被锁了；④ 有没有弹双因子认证（`hnu_query` 的错误信息里会写清楚是哪种）。

**`chart.html` 打开一片空白**
页面里的 ECharts 是从 CDN 拉的，断网就没图。要么联网，要么把 `main.cpp` 里那行 `<script src="https://cdn.jsdelivr.net/npm/echarts@5/dist/echarts.min.js">` 换成本地文件。（网页版 `web/` 已经不吃这个亏了——ECharts 直接编译进二进制。）

**分布图里少了一个月**
`dist N` 是**逐月**发请求的，某个月没数据（比如还没开学）会打印一行「跳过 2026-08：…」然后继续。终端里有这行就不是 bug。

**为什么不用 hnu_query 直接画图**
`hnu_query` 是 Rust 库，不是命令行工具，也不管画图。而且 Rust 的图表生态对新手不算友好，C++ 这边用「生成 HTML」的方式反而是最省事的——**不动任何第三方库，浏览器就是渲染器**。

**学校自己那个大模型平台呢？**
`hnu_query` 里有管理它 API Token 的模块，但用起来仍然要先登录、再换 token，多一层手续；智谱开放平台注册完直接给 key，调通更快。等你要在校园网内做批量任务时，再考虑换成学校的平台。

---

## 下一步玩法

- 把 `over_usage_gb > 0` 的月份标红，一眼看出哪个月超了套餐。
- 加一条「12 个月移动平均」线。
- ✅ 按应用分布图已经做好了（`dist` 模式 + 同一个 C++ 程序会自己认）。想改味道就去 `makePie` 里动 `radius`，比如改成实心饼。
- 把上传/下载也画进分布图：`sample_distribution.csv` 里已经有这两列，加一组 stacked bar 就行。
- 让它每天自动跑一次，把结果推到手机上（这部分我可以另外给你写）。
