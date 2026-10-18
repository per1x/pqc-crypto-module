#!/usr/bin/env python3
"""find_board.py —— 在局域网里找密码机（板子走 DHCP，地址随网络而变）

    python3 tools/find_board.py            # 扫本机各网卡所在的 /24，再试链路本地地址
    python3 tools/find_board.py 10.0.3.0   # 指定要扫的 /24

判据：22 口的 SSH 横幅是 dropbear，并且 9797（mTLS 远程口）开着。
只用标准库，Windows / macOS / Linux 都能跑。
"""
import ipaddress
import socket
import sys
from concurrent.futures import ThreadPoolExecutor

LINK_LOCAL = "169.254.50.175"   # 网线直插电脑时用（见 board/scripts/hsm-boot.sh）


def local_nets():
    """本机所有私网 IPv4 所在的 /24。默认路由可能走代理的虚拟网卡（198.18.x），
    所以不能只看出口地址，要把各网卡的地址都列出来。"""
    import re
    import subprocess
    cmds = [["ipconfig"]] if sys.platform == "win32" else [["ifconfig"], ["ip", "-4", "addr"]]
    text = ""
    for c in cmds:
        try:
            text += subprocess.run(c, capture_output=True, text=True, errors="ignore").stdout
        except OSError:
            pass
    nets = []
    for a in re.findall(r"(?<![\d.])(\d{1,3}(?:\.\d{1,3}){3})(?![\d.])", text):
        try:
            ip = ipaddress.ip_address(a)
        except ValueError:
            continue
        if not ip.is_private or ip.is_loopback or ip.is_link_local:
            continue
        if ip in ipaddress.ip_network("198.18.0.0/15") or a.endswith((".0", ".255")):
            continue
        n = ipaddress.ip_network(a + "/24", strict=False)
        if n not in nets:
            nets.append(n)
    return nets


def is_board(ip):
    try:
        with socket.create_connection((ip, 22), timeout=0.6) as s:
            s.settimeout(1.5)
            banner = s.recv(64)
    except OSError:
        return False
    if b"dropbear" not in banner:
        return False
    try:
        socket.create_connection((ip, 9797), timeout=1.0).close()
    except OSError:
        return False
    return True


def main():
    if len(sys.argv) > 1:
        nets = [ipaddress.ip_network(sys.argv[1] + "/24", strict=False)]
    else:
        nets = local_nets()
        if not nets:
            sys.exit("找不到本机的私网地址，请手动指定网段：find_board.py 192.168.1.0")
    hosts = [str(h) for n in nets for h in n.hosts()] + [LINK_LOCAL]
    print(f"扫描 {', '.join(map(str, nets))} 和 {LINK_LOCAL} …")
    with ThreadPoolExecutor(max_workers=64) as ex:
        found = [ip for ip, ok in zip(hosts, ex.map(is_board, hosts)) if ok]
    if not found:
        sys.exit("没找到。确认板子上电 35 秒以上、网线插在 eth1，或看串口登录提示里的地址。")
    for ip in found:
        print(f"密码机：{ip}    ssh root@{ip}（口令 root）")


if __name__ == "__main__":
    main()
