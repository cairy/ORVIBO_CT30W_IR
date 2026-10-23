#!/usr/bin/env python3
"""把 web/ 下的前端资源压缩后生成为 C 头文件，编进固件。

为什么编进固件、而不是丢进 LittleFS：
    ESP8266 的 OTA 只更新 app，不更新文件系统。网页若放文件系统，改一次网页就得
    接线刷一次 uploadfs —— OTA 的意义（不拆壳）就没了。编进 app 就跟固件一起 OTA。

为什么 gzip：
    index.html + app.js + app.css 原始约 75KB，gzip 后约 18KB。既省 app 空间，
    也省 WiFi 传输时间（手机打开更快）。

为什么 gzip 之外还要 minify（而"gzip 已经压过一遍了"不是理由）：
    本项目的注释是密集的中文说明文字，重复度极低 —— gzip 对"只在文中出现一次"的
    长句几乎没辙。实测（2026-09-28，compresslevel=9）：
        app.js     70,086 -> 24,240 (gzip) -> 15,613 (gzip+minify)   −35.6%
        app.css    13,477 ->  5,056         ->  2,445                −51.6%
        theme.css   4,371 ->  2,542         ->    788                −69.0%
        合计       87,934 -> 31,838         -> 18,846                −40.8%
    也就是说"gzip 之后再 minify"仍能少发约 13 KB。面板是在 AP 热点上打开的，
    这 13 KB 是实打实的打开时间。
    想调试原始版式时：设环境变量 WEB_ASSETS_NO_MINIFY=1 重新构建即可。

为什么 minify 做得这么保守（只砍"整行注释"与缩进，绝不合并行）：
    自己手写的 JS 压缩器最容易出的两类事是 ①改动 ASI 语义（`return\n x`）、
    ②把正则字面量或字符串里的 `//` 当注释砍掉（本项目 app.js 里就有
    `'http://'` 这样的字符串）。两者都是"编译通过、运行期静默出错"。
    所以这里的规则窄到几乎不可能出错：
      · 一行的 trim 结果**整行**都是注释（`// …` 或 `/* … */`）才整行丢弃；
        行尾注释一律保留；
      · 只去掉缩进与空行，**不合并任何行**（换行是 JS 的语法元素，不能动）。
    HTML 完全不压缩 —— 空白在 HTML 里有语义（内联元素间距、<pre>），
    而实测 index.html 的 gzip 收益只有 4.5%，不值这个险。

为什么还要额外产出一份"内联版"（WEB_INDEX_BUNDLE）：
    本机空闲堆只有约 2.8KB，而浏览器打开面板时会为 /、/app.css、/app.js **并发**开 3 条连接。
    并发响应同时占用多条 TCP 收发缓冲，会把堆抽干，紧接着 WiFi 驱动在系统上下文里
    处理信标（wDev_ProcessFiq → cnx_update_bss_more → memcpy）拿不到内存就空指针崩溃 ——
    实测（2026-09-26）表现为"打开/刷新网页就把设备刷重启"。
    把 CSS/JS 内联进 HTML 后，面板只需 1 次请求，并发连接数直接降到 1。
    （当初为"防旧缓存"还保留过 /app.css、/app.js 两个独立路由，2026-09-28 已删除：
     它们靠 gzip 后各占 3.1 KB + 15.6 KB flash，而 sendAsset_P 本来就发
     `Cache-Control: no-cache`，旧页面根本拿不到陈旧资源 —— 理由不成立。）

为什么还要展开 @import（web/theme.css）：
    设备上有三个页面（面板 / 配网页 / 安全模式页），配色只写一份、放在 web/theme.css，
    各页面用 `@import url("theme.css");` 引用。构建时在这里把它展开成实际内容，于是：
      · 页面仍然是一次请求拿全（配网页与安全模式页是"设备出问题时才用"的页面，
        绝不能依赖第二个请求成功 —— 那是救援页面）；
      · 配色只有一份来源，改一处三个页面一起变，不会各自漂移。
    直接在浏览器里以 file:// 打开源文件时，浏览器会自己解析 @import，同样能看。

由 platformio.ini 的 extra_scripts = pre:tools/gen_web_assets.py 在构建配置阶段自动执行，
不需要手工运行。
"""

import gzip
import os
import re

# 这个脚本有两种跑法，工程目录的取法不同：
#   1) PlatformIO 构建时（extra_scripts = pre:tools/gen_web_assets.py）：
#      由 SCons 宿主 exec，没有 __file__，只能从 env 里取 PROJECT_DIR；
#   2) 手工 `python tools/gen_web_assets.py`：用 __file__ 推。
try:
    Import("env")  # noqa: F821  SCons 注入的函数，手工执行时不存在
    ROOT = env["PROJECT_DIR"]  # noqa: F821
except NameError:
    ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

WEB_DIR = os.path.join(ROOT, "web")
OUT_PATH = os.path.join(ROOT, "src", "network", "web_assets.h")

