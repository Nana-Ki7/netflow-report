// netflow-chart —— 把校园网流量 CSV 画成图
//
// 一个程序吃两种 CSV，自己看表头判断是哪种：
//   A. 按月汇总（列里有 month,total_gb…）→ 月度趋势：柱状图 + 上传/下载折线
//   B. 按应用分布（列里有 app）        → 流量分布：一级分类饼图 + 应用 Top15 条形图
//
// 两种都不依赖任何第三方库，都干两件事：
//   1) 在终端里打一个 ASCII 图（没网、没浏览器也能看）
//   2) 生成一个 chart.html，用 ECharts 画出正经的图
//
// 编译（Linux / macOS / WSL 都行）：
//     g++ -std=c++17 -O2 -Wall -o netflow-chart main.cpp
// 运行：
//     ./netflow-chart data/sample_monthly.csv
//     然后浏览器打开生成的 chart.html
//
// 数据是上一层的 Rust 程序导出的 CSV，列名见文件头部的注释。

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------- 数据结构

/// 一个月的数据。
/// 缺失的列用 -1 表示「这列不存在」，画图时就不会硬塞一条 0 值的线。
struct MonthData {
    std::string month;    // "2026-03"
    double total = 0.0;   // 总流量，GB
    double upload = -1.0; // 上传，GB
    double download = -1.0;
    double over = -1.0;   // 超额流量，GB
    double pay = -1.0;    // 应缴费用，元
};

// ---------------------------------------------------------------- 小工具

/// 按分隔符切字符串。够用就行：我们的 CSV 里没有引号、逗号和换行。
std::vector<std::string> split(const std::string& line, char sep) {
    std::vector<std::string> parts;
    std::string current;
    std::istringstream stream(line);
    while (std::getline(stream, current, sep)) {
        parts.push_back(current);
    }
    // 结尾是分隔符时 std::getline 会把最后一段吞掉，这里补回来
    if (!line.empty() && line.back() == sep) {
        parts.emplace_back();
    }
    return parts;
}

/// 去掉首尾空白
std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

/// 字符串转 double；转不了就返回 fallback（CSV 里可能是空的）
double toDouble(const std::string& s, double fallback) {
    const std::string t = trim(s);
    if (t.empty()) {
        return fallback;
    }
    try {
        return std::stod(t);
    } catch (const std::exception&) {
        return fallback;
    }
}

// ---------------------------------------------------------------- 读 CSV

/// 读 CSV。靠第一行的**列名**定位每一列，列的个数和顺序变了都不用改代码。
std::vector<MonthData> readCsv(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("打不开文件：" + path);
    }

    std::vector<MonthData> rows;
    std::map<std::string, size_t> column; // 列名 -> 第几列
    std::string line;
    bool first_row = true;

    while (std::getline(in, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue; // 空行和注释行直接跳过
        }
        const auto cells = split(trimmed, ',');

        if (first_row) {
            first_row = false;
            for (size_t i = 0; i < cells.size(); ++i) {
                column[trim(cells[i])] = i;
            }
            if (column.count("month") == 0 || column.count("total_gb") == 0) {
                throw std::runtime_error("CSV 至少要有 month 和 total_gb 两列");
            }
            continue;
        }

        // 按列名取值；列不存在就说明这列的数据不存在
        const auto cell = [&](const std::string& name) -> std::string {
            const auto it = column.find(name);
            if (it == column.end() || it->second >= cells.size()) {
                return "";
            }
            return cells[it->second];
        };

        MonthData row;
        row.month = trim(cell("month"));
        if (row.month.empty()) {
            continue;
        }
        row.total = toDouble(cell("total_gb"), 0.0);
        row.upload = toDouble(cell("upload_gb"), -1.0);
        row.download = toDouble(cell("download_gb"), -1.0);
        row.over = toDouble(cell("over_usage_gb"), -1.0);
        row.pay = toDouble(cell("should_pay_yuan"), -1.0);
        rows.push_back(row);
    }

    if (rows.empty()) {
        throw std::runtime_error("CSV 里一行数据都没有：" + path);
    }
    return rows;
}

// ---------------------------------------------------------------- 读「分布」CSV

/// 一条「应用维度」的流量记录：某个月里，某个应用用了多少流量。
/// `app` 是接口给的路径式分类，形如 `/网络游戏/steam平台`。
struct DistItem {
    std::string month;      // "2026-09"
    std::string app;        // "/网络游戏/steam平台"
    double total = 0.0;     // GB
    double download = -1.0; // GB，-1 表示这列不存在
    double upload = -1.0;   // GB
    double percentage = -1.0;
};

