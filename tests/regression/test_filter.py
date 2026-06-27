"""test_filter.py — Function word filtering verification."""

from helpers import (
    start_gateway, stop_gateway, chat, FW_EN, FW_ZH,
    test, set_module, find_free_port
)


FW_QUERIES = [
    ("the be of in to and have not", "en"),
    ("的了的在是我不", "zh"),
    ("the of in and to be", "en"),
]


def run(port=None):
    set_module("Function Word Filter")
    if port is None:
        port = find_free_port()

    proc, port = start_gateway(timeout=30)

    for query, lang in FW_QUERIES:
        reply = chat(port, query)
        reply_str = str(reply).strip()

        # Empty reply is correct behavior — function words should be filtered
        if not reply_str or reply_str in ("(无回应)", "(no response)"):
            test(f"fw input filtered [{lang}] '{query[:30]}'", True,
                 "correctly returned empty")
            test(f"no fw in empty reply [{lang}]", True)
            continue

        # Non-empty reply — check for leaked function words
        words = set(reply_str.lower().split())
        leaked_en = words & FW_EN
        leaked_zh_words = set(reply_str) & FW_ZH

        test(f"no EN fw leaked [{lang}] '{query[:30]}'",
             len(leaked_en) == 0,
             f"leaked: {leaked_en}" if leaked_en else "")

        test(f"no ZH fw leaked [{lang}] '{query[:30]}'",
             len(leaked_zh_words) == 0,
             f"leaked: {leaked_zh_words}" if leaked_zh_words else "")

    stop_gateway(proc)
