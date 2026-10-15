"""
生成 main/ui_fonts.c（KK_UI 所需的 u8g2 字模子集）

字符集取自 main/ui_app.c 与 main/app_main.c 中的全部字符串字面量，
再并上可打印 ASCII（0x20~0x7E）以覆盖动态数字和符号。
通过 LEDFont 在线 API 生成，只提交字符本身，不提交源码或其它内容。
接口说明：https://ledfont.botelvdong.com/agent-api.md

用法：python tools/gen_ui_fonts.py
"""
import json
import pathlib
import re
import sys
import time
import urllib.error
import urllib.request

API = "https://ledfont.botelvdong.com/api/v1/compressed-fonts/generate"
ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCES = [ROOT / "main" / "ui_app.c", ROOT / "main" / "app_main.c"]
OUT_C = ROOT / "main" / "ui_fonts.c"
OUT_JSON = ROOT / "main" / "ui_fonts.json"

ASCII = "".join(chr(c) for c in range(0x20, 0x7F))
HOME_LABELS = "运行设置状态关于"
BIG_CHARS = "0123456789.%-"

STRING_RE = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
ESCAPES = {"n": "", "t": "", "r": "", '"': '"', "\\": "\\", "'": "'", "?": "?", "0": ""}


def literal_chars():
    chars = set()
    for path in SOURCES:
        text = path.read_text(encoding="utf-8")
        # 去掉注释，避免把注释里的引号内容当成界面文字
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        text = re.sub(r"//[^\n]*", "", text)
        for m in STRING_RE.finditer(text):
            s = re.sub(r"\\(.)", lambda e: ESCAPES.get(e.group(1), ""), m.group(1))
            chars.update(c for c in s if c >= " ")
    return chars


def ordered(chars):
    return "".join(sorted(set(chars), key=ord))


def generate(font, size, text, array_name):
    body = json.dumps({"font": font, "size": size, "text": text, "array_name": array_name},
                      ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(API, data=body, method="POST",
                                 headers={"Content-Type": "application/json; charset=utf-8",
                                          "Accept": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        sys.exit(f"{array_name}: HTTP {e.code}\n{e.read().decode('utf-8', 'replace')}")
    d = data["data"]
    want = set(text)
    got = set(d["characters"])
    if want - got:
        sys.exit(f"{array_name}: missing glyphs {ordered(want - got)!r}")
    return d


def main():
    ui_chars = ordered(literal_chars() | set(ASCII))
    fonts = [
        # (数组名, 字体, 字号, 字符, 用途)
        ("ui_font_home", "wenquanyi_13px", 14, ordered(HOME_LABELS), "首页标签"),
        ("ui_font_title", "wenquanyi_9pt", 12, ui_chars, "标题栏"),
        ("ui_font_body", "wenquanyi_13px", 14, ui_chars, "正文/菜单/弹框"),
        ("ui_font_big", "WenQuanDengKuanWeiMiHei", 26, ordered(BIG_CHARS), "运行页大号数字"),
    ]

    parts = [
        "/*",
        " * KK_UI 界面字模，由 tools/gen_ui_fonts.py 通过 LEDFont Agent API v1 生成，请勿手改。",
        " * 字体来源：文泉驿（WenQuanYi）；生成服务未返回字体许可信息，",
        " * 字模不随 KK_UI 的 MIT 许可证授权，详见 ui_fonts.json。",
        " */",
        '#include "ui_fonts.h"',
        "",
    ]
    meta = {"source": "https://ledfont.botelvdong.com/agent-api.md",
            "generated_at": time.strftime("%Y-%m-%d"),
            "license_note": "Service response contains no font license.",
            "fonts": []}

    for i, (name, font, size, text, role) in enumerate(fonts):
        if i:
            time.sleep(1)
        d = generate(font, size, text, name)
        print(f"{name}: {font} {size}px, {d['glyph_count']} glyphs, {d['byte_count']} B")
        parts += [f"/* {role}：{font} {size}px，{d['glyph_count']} 字形 */", d["c_source"].strip(), ""]
        meta["fonts"].append({"array_name": name, "role": role, "font": font, "size": size,
                              "glyph_count": d["glyph_count"], "byte_count": d["byte_count"],
                              "characters": d["characters"]})

    OUT_C.write_text("\n".join(parts), encoding="utf-8", newline="\n")
    OUT_JSON.write_text(json.dumps(meta, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {OUT_C.relative_to(ROOT)} and {OUT_JSON.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
