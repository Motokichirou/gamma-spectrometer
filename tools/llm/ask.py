"""Черновая работа на рабочем сервере Qwen3.8-27B (OpenAI-совместимый API, LiteLLM).

Ключ — только из переменной окружения WORK_LLM_KEY. Результат — черновик: Claude его проверяет.
Контекст 65 536 токенов на вход+выход+рассуждения (≈ 150 КБ русского текста на вход максимум).

  python tools/llm/ask.py "задание" [-f файл ...] [--think] [-o out.md] [--max 8000]
  python tools/llm/ask.py -p prompt.md -f a.c -f b.h -o draft.md
"""
import argparse, json, os, sys, time, urllib.request

sys.stdout.reconfigure(encoding="utf-8")
URL = os.environ.get("WORK_LLM_URL", "http://10.10.8.5:8720/v1") + "/chat/completions"
SYSTEM = ("Ты помощник инженера-электронщика. Отвечай по-русски, кратко и точно. "
          "Опирайся только на переданные файлы и задание; если данных нет — так и скажи, не выдумывай. "
          "Код и числа не сочиняй сверх нужного.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("task", nargs="?", default="")
    ap.add_argument("-p", "--prompt-file")
    ap.add_argument("-f", "--file", action="append", default=[])
    ap.add_argument("--think", action="store_true", help="модель med-think (с рассуждениями)")
    ap.add_argument("--max", type=int, default=8000, help="max_tokens ответа")
    ap.add_argument("-t", "--temp", type=float, default=0.2)
    ap.add_argument("-o", "--out")
    a = ap.parse_args()
    key = os.environ.get("WORK_LLM_KEY") or sys.exit("нет WORK_LLM_KEY")
    task = a.task
    if a.prompt_file:
        task = open(a.prompt_file, encoding="utf-8").read() + ("\n\n" + task if task else "")
    parts = [task]
    for f in a.file:
        parts.append(f"\n\n===== ФАЙЛ {f} =====\n" + open(f, encoding="utf-8", errors="replace").read())
    user = "".join(parts)
    body = {"model": "med-think" if a.think else "med-default", "temperature": a.temp,
            "max_tokens": a.max,
            "messages": [{"role": "system", "content": SYSTEM}, {"role": "user", "content": user}]}
    req = urllib.request.Request(URL, json.dumps(body, ensure_ascii=False).encode("utf-8"),
                                 {"Content-Type": "application/json; charset=utf-8",
                                  "Authorization": f"Bearer {key}"})
    t0 = time.time()
    with urllib.request.urlopen(req, timeout=1800) as r:
        resp = json.load(r)
    msg = resp["choices"][0]["message"]["content"] or ""
    u = resp.get("usage", {})
    info = (f"[{body['model']}: вход {u.get('prompt_tokens')}, выход {u.get('completion_tokens')}, "
            f"{time.time() - t0:.0f} с, finish={resp['choices'][0].get('finish_reason')}]")
    if a.out:
        open(a.out, "w", encoding="utf-8").write(msg)
        print(info, "->", a.out)
    else:
        print(msg + "\n\n" + info)


if __name__ == "__main__":
    main()
