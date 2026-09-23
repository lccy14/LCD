"""
电脑监控 · PC 端（读取传感器数据并通过 TCP 推送给 ESP32）

【数据源：自动二选一，无需手动切换】
  1. 若本机运行了 AIDA64 且开启了「共享内存」→ 用 AIDA64（温度/风扇/电压最全）；
  2. 否则自动回退到「直读模式」：用 psutil 读 CPU 占用、每核、内存、磁盘、网络，
     若装了 LibreHardwareMonitor / OpenHardwareMonitor 并开启 WMI，还能读到温度/风扇。
  → 换句话说：不装 AIDA64 也能跑，只是少了硬件级温度/风扇。

一个文件搞定两件事：
  1. 读取上述传感器（标签 / 数值 / 单位）；
  2. 起一个本地 TCP 服务，把数据按 "label|value|unit" 每行发给 ESP32 的「电脑监控」App。

前置（直读模式，推荐）：
  pip install psutil wmi        # wmi 仅在想读温度/风扇时需要

前置（AIDA64 模式，可选）：
  选项 → 偏好设置 → 硬件监视工具 → 外部应用程序 → 勾选「启用共享内存」
（AIDA64 共享内存不是二进制结构体，而是以 0x00 结尾的 XML 片段字符串，
 每个传感器是一个 <sys>...</sys> 块，内部通常为 <id>/<label>/<value>/<unit> 标签。）

用法（在 Windows 上）：
  cd C:\esp32\LCD\tools
  python pc_mon_server.py            # 启动 TCP 服务（默认 0.0.0.0:8123），推数据给 ESP32
  python pc_mon_server.py --dump    # 仅打印 AIDA64 原始 XML（排查标签格式用）
  python pc_mon_server.py --list    # 自动选数据源，打印传感器到控制台（不启服务）
  python pc_mon_server.py --direct  # 强制直读模式并打印（不启服务）

ESP32 端 (main/pc_mon.c) 每秒连一次本服务读取数据。启动后会打印本机局域网 IP，
请把该 IP 填进 pc_mon.c 的 PC_MON_SERVER_IP 宏后重新编译烧录。
"""

import ctypes
import json
import re
import socket
import sys
import threading
from ctypes import wintypes

# ---------------- AIDA64 共享内存读取（仅 Windows） ----------------

kernel32 = ctypes.windll.kernel32

FILE_MAP_READ = 0x0004
SHARED_MEMORY_NAME = "AIDA64_SensorValues"

# 设置 ctypes 原型，避免 64 位下参数类型推断错误
kernel32.OpenFileMappingW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
kernel32.OpenFileMappingW.restype = wintypes.HANDLE
kernel32.MapViewOfFile.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.c_size_t]
kernel32.MapViewOfFile.restype = ctypes.c_void_p
kernel32.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
kernel32.UnmapViewOfFile.restype = wintypes.BOOL
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.CloseHandle.restype = wintypes.BOOL


def _open_shared_memory():
    """返回 (句柄, 映射指针)；失败时抛错。"""
    h_map = kernel32.OpenFileMappingW(FILE_MAP_READ, False, SHARED_MEMORY_NAME)
    if not h_map:
        raise RuntimeError(
            "无法打开共享内存 '%s'。\n"
            "请确认：1) 已安装并运行 AIDA64；"
            "2) 已开启「启用共享内存」；3) 当前为 Windows 系统。"
            % SHARED_MEMORY_NAME
        )
    try:
        p_buf = kernel32.MapViewOfFile(h_map, FILE_MAP_READ, 0, 0, 0)
        if not p_buf:
            kernel32.CloseHandle(h_map)
            raise RuntimeError("MapViewOfFile 失败。")
        return h_map, p_buf
    except Exception:
        kernel32.CloseHandle(h_map)
        raise


