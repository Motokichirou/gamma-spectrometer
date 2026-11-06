"""Локальный смысловой поиск по даташитам (docs/refs/*.pdf) и докам проекта (docs/*.md, SPEC.md).

Эмбеддинги — bge-m3 через локальную Ollama (127.0.0.1:11434); данные никуда не уходят.
Индекс — D:/gamma_data/refindex/ (вне репозитория).

  python tools/llm/refsearch.py build            # (пере)индексировать изменившиеся файлы
  python tools/llm/refsearch.py q "нагрузка ОУ на АЦП время установления" [-k 8] [--only DS12288]
"""
import argparse, hashlib, json, os, re, sys, time, urllib.request
import numpy as np

sys.stdout.reconfigure(encoding="utf-8")
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
IDX = os.environ.get("REFINDEX", "D:/gamma_data/refindex")
OLLAMA = "http://127.0.0.1:11434/api/embed"
MODEL = "bge-m3"
CHUNK, OVERLAP = 1400, 200


def sources():
    out = []
    for d, _, fs in os.walk(os.path.join(ROOT, "docs", "refs")):
        out += [os.path.join(d, f) for f in fs if f.lower().endswith((".pdf", ".txt", ".md"))]
    out += [os.path.join(ROOT, "docs", f) for f in os.listdir(os.path.join(ROOT, "docs")) if f.endswith(".md")]
    for extra in ("firmware/board_g474cb/SPEC.md", "CLAUDE.md", "firmware/README.md"):
        p = os.path.join(ROOT, extra)
        if os.path.exists(p):
            out.append(p)
    return sorted(set(out))


def split(text):
    text = re.sub(r"[ \t]+", " ", text).strip()
    if len(text) < 40:
        return []
    res, i = [], 0
    while i < len(text):
        res.append(text[i:i + CHUNK])
        i += CHUNK - OVERLAP
    return res


def pages(path):
    """-> [(page_no (1-based) или 0 для текстов, text)]"""
    if path.lower().endswith(".pdf"):
        import fitz
        with fitz.open(path) as doc:
            return [(n + 1, p.get_text()) for n, p in enumerate(doc)]
    txt = open(path, encoding="utf-8", errors="replace").read()
    # md: режем по заголовкам, номер «страницы» = номер строки начала раздела
    parts, start, buf = [], 1, []
    for ln, line in enumerate(txt.splitlines(), 1):
        if line.startswith("#") and buf:
            parts.append((start, "\n".join(buf))); buf, start = [], ln
        buf.append(line)
    parts.append((start, "\n".join(buf)))
    return parts


def embed(texts):
    vecs = []
    for i in range(0, len(texts), 32):
        body = json.dumps({"model": MODEL, "input": texts[i:i + 32]}).encode()
        req = urllib.request.Request(OLLAMA, body, {"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=600) as r:
            vecs += json.load(r)["embeddings"]
    v = np.asarray(vecs, dtype=np.float32)
    return v / np.linalg.norm(v, axis=1, keepdims=True)


def fhash(p):
    st = os.stat(p)
    return hashlib.md5(f"{st.st_size}:{st.st_mtime_ns}".encode()).hexdigest()


def load():
    meta_p = os.path.join(IDX, "meta.json")
    if not os.path.exists(meta_p):
        return {"files": {}}, [], np.zeros((0, 1024), np.float32)
    meta = json.load(open(meta_p, encoding="utf-8"))
    chunks = json.load(open(os.path.join(IDX, "chunks.json"), encoding="utf-8"))
    vec = np.load(os.path.join(IDX, "vec.npy"))
    return meta, chunks, vec


def build():
    os.makedirs(IDX, exist_ok=True)
    meta, chunks, vec = load()
    srcs = sources()
    rel = lambda p: os.path.relpath(p, ROOT).replace("\\", "/")
    want = {rel(p): fhash(p) for p in srcs}
    keep = [i for i, c in enumerate(chunks) if want.get(c["f"]) == meta["files"].get(c["f"])]
    chunks = [chunks[i] for i in keep]
    vec = vec[keep] if len(keep) else np.zeros((0, 1024), np.float32)
    have = {c["f"] for c in chunks}
    todo = [p for p in srcs if rel(p) not in have or meta["files"].get(rel(p)) != want[rel(p)]]
    t0 = time.time()
    for n, p in enumerate(todo, 1):
        new = []
        try:
            for pg, txt in pages(p):
                new += [{"f": rel(p), "p": pg, "t": c} for c in split(txt)]
        except Exception as e:
            print(f"  ! {rel(p)}: {e}"); continue
        if new:
            v = embed([c["t"] for c in new])
            chunks += new
            vec = np.vstack([vec, v]) if len(vec) else v
        meta["files"][rel(p)] = want[rel(p)]
        print(f"[{n}/{len(todo)}] {rel(p)}: {len(new)} кусков, {time.time() - t0:.0f} с", flush=True)
        # сохраняем после каждого файла — прерывание не теряет работу
        json.dump(meta, open(os.path.join(IDX, "meta.json"), "w", encoding="utf-8"), ensure_ascii=False)
        json.dump(chunks, open(os.path.join(IDX, "chunks.json"), "w", encoding="utf-8"), ensure_ascii=False)
        np.save(os.path.join(IDX, "vec.npy"), vec)
    meta["files"] = {k: v for k, v in meta["files"].items() if k in want}
    json.dump(meta, open(os.path.join(IDX, "meta.json"), "w", encoding="utf-8"), ensure_ascii=False)
    print(f"готово: {len(chunks)} кусков из {len(want)} файлов")


def query(q, k, only):
    meta, chunks, vec = load()
    if not chunks:
        sys.exit("индекса нет: python tools/llm/refsearch.py build")
    s = vec @ embed([q])[0]
    if only:
        mask = np.array([only.lower() in c["f"].lower() for c in chunks])
        s = np.where(mask, s, -1)
    for i in np.argsort(-s)[:k]:
        c = chunks[i]
        where = f"стр. {c['p']}" if c["f"].lower().endswith(".pdf") else f"строка {c['p']}"
        snip = re.sub(r"\s+", " ", c["t"])[:500]
        print(f"\n== {s[i]:.3f}  {c['f']}  {where}\n{snip}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest="cmd", required=True)
    sp.add_parser("build")
    qp = sp.add_parser("q")
    qp.add_argument("text")
    qp.add_argument("-k", type=int, default=6)
    qp.add_argument("--only", default="", help="подстрока имени файла")
    a = ap.parse_args()
    build() if a.cmd == "build" else query(a.text, a.k, a.only)