/// 一眼看出这份 CSV 是「按月汇总」还是「按应用分布」：表头里有 `app` 就是分布数据。
/// 这样一个可执行文件就能吃两种数据，不用编译两遍。
bool looksLikeDistribution(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("打不开文件：" + path);
    }
    std::string line;
    while (std::getline(in, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue; // 空行和注释行跳过，表头才是第一行有内容的
        }
        return trimmed.find("app") != std::string::npos;
    }
    throw std::runtime_error("CSV 是空的：" + path);
}

/// 读「按应用分布」的 CSV：`month,app,total_gb,download_gb,upload_gb,percentage`
std::vector<DistItem> readDistributionCsv(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("打不开文件：" + path);
    }

    std::vector<DistItem> rows;
    std::map<std::string, size_t> column; // 列名 -> 第几列
    std::string line;
    bool first_row = true;

    while (std::getline(in, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }
        const auto cells = split(trimmed, ',');

        if (first_row) {
            first_row = false;
            for (size_t i = 0; i < cells.size(); ++i) {
                column[trim(cells[i])] = i;
            }
            if (column.count("app") == 0 || column.count("total_gb") == 0) {
                throw std::runtime_error("分布 CSV 至少要有 app 和 total_gb 两列");
            }
            continue;
        }

        const auto cell = [&](const std::string& name) -> std::string {
            const auto it = column.find(name);
            if (it == column.end() || it->second >= cells.size()) {
                return "";
            }
            return cells[it->second];
        };

        DistItem row;
        row.app = trim(cell("app"));
        row.month = trim(cell("month"));
        if (row.app.empty()) {
            continue;
        }
        row.total = toDouble(cell("total_gb"), 0.0);
        row.download = toDouble(cell("download_gb"), -1.0);
        row.upload = toDouble(cell("upload_gb"), -1.0);
        row.percentage = toDouble(cell("percentage"), -1.0);
        rows.push_back(row);
    }

    if (rows.empty()) {
        throw std::runtime_error("CSV 里一行数据都没有：" + path);
    }
    return rows;
}

