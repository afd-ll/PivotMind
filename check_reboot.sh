#!/bin/bash
echo "=== 当前进程 ==="
ps aux | grep pivotmind_gatew | grep -v grep
echo "=== 状态 ==="
curl -s http://localhost:8080/status
echo ""
echo "=== 日志尾部 ==="
tail -40 ~/pivotmind/gateway.log
echo ""
echo "=== OOM/dmesg ==="
dmesg | grep -i "killed\|oom\|pivot" | tail -10
echo ""
echo "=== 系统日志 ==="
journalctl -u pivotmind --no-pager -n 20 2>/dev/null || echo "无systemd服务"
