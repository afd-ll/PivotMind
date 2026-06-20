#!/bin/bash
echo "=== 退出事件 ==="
journalctl -u pivotmind --no-pager -o short-iso 2>&1 | grep -E "deactivated|Main PID exited|Consumed" | tail -40
echo "=== 崩溃前最后日志 (倒数第2次启动) ==="
journalctl -u pivotmind --no-pager -o short-iso 2>&1 | grep -B2 "Main PID exited" | tail -20
echo "=== 最近一次退出详情 ==="
journalctl -u pivotmind --no-pager 2>&1 | grep "SIG\|signal\|killed\|TERM\|KILL\|status" | tail -10