def read_raw_xml():
    """打开共享内存，返回以 0x00 结尾的 XML 字符串（bytes）。"""
    h_map, p_buf = _open_shared_memory()
    try:
        # c_char_p 会读到第一个 \x00 为止，正好对应 AIDA64 的空结尾字符串
        cstr = ctypes.cast(p_buf, ctypes.c_char_p).value
        if cstr is None:
            cstr = b""
        return cstr
    finally:
        kernel32.UnmapViewOfFile(p_buf)
        kernel32.CloseHandle(h_map)


def parse_sensors(xml_bytes):
    """解析 AIDA64 XML 片段，返回 [(label, value, unit), ...]。"""
    text = xml_bytes.decode("utf-8", "ignore")
    blocks = re.findall(r"<sys>(.*?)</sys>", text, re.DOTALL | re.IGNORECASE)
    sensors = []

    def grab(block, tag):
        m = re.search(r"<%s>(.*?)</%s>" % (tag, tag), block, re.DOTALL | re.IGNORECASE)
        return m.group(1).strip() if m else ""

    for b in blocks:
        label = grab(b, "label") or grab(b, "id")
        value = grab(b, "value")
        unit = grab(b, "unit")
        if label:
            sensors.append((label, value, unit))
    return sensors


def collect_direct():
    """不依赖 AIDA64，直接用 psutil / wmi 读系统传感器。
    返回 [(label, value, unit), ...]；缺依赖时返回带提示的行。"""
    try:
        import psutil
    except ImportError:
        return [("提示", "未安装 psutil，请运行: pip install psutil wmi", "")]

    sensors = []
    # CPU 总体占用
    cpu = psutil.cpu_percent(interval=0.3)
    sensors.append(("CPU 占用", "%.0f" % cpu, "%"))
    # 每核心占用
    cores = psutil.cpu_percent(interval=0.1, percpu=True)
    for i, c in enumerate(cores):
        sensors.append(("CPU 核心%d" % (i + 1), "%.0f" % c, "%"))
    # 内存
    vm = psutil.virtual_memory()
    sensors.append(("内存占用", "%.0f" % vm.percent, "%"))
    sensors.append(("已用内存", "%.1f" % (vm.used / 1073741824.0), "GB"))
    sensors.append(("可用内存", "%.1f" % (vm.available / 1073741824.0), "GB"))
    # 磁盘 C 盘
    try:
        du = psutil.disk_usage("C:\\")
        sensors.append(("C盘占用", "%.0f" % du.percent, "%"))
    except Exception:
        pass
    # 温度 / 风扇 / 负载（需 Libre/OpenHardwareMonitor 并开启 WMI）
    sensors.extend(_wmi_sensors())
    return sensors


