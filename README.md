# 校园网流量 · 网页版

在网页上填学号密码 → 服务端用 [hnu_query](https://github.com/qnxg/hnu_query) 登录取数 → 浏览器里直接出图。

```
浏览器（登录框 + 图表） → Rust(axum) 服务 → hnu_query 登录取数 → JSON → ECharts 画图
```

凭据只在**这一次请求的内存**里过一遍，用完即弃：不落盘、不写日志、不进响应。

---

## 跑起来

```bash
cd web
cargo run --release
# 然后打开 http://127.0.0.1:8080
```

- 登录和取数需要身处**校园网或学校 VPN** 内。
- 没进校园网、手上也没凭据？点页面上的「**先看示例数据**」，直接看界面和图表长什么样（不碰登录、不读真实用量）。

## 页面里有什么

- 顶上一排 **KPI 卡**：累计流量 / 月均 / 超套餐月数 / 累计应缴 / 峰值月。
- 三张图：**月度趋势**（渐变柱 + 上传/下载/费用折线）、**应用分布**（甜甜圈）、**应用 Top15**。
- 分布和 Top15 的应用带**品牌图标**（Steam、Bilibili、QQ、网易云……），没收录的分类回退 emoji。
- 每张图可「**存 SVG**」，矢量、放大不糊。
- **页面完全离线**：ECharts 本体和品牌图标都编译进二进制（走 `/echarts.min.js`、`/brands.js`），一个 CDN 都不碰。

## 接口

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/` | 页面本体 |
| GET | `/echarts.min.js` | 内嵌的 ECharts |
| GET | `/brands.js` | 内嵌的品牌图标表 |
| POST | `/api/data` | 入参 `{stu_id, password, dist_months}` → 出参 `{ok, data:{monthly, distribution}}` |
| GET | `/api/demo` | 免登录示例数据，结构与 `/api/data` 的 `data` 一致 |

`dist_months` 是「按应用分布」取最近几个月，`0` 表示不取——这部分要逐月发请求，会慢一点。

---

## 目录

```
netflow-report/
├── web/                   # 网页版（主线）
│   ├── Cargo.toml
│   ├── src/main.rs        # axum 服务：登录 + 取数 + 发页面
│   └── static/
│       ├── index.html     # 登录框 + KPI 卡 + ECharts 图表
│       ├── echarts.min.js # ECharts 本体（内嵌，不依赖 CDN）
│       ├── brands.js      # 品牌图标（Simple Icons，内嵌）
│       └── demo.json      # 免登录示例数据
└── export-rust/           # 命令行取数：登录 → 输出按月 CSV
    ├── Cargo.toml
    └── src/main.rs
```

---

## 只想要数据？用命令行取数

不想开浏览器、想把原始数据落成 CSV 时，用 `export-rust`：

```bash
cd export-rust
export HNU_STU_ID=你的学号
export HNU_PASSWORD=你的个人门户密码
cargo run --release -- order > netflow_monthly.csv
```

三种模式：

| 模式 | 说明 |
| --- | --- |
| `order` | 一次请求拿到全部历史账单，字段最全（含超额流量、应缴费用） |
| `detail <月数>` | 逐月查总量趋势（慢，无账单金额） |
| `dist <月数>` | 按应用分类的流量分布 |

---

## 数据格式

命令行输出的 CSV：

| 列名 | 含义 | 单位 |
| --- | --- | --- |
| `month` | 月份，如 `2026-03` | — |
| `total_gb` | 总流量（上传 + 下载） | GB |
| `upload_gb` | 上行流量 | GB |
| `download_gb` | 下行流量 | GB |
| `over_usage_gb` | 超出套餐的流量 | GB |
| `should_pay_yuan` | 应缴费用 | 元 |

`dist` 模式（分布）用另一张表：

| 列名 | 含义 | 单位 |
| --- | --- | --- |
| `month` | 月份，如 `2026-09` | — |
| `app` | 应用分类路径，如 `/网络游戏/steam平台` | — |
| `total_gb` | 该应用当月总流量 | GB |
| `download_gb` | 下载流量 | GB |
| `upload_gb` | 上传流量 | GB |
| `percentage` | 占当月总流量的比例 | 0~1 小数 |

网页版的 `/api/data` 返回的 `monthly` / `distribution` 就是这两张表转成的 JSON。

---

## 凭据怎么放

- **网页版**：学号密码由页面提交，只在服务端那次请求的内存里用一遍，不落盘、不写日志。
- **命令行版**：只从环境变量读（`HNU_STU_ID` / `HNU_PASSWORD`），源码里没有任何真实凭据。
- 别把密码写进 `Cargo.toml`、别写进脚本、别 commit 上去；`.env` 记得进 `.gitignore`。

---

## 常见问题

**`cargo` 报 `edition 2024` 相关错误**
Rust 版本太旧。`rustup update stable`，或用 1.85 以上。

**登录一直失败**
按顺序排查：① 是不是在校园网/VPN 内；② 密码是不是最近改过；③ 账号是否被锁；④ 有没有弹双因子认证（`hnu_query` 的错误信息里会写清是哪种）。

**分布图里少了一个月**
`dist N` 是逐月发请求的，某个月没数据（比如还没开学）会跳过——不是 bug。

**为什么不用 hnu_query 直接画图**
它是 Rust 库，不是命令行工具，也不管画图。取数归它，画图交给浏览器/前端，分工最省事。
