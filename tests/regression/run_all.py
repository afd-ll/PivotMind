#!/usr/bin/env python3
"""run_all.py — PivotMind regression test suite runner.

Usage:
    python3 run_all.py                  # run all tests
    python3 run_all.py --smoke          # only smoke tests
    python3 run_all.py --port 18080     # use specific port
    python3 run_all.py --binary ./custom_gateway
"""

import sys
import os
import argparse
import time

sys.path.insert(0, os.path.dirname(__file__))

from helpers import PROJECT_ROOT, summary, find_free_port


def main():
    parser = argparse.ArgumentParser(description="PivotMind Regression Tests")
    parser.add_argument("--smoke", action="store_true", help="Run only smoke tests")
    parser.add_argument("--response", action="store_true", help="Run only response quality tests")
    parser.add_argument("--filter", action="store_true", help="Run only function word filter tests")
    parser.add_argument("--stress", action="store_true", help="Run only stress tests")
    parser.add_argument("--port", type=int, default=0, help="Gateway port (0=auto)")
    parser.add_argument("--binary", default=None, help="Gateway binary path")
    parser.add_argument("--stress-count", type=int, default=20, help="Stress test request count")
    args = parser.parse_args()

    port = args.port if args.port > 0 else None
    run_all = not (args.smoke or args.response or args.filter or args.stress)

    print("=" * 50)
    print("PivotMind Regression Test Suite")
    print(f"Binary: {args.binary or 'auto-detect'}")
    print(f"Port:   {port or 'auto'}")
    print(f"Project: {PROJECT_ROOT}")
    print("=" * 50)

    start_time = time.time()

    if run_all or args.smoke:
        from test_smoke import run as run_smoke
        run_smoke(port)

    if run_all or args.response:
        from test_response import run as run_response
        run_response(port)

    if run_all or args.filter:
        from test_filter import run as run_filter
        run_filter(port)

    if run_all or args.stress:
        from test_stress import run as run_stress
        run_stress(port, args.stress_count)

    elapsed = time.time() - start_time
    print(f"\nElapsed: {elapsed:.1f}s")
    success = summary()
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