# 要读取的源文件：C 符号名留空表示"只作输入、不单独产出数组"。
# app.css / app.js / index.html 都是这种 —— 它们的内容会被内联进 WEB_INDEX_BUNDLE，
# 再单独编一份就只是为了那两个已删除的 /app.css、/app.js 路由（见下面的 OUTPUTS 说明）。
SOURCES = [
    (None, "index.html", "text/html; charset=utf-8"),
    (None, "app.css", "text/css; charset=utf-8"),
    (None, "app.js", "application/javascript; charset=utf-8"),
    ("WEB_SETUP_HTML", "setup.html", "text/html; charset=utf-8"),
    ("WEB_SAFE_HTML", "safe.html", "text/html; charset=utf-8"),
]

THEME_FILE = "theme.css"
# 与源文件里的写法必须逐字一致，否则展开会静默失效（配色集体丢失）
IMPORT_THEME = '@import url("theme.css");'


def emit_bytes(symbol, data):
    """生成 16 字节一行的 PROGMEM 数组，避免单行过长。"""
    lines = ["static const uint8_t %s[] PROGMEM = {" % symbol]
    for offset in range(0, len(data), 16):
        chunk = data[offset:offset + 16]
        lines.append("  " + "".join("0x%02X," % b for b in chunk))
    lines.append("};")
    return "\n".join(lines)


# index.html 里对 CSS/JS 的引用原文（内联时替换掉）。
# 与 web/index.html 里的写法必须逐字一致，否则内联会静默失效
# —— 面板会退回 3 条并发请求，也就是我们要避免的那条路径。
LINK_TAG = '<link rel="stylesheet" href="app.css">'
SCRIPT_TAG = '<script src="app.js"></script>'


def inline_theme(text, theme):
    """把 `@import url("theme.css");` 换成 theme.css 的实际内容。

    只认这一条 import —— 不要写成通用 CSS 解析器，那样既容易出错，
    也会让"内联失败"从编译期错误退化成运行期悄悄少一段样式。
    """
    if IMPORT_THEME not in text:
        return text
    return text.replace(IMPORT_THEME, theme)


# ============================================================================
# minify（默认开，WEB_ASSETS_NO_MINIFY=1 关）——仅限 CSS 与 JS，规则见文件头注释
# ============================================================================

MINIFY = os.environ.get("WEB_ASSETS_NO_MINIFY", "").strip() == ""


def minify_css(text):
    """去 /* 注释 */ + 去缩进/空行。

    注释**替换成一个空格**而不是空串：`1px/*x*/solid` 这种写法删空会拼成
    `1pxsolid`，换成空格就成了 `1px solid`，两种写法都安全。
    """
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return "\n".join(line.strip() for line in text.split("\n") if line.strip())


def minify_js(text):
    """整行注释 + 缩进。**不合并行**、**不删行尾注释**（理由见文件头注释）。"""
    out = []
    in_block = False
    for line in text.split("\n"):
        stripped = line.strip()
        if in_block:
            end = stripped.find("*/")
            if end < 0:
                continue                      # 块注释还没结束
            stripped = stripped[end + 2:].strip()
            in_block = False
            if not stripped:
                continue
        if stripped.startswith("/*"):
            # 只认"从本行行首开始的"块注释；行内出现的 /* 一律当代码处理
            end = stripped.find("*/", 2)
            if end < 0:
                in_block = True
                continue
            stripped = stripped[end + 2:].strip()
            if not stripped:
                continue
        if stripped.startswith("//"):
            continue
        out.append(stripped)
    return "\n".join(out)


MINIFIERS = {
    ".css": minify_css,
    ".js": minify_js,
}


def build_bundle(index_html, css, js):
    """把 CSS/JS 内联进 HTML，让面板一次请求就能拿全（理由见文件头注释）。"""
    text = index_html.decode("utf-8")
    if LINK_TAG not in text or SCRIPT_TAG not in text:
        raise SystemExit(
            "[gen_web_assets] index.html 里的 %s / %s 未找到，内联版无法生成"
            % (LINK_TAG, SCRIPT_TAG))
    text = text.replace(LINK_TAG, "<style>\n" + css.decode("utf-8") + "\n</style>")
    text = text.replace(SCRIPT_TAG, "<script>\n" + js.decode("utf-8") + "\n</script>")
    return text.encode("utf-8")


