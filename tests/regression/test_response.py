"""test_response.py — Response quality checks."""

from helpers import (
    start_gateway, stop_gateway, chat, count_fw,
    test, set_module, find_free_port
)
import json
import os


QUESTIONS_FILE = os.path.join(os.path.dirname(__file__), "questions.json")


def load_questions():
    if not os.path.exists(QUESTIONS_FILE):
        return [
            {"q": "你好", "lang": "zh"},
            {"q": "什么是意识", "lang": "zh"},
            {"q": "hello", "lang": "en"},
            {"q": "What is learning", "lang": "en"},
        ]
    with open(QUESTIONS_FILE) as f:
        return json.load(f)


def run(port=None):
    set_module("Response Quality")
    if port is None:
        port = find_free_port()

    proc, port = start_gateway(timeout=30)
    questions = load_questions()

    for item in questions:
        q = item["q"]
        lang = item.get("lang", "?")
        reply = chat(port, q)
        reply_str = str(reply) if reply else ""

        # Non-empty OR explicit "no response" — both are valid behaviors
        is_empty = not reply_str or reply_str in ("(无回应)", "(no response)", "null", "None")
        is_explicit_empty = reply_str in ("(无回应)", "(no response)")

        # Crash check is already in smoke test. Here we check the response state.
        if is_explicit_empty:
            test(f"reply valid empty [{lang}] '{q[:30]}'", True,
                 "cold-start: correctly returned (无回应)")
        elif is_empty:
            test(f"reply non-empty [{lang}] '{q[:30]}'", False,
                 f"got: '{reply_str[:60]}'")
        else:
            test(f"reply has content [{lang}] '{q[:30]}'", True,
                 reply_str[:60])

            # Reasonable length
            test(f"reply length OK [{lang}]",
                 1 <= len(reply_str) <= 500,
                 f"length={len(reply_str)}")

            # Function word ratio (warn only for English single-word replies)
            if lang == "en" and len(reply_str.split()) > 1:
                en_fw, zh_fw = count_fw(reply_str)
                total_words = max(len(reply_str.split()), 1)
                fw_ratio = (en_fw + zh_fw) / max(total_words, 1)
                if fw_ratio < 0.7:
                    test(f"fw ratio OK [{lang}] '{q[:20]}'", True,
                         f"{en_fw+zh_fw}/{total_words}={fw_ratio:.0%}")
                else:
                    test(f"fw ratio HIGH [{lang}] '{q[:20]}'", False,
                         f"{en_fw+zh_fw}/{total_words}={fw_ratio:.0%}")

    stop_gateway(proc)