def _lhm_web_sensors():
    """通过 LibreHardwareMonitor 的 Remote Web Server 读传感器。
    默认端点 http://127.0.0.1:8085/data.json，无需管理员。
    没运行 LHM 或未开 Web Server 时返回空列表，不影响其它数据源。"""
    try:
        from urllib.request import urlopen
    except ImportError:
        return []

    TYPE_MAP = {
        "Temperature": ("温度", "°C"),
        "Voltage":     ("电压", "V"),
        "Fan":         ("风扇", "RPM"),
        "Load":        ("占用", "%"),
        "Power":       ("功耗", "W"),
        "Clock":       ("频率", "MHz"),
        "Current":     ("电流", "A"),
    }
    HW_CN = {
        "CPU VCore": "CPU 核心", "CPU Core": "CPU 核心", "CPU": "CPU",
        "Motherboard": "主板", "System": "系统", "Chipset": "芯片组",
        "PCH": "南桥", "DRAM": "内存", "Memory": "内存", "GPU Core": "显卡核心",
        "GPU": "显卡", "VCCSA": "VCCSA", "VCCIO": "VCCIO",
        "CPU OPT": "CPU 辅助", "CPU Fan": "CPU", "Chassis": "机箱", "Aux": "辅助",
    }
    FMT = {"Temperature": "%.1f", "Voltage": "%.3f", "Fan": "%.0f",
           "Load": "%.0f", "Power": "%.1f", "Clock": "%.0f", "Current": "%.3f"}

    try:
        with urlopen("http://127.0.0.1:8085/data.json", timeout=1.5) as resp:
            tree = json.loads(resp.read().decode("utf-8"))
    except Exception:
        return []

    sensors = []

    def walk(node):
        if isinstance(node, list):
            for n in node:
                walk(n)
            return
        if not isinstance(node, dict):
            return
        st = node.get("Type") or node.get("SensorType")
        text = node.get("Text") or node.get("text")
        value = node.get("Value") or node.get("value")
        if st and text and value and st in TYPE_MAP:
            if st == "Load" and text.lower().startswith("cpu"):
                return  # CPU 占用已由 psutil 提供，避免重复
            # 取数值：优先 RawValue，其次从 Value 字符串里解析
            raw = node.get("RawValue")
            try:
                num = float(raw)
            except (TypeError, ValueError):
                m = re.search(r"[-+]?\d*\.?\d+", str(value))
                num = float(m.group()) if m else None
            if num is None:
                return
            hw = text
            for k in sorted(HW_CN, key=len, reverse=True):
                if text.startswith(k):
                    hw = HW_CN[k]
                    break
            suffix, unit = TYPE_MAP[st]
            sensors.append((hw + suffix, FMT[st] % num, unit))
        # 递归子节点
        children = node.get("Children") or node.get("children") or []
        walk(children)

    walk(tree)

    # 过滤噪声传感器（D3D/视频/网络活动/读写活动等）、去重，并截到 40 条，
    # 让 ESP32 的 buf(2048)/MAX_SENSORS(40) 不会溢出，优先保留温度/电压/风扇。
    NOISE = ("D3D", "Video Decode", "Video Processing", "Overlay", "Copy",
             "Network Utilization", "Read Activity", "Write Activity", "Total Activity")
    seen = set()
    out = []
    for label, val, unit in sensors:
        if any(n in label for n in NOISE):
            continue
        if label in seen:
            continue
        seen.add(label)
        out.append((label, val, unit))
        if len(out) >= 40:
            break
    return out


def _wmi_sensors():
    """通过 WMI 读 Libre/OpenHardwareMonitor 暴露的硬件传感器（温度/电压/风扇/功耗/频率）。
    没装对应软件时返回空列表，不影响 psutil 的其它指标。
    传感器名（英文）会顺手翻成中文，方便 ESP32 直接显示。"""
    try:
        import wmi
    except ImportError:
        return []

    # 硬件名（OHM/LHM 的 Name 前缀）→ 中文
    HW_CN = {
        "CPU VCore": "CPU 核心", "CPU Core": "CPU 核心", "CPU": "CPU",
        "Motherboard": "主板", "System": "系统", "Chipset": "芯片组",
        "PCH": "南桥", "DRAM": "内存", "Memory": "内存", "GPU Core": "显卡核心",
        "GPU": "显卡", "VCCSA": "VCCSA", "VCCIO": "VCCIO",
        "CPU OPT": "CPU 辅助", "CPU Fan": "CPU", "Chassis": "机箱", "Aux": "辅助",
    }
    TYPE_CN = {"Temperature": "温度", "Voltage": "电压", "Fan": "风扇",
               "Load": "占用", "Power": "功耗", "Clock": "频率"}
    UNIT = {"Temperature": "°C", "Voltage": "V", "Fan": "RPM",
            "Load": "%", "Power": "W", "Clock": "MHz"}
    FMT = {"Temperature": "%.1f", "Voltage": "%.3f", "Fan": "%.0f",
           "Load": "%.0f", "Power": "%.1f", "Clock": "%.0f"}

    out = []
    for ns in ("root\\LibreHardwareMonitor", "root\\OpenHardwareMonitor"):
        try:
            c = wmi.WMI(namespace=ns)
        except Exception:
            continue
        try:
            for s in c.Sensor():
                st = getattr(s, "SensorType", "")
                if st not in TYPE_CN:
                    continue
                if st == "Load" and (s.Name or "").lower().startswith("cpu"):
                    continue  # CPU 占用已由 psutil 提供，避免重复
                name = s.Name or ""
                hw = name
                for k in sorted(HW_CN, key=len, reverse=True):
                    if name.startswith(k):
                        hw = HW_CN[k]
                        break
                label = hw + TYPE_CN[st]
                try:
                    val = FMT[st] % float(s.Value)
                except (TypeError, ValueError):
                    val = str(s.Value)
                out.append((label, val, UNIT[st]))
        except Exception:
            pass
        if out:
            return out
    return out


