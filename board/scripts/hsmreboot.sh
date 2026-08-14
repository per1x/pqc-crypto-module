#!/bin/sh
# hsmreboot.sh —— 这块板子上唯一有效的重启方式
#
# `reboot` 在这块板子上**不生效**（进程收到信号但机器不动）。可用的是内核的
# sysrq 通道，而且必须按 s→u→b 的顺序：先把脏页写盘、再把根挂成只读、
# 最后才复位。少了前两步就是一次硬断电，SD 卡上的日志会丢 ——
# 而排查启动问题时，恰恰只有那些日志能说明上一次发生了什么。
#
# 串口发 BREAK 触发 sysrq **无效**，试过。
#
# 脱离终端跑：复位发生时这条 SSH 会断，如果命令还挂在会话上，
# 有可能在 b 之前就被 SIGHUP 掐掉，于是只 sync 了没重启 —— 看起来像"没反应"。
set -u
echo "板子将在 3 秒后重启（sync → 只读 → 复位）"
setsid nohup sh -c '
    sleep 3
    echo 1 > /proc/sys/kernel/sysrq
    sync
    echo s > /proc/sysrq-trigger      # 写盘
    sleep 1
    echo u > /proc/sysrq-trigger      # 根挂只读
    sleep 1
    echo b > /proc/sysrq-trigger      # 复位
' >/dev/null 2>&1 </dev/null &
echo "已触发。约 35 秒后 ssh root@192.168.50.175 应当可用。"
