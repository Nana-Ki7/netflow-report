//! 校园网流量 → 按月 CSV 导出器
//!
//! 它是整个方案里的「取数层」：登录湖大校内系统，把校园网流量数据按「月」整理成 CSV。
//! 画图的事交给 C++ 那边做（见 ../cpp/）。
//!
//! # 运行前提
//!
//! 必须在**校园网内**或连上**学校 VPN**之后运行，否则连不上 `cas.hnu.edu.cn` / `ll.hnu.edu.cn`。
//!
//! # 用法
//!
//! ```bash
//! # 凭据只从环境变量读，绝不写进代码
//! export HNU_STU_ID=你的学号
//! export HNU_PASSWORD=你的个人门户密码
//!
//! cargo run --release -- order          # 一次请求拿到全部历史月份账单（字段最全，画按月趋势）
//! cargo run --release -- detail 12      # 逐月查总量，取最近 12 个月（慢，但没有账单金额）
//! cargo run --release -- dist 1         # 取最近 1 个月的「按应用」流量分布（画分布图用这个）
//! ```
//!
//! 结果打到标准输出，重定向到文件即可：
//!
//! ```bash
//! cargo run --release -- order > ../cpp/data/netflow_monthly.csv
//! ```

use chrono::{Datelike, Local};
use hnu_query::{
    cas::login::CasToken,
    netflow::{self, login::NetflowToken},
};
use std::{
    env,
    io::{BufWriter, Write},
    time::Duration,
};

/// 账单里的字节数换算成 GB 用
const BYTES_PER_GB: f64 = 1024.0 * 1024.0 * 1024.0;
/// 流量明细里的 KB 换算成 GB 用（接口文档写的单位是 KB）
const KB_PER_GB: f64 = 1024.0 * 1024.0;

#[tokio::main]
async fn main() {
    let args: Vec<String> = env::args().skip(1).collect();
    let mode = args.first().map_or("order", String::as_str);

    // ---- 第 0 步：拿凭据。只从环境变量读，避免密码进代码、进 git ----
    let stu_id = env::var("HNU_STU_ID").unwrap_or_default();
    let password = env::var("HNU_PASSWORD").unwrap_or_default();
    if stu_id.is_empty() || password.is_empty() {
        fail("请先设置环境变量 HNU_STU_ID 和 HNU_PASSWORD（别写进代码里）");
    }

    // ---- 第 1 步：登录统一身份认证系统（CAS），拿 CAS 令牌 ----
    let cas_token = match CasToken::acquire_by_login(&stu_id, &password).await {
        Ok(t) => t,
        Err(e) => fail(&format!(
            "CAS 登录失败：{e:?}\n常见原因：不在校园网/VPN 内、密码错、需要双因子认证、账号被锁"
        )),
    };
    eprintln!("[1/2] CAS 登录成功");

    // ---- 第 2 步：用 CAS 令牌换校园网流量系统的令牌 ----
    let netflow_token = match NetflowToken::acquire_by_cas_login(&cas_token).await {
        Ok(t) => t,
        Err(e) => fail(&format!("校园网流量系统登录失败：{e:?}")),
    };
    eprintln!("[2/2] 校园网流量系统登录成功");

    // ---- 第 3 步：取数并输出 CSV ----
    match mode {
        "order" => export_by_order(&netflow_token).await,
        "detail" => {
            let months: u32 = args.get(1).and_then(|s| s.parse().ok()).unwrap_or(12);
            export_by_month_detail(&netflow_token, months).await;
        }
        "dist" => {
            let months: u32 = args.get(1).and_then(|s| s.parse().ok()).unwrap_or(1);
            export_distribution(&netflow_token, months).await;
        }
        other => fail(&format!(
            "未知模式 `{other}`。可用：order（账单） / detail <月数>（按月趋势） / dist <月数>（按应用分布）"
        )),
    }
}

/// 用「流量账单」接口取数。
///
/// 这个接口一次就能拿到**所有**历史月份，字段还包含超额流量和应缴费用，
/// 是做按月图表的首选。CSV 列：
/// `month,total_gb,upload_gb,download_gb,over_usage_gb,should_pay_yuan`
async fn export_by_order(token: &NetflowToken) {
    let orders = match netflow::get_order(token).await {
        Ok(o) => o,
        Err(e) => fail(&format!("获取流量账单失败：{e:?}")),
    };
    if orders.is_empty() {
        fail("账单是空的，说明这个接口没返回数据（可能是账号从未用过校园网）");
    }

    let mut out = BufWriter::new(std::io::stdout());
    writeln!(
        out,
        "month,total_gb,upload_gb,download_gb,over_usage_gb,should_pay_yuan"
    )
    .unwrap();

    // 账单通常是「新 → 旧」，反转一下，画出来的折线才是从左到右的时间顺序
    for item in orders.iter().rev() {
        let upload_gb = item.upload_usage as f64 / BYTES_PER_GB;
        let download_gb = item.download_usage as f64 / BYTES_PER_GB;
        writeln!(
            out,
            "{},{:.2},{:.2},{:.2},{:.2},{:.2}",
            item.time,
            upload_gb + download_gb,
            upload_gb,
            download_gb,
            item.over_usage,
            item.should_pay
        )
        .unwrap();
    }
    out.flush().unwrap();
    eprintln!("完成：{} 个月", orders.len());
}