/// 把 `/网络游戏/steam平台` 拆成 {"网络游戏", "steam平台"}
std::vector<std::string> splitAppPath(const std::string& app) {
    std::vector<std::string> parts;
    std::string current;
    for (const char ch : app) {
        if (ch == '/') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

/// 一级分类（饼图用）：`/网络游戏/steam平台` -> `网络游戏`
std::string topCategory(const std::string& app) {
    const auto parts = splitAppPath(app);
    return parts.empty() ? "未分类" : parts.front();
}

/// 展示名（条形图用）：去掉开头的斜杠，`/网络游戏/steam平台` -> `网络游戏/steam平台`
std::string displayName(const std::string& app) {
    std::string s = app;
    while (!s.empty() && s.front() == '/') {
        s.erase(s.begin());
    }
    return s.empty() ? app : s;
}

/// 按总流量从大到小排序
void sortByTotalDesc(std::vector<DistItem>& items) {
    std::sort(items.begin(), items.end(),
              [](const DistItem& a, const DistItem& b) { return a.total > b.total; });
}

/// 一个字符串在终端里占几列。中文/全角算 2 列，其余算 1 列。
/// 不处理这个的话，终端里的柱状图会歪。
std::size_t displayWidth(const std::string& s) {
    std::size_t width = 0;
    for (std::size_t i = 0; i < s.size();) {
        const auto first = static_cast<unsigned char>(s[i]);
        std::size_t length = 1;
        unsigned int code_point = first;
        if (first >= 0xF0) {
            length = 4;
            code_point = first & 0x07u;
        } else if (first >= 0xE0) {
            length = 3;
            code_point = first & 0x0Fu;
        } else if (first >= 0xC0) {
            length = 2;
            code_point = first & 0x1Fu;
        }
        for (std::size_t k = 1; k < length && i + k < s.size(); ++k) {
            code_point = (code_point << 6) |
                         (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
        }
        const bool wide = code_point >= 0x1100 &&
                          (code_point <= 0x115F || code_point == 0x2329 ||
                           code_point == 0x232A || (code_point >= 0x2E80 && code_point <= 0xA4CF) ||
                           (code_point >= 0xAC00 && code_point <= 0xD7A3) ||
                           (code_point >= 0xF900 && code_point <= 0xFAFF) ||
                           (code_point >= 0xFE30 && code_point <= 0xFE6F) ||
                           (code_point >= 0xFF00 && code_point <= 0xFF60) ||
                           (code_point >= 0xFFE0 && code_point <= 0xFFE6) ||
                           (code_point >= 0x20000 && code_point <= 0x3FFFD));
        width += wide ? 2u : 1u;
        i += length;
    }
    return width;
}

/// 按「显示宽度」补空格。宽度已经够了就原样返回。
std::string padTo(const std::string& s, std::size_t target) {
    const std::size_t width = displayWidth(s);
    return width >= target ? s : s + std::string(target - width, ' ');
}

// ---------------------------------------------------------------- 终端柱状图

void printAsciiChart(const std::vector<MonthData>& rows) {
    const double max_total = std::max_element(
                                 rows.begin(), rows.end(),
                                 [](const MonthData& a, const MonthData& b) { return a.total < b.total; })
                                 ->total;
    constexpr int kWidth = 40;

    std::cout << "\n校园网流量 · 按月分布（单位 GB）\n";
    std::cout << std::string(58, '-') << "\n";
    for (const auto& row : rows) {
        const int bars = max_total > 0.0
                             ? std::max(1, static_cast<int>(std::lround(row.total / max_total * kWidth)))
                             : 0;
        std::ostringstream label;
        label << row.month << " | " << std::string(bars, '#') << " " << std::fixed
              << std::setprecision(1) << row.total;
        std::cout << label.str() << "\n";
    }
    std::cout << std::string(58, '-') << "\n";

    double sum = 0.0;
    for (const auto& row : rows) {
        sum += row.total;
    }
    std::cout << "共 " << rows.size() << " 个月，合计 " << std::fixed << std::setprecision(1) << sum
              << " GB，月均 " << (sum / static_cast<double>(rows.size())) << " GB\n";
}

// ---------------------------------------------------------------- 终端分布图

/// 每个月一段，把用得最多的前 12 个应用横着排出来。
void printAsciiDistribution(const std::vector<DistItem>& items) {
    std::map<std::string, std::vector<DistItem>> by_month;
    for (const auto& item : items) {
        by_month[item.month.empty() ? "本月" : item.month].push_back(item);
    }

    constexpr int kWidth = 30;
    constexpr size_t kTopN = 12;

    for (auto& [month, list] : by_month) {
        sortByTotalDesc(list);

        double sum = 0.0;
        for (const auto& item : list) {
            sum += item.total;
        }
        const double max_total = list.front().total;

        std::cout << "\n校园网流量 · 按应用分布 " << month << "（单位 GB，共 " << list.size()
                  << " 项，合计 " << std::fixed << std::setprecision(2) << sum << "）\n";
        std::cout << std::string(66, '-') << "\n";
        for (size_t i = 0; i < list.size() && i < kTopN; ++i) {
            const auto& item = list[i];
            const int bars =
                max_total > 0.0
                    ? std::max(1, static_cast<int>(std::lround(item.total / max_total * kWidth)))
                    : 0;
            std::ostringstream label;
            label << std::setw(2) << (i + 1) << ". " << displayName(item.app);
            std::cout << padTo(label.str(), 38) << "| " << std::string(bars, '#')
                      << " " << std::fixed << std::setprecision(2) << item.total;
            if (item.percentage >= 0.0) {
                std::cout << " (" << std::fixed << std::setprecision(1) << item.percentage * 100
                          << "%)";
            }
            std::cout << "\n";
        }
        if (list.size() > kTopN) {
            std::cout << "    …… 其余 " << (list.size() - kTopN) << " 项未显示\n";
        }
        std::cout << std::string(66, '-') << "\n";
    }
}

// ---------------------------------------------------------------- 生成 HTML

std::string joinNumbers(const std::vector<MonthData>& rows, double MonthData::*field) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2);
    for (size_t i = 0; i < rows.size(); ++i) {
        out << (i == 0 ? "" : ",") << rows[i].*field;
    }
    return out.str();
}

std::string joinMonths(const std::vector<MonthData>& rows) {
    std::string out;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        out += "\"" + rows[i].month + "\"";
    }
    return out;
}

