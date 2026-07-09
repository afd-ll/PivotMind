#!/bin/bash
echo "=== systemd服务 ==="
systemctl list-units --all 2>/dev/null | grep pivot || echo "无systemd服务"
ls /etc/systemd/system/*pivot* 2>/dev/null || echo "无service文件"

echo "=== 进程树 ==="
ps -ef | grep pivotmind | grep -v grep
ps -eo pid,lstart,etime,cmd | grep pivotmind_gatew | grep -v grep

echo "=== 历史重启痕迹 ==="
last -x 2>/dev/null | head -5
echo "---"
uptime

echo "=== ssh登录历史 ==="
last -n 5 2>/dev/null

echo "=== 定时任务 ==="
crontab -l 2>/dev/null || echo "无crontab"
ls /etc/cron.*/*pivot* 2>/dev/null || echo "无cron任务"

echo "=== 进程启动时间戳 ==="
stat /proc/$(pgrep -n pivotmind_gatew)/exe 2>/dev/null
cat /proc/$(pgrep -n pivotmind_gatew)/stat 2>/dev/null | awk '{print "started at boot jiffies:", $22}'