def main():
    parts = [
        "// 本文件由 tools/gen_web_assets.py 自动生成，请勿手改。",
        "// 改网页请改 web/ 下的源文件，构建时会自动重新生成。",
        "#ifndef WEB_ASSETS_H",
        "#define WEB_ASSETS_H",
        "",
        "#include <Arduino.h>",
        "",
        "// 资源均为 gzip 压缩后的字节流；服务时必须带 Content-Encoding: gzip",
        "#define WEB_ASSETS_GZIPPED 1",
        "",
    ]

    total_raw = 0
    total_gz = 0

    # 只读取，先不决定产出哪些数组（见 SOURCES / OUTPUTS 的分工）
    raw_by_name = {}
    for symbol, filename, mime in SOURCES:
        path = os.path.join(WEB_DIR, filename)
        if not os.path.exists(path):
            raise SystemExit("[gen_web_assets] 缺少源文件: %s" % path)
        with open(path, "rb") as handle:
            raw_by_name[filename] = handle.read()

    theme_path = os.path.join(WEB_DIR, THEME_FILE)
    if not os.path.exists(theme_path):
        raise SystemExit("[gen_web_assets] 缺少配色文件: %s" % theme_path)
    with open(theme_path, "r", encoding="utf-8") as handle:
        theme_css = handle.read()

    # minify 放在"展开 @import"之前：这样三个页面的 <style> 里内联的都是压缩过的
    # theme.css（HTML 本身不压缩，见 minify_css/minify_js 的说明）。
    raw_before = sum(len(v) for v in raw_by_name.values()) + len(theme_css)
    if MINIFY:
        for filename in list(raw_by_name):
            fn = MINIFIERS.get(os.path.splitext(filename)[1].lower())
            if fn is not None:
                raw_by_name[filename] = fn(raw_by_name[filename].decode("utf-8")).encode("utf-8")
        theme_css = minify_css(theme_css)
    raw_after = sum(len(v) for v in raw_by_name.values()) + len(theme_css)

    # 每个页面/CSS 都先展开 @import，再各自成为独立资源
    for filename in raw_by_name:
        if filename == THEME_FILE:
            continue
        text = raw_by_name[filename].decode("utf-8")
        raw_by_name[filename] = inline_theme(text, theme_css).encode("utf-8")

    # 产出的数组。
    #
    # ⚠️ 这里曾经还产出 WEB_INDEX_HTML / WEB_APP_CSS / WEB_APP_JS 三份独立数组，
    # 供 /app.css、/app.js 两个路由与"非内联版首页"使用，理由是"防旧缓存页面来取"。
    # 但这个理由不成立：sendAsset_P() 一直就带着 `Cache-Control: no-cache`
    # （web_server.cpp，注释写着"网页与固件同号更新，别让浏览器拿旧缓存"），
    # 旧页面根本拿不到陈旧资源。而 WEB_INDEX_HTML 更是**没有任何路由引用它**。
    # 三份加起来约 22.5 KB flash，纯属白占。故只产出真正会被服务的两份页面 + 内联版。
    outputs = []
    for symbol, filename, mime in SOURCES:
        if symbol is None:
            continue                      # index.html / app.css / app.js：只作内联输入
        outputs.append((symbol, raw_by_name[filename], mime, filename))

    # 内联版：面板一次请求拿全（见文件头注释）
    bundle = build_bundle(raw_by_name["index.html"],
                          raw_by_name["app.css"],
                          raw_by_name["app.js"])
    outputs.append(("WEB_INDEX_BUNDLE", bundle, "text/html; charset=utf-8",
                    "index.html(内联 css/js)"))

    for symbol, raw, mime, label in outputs:
        # mtime=0 让同样的输入产出同样的字节，避免每次构建都改动头文件
        packed = gzip.compress(raw, compresslevel=9, mtime=0)

        total_raw += len(raw)
        total_gz += len(packed)

        parts.append("// %s: %d -> %d bytes (gzip)" % (label, len(raw), len(packed)))
        parts.append(emit_bytes(symbol, packed))
        parts.append("static const size_t %s_LEN = %d;" % (symbol, len(packed)))
        parts.append('static const char %s_TYPE[] PROGMEM = "%s";' % (symbol, mime))
        parts.append("")

    parts.append("#endif // WEB_ASSETS_H")
    parts.append("")

    content = "\n".join(parts)

    # 内容没变就不重写，避免触发无谓的重新编译
    old = None
    if os.path.exists(OUT_PATH):
        with open(OUT_PATH, "r", encoding="utf-8") as handle:
            old = handle.read()

    if old != content:
        os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
        with open(OUT_PATH, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(content)
        print("[gen_web_assets] 已生成 %s (raw %d -> gzip %d bytes; minify %s)"
              % (os.path.relpath(OUT_PATH, ROOT), total_raw, total_gz,
                 ("开 %d -> %d B" % (raw_before, raw_after)) if MINIFY else "关"))
    else:
        # 措辞要说准：这里指的是"与上一次生成的 web_assets.h 内容一致，所以没有重写"，
        # 不是"没有资源"。以前写"资源无变化，跳过"，而它同时又会打印本次算出来的
        # gzip 字节数 —— 字节数变了却还说"无变化"，很容易让人怀疑固件里到底是不是新的。
        print("[gen_web_assets] 与已生成的 web_assets.h 一致，未重写 "
              "(raw %d / gzip %d bytes; minify %s)"
              % (total_raw, total_gz, "开" if MINIFY else "关"))


# 无条件执行：被 SCons 宿主 exec 时 __name__ 不是 "__main__"，
# 用 if __name__ == "__main__" 会导致构建时根本不跑。
main()
