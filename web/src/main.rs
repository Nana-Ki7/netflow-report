//! netflow-web —— 校园网流量的网页版
//!
//! 浏览器打开页面 → 填「学号 + 密码」→ 服务端用 hnu_query 登录并取数 → 页面用 ECharts 画图。
//!
//! # 和 export-rust 的关系
//! 取数逻辑一模一样（同一个 `hnu_query` 库、同样的 CAS 登录 → 换令牌 → 取账单/明细），
//! 区别只在「出口」：那边吐 CSV，这边吐 JSON 给网页。
//!
//! # 安全
//! 学号/密码只在**这次请求的内存**里用一遍，用完即弃：不落盘、不写日志、不进任何响应。
//!
//! # 运行
//! 需要身处**校园网内**或连上**学校 VPN**，否则 CAS 登录会失败。
//!
//! ```bash
//! cd web
//! cargo run --release
//! # 然后浏览器打开 http://127.0.0.1:8080
//! ```

use axum::{
    Json, Router,
    response::{Html, IntoResponse},
    routing::{get, post},
};
use chrono::{Datelike, Local};
use hnu_query::{
    cas::login::CasToken,
    netflow::{self, login::NetflowToken},
};
use serde::Deserialize;
use serde_json::{json, Value};
use std::time::Duration;

/// 账单里的字节数（B）换算成 GB
const BYTES_PER_GB: f64 = 1024.0 * 1024.0 * 1024.0;
/// 流量明细里的 KB 换算成 GB
const KB_PER_GB: f64 = 1024.0 * 1024.0;

/// 前端页面直接编译进二进制，省得再配静态目录服务
const INDEX_HTML: &str = include_str!("../static/index.html");
/// ECharts 直接内嵌，随服务一起发 —— 页面不再依赖任何 CDN（国内访问 jsdelivr 常失败）
const ECHARTS_JS: &[u8] = include_bytes!("../static/echarts.min.js");

/// 免登录的示例数据：由 cpp/data 那两份样例 CSV 生成，纯演示用。
/// 有了它，没进校园网、手上又没凭据的人也能先把页面和图表看一遍。
const DEMO_JSON: &str = include_str!("../static/demo.json");

#[tokio::main]
async fn main() {
    let app = Router::new()
        .route("/", get(index))
        .route("/echarts.min.js", get(echarts_js))
        .route("/api/data", post(api_data))
        .route("/api/demo", get(api_demo));

    let addr = "127.0.0.1:8080";
    let listener = tokio::net::TcpListener::bind(addr)
        .await
        .unwrap_or_else(|e| panic!("绑定 {addr} 失败（端口被占用？）：{e}"));
    println!("netflow-web 已启动：http://{addr}");
    println!("提示：登录和取数需要身处校园网或学校 VPN 内。");

    axum::serve(listener, app).await.unwrap();
}

/// 首页：把内嵌的 HTML 原样返回
async fn index() -> Html<&'static str> {
    Html(INDEX_HTML)
}

/// 把内嵌的 ECharts 原样发出，替代原来的 CDN
async fn echarts_js() -> impl IntoResponse {
    (
        [(
            axum::http::header::CONTENT_TYPE,
            "application/javascript; charset=utf-8",
        )],
        ECHARTS_JS,
    )
}

/// 前端提交上来的登录参数
#[derive(Deserialize)]
struct LoginReq {
    stu_id: String,
    password: String,
    /// 「按应用分布」要取最近几个月；0 表示不取（这部分要逐月请求，慢）
    #[serde(default)]
    dist_months: u32,
}

/// 唯一的业务接口：登录 + 取数，一次把两种图的数据都带回去。
///
/// 不做服务端会话：每次请求都重新登录一遍，凭据用完即弃。
/// 流量账单的变化本来就不频繁，够用了，也最不容易留痕。
async fn api_data(Json(req): Json<LoginReq>) -> Json<Value> {
    if req.stu_id.trim().is_empty() || req.password.is_empty() {
        return Json(json!({"ok": false, "error": "学号和密码都不能为空"}));
    }
    match fetch(&req).await {
        Ok(data) => Json(json!({"ok": true, "data": data})),
        Err(e) => Json(json!({"ok": false, "error": e})),
    }
}