def read_sensors():
    """高层接口：按 AIDA64 → LHM Web Server → WMI → psutil 的优先级取数据。"""
    # 1) AIDA64 共享内存（数据最全）
    try:
        raw = read_raw_xml()
        sensors = parse_sensors(raw)
        if sensors:
            return sensors
        sample = raw[:800].decode("utf-8", "ignore")
        print("【AIDA64 解析为空，回退到直读模式】原始 XML 前 800 字符：")
        print("-" * 60)
        print(sample)
        print("-" * 60)
    except Exception as e:
        print("【未检测到 AIDA64 共享内存，改用直读模式】%s" % e)

    # 2) LibreHardwareMonitor 的 Remote Web Server（无需管理员，数据全）
    web = _lhm_web_sensors()
    if web:
        return web

    # 3) Libre/OpenHardwareMonitor 的 WMI 接口（需要管理员及 GUI 支持）
    wmi = _wmi_sensors()
    if wmi:
        return wmi

    # 4) psutil 兜底（CPU/内存/磁盘）
    return collect_direct()


def print_sensors():
    sensors = read_sensors()
    if not sensors:
        return
    print("传感器数据（共 %d 条）\n" % len(sensors))
    for label, value, unit in sensors:
        text = value + ((" " + unit) if unit else "")
        print("%-32s : %s" % (label, text))


# ---------------- TCP 服务（推送给 ESP32） ----------------

HOST = "0.0.0.0"
PORT = 8123


def get_lan_ip():
    """用一个 UDP 连接到公网，拿到本机局域网 IP（不会真的发包）。"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()


def handle_client(conn):
    """每个 ESP32 连接：发一份当前传感器快照，然后关闭连接。"""
    try:
        sensors = read_sensors()
        lines = ["%s|%s|%s" % (label, value, unit) for label, value, unit in sensors]
        data = "\n".join(lines) + "\n"
        conn.sendall(data.encode("utf-8"))
    except Exception as e:
        # 读不到 AIDA64 也回一行错误，方便 ESP32 知道状态
        try:
            conn.sendall(("ERR|%s|\n" % e).encode("utf-8"))
        except Exception:
            pass
    finally:
        conn.close()


def run_server():
    ip = get_lan_ip()
    print("=" * 56)
    print("本机局域网 IP：%s" % ip)
    print("请把 main/pc_mon.c 里的 PC_MON_SERVER_IP 改成上面这个 IP")
    print("端口：%d" % PORT)
    print("=" * 56)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, PORT))
    srv.listen(5)
    print("AIDA64 监控服务端已启动，等待 ESP32 连接... (Ctrl+C 退出)")

    try:
        while True:
            conn, addr = srv.accept()
            print("客户端连接：%s" % (addr,))
            t = threading.Thread(target=handle_client, args=(conn,), daemon=True)
            t.start()
    except KeyboardInterrupt:
        print("\n已退出。")
    finally:
        srv.close()


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--dump":
        print(read_raw_xml().decode("utf-8", "ignore"))
    elif len(sys.argv) > 1 and sys.argv[1] == "--direct":
        for label, value, unit in collect_direct():
            text = value + ((" " + unit) if unit else "")
            print("%-20s : %s" % (label, text))
    elif len(sys.argv) > 1 and sys.argv[1] == "--list":
        print_sensors()
    else:
        run_server()


if __name__ == "__main__":
    main()