void writeHtml(const std::vector<MonthData>& rows, const std::string& path) {
    const bool has_upload = rows.front().upload >= 0.0;
    const bool has_download = rows.front().download >= 0.0;
    const bool has_pay = rows.front().pay >= 0.0;

    std::vector<std::string> series;
    series.push_back(
        "{ name: '总流量', type: 'bar', data: [" + joinNumbers(rows, &MonthData::total) +
        "], itemStyle: { borderRadius: [4, 4, 0, 0], color: '#5b8ff9' }, "
        "label: { show: true, position: 'top', formatter: '{c}' } }");
    if (has_upload) {
        series.push_back("{ name: '上传', type: 'line', smooth: true, symbolSize: 6, data: [" +
                         joinNumbers(rows, &MonthData::upload) +
                         "], itemStyle: { color: '#61ddaa' } }");
    }
    if (has_download) {
        series.push_back("{ name: '下载', type: 'line', smooth: true, symbolSize: 6, data: [" +
                         joinNumbers(rows, &MonthData::download) +
                         "], itemStyle: { color: '#f6bd16' } }");
    }
    if (has_pay) {
        series.push_back(
            "{ name: '应缴费用', type: 'line', yAxisIndex: 1, smooth: true, symbolSize: 6, data: [" +
            joinNumbers(rows, &MonthData::pay) + "], itemStyle: { color: '#e8684a' } }");
    }

    std::string joined;
    for (size_t i = 0; i < series.size(); ++i) {
        joined += (i == 0 ? "" : ",\n    ") + series[i];
    }

    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("写不了文件：" + path);
    }
    out << R"(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<title>校园网流量 · 按月分布</title>
<script src="https://cdn.jsdelivr.net/npm/echarts@5/dist/echarts.min.js"></script>
<style>
  html, body { margin: 0; height: 100%; }
  body { font-family: system-ui, -apple-system, "PingFang SC", "Microsoft YaHei", sans-serif; background: #f7f8fa; }
  #chart { width: 100%; height: 100%; }
  #tip { position: fixed; left: 12px; bottom: 8px; color: #999; font-size: 12px; }
</style>
</head>
<body>
<div id="chart"></div>
<div id="tip">数据来自 hnu_query 校园网流量接口 · 由 netflow-chart 生成</div>
<script>
const months = [)" << joinMonths(rows)
        << R"(];
const chart = echarts.init(document.getElementById('chart'));
chart.setOption({
  title: { text: '校园网流量 · 按月分布', subtext: '柱：总流量(GB)　线：上传/下载(GB))"
        << (has_pay ? "、应缴费用(元)" : "") << R"(', left: 'center' },
  tooltip: { trigger: 'axis', axisPointer: { type: 'shadow' } },
  legend: { top: 62 },
  grid: { left: 70, right: )" << (has_pay ? "70" : "40") << R"(, top: 110, bottom: 60 },
  xAxis: { type: 'category', data: months, axisLabel: { rotate: 45 } },
  yAxis: [
    { type: 'value', name: '流量 (GB)' },
    { type: 'value', name: '费用 (元)', show: )" << (has_pay ? "true" : "false")
        << R"(, splitLine: { show: false } }
  ],
  series: [
    )" << joined << R"(
  ]
});
window.addEventListener('resize', () => chart.resize());
</script>
</body>
</html>
)";
    out.close();
    std::cout << "已生成图表：" << path << "\n";
}

// ---------------------------------------------------------------- 生成分布 HTML

/// JSON 字符串转义：只处理最要紧的两个字符，够用
std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char ch : s) {
        if (ch == '"' || ch == '\\') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    return out;
}

/// 把「按应用分布」画成 HTML：每个月一块，左边一级分类饼图，右边应用 Top15 条形图
void writeDistributionHtml(const std::vector<DistItem>& items, const std::string& path) {
    std::map<std::string, std::vector<DistItem>> by_month;
    for (const auto& item : items) {
        by_month[item.month.empty() ? "本月" : item.month].push_back(item);
    }

    constexpr size_t kTopN = 15;

    std::ostringstream sections;
    std::ostringstream script;

    size_t index = 0;
    for (auto& [month, list] : by_month) {
        sortByTotalDesc(list);

        // 饼图：把同一一级分类下的应用加起来
        std::map<std::string, double> categories;
        double sum = 0.0;
        for (const auto& item : list) {
            categories[topCategory(item.app)] += item.total;
            sum += item.total;
        }

        // 条形图：取前 kTopN 项，倒过来放（ECharts 的横向条形图是从下往上画的）
        const size_t count = std::min(kTopN, list.size());
        std::vector<DistItem> top(list.begin(), list.begin() + count);
        std::reverse(top.begin(), top.end());

        sections << "  <section class=\"card\">\n"
                 << "    <h2>" << month << " · 应用流量分布 <span>合计 " << std::fixed
                 << std::setprecision(2) << sum << " GB · " << list.size() << " 项</span></h2>\n"
                 << "    <div class=\"charts\">\n"
                 << "      <div id=\"pie" << index << "\" class=\"pie\"></div>\n"
                 << "      <div id=\"bar" << index << "\" class=\"bar\"></div>\n"
                 << "    </div>\n"
                 << "  </section>\n";

        script << "  makePie('pie" << index << "', [";
        bool first = true;
        for (const auto& [name, value] : categories) {
            script << (first ? "" : ", ") << "{name: \"" << jsonEscape(name) << "\", value: "
                   << std::fixed << std::setprecision(2) << value << "}";
            first = false;
        }
        script << "]);\n";

        script << "  makeBar('bar" << index << "', [";
        for (size_t i = 0; i < top.size(); ++i) {
            script << (i == 0 ? "" : ", ") << "[\"" << jsonEscape(displayName(top[i].app))
                   << "\", " << std::fixed << std::setprecision(2) << top[i].total << "]";
        }
        script << "]);\n";

        ++index;
    }

    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("写不了文件：" + path);
    }
    out << R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<title>校园网流量 · 应用分布</title>