/// 示例数据：不碰任何凭据，直接把编好的演示数据吐给页面。
///
/// 结构与 [`api_data`] 的返回完全一致，前端拿到就能直接画。
async fn api_demo() -> Json<Value> {
    match serde_json::from_str::<Value>(DEMO_JSON) {
        Ok(data) => Json(json!({ "ok": true, "demo": true, "data": data })),
        Err(e) => Json(json!({ "ok": false, "error": format!("示例数据解析失败：{e}") })),
    }
}

/// 真正的取数流程。任何一步失败都把可读的原因返回给前端。
async fn fetch(req: &LoginReq) -> Result<Value, String> {
    // ---- 第 1 步：CAS 统一身份认证 ----
    let cas = CasToken::acquire_by_login(&req.stu_id, &req.password)
        .await
        .map_err(|e| {
            format!(
                "CAS 登录失败：{e:?}\n（常见原因：不在校园网/VPN 内、密码错、需要双因子认证、账号被锁）"
            )
        })?;

    // ---- 第 2 步：用 CAS 令牌换校园网流量系统的令牌 ----
    let token = NetflowToken::acquire_by_cas_login(&cas)
        .await
        .map_err(|e| format!("流量系统登录失败：{e:?}"))?;

    // ---- 第 3 步：按月趋势（一次请求拿全部历史账单，字段最全）----
    let orders = netflow::get_order(&token)
        .await
        .map_err(|e| format!("取流量账单失败：{e:?}"))?;

    let monthly: Vec<Value> = orders
        .iter()
        .rev() // 账单通常是「新 → 旧」，翻过来画折线才是时间顺序
        .map(|o| {
            let up = o.upload_usage as f64 / BYTES_PER_GB;
            let down = o.download_usage as f64 / BYTES_PER_GB;
            json!({
                "month": format!("{}", o.time),
                "total_gb": round2(up + down),
                "upload_gb": round2(up),
                "download_gb": round2(down),
                "over_usage_gb": round2(o.over_usage),
                "should_pay_yuan": round2(o.should_pay),
            })
        })
        .collect();

    // ---- 第 4 步：按应用分布（可选，逐月请求，慢）----
    let mut distribution: Vec<Value> = Vec::new();
    if req.dist_months > 0 {
        let now = Local::now();
        let (this_year, this_month) = (now.year() as u16, now.month() as u8);
        // back 从大到小，输出即按时间升序
        for back in (0..req.dist_months).rev() {
            let (year, month) = shift_back(this_year, this_month, back);
            match netflow::get_month_detail(&token, year, month).await {
                Ok(detail) => {
                    let label = format!("{year}-{month:02}");
                    for item in &detail.items {
                        distribution.push(json!({
                            "month": label,
                            "app": item.app,
                            "total_gb": round4(item.total / KB_PER_GB),
                            "download_gb": round4(item.download / KB_PER_GB),
                            "upload_gb": round4(item.upload / KB_PER_GB),
                            "percentage": item.percentage,
                        }));
                    }
                }
                // 某个月没数据（比如还没开学）跳过就好，不该让整次请求失败
                Err(_) => {}
            }
            // 温柔一点，别把学校接口打爆
            tokio::time::sleep(Duration::from_millis(400)).await;
        }
    }

    Ok(json!({ "monthly": monthly, "distribution": distribution }))
}

/// 保留两位小数（给前端看的数值，别带一长串浮点尾巴）
fn round2(x: f64) -> f64 {
    (x * 100.0).round() / 100.0
}

/// 保留四位小数
fn round4(x: f64) -> f64 {
    (x * 10000.0).round() / 10000.0
}

/// 把「年 + 月」往回退 `back` 个月。换成「绝对月序号」算最不容易出错。
fn shift_back(year: u16, month: u8, back: u32) -> (u16, u8) {
    let absolute = year as u32 * 12 + (month as u32 - 1);
    let absolute = absolute.saturating_sub(back);
    ((absolute / 12) as u16, (absolute % 12) as u8 + 1)
}
