#!/usr/bin/env python3
"""把智谱 GLM 当私教的一个最小示例。

只用标准库（urllib / json），不装任何东西就能跑。

用法：
    export GLM_API_KEY=你的key          # 你自己的 key，别写进代码、别提交到 git
    python3 ask_glm.py explain                      # 让它读 CSV，讲这张图的趋势
    python3 ask_glm.py review cpp/main.cpp          # 让它批改代码
    python3 ask_glm.py ask "为什么这里用 CSV 不用 JSON"

可选环境变量：
    GLM_MODEL     默认 glm-4-flash（便宜快），换成 glm-4-plus / glm-4-air 也行
    GLM_BASE_URL  默认智谱开放平台的 v4 地址
"""

import json
import os
import sys
import time
import urllib.error
import urllib.request

# 智谱开放平台的 OpenAI 兼容接口，最后一段就是 chat/completions
BASE_URL = os.environ.get("GLM_BASE_URL", "https://open.bigmodel.cn/api/paas/v4/chat/completions")
# 注意：下面这个默认值是空的。key 只从环境变量来，绝不硬编码进源码。
API_KEY = os.environ.get("GLM_API_KEY", "")
MODEL = os.environ.get("GLM_MODEL", "glm-4-flash")

SYSTEM_PROMPT = (
    "你是一个带新手的编程私教。对方是本科生，Rust 和 C++ 都在入门阶段。"
    "回答要求：先说结论，再讲为什么；代码要能直接跑；不要堆砌术语，"
    "遇到他代码里的问题时先指出是哪一行、为什么错、怎么改。"
)


def ask(messages, temperature=0.6, retries=3, timeout=60):
    """调一次 GLM。失败自动重试（指数退避），专门处理限流。"""
    if not API_KEY:
        sys.exit("请先设置环境变量 GLM_API_KEY（在智谱开放平台申请，别写进代码里）")

    payload = json.dumps(
        {"model": MODEL, "messages": messages, "temperature": temperature, "stream": False}
    ).encode("utf-8")
    headers = {"Content-Type": "application/json", "Authorization": f"Bearer {API_KEY}"}

    for attempt in range(retries):
        request = urllib.request.Request(BASE_URL, data=payload, headers=headers)
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                data = json.loads(response.read().decode("utf-8"))
            # 正常返回长这样：choices[0].message.content
            return data["choices"][0]["message"]["content"]

        except urllib.error.HTTPError as error:
            body = error.read().decode("utf-8", errors="replace")
            # 429 是限流，5xx 是服务端抽风，这两种等一会儿重试是有意义的
            if error.code in (429, 500, 502, 503) and attempt < retries - 1:
                wait = 2**attempt  # 1s, 2s, 4s...
                print(f"[{error.code}] 等一下重试（{wait}s）...", file=sys.stderr)
                time.sleep(wait)
                continue
            sys.exit(f"请求失败 {error.code}：{body}")

        except urllib.error.URLError as error:
            if attempt < retries - 1:
                time.sleep(2**attempt)
                continue
            sys.exit(f"网络不通：{error.reason}（校园网/VPN 或者代理的问题？）")

    sys.exit("重试次数用完了")


def read_file(path, limit=8000):
    with open(path, encoding="utf-8") as handle:
        text = handle.read()
    return text[:limit] + ("\n...(内容过长已截断)" if len(text) > limit else "")


def main():
    command = sys.argv[1] if len(sys.argv) > 1 else "explain"

    if command == "explain":
        csv_path = sys.argv[2] if len(sys.argv) > 2 else "cpp/data/sample_monthly.csv"
        prompt = (
            "下面是我从学校校园网系统导出的按月流量数据（CSV）。"
            "请做三件事：1) 用两三句话总结流量趋势，指出哪几个月最高、可能的原因；"
            "2) 如果我要用 C++ 把它画成图，你会怎么组织代码；"
            "3) 给我出两道练习题检查我是否真的看懂了这个数据结构。\n\n"
            f"```csv\n{read_file(csv_path)}\n```"
        )

    elif command == "review":
        file_path = sys.argv[2] if len(sys.argv) > 2 else "cpp/main.cpp"
        prompt = (
            "请批改下面这段代码。要求：先列问题（指出行号和原因），再给修改后的片段，"
            "最后说一条我这次写得好的地方。\n\n"
            f"```\n{read_file(file_path)}\n```"
        )

    elif command == "ask":
        if len(sys.argv) < 3:
            sys.exit('用法：python3 ask_glm.py ask "你的问题"')
        prompt = sys.argv[2]

    else:
        sys.exit(f"未知命令 {command}。可用：explain / review / ask")

    answer = ask(
        [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": prompt},
        ]
    )
    print(answer)


if __name__ == "__main__":
    main()
