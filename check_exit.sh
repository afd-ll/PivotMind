#!/bin/bash
journalctl -u pivotmind --no-pager -o short-iso 2>&1 | grep -E "Deactivated|code=|Main PID|Consumed|心跳启动" | tail -50
