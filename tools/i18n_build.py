#!/usr/bin/env python3
"""把 po/*.po 转成 C 侧渲染用的 i18n JSON 字典。

背景：配置表单由 C（etd_api_config.c）生成 HTML，C 侧读不到 LuCI 的 .lmo，
因此构建期把 po 转成一份扁平的 msgid → msgstr JSON，运行时由 etd_i18n.c 加载。

产物（默认）：
  luci-app-easytier/root/usr/share/easytier/i18n/zh_Hans.json
  luci-app-easytier/root/usr/share/easytier/i18n/en.json      （identity 映射）

用法：
  python3 tools/i18n_build.py                 # 生成/更新上述文件
  python3 tools/i18n_build.py --check         # 校验文件是否与 po 同步（CI）
  python3 tools/i18n_build.py --out DIR       # 指定输出目录
  python3 tools/i18n_build.py --verbose       # 打印每个语言的条目数

po 变更后必须重跑本脚本并提交产物（tests/run_web.sh 会跑 --check）。
"""

import argparse
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_PO_DIR = os.path.join(ROOT, "luci-app-easytier", "po")
DEFAULT_OUT = os.path.join(ROOT, "luci-app-easytier", "root", "usr", "share",
                           "easytier", "i18n")

# po 文件里的 C 风格转义（只处理 po 实际会用到的几个）
_ESCAPES = {
    "n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\",
    "a": "\a", "b": "\b", "f": "\f", "v": "\v", "0": "\0",
}


def _unquote(text):
    """解析 po 里的一个 "..."/多行拼接串（已去掉关键字前缀）。"""
    out = []
    i = 0
    while i < len(text):
        ch = text[i]
        if ch == "\\" and i + 1 < len(text):
            out.append(_ESCAPES.get(text[i + 1], text[i + 1]))
            i += 2
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def _split_quoted(line):
    """把一个 po 行里的所有 "..." 片段解出来（用于长串跨行拼接）。"""
    parts = []
    i = 0
    n = len(line)
    while i < n:
        if line[i] != '"':
            i += 1
            continue
        j = i + 1
        while j < n:
            if line[j] == "\\":
                j += 2
                continue
            if line[j] == '"':
                break
            j += 1
        parts.append(_unquote(line[i + 1:j]))
        i = j + 1
    return parts


def parse_po(path, require_msgstr=True):
    """返回 {msgid: msgstr}，跳过表头、空翻译与 fuzzy 条目。

    require_msgstr=False 用于 .pot（模板里 msgstr 全是空串，只需要 msgid 集合）。
    """
    entries = {}
    msgid = None
    msgstr = None
    fuzzy = False
    mode = None

    def flush():
        nonlocal msgid, msgstr, fuzzy
        if not fuzzy and msgid and (msgstr or not require_msgstr):
            entries[msgid] = msgstr or msgid
        msgid = None
        msgstr = None
        fuzzy = False

    with open(path, "r", encoding="utf-8") as fp:
        for raw in fp:
            line = raw.rstrip("\r\n")
            stripped = line.strip()
            if not stripped:
                flush()
                mode = None
                continue
            if stripped.startswith("#"):
                if "fuzzy" in stripped and msgid is None:
                    fuzzy = True
                continue
            if stripped.startswith("msgid_plural"):
                continue
            if stripped.startswith("msgid "):
                if msgstr is not None:
                    flush()
                msgid = "".join(_split_quoted(stripped[len("msgid "):]))
                mode = "id"
                continue
            if stripped.startswith("msgstr["):
                idx = stripped.index("]")
                # 复数形式只取第一变化（本应用没有真正的复数文案）
                if stripped[7:idx].strip() == "0":
                    msgstr = "".join(_split_quoted(stripped[idx + 1:]))
                    mode = "str"
                else:
                    mode = None
                continue
            if stripped.startswith("msgstr "):
                msgstr = "".join(_split_quoted(stripped[len("msgstr "):]))
                mode = "str"
                continue
            if stripped.startswith('"'):
                chunk = "".join(_split_quoted(stripped))
                if mode == "id" and msgid is not None:
                    msgid += chunk
                elif mode == "str" and msgstr is not None:
                    msgstr += chunk
                continue
    flush()
    return entries


def dump_json(entries):
    """稳定的 JSON 文本：键排序、每条一行、保留 UTF-8 原文。"""
    if not entries:
        return "{}\n"
    body = ",\n".join(
        "%s: %s" % (json.dumps(k, ensure_ascii=False), json.dumps(v, ensure_ascii=False))
        for k, v in sorted(entries.items())
    )
    return "{\n" + body + "\n}\n"


def build_all(po_dir):
    """返回 {lang: entries}。en 由 .pot 生成 identity 映射。"""
    result = {}
    pot = os.path.join(po_dir, "templates", "easytier.pot")
    if os.path.exists(pot):
        ids = parse_po(pot, require_msgstr=False)
        result["en"] = {k: k for k in ids}
    for name in sorted(os.listdir(po_dir)):
        po = os.path.join(po_dir, name, "easytier.po")
        if name == "templates" or not os.path.exists(po):
            continue
        result[name] = parse_po(po)
    return result


def main(argv):
    ap = argparse.ArgumentParser(description="po → i18n JSON")
    ap.add_argument("--po-dir", default=DEFAULT_PO_DIR)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--check", action="store_true",
                    help="只校验，不写文件；有差异时退出码 1")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args(argv)

    langs = build_all(args.po_dir)
    if not langs:
        print("[i18n] 没有找到 po 文件：%s" % args.po_dir, file=sys.stderr)
        return 1

    stale = []
    for lang in sorted(langs):
        text = dump_json(langs[lang])
        path = os.path.join(args.out, "%s.json" % lang)
        if args.verbose:
            print("[i18n] %-10s %4d 条 → %s" % (lang, len(langs[lang]), path))
        if args.check:
            try:
                with open(path, "r", encoding="utf-8") as fp:
                    if fp.read() != text:
                        stale.append(path)
            except OSError:
                stale.append(path)
            continue
        os.makedirs(args.out, exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as fp:
            fp.write(text)

    if args.check:
        if stale:
            print("[i18n] 以下文件与 po 不一致，请重跑 tools/i18n_build.py：",
                  file=sys.stderr)
            for path in stale:
                print("  - %s" % path, file=sys.stderr)
            return 1
        print("[i18n] 全部最新")
        return 0

    print("[i18n] 已生成 %d 个语言包到 %s" % (len(langs), args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