<script src="https://cdn.jsdelivr.net/npm/echarts@5/dist/echarts.min.js"></script>
<style>
  html, body { margin: 0; }
  body { font-family: system-ui, -apple-system, "PingFang SC", "Microsoft YaHei", sans-serif; background: #f7f8fa; padding-bottom: 24px; }
  .card { background: #fff; margin: 16px; padding: 8px 16px 20px; border-radius: 12px; box-shadow: 0 1px 3px rgba(0,0,0,.08); }
  .card h2 { font-size: 16px; margin: 10px 0 6px; }
  .card h2 span { font-weight: 400; font-size: 12px; color: #999; }
  .charts { display: flex; flex-wrap: wrap; gap: 12px; }
  .pie { flex: 1 1 340px; height: 420px; min-width: 300px; }
  .bar { flex: 2 1 520px; height: 420px; min-width: 300px; }
</style>
</head>
<body>
)HTML" << sections.str() << R"HTML(<script>
function makePie(id, data) {
  const chart = echarts.init(document.getElementById(id));
  chart.setOption({
    title: { text: '按一级分类', left: 'center', textStyle: { fontSize: 14 } },
    tooltip: { trigger: 'item', formatter: '{b}: {c} GB ({d}%)' },
    legend: { bottom: 0, type: 'scroll' },
    series: [{
      type: 'pie', radius: ['34%', '58%'], center: ['50%', '46%'], data: data,
      label: { formatter: '{b} {d}%' }, itemStyle: { borderColor: '#fff', borderWidth: 2 }
    }]
  });
  window.addEventListener('resize', () => chart.resize());
}
function makeBar(id, pairs) {
  const chart = echarts.init(document.getElementById(id));
  chart.setOption({
    title: { text: '应用 Top )HTML" << kTopN << R"HTML(（按总流量）', left: 'center', textStyle: { fontSize: 14 } },
    tooltip: { trigger: 'axis', axisPointer: { type: 'shadow' }, valueFormatter: v => v + ' GB' },
    grid: { left: 160, right: 70, top: 46, bottom: 24 },
    xAxis: { type: 'value', name: 'GB' },
    yAxis: { type: 'category', data: pairs.map(p => p[0]), axisLabel: { width: 150, overflow: 'truncate' } },
    series: [{
      type: 'bar', data: pairs.map(p => p[1]),
      itemStyle: { borderRadius: [0, 4, 4, 0], color: '#5b8ff9' },
      label: { show: true, position: 'right', formatter: '{c}' }
    }]
  });
  window.addEventListener('resize', () => chart.resize());
}
)HTML" << script.str() << R"HTML(</script>
<div id="tip" style="text-align:center;color:#999;font-size:12px">数据来自 hnu_query 校园网流量接口 · 由 netflow-chart 生成</div>
</body>
</html>
)HTML";
    out.close();
    std::cout << "已生成分布图：" << path << "\n";
}

} // namespace

int main(int argc, char** argv) {
    const std::string csv_path = argc > 1 ? argv[1] : "data/sample_monthly.csv";
    const std::string html_path = argc > 2 ? argv[2] : "chart.html";

    try {
        // 同一个程序吃两种 CSV：表头里有 app 的当「按应用分布」，否则当「按月汇总」
        if (looksLikeDistribution(csv_path)) {
            const auto items = readDistributionCsv(csv_path);
            printAsciiDistribution(items);
            writeDistributionHtml(items, html_path);
        } else {
            const auto rows = readCsv(csv_path);
            printAsciiChart(rows);
            writeHtml(rows, html_path);
        }
    } catch (const std::exception& e) {
        std::cerr << "出错了：" << e.what() << "\n";
        std::cerr << "用法：" << argv[0] << " [csv 路径] [输出的 html 路径]\n";
        return 1;
    }
    return 0;
}