/// 用「月流量明细」接口取数：一次查一个月，往前推 `months` 个月。
///
/// 好处是能拿到 `items`（按应用分类的流量占比）；坏处是要循环发请求，慢，而且
/// 明细接口不返回账单金额。CSV 列：`month,total_gb,upload_gb,download_gb`
async fn export_by_month_detail(token: &NetflowToken, months: u32) {
    let now = Local::now();
    let (this_year, this_month) = (now.year() as u16, now.month() as u8);

    let mut out = BufWriter::new(std::io::stdout());
    writeln!(out, "month,total_gb,upload_gb,download_gb").unwrap();

    // back 从大到小，输出即按时间升序
    for back in (0..months).rev() {
        let (year, month) = shift_back(this_year, this_month, back);
        match netflow::get_month_detail(token, year, month).await {
            Ok(detail) => {
                writeln!(
                    out,
                    "{}-{:02},{:.2},{:.2},{:.2}",
                    year,
                    month,
                    detail.total / KB_PER_GB,
                    detail.upload / KB_PER_GB,
                    detail.download / KB_PER_GB,
                )
                .unwrap();
                // 顺手把用得最多的应用打到 stderr，方便你在终端里瞄一眼
                if let Some(top) = detail
                    .items
                    .iter()
                    .max_by(|a, b| a.total.partial_cmp(&b.total).unwrap())
                {
                    eprintln!(
                        "  {}-{:02} 拿得最多的：{}（{:.2} GB）",
                        year,
                        month,
                        top.app,
                        top.total / KB_PER_GB
                    );
                }
            }
            // 某个月没数据（比如还没开学）不该让整个程序挂掉，跳过就好
            Err(e) => eprintln!("跳过 {year}-{month:02}：{e:?}"),
        }
        // 温柔一点，别把学校接口打爆
        tokio::time::sleep(Duration::from_millis(400)).await;
    }
    out.flush().unwrap();
}

/// 用「月流量明细」接口导出**按应用**的流量分布 —— 这就是画分布图的数据源。
///
/// 一次查一个月；`items` 里每个元素是一个应用分类（形如 `/网络游戏/steam平台`），
/// 自带它占了当月多少比例。CSV 列：
/// `month,app,total_gb,download_gb,upload_gb,percentage`
async fn export_distribution(token: &NetflowToken, months: u32) {
    let now = Local::now();
    let (this_year, this_month) = (now.year() as u16, now.month() as u8);

    let mut out = BufWriter::new(std::io::stdout());
    writeln!(out, "month,app,total_gb,download_gb,upload_gb,percentage").unwrap();

    let mut written = 0usize;
    // back 从大到小，输出即按时间升序
    for back in (0..months).rev() {
        let (year, month) = shift_back(this_year, this_month, back);
        match netflow::get_month_detail(token, year, month).await {
            Ok(detail) => {
                let label = format!("{year}-{month:02}");
                for item in &detail.items {
                    // 应用的分类名理论上可能带英文逗号，会把 CSV 的列冲乱，
                    // 这里换成全角逗号——只影响显示，不影响数值。
                    let app = item.app.replace(',', "，");
                    writeln!(
                        out,
                        "{label},{app},{:.4},{:.4},{:.4},{:.4}",
                        item.total / KB_PER_GB,
                        item.download / KB_PER_GB,
                        item.upload / KB_PER_GB,
                        item.percentage,
                    )
                    .unwrap();
                }
                written += detail.items.len();
                eprintln!(
                    "  {label}：{} 项应用，合计 {:.2} GB",
                    detail.items.len(),
                    detail.total / KB_PER_GB
                );
            }
            // 某个月没数据（比如还没开学）不该让整个程序挂掉
            Err(e) => eprintln!("跳过 {year}-{month:02}：{e:?}"),
        }
        // 温柔一点，别把学校接口打爆
        tokio::time::sleep(Duration::from_millis(400)).await;
    }
    out.flush().unwrap();
    if written == 0 {
        fail("一条分布数据都没拿到。常见原因：不在校园网/VPN 内，或这段时间确实没有流量记录");
    }
    eprintln!("完成：{written} 行");
}

/// 把「年 + 月」往回退 `back` 个月。换成「绝对月序号」算最不容易出错。
fn shift_back(year: u16, month: u8, back: u32) -> (u16, u8) {
    let absolute = year as u32 * 12 + (month as u32 - 1);
    let absolute = absolute.saturating_sub(back);
    ((absolute / 12) as u16, (absolute % 12) as u8 + 1)
}

/// 统一的报错出口
fn fail(message: &str) -> ! {
    eprintln!("错误：{message}");
    std::process::exit(1)
}
