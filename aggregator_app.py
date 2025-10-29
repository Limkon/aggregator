# -*- coding: utf-8 -*-

# --- Windows 7 兼容性说明 ---
# 1. Python 版本: 此脚本建议使用 Python 3.8.x 版本运行
# 2. 安装依赖库: pip install requests pysocks certifi futures
# -----------------------------------
# (修改) Sing-box 真人测速说明
# 1. 核心文件: 要使用 "Sing-box 真人测速"，必须下载 Sing-box 核心。
#    - Windows: 下载 suitable zip, 解压，将 `sing-box.exe` 放在本脚本同目录。
#    - macOS/Linux: 将 `sing-box` 可执行文件放在本脚本同目录。
# 2. 测试目标: 默认 'http://www.msftconnecttest.com/connecttest.txt'
# 3. !!! 重要: 必须将 sing-box.exe 和本程序添加到杀毒软件/防火墙白名单 !!!
# -----------------------------------

import tkinter as tk
from tkinter import scrolledtext, messagebox, ttk, filedialog
import base64
import time
import threading
import requests
import sys
import certifi
import json
import re
import traceback
import urllib.parse
from concurrent.futures import ThreadPoolExecutor, as_completed, wait, FIRST_COMPLETED
import queue
import socket
import subprocess
import os
import platform

# 为了支持SOCKS代理，需要安装 PySocks
try:
    import socks
except ImportError:
    class FakeSocks:
        def set_default_proxy(self, *args, **kwargs):
            pass
    sys.modules['socks'] = FakeSocks()


# --- 通用右键菜单功能 ---
class TextWidgetContextMenu:
    def __init__(self, widget):
        self.widget = widget
        self.menu = tk.Menu(widget, tearoff=0)
        self.menu.add_command(label="剪切", command=self.cut)
        self.menu.add_command(label="复制", command=self.copy)
        self.menu.add_command(label="粘贴", command=self.paste)
        self.menu.add_separator()
        self.menu.add_command(label="全选", command=self.select_all)
        widget.bind("<Button-3>", self.show_menu)

    def show_menu(self, event):
        try:
            # 简化状态检查
            has_selection = False
            try:
                # 检查是否有选中文本，对 Entry 和 Text 都有效
                if self.widget.selection_get():
                    has_selection = True
            except tk.TclError:
                pass # 没有选择

            clipboard_content = ""
            try:
                clipboard_content = self.widget.clipboard_get()
            except tk.TclError:
                 pass # 没有剪贴板内容或错误

            self.menu.entryconfig("剪切", state=tk.NORMAL if has_selection else tk.DISABLED)
            self.menu.entryconfig("复制", state=tk.NORMAL if has_selection else tk.DISABLED)
            self.menu.entryconfig("粘贴", state=tk.NORMAL if clipboard_content else tk.DISABLED)

        except Exception: # 通用捕获
             self.menu.entryconfig("剪切", state=tk.DISABLED)
             self.menu.entryconfig("复制", state=tk.DISABLED)
             self.menu.entryconfig("粘贴", state=tk.DISABLED)

        self.menu.tk_popup(event.x_root, event.y_root)


    def cut(self): self.widget.event_generate("<<Cut>>")
    def copy(self): self.widget.event_generate("<<Copy>>")
    def paste(self): self.widget.event_generate("<<Paste>>")
    def select_all(self):
        # 处理不同控件类型的全选
        if hasattr(self.widget, 'select_range'): # Entry 控件
             self.widget.select_range(0, tk.END)
        elif hasattr(self.widget, 'tag_add'): # Text/ScrolledText 控件
             # 需要检查控件是否为空，否则 tag_add 会报错
             try:
                 # 检查控件是否存在并且有内容
                 if self.widget.winfo_exists() and self.widget.get("1.0", tk.END).strip():
                     self.widget.tag_add(tk.SEL, "1.0", tk.END)
             except tk.TclError:
                 pass # 忽略错误 (例如控件已被销毁)


# (修复 PlaceholderEntry 递归错误的完整类 v6.1.1)
class PlaceholderEntry(ttk.Entry):
    def __init__(self, container, placeholder, *args, **kwargs):
        super().__init__(container, *args, **kwargs)
        self.placeholder = placeholder
        self.placeholder_color = 'grey'
        style_name = getattr(self, "style", f"{self}.TEntry") # 获取样式名，提供备用
        style = ttk.Style()
        # 获取当前 Entry 的默认前景色
        # 需要处理可能不存在 'foreground' 选项的情况
        try:
            # 尝试获取特定样式的前景色
            self.default_fg_color = style.lookup(self.winfo_class(), 'foreground')
        except tk.TclError:
             # 如果失败，尝试获取 TEntry 的前景色
             try:
                  self.default_fg_color = style.lookup("TEntry", 'foreground')
             except tk.TclError:
                  self.default_fg_color = 'black' # Fallback color


        self.is_placeholder = False # 初始化为 False

        self.bind("<FocusIn>", self._focus_in)
        self.bind("<FocusOut>", self._focus_out)

        self._put_placeholder() # 初始设置占位符

    def _put_placeholder(self):
        # 仅当输入框为空且未获得焦点时放置占位符
        # 使用winfo_exists()检查控件是否存在
        if self.winfo_exists() and not super().get() and self.focus_get() != self:
            # 先调用父类的 delete 和 insert
            super().delete(0, tk.END)
            super().insert(0, self.placeholder)
            self.configure(foreground=self.placeholder_color) # 使用 configure 修改颜色
            self.is_placeholder = True
            # 将光标移到开头，避免显示在占位符中间
            self.icursor(0)

    def _clear_placeholder(self):
        # 清除占位符文本并恢复默认颜色
        # 使用winfo_exists()检查控件是否存在
        if self.winfo_exists() and self.is_placeholder:
             super().delete('0', 'end')
             self.configure(foreground=self.default_fg_color) # 使用 configure 修改颜色
             self.is_placeholder = False

    def _focus_in(self, *args):
        if self.is_placeholder:
            self._clear_placeholder()

    def _focus_out(self, *args):
        # 失去焦点时，如果为空则放回占位符
        self._put_placeholder()

    # 重写 get 方法，如果显示的是占位符则返回空字符串
    def get(self):
        # 使用winfo_exists()检查控件是否存在
        if not self.winfo_exists():
            return ""
        content = super().get()
        if self.is_placeholder:
            return ""
        return content

    # 保留原始方法名以便兼容
    def get_real_text(self):
        return self.get()

    # 重写 delete - 先清除占位符（如果需要），然后调用父类 delete
    def delete(self, first, last=None):
        if self.winfo_exists():
            if self.is_placeholder:
                 # 仅当控件获得焦点时才清除占位符
                 if self.focus_get() == self:
                     self._clear_placeholder()
                 else:
                     return # 如果未获得焦点时不执行删除操作
            # 在可能清除占位符后调用父类 delete
            super().delete(first, last)
            # 删除后，如果失去焦点，_focus_out 会处理占位符的回填

    # 重写 insert - 先清除占位符（如果需要），然后调用父类 insert
    def insert(self, index, string):
        if self.winfo_exists():
            if self.is_placeholder:
                self._clear_placeholder()
            super().insert(index, string)


# --- 从 nexavor/aggregator 项目移植的辅助函数 ---
class NexavorUtils:
    USER_AGENT = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/123.0.0.0 Safari/537.36"
    DEFAULT_HTTP_HEADERS = {"User-Agent": USER_AGENT, "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.9"}
    # (修改) 扩展协议列表
    NODE_PROTOCOLS = ("vmess://", "ss://", "ssr://", "trojan://", "vless://", "tuic://", "hysteria://", "hysteria2://", "hy2://")
    PROTOCOL_REGEX = r"(?:vmess|trojan|ss|ssr|vless|tuic|hysteria2?|hy2)://[a-zA-Z0-9:.?+=@%&#_\-/]{10,}" # (修改) 更新正则表达式
    
    @staticmethod
    def http_get(url, headers=None, params=None, retry=3, proxy=None, timeout=15):
        if not (url.startswith('http://') or url.startswith('https://')): return ""
        if retry <= 0: return ""
        headers = headers if headers else NexavorUtils.DEFAULT_HTTP_HEADERS
        proxies = {'http': proxy, 'https': proxy} if proxy else None
        try:
            # 添加 stream=True 和显式 close 以更好地管理资源
            with requests.get(url, headers=headers, params=params, proxies=proxies,
                              timeout=timeout, verify=certifi.where(), stream=True) as response:
                response.raise_for_status()
                # 显式读取内容
                content = response.text
                return content
        except Exception:
            time.sleep(1)
            return NexavorUtils.http_get(url, headers, params, retry - 1, proxy, timeout)

# --- 整合后的并发搜索核心 ---
class DynamicSubscriptionFinder:
    def __init__(self, gui_queue, proxy_address=None, stop_event=None, lock=None):
        self.gui_queue = gui_queue
        self.proxy_address = proxy_address
        self.utils = NexavorUtils()
        self.stop_event = stop_event or threading.Event()
        self.lock = lock or threading.Lock()

    def _log(self, message):
        self.gui_queue.put(('log', message))

    def search_github_for_keyword(self, token, search_query, pages=2):
        if self.stop_event.is_set(): return []
        self._log(f"[并发搜索] 开始处理关键字: '{search_query}'...")
        links = set()
        headers = {"Accept": "application/vnd.github+json", "X-GitHub-Api-Version": "2022-11-28", "User-Agent": self.utils.USER_AGENT}
        if token: headers["Authorization"] = f"Bearer {token}"
        
        for page in range(1, pages + 1):
            if self.stop_event.is_set(): self._log(f"[并发搜索] 关键字 '{search_query}' 的任务已中止。"); break
            
            params = {'q': search_query, 'sort': 'indexed', 'order': 'desc', 'per_page': 100, 'page': page}
            api_url = "https://api.github.com/search/code"
            try:
                content = self.utils.http_get(api_url, params=params, headers=headers, proxy=self.proxy_address)
                if not content: continue
                # 添加JSON解析的健壮性
                try:
                     data = json.loads(content)
                except json.JSONDecodeError:
                     self._log(f"[并发搜索] 解析 GitHub API 响应失败 (非 JSON): {content[:100]}...")
                     continue # 跳过无效响应
                items = data.get("items", [])
                if not items: break
                
                for item in items:
                    if self.stop_event.is_set(): break
                    html_url = item.get("html_url")
                    if html_url: links.add(html_url.replace("github.com", "raw.githubusercontent.com").replace("/blob/", "/"))
                time.sleep(2) # 保留速率限制等待
            except Exception as e:
                self._log(f"[并发搜索] 处理关键字 '{search_query}' 第 {page} 页时出错: {e}"); break
        
        self._log(f"[并发搜索] 关键字 '{search_query}' 完成，找到 {len(links)} 个潜在链接。")
        return list(links)

    def fetch_and_extract_from_url(self, file_url):
        if self.stop_event.is_set(): return []
        
        content = self.utils.http_get(file_url, proxy=self.proxy_address)
        if not content: return []

        subscriptions = self.extract_subscriptions_from_content(content)
        if subscriptions:
            # 仅在记录日志时使用锁以减少争用
            with self.lock:
                self._log(f"  => 从 {file_url} 找到 {len(subscriptions)} 个链接。")
        return subscriptions

    def extract_subscriptions_from_content(self, content):
        if not content: return []
        cleaned_links = set()
        # 现有的正则表达式似乎可以
        sub_regex = r"https?://(?:[a-zA-Z0-9\u4e00-\u9fa5\-]+\.)+[a-zA-Z0-9\u4e00-\u9fa5\-]+(?:(?:(?:/index.php)?/api/v1/client/subscribe\?token=[a-zA-Z0-9]{16,32})|(?:/link/[a-zA-Z0-9]+\?(?:sub|mu|clash)=\d)|(?:/(?:s|sub)/[a-zA-Z0-9]{32}))"
        extra_regex = r"https?://(?:[a-zA-Z0-9\u4e00-\u9fa5\-]+\.)+[a-zA-Z0-9\u4e00-\u9fa5\-]+/sub\?(?:\S+)?target=\S+"
        
        cleaned_links.update(re.findall(sub_regex, content, re.I))
        cleaned_links.update(re.findall(extra_regex, content, re.I))
        cleaned_links.update(re.findall(self.utils.PROTOCOL_REGEX, content, re.I))
        return [link.strip() for link in cleaned_links if link.strip()] # 确保没有空字符串


    def find(self, executor, github_token, queries, pages):
        potential_files = set()
        self._log(f"=== 第一阶段: 开始并发搜索 {len(queries)} 个关键字 ===")
        if not github_token:
            self._log("[GitHub搜索] 严重警告：未提供任何有效 GitHub Token，搜索请求将极大概率失败！")
        
        future_to_query = {executor.submit(self.search_github_for_keyword, github_token, f'"{query}" in:file', pages): query for query in queries}
        completed_futures = 0
        total_futures = len(future_to_query)
        for future in as_completed(future_to_query):
            completed_futures += 1
            if self.stop_event.is_set(): break
            try:
                result = future.result()
                if result: potential_files.update(result)
            except Exception as e:
                query_name = future_to_query.get(future, "未知") # 如果可能，获取查询名称
                self._log(f"一个关键字 '{query_name}' 搜索任务失败: {e}")
            # 为搜索阶段添加进度日志
            if completed_futures % 10 == 0 or completed_futures == total_futures:
                 self._log(f"搜索进度: {completed_futures}/{total_futures}")

        
        if self.stop_event.is_set(): return
        
        self._log(f"\n=== 第二阶段: 从 {len(potential_files)} 个文件中并发提取订阅链接... ===")
        if not potential_files: self._log("未找到任何可能的文件。"); return
            
        future_to_url = {executor.submit(self.fetch_and_extract_from_url, url): url for url in potential_files}
        completed_futures = 0
        total_futures = len(future_to_url)
        for future in as_completed(future_to_url):
            completed_futures += 1
            if self.stop_event.is_set(): break
            # 在提取期间更频繁地记录进度
            if completed_futures % 10 == 0 or completed_futures == total_futures:
                self._log(f"提取进度: {completed_futures}/{total_futures}")
            try:
                subscriptions = future.result()
                if subscriptions: self.gui_queue.put(('found_links', subscriptions))
            except Exception as e:
                 url_name = future_to_url.get(future, "未知URL")
                 self._log(f"一个文件提取任务失败 ({url_name[:50]}...): {e}")

# --- (v6.1.10) 后端处理核心 (集成测速) ---
class RealProxyAggregator:
    # (修改) Sing-box 核心可执行文件名
    SINGBOX_EXECUTABLE = "sing-box.exe" if platform.system() == "Windows" else "sing-box"
    # (新增) Sing-box 测速起始端口
    SINGBOX_BASE_PORT = 10900
    
    def __init__(self, gui_queue):
        self.gui_queue = gui_queue
        
        # (修改) 查找 Sing-box 核心 (兼容 .py 和 PyInstaller --onefile/--onedir)
        if getattr(sys, 'frozen', False) and hasattr(sys, '_MEIPASS'):
            # 场景1: 运行在 PyInstaller --onefile 打包环境中
            # sys._MEIPASS 指向解压后的临时文件夹
            base_path = sys._MEIPASS
        elif getattr(sys, 'frozen', False):
            # 场景2: 运行在 PyInstaller --onedir (多文件) 打包环境中
            # sys.executable 是打包后的 exe 文件的路径
            base_path = os.path.dirname(sys.executable)
        else:
            # 场景3: 运行在普通的 .py 脚本环境中 (与程序同一目录)
            # __file__ 是 .py 脚本的路径
            base_path = os.path.dirname(os.path.abspath(__file__))

        self.singbox_path = os.path.join(base_path, self.SINGBOX_EXECUTABLE)
        self.singbox_exists = os.path.isfile(self.singbox_path)
        
        if not self.singbox_exists:
             self._log(f"警告：未在 '{base_path}' 目录找到 {self.SINGBOX_EXECUTABLE}。'Sing-box 真人测速' 功能将不可用。")

    def _log(self, msg): self.gui_queue.put(('log', msg))

    def fetch_and_parse_url(self, url, proxy_address=None):
        self._log(f"正在下载订阅: {url}")
        try:
            raw_content = NexavorUtils.http_get(url, proxy=proxy_address)
            if not raw_content: raise ValueError("下载内容为空")
            
            # --- 内容验证核心逻辑 ---
            # 1. 优先尝试 Base64 解码
            try:
                # 需要更优雅地处理潜在的填充错误
                raw_b64 = raw_content.strip()
                missing_padding = len(raw_b64) % 4
                if missing_padding:
                     raw_b64 += '=' * (4 - missing_padding)
                decoded_content = base64.b64decode(raw_b64).decode('utf-8', errors='ignore') # 忽略解码错误
                self._log(f"  -> {url} 解码为 Base64 成功。")
                nodes = [node.strip() for node in decoded_content.splitlines()
                         if node.strip() and node.strip().startswith(NexavorUtils.NODE_PROTOCOLS)]
                if nodes:
                     self._log(f"  -> 从 {url} 的 Base64 内容中解析出 {len(nodes)} 个节点。")
                     return nodes
            except Exception: # 捕获更广泛的异常，包括 binascii.Error
                pass

            # 2. 如果不是 Base64，扫描原文是否包含节点
            plain_text_nodes = re.findall(NexavorUtils.PROTOCOL_REGEX, raw_content, re.I)
            if plain_text_nodes:
                self._log(f"  -> {url} 为纯文本格式，扫描到 {len(plain_text_nodes)} 个节点。")
                # 再次过滤结果以确保
                return [node.strip() for node in plain_text_nodes
                        if node.strip() and node.strip().startswith(NexavorUtils.NODE_PROTOCOLS)]


            # 3. 如果两者都不是，则判定为无效内容
            self._log(f"  -> 警告：{url} 的内容既不是有效 Base64 订阅，也未直接包含节点信息。将忽略此源。")
            return []
            
        except Exception as e:
            self.gui_queue.put(('log', f"错误：处理 {url} 失败。原因: {e}"))
            return []

    @staticmethod
    def _tcp_ping(address, port, timeout=2):
        """尝试TCP连接并返回延迟(ms)，失败返回 float('inf')"""
        try:
            port_int = int(port)
            if port_int <= 0 or port_int > 65535:
                return float('inf')
        except (ValueError, TypeError):
            return float('inf')

        try:
            addr_info_list = socket.getaddrinfo(address, port_int, 0, socket.SOCK_STREAM)
            if not addr_info_list:
                return float('inf') 

            target_addr_info = addr_info_list[0]
            af, socktype, proto, canonname, sa = target_addr_info
            
            start_time = time.monotonic() # 使用 monotonic 时钟测量时长
            with socket.socket(af, socktype, proto) as sock:
                sock.settimeout(timeout)
                sock.connect(sa)
                end_time = time.monotonic()
                return (end_time - start_time) * 1000  # 返回毫秒
        except (socket.error, socket.timeout, OverflowError, OSError): 
            return float('inf')
        except Exception:
            return float('inf')

    @staticmethod
    def _parse_node_link_for_tcp(link):
        """专用于 TCP Ping 的解析器"""
        try:
            if link.startswith("vmess://"):
                try:
                    # 处理潜在的填充错误
                    b64_part = link[8:]
                    b64_part += '=' * (-len(b64_part) % 4)
                    decoded = base64.b64decode(b64_part).decode('utf-8', errors='ignore')
                    data = json.loads(decoded)
                    return data.get('add'), data.get('port')
                except Exception:
                    return None 

            elif link.startswith(("vless://", "trojan://")):
                parsed_url = urllib.parse.urlparse(link)
                return parsed_url.hostname, parsed_url.port

            elif link.startswith("ss://"):
                try:
                    parsed_url = urllib.parse.urlparse(link)
                    # 首先检查 SIP002 格式 (user:pass@host:port)
                    # 检查 netloc 中是否有 '@' 且 '@' 之后的部分包含 ':'
                    if '@' in parsed_url.netloc and ':' in parsed_url.netloc.split('@', 1)[1]:
                         host_port_part = parsed_url.netloc.split('@', 1)[1]
                         # 从右侧分割一次以处理 IPv6 地址 [host]:port
                         host, port = host_port_part.rsplit(':', 1)
                         return host.strip('[]'), port # 移除 IPv6 方括号

                    # 检查 base64 格式 (ss://b64...)
                    # 假设如果 netloc 中没有 '@' 且 path 为空，则可能是 base64 格式
                    if not parsed_url.path and '@' not in parsed_url.netloc:
                         main_part = parsed_url.netloc
                         if main_part:
                              # 使用 urlsafe_b64decode 并处理填充
                              padding = '=' * (-len(main_part) % 4)
                              # 容错解码
                              decoded = base64.urlsafe_b64decode(main_part + padding).decode('utf-8', errors='ignore')
                              # 期望格式 method:pass@host:port
                              if '@' in decoded and ':' in decoded.split('@', 1)[1]:
                                   server_part = decoded.split('@')[1]
                                   host_port = server_part.rsplit(':', 1)
                                   if len(host_port) == 2:
                                       host = host_port[0].strip('[]')
                                       return host, host_port[1]
                except Exception:
                    return None
                return None


            elif link.startswith("ssr://"): # SSR 仍然只解析地址端口用于 TCP
                try:
                    decoded_part = link[6:]
                    padding = '=' * (-len(decoded_part) % 4)
                    decoded = base64.urlsafe_b64decode(decoded_part + padding).decode('utf-8', errors='ignore')
                    main_parts = decoded.split(':')
                    if len(main_parts) >= 2:
                        return main_parts[0], main_parts[1]
                except Exception:
                    return None 
            elif link.startswith(("hy2://", "hysteria2://")):
                 try:
                     parsed_url = urllib.parse.urlparse(link)
                     return parsed_url.hostname, parsed_url.port
                 except Exception:
                     return None
            elif link.startswith("hysteria://"):
                 try:
                     parsed_url = urllib.parse.urlparse(link)
                     return parsed_url.hostname, parsed_url.port
                 except Exception:
                      return None


            return None 
        
        except Exception:
            return None 

    # --- (重构) Sing-box 解析器 ---
    
    # (新增) 内部辅助函数：检查 IP 地址
    @staticmethod
    def _is_ip_address(s):
        """辅助函数: 检查字符串是否为 IP 地址 (v4 or v6)"""
        if not s: return False
        # 检查 IPv4
        if '.' in s:
            try:
                socket.inet_pton(socket.AF_INET, s)
                return True
            except socket.error:
                pass
        # 检查 IPv6 (需要包含 ':')
        if ':' in s:
            try:
                # 移除潜在的方括号
                s_no_brackets = s.strip('[]')
                socket.inet_pton(socket.AF_INET6, s_no_brackets)
                return True
            except socket.error:
                pass
        return False

    # (新增) Vmess 解析器
    @staticmethod
    def _parse_singbox_vmess(link, params, fragment):
        try:
            b64_part = link[8:]
            b64_part += '=' * (-len(b64_part) % 4)
            config = json.loads(base64.b64decode(b64_part).decode('utf-8', errors='ignore'))
        except Exception: 
            return None # Vmess 格式无效

        outbound = {
            "type": "vmess",
            "tag": fragment or f"vmess_{config.get('add', '')}_{config.get('port', 443)}",
            "server": config.get("add", ""),
            "server_port": int(config.get("port", 443)),
            "uuid": config.get("id", ""),
            "security": config.get("scy", "auto"),
            "alter_id": int(config.get("aid", 0)),
        }
        if params.get("packetEncoding"):
            outbound["packet_encoding"] = params.get("packetEncoding")

        net = config.get("net", "tcp")
        tls_enabled = config.get("tls", "none") == "tls"
        
        if tls_enabled:
            tls_config = {"enabled": True}
            server_name = config.get("sni", config.get("host", ""))
            tls_config["insecure"] = params.get("allowInsecure") == "1"
            if server_name and not RealProxyAggregator._is_ip_address(server_name):
                 tls_config["server_name"] = server_name
            
            alpn_str = params.get("alpn")
            if alpn_str:
                 tls_config["alpn"] = [s.strip() for s in alpn_str.split(',')]
            
            outbound["tls"] = tls_config

        if net == "ws":
            outbound["transport"] = {
                "type": "ws",
                "path": config.get("path", "/"),
                "headers": {"Host": config.get("host", config.get("add", ""))}
            }
        elif net == "grpc":
            outbound["transport"] = {
                "type": "grpc",
                "service_name": config.get("path", "")
            }
        return outbound

    # (新增) Vless/Trojan 解析器
    @staticmethod
    def _parse_singbox_vless_trojan(parsed_url, params, fragment):
        protocol = parsed_url.scheme
        hostname = parsed_url.hostname 
        port = parsed_url.port
        
        outbound = {
            "type": protocol,
            "tag": fragment or f"{protocol}_{hostname}_{port}",
            "server": hostname,
            "server_port": port,
        }
        
        if protocol == "vless":
            outbound["uuid"] = parsed_url.username
            if params.get("flow"): 
                outbound["flow"] = params.get("flow") 
        else: # trojan
            outbound["password"] = parsed_url.username

        security_type = params.get("security", "none") 
        tls_config = None 
        
        is_tls_enabled = False
        if protocol == "vless" and security_type in ("tls", "xtls", "reality"):
            is_tls_enabled = True
        elif protocol == "trojan" and security_type != "none": 
            is_tls_enabled = True

        if is_tls_enabled:
            tls_config = {"enabled": True}
            tls_config["insecure"] = params.get("allowInsecure") == "1" 
            
            sni = params.get("sni")
            server_name_to_use = None
            
            if sni and not RealProxyAggregator._is_ip_address(sni):
                server_name_to_use = sni
            elif not sni and hostname and not RealProxyAggregator._is_ip_address(hostname):
                 server_name_to_use = hostname
            
            if server_name_to_use:
                tls_config["server_name"] = server_name_to_use

            alpn_str = params.get("alpn")
            if alpn_str:
                 tls_config["alpn"] = [s.strip() for s in alpn_str.split(',')]

            if protocol == "vless" and security_type == "reality":
                tls_config["reality"] = {"enabled": True}
                if params.get("pbk"): tls_config["reality"]["public_key"] = params.get("pbk")
                if params.get("sid"): tls_config["reality"]["short_id"] = params.get("sid")
                
            utls_fp = params.get("fp")
            if utls_fp:
                 tls_config["utls"] = {"enabled": True, "fingerprint": utls_fp}

            if tls_config:
                outbound["tls"] = tls_config

        transport_type = params.get("type", "tcp")
        if transport_type == "ws":
            outbound["transport"] = {
                "type": "ws",
                "path": params.get("path", "/"),
                "headers": {"Host": params.get("host", hostname)}
            }
        elif transport_type == "grpc":
            outbound["transport"] = {
                "type": "grpc",
                "service_name": params.get("serviceName", "") 
            }
        return outbound

    # (新增) Shadowsocks 解析器
    @staticmethod
    def _parse_singbox_ss(parsed_url, params, fragment):
        method, password, address, port = None, None, None, None
        
        if '@' in parsed_url.netloc and ':' in parsed_url.netloc.split('@', 1)[1]:
            user_info_b64, host_port = parsed_url.netloc.split('@', 1)
            host, port_str = host_port.rsplit(':', 1)
            address = host.strip('[]')
            port = int(port_str)
            try:
                user_info_b64 += '=' * (-len(user_info_b64) % 4)
                decoded_user = base64.urlsafe_b64decode(user_info_b64).decode('utf-8', errors='ignore')
                method, password = decoded_user.split(':', 1)
            except Exception: return None

        elif parsed_url.username and parsed_url.password and parsed_url.hostname and parsed_url.port:
             method = urllib.parse.unquote(parsed_url.username)
             password = urllib.parse.unquote(parsed_url.password)
             address = parsed_url.hostname
             port = parsed_url.port

        elif not parsed_url.path and '@' not in parsed_url.netloc and ':' not in parsed_url.netloc:
             try:
                b64_part = parsed_url.netloc
                b64_part += '=' * (-len(b64_part) % 4)
                decoded_full = base64.urlsafe_b64decode(b64_part).decode('utf-8', errors='ignore')
                user_pass, host_port = decoded_full.split('@', 1)
                method, password = user_pass.split(':', 1)
                address, port_str = host_port.rsplit(':', 1)
                address = address.strip('[]')
                port = int(port_str)
             except Exception: return None
        else: 
            return None 

        supported_methods = [
            "aes-256-gcm", "aes-128-gcm", "chacha20-ietf-poly1305",
            "2022-blake3-aes-128-gcm", "2022-blake3-aes-256-gcm",
            "2022-blake3-chacha20-poly1305"
        ]
        method_lower = method.lower()
        if method_lower == "chacha20-poly1305": method = "chacha20-ietf-poly1305"
        if method not in supported_methods: return None 

        outbound = {
            "type": "shadowsocks",
            "tag": fragment or f"ss_{address}_{port}",
            "server": address,
            "server_port": port,
            "method": method,
            "password": password
        }

        plugin_param = params.get("plugin")
        if plugin_param:
             try:
                plugin_parts = plugin_param.split(';')
                plugin_name = plugin_parts[0].lower().strip()
                plugin_opts = {}
                for part in plugin_parts[1:]:
                    part = part.strip()
                    if not part: continue
                    if '=' in part:
                        key, value = part.split('=', 1)
                        plugin_opts[key.strip()] = value.strip()
                    else:
                         plugin_opts[part.strip()] = True 

                if plugin_name == "v2ray-plugin" and plugin_opts.get("mode") == "websocket":
                     ws_path = plugin_opts.get("path", "/")
                     ws_host = plugin_opts.get("host", address)
                     outbound["transport"] = {
                         "type": "ws",
                         "path": ws_path,
                         "headers": {"Host": ws_host}
                     }
                     if plugin_opts.get("tls") is True or plugin_opts.get("tls", "").lower() == "true":
                          tls_config = {"enabled": True}
                          sni = ws_host
                          if sni and not RealProxyAggregator._is_ip_address(sni):
                               tls_config["server_name"] = sni
                          tls_config["insecure"] = plugin_opts.get("insecure") is True or plugin_opts.get("insecure", "").lower() == "true"
                          outbound["tls"] = tls_config
             except Exception:
                  pass # 忽略插件解析失败
        return outbound

    # (新增) Hysteria v1 解析器
    @staticmethod
    def _parse_singbox_hysteria(parsed_url, params, fragment):
         hostname = parsed_url.hostname
         port = parsed_url.port
         auth = params.get("auth") or parsed_url.username or parsed_url.password
         sni_host = params.get("peer") or hostname
         insecure = params.get("insecure", "0") == "1"
         up_mbps_str = params.get("upmbps", "10")
         down_mbps_str = params.get("downmbps", "50")
         recv_window_conn = params.get("recv_window_conn") 
         recv_window = params.get("recv_window")        

         if not auth: return None 

         try:
             up_mbps = int(up_mbps_str)
             down_mbps = int(down_mbps_str)
         except ValueError:
             return None 

         outbound = {
             "type": "hysteria",
             "tag": fragment or f"hy_{hostname}_{port}",
             "server": hostname,
             "server_port": port,
             "up_mbps": up_mbps,
             "down_mbps": down_mbps,
             "auth_str": auth,
             "tls": {
                 "enabled": True, 
                 "insecure": insecure,
             }
         }
         if sni_host and not RealProxyAggregator._is_ip_address(sni_host):
             outbound["tls"]["server_name"] = sni_host
         if recv_window_conn:
             try: outbound["recv_window_conn"] = int(recv_window_conn)
             except ValueError: pass
         if recv_window:
             try: outbound["recv_window"] = int(recv_window)
             except ValueError: pass
         return outbound

    # (新增) Hysteria v2 解析器
    @staticmethod
    def _parse_singbox_hysteria2(parsed_url, params, fragment):
         hostname = parsed_url.hostname
         port = parsed_url.port
         password = parsed_url.username or parsed_url.password
         sni_host = params.get("sni") or hostname
         insecure = params.get("insecure", "0") == "1"

         if not password: return None 

         outbound = {
             "type": "hysteria2",
             "tag": fragment or f"hy2_{hostname}_{port}",
             "server": hostname,
             "server_port": port,
             "password": password,
             "tls": {
                 "enabled": True, 
                 "insecure": insecure,
             }
         }
         if sni_host and not RealProxyAggregator._is_ip_address(sni_host):
             outbound["tls"]["server_name"] = sni_host
         return outbound

    # (修改) 全新的 Sing-box 解析器 (v6.1.10 - 重构为调度器)
    @staticmethod
    def _parse_node_link_for_singbox(link):
        """专用于 Sing-box 的全新高级解析器 (v6.1.10 - 调度器)"""
        try:
            outbound = None
            parsed_url = urllib.parse.urlparse(link)
            # 使用 parse_qs 保留重复参数
            params_list = urllib.parse.parse_qs(parsed_url.query)
            # 将列表值转换为单个值（取第一个）
            params = {k: v[0] for k, v in params_list.items()}
            fragment = parsed_url.fragment # 用于 tag/name

            if link.startswith("vmess://"):
                outbound = RealProxyAggregator._parse_singbox_vmess(link, params, fragment)
            
            elif link.startswith(("vless://", "trojan://")):
                outbound = RealProxyAggregator._parse_singbox_vless_trojan(parsed_url, params, fragment)

            elif link.startswith("ss://"):
                outbound = RealProxyAggregator._parse_singbox_ss(parsed_url, params, fragment)

            elif link.startswith("hysteria://"):
                outbound = RealProxyAggregator._parse_singbox_hysteria(parsed_url, params, fragment)

            elif link.startswith(("hy2://", "hysteria2://")):
                outbound = RealProxyAggregator._parse_singbox_hysteria2(parsed_url, params, fragment)
            
            elif link.startswith("ssr://"):
                return None # 显式跳过 SSR

            if outbound: # 如果成功解析了任何支持的协议
                # 确保 tag 存在
                if not outbound.get("tag"):
                     proto = outbound.get('type', 'unknown')
                     serv = outbound.get('server', 'noserver')
                     outbound["tag"] = f"{proto}_{serv}_{int(time.time()*1000)}"
                return outbound

        except Exception as e:
            # 记录详细的解析错误以供调试 (保持注释状态)
            # traceback_str = traceback.format_exc() 
            # self.gui_queue.put(('log', f"解析 {link[:30]}... 失败: {e}\n{traceback_str}"))
            return None
        return None


    # (修改) 全新的 Sing-box 配置生成器
    @staticmethod
    def _generate_singbox_config(node_details, local_port):
        """动态生成 Sing-box 配置文件"""
        config = {
            "log": {"level": "warn"}, # 减少日志噪音
            "inbounds": [{
                "type": "socks", # 使用 SOCKS 入站进行测试
                "tag": "socks-in",
                "listen": "127.0.0.1",
                "listen_port": local_port
            }],
            "outbounds": [node_details], # node_details 是解析后的出站字典
            "route": {
                "rules": [{
                    "inbound": ["socks-in"],
                    "outbound": node_details["tag"] # 路由到特定的出站 tag
                }]
            }
        }
        # 使用 indent 以便调试时阅读
        return json.dumps(config, indent="\t")

    # (修改) 全新的 Sing-box 测试函数 (切换回 socks5h + 增强日志 v6.1.5)
    def _singbox_real_test(self, node_link, local_port, timeout, test_url):
        """Sing-box 真实延迟测试 (带详细诊断日志 v6.1.5)"""
        
        node_details = self._parse_node_link_for_singbox(node_link)
        if not node_details:
            # 解析失败日志已在解析器中处理 (如果取消了注释)
            return float('inf') 

        config_str = self._generate_singbox_config(node_details, local_port)
        # 为 sing-box 配置文件使用不同的前缀
        config_filename = f"sb_config_port_{local_port}.json"
        
        process = None
        config_filepath = None # 初始化路径变量
        try:
            # 确保临时文件写入当前工作目录或指定的临时目录
            config_filepath = os.path.join(os.getcwd(), config_filename)
            with open(config_filepath, 'w', encoding='utf-8') as f:
                f.write(config_str)

            # (新增) 记录生成的 outbound 配置 (去除 tag 以减少噪音)
            logged_details = node_details.copy()
            logged_details.pop('tag', None)
            # 尝试更简洁地记录配置
            # self.gui_queue.put(('log', f"  [诊断 {local_port}]: Outbound: {json.dumps(logged_details)}"))

            startupinfo = None
            if platform.system() == "Windows":
                startupinfo = subprocess.STARTUPINFO()
                startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
                startupinfo.wShowWindow = subprocess.SW_HIDE

            cmd = [self.singbox_path, "run", "-c", config_filepath]
            # (新增) 记录启动命令
            # self.gui_queue.put(('log', f"  [诊断 {local_port}]: Executing: {' '.join(cmd)}"))

            process = subprocess.Popen(
                cmd,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE, # 捕获 stderr 以检查配置错误
                startupinfo=startupinfo,
                text=True, # 将 stderr 解码为文本
                encoding='utf-8', errors='ignore' # 处理潜在的解码错误
            )
            
            # 等待片刻并检查进程是否立即退出
            time.sleep(0.7) # Sing-box 可能需要稍长时间启动/失败
            ret_code = process.poll()
            if ret_code is not None:
                 stderr_output = ""
                 try:
                     # 尝试读取尽可能多的 stderr 输出
                     stderr_output = process.stderr.read()
                 except Exception: pass
                 # 在 stderr 中尝试查找常见的配置错误
                 err_lower = stderr_output.lower()
                 if "configuration error" in err_lower or \
                    "decode config" in err_lower or \
                    "failed to create outbound" in err_lower or \
                    "failed to parse config" in err_lower:
                      error_msg = f"配置错误"
                 elif "fatal" in err_lower: # 捕获其他致命错误
                      error_msg = f"启动时发生严重错误"
                 else:
                      error_msg = f"启动失败 (Code: {ret_code})"
                 self.gui_queue.put(('log', f"  [Sing-box 失败 {local_port}]: {error_msg} -> {node_link[:40]}..."))
                 # (修改) 记录更详细的 stderr
                 if stderr_output:
                     self.gui_queue.put(('log', f"  [Sing-box Stderr {local_port}]: {stderr_output[:300]}...")) # 记录前 300 字符
                 return float('inf')
            
            # (新增) 如果进程仍在运行，记录日志
            # self.gui_queue.put(('log', f"  [诊断 {local_port}]: Sing-box 进程已启动，尝试连接..."))

            # (修改 v6.1.5) 切换回 socks5h
            proxies = {
                'http': f'socks5h://127.0.0.1:{local_port}',
                'https': f'socks5h://127.0.0.1:{local_port}'
            }
            
            response = None # 初始化 response
            start_time = time.monotonic() # 使用 monotonic 时钟
            response = requests.get(test_url, proxies=proxies, timeout=timeout, verify=certifi.where())
            response.raise_for_status() # Raises HTTPError for bad responses (4xx or 5xx)
            end_time = time.monotonic()
            
            # (新增) 记录成功日志
            latency = (end_time - start_time) * 1000
            # self.gui_queue.put(('log', f"  [Sing-box 成功 {local_port}]: 延迟 {latency:.0f} ms -> {node_link[:40]}..."))
            return latency # 毫秒

        # (修改 v6.1.5) 捕获 HTTPError 并记录更详细信息
        except requests.exceptions.HTTPError as e:
             status_code = e.response.status_code if e.response is not None else "N/A"
             response_text = ""
             if e.response is not None:
                  try:
                      response_text = e.response.text[:100] # 获取前100个字符
                  except Exception: pass
             error_details = f"HTTPError {status_code}"
             if response_text:
                  # 清理HTML标签以便阅读
                  clean_text = re.sub('<[^<]+?>', '', response_text).strip()
                  error_details += f" (内容: {clean_text}...)"

             self.gui_queue.put(('log', f"  [Sing-box 失败 {local_port}]: {error_details} -> {node_link[:40]}..."))
             return float('inf')
        except requests.exceptions.ProxyError as e:
            # 这通常表示本地 SOCKS 服务器无法访问 (杀毒软件/防火墙?)
            self.gui_queue.put(('log', f"  [Sing-box 失败 {local_port}]: 本地代理错误 (杀毒软件/防火墙?) -> {node_link[:40]}... ({e})"))
            return float('inf') 
        except requests.exceptions.ConnectionError as e:
            # 这表示通过代理连接错误 (节点可能已失效)
            self.gui_queue.put(('log', f"  [Sing-box 失败 {local_port}]: 远端连接错误 -> {node_link[:40]}... ({e})"))
            return float('inf') 
        except requests.exceptions.Timeout:
            self.gui_queue.put(('log', f"  [Sing-box 失败 {local_port}]: 超时 -> {node_link[:40]}..."))
            return float('inf')
        except Exception as e:
            self.gui_queue.put(('log', f"  [Sing-box 失败 {local_port}]: {type(e).__name__} -> {node_link[:40]}... ({e})"))
            return float('inf') 
        
        finally:
            # 健壮地终止 Sing-box 进程
            if process:
                try:
                    process.terminate() 
                    process.wait(timeout=1.0) 
                except subprocess.TimeoutExpired:
                    try:
                        process.kill()
                        process.wait(timeout=1.0) 
                    except Exception: pass 
                except Exception: pass 
            
            # 使用 config_filepath 删除文件
            if config_filepath and os.path.exists(config_filepath):
                try: os.remove(config_filepath)
                except Exception: pass 

# --- 图形化界面 (GUI) ---
class AggregatorApp:
    def __init__(self, root):
        self.root = root
        self.root.title("代理聚合器 v6.1.10 (代码重构)") # (修改) 标题
        self.root.geometry("850x980") 
        
        self.internal_github_token = "github_pat_" # 考虑使其可配置或移除占位符
        self.gui_queue = queue.Queue()
        self.stop_task_event = threading.Event()
        self.thread_lock = threading.Lock()
        self.executor = None
        self.found_links = set()
        self.full_result_text = ""
        
        self.aggregator = RealProxyAggregator(self.gui_queue)
        self.local_ports_queue = None 
        
        self._setup_ui()
        self.process_gui_queue()
        
        # (修改) 检查 Sing-box 可执行文件并更新单选按钮标签
        if not self.aggregator.singbox_exists:
            self.sb_test_radio.config(state='disabled') # (修改) 禁用 sing-box 单选按钮
            self.tcp_test_radio.invoke() 
            self._append_log(f"错误: 未在脚本目录找到 {self.aggregator.SINGBOX_EXECUTABLE}。Sing-box 真人测速已禁用。")
        else:
             self.sb_test_radio.config(text="Sing-box 真人测速 (测代理延迟)") # 如果存在则更新标签


    def _setup_ui(self):
        main_frame = ttk.Frame(self.root, padding="10")
        main_frame.pack(fill="both", expand=True)
        main_frame.columnconfigure(0, weight=1); main_frame.rowconfigure(7, weight=1); main_frame.rowconfigure(8, weight=1)

        search_frame = ttk.LabelFrame(main_frame, text="在线搜索订阅", padding="10")
        search_frame.grid(row=0, column=0, sticky="ew", pady=(0, 10))
        search_frame.columnconfigure(1, weight=1)

        ttk.Label(search_frame, text="GitHub Token:").grid(row=0, column=0, padx=(0, 10), pady=(0, 5), sticky='w')
        self.github_token_entry = PlaceholderEntry(search_frame, "（可选，提高搜索成功率）") # 更新占位符
        self.github_token_entry.grid(row=0, column=1, columnspan=2, sticky="ew", pady=(0, 5))
        
        ttk.Label(search_frame, text="搜索关键字 (逗号隔开):").grid(row=1, column=0, padx=(0, 10), pady=(5, 5), sticky='w')
        self.search_query_entry = ttk.Entry(search_frame)
        self.search_query_entry.insert(0, "clash,v2ray,sub,SSR,vmess,trojan,vless,SS,hysteria,hy2,节点") # (修改) 添加 hy2
        self.search_query_entry.grid(row=1, column=1, columnspan=2, sticky="ew", pady=(5, 5))
        
        ttk.Label(search_frame, text="搜索页数 (1-10):").grid(row=2, column=0, padx=(0, 10), pady=(5, 0), sticky='w')
        self.pages_spinbox = ttk.Spinbox(search_frame, from_=1, to=10, width=5)
        self.pages_spinbox.set(2)
        self.pages_spinbox.grid(row=2, column=1, sticky='w', pady=(5, 0))
        
        search_buttons_frame = ttk.Frame(search_frame)
        search_buttons_frame.grid(row=0, column=3, rowspan=3, padx=(15, 0), sticky='ns')
        self.search_button = ttk.Button(search_buttons_frame, text="开始并发搜索", command=self.run_search_thread)
        self.search_button.pack(fill='x', expand=True, ipady=5)
        self.stop_task_button = ttk.Button(search_buttons_frame, text="中止任务", command=self.stop_task, state='disabled')
        self.stop_task_button.pack(fill='x', expand=True, ipady=5, pady=(5,0))
        
        sub_frame = ttk.LabelFrame(main_frame, text="订阅链接 / 直接节点 (一行一个)", padding="5")
        sub_frame.grid(row=1, column=0, sticky="ew", pady=(0, 5))
        self.sub_links_text = scrolledtext.ScrolledText(sub_frame, height=8, width=100, relief='solid', bd=1)
        self.sub_links_text.pack(fill='x', expand=True)

        proxy_frame = ttk.LabelFrame(main_frame, text="代理服务器 (用于访问订阅和搜索)", padding="5")
        proxy_frame.grid(row=2, column=0, sticky="ew", pady=5)
        proxy_line_frame = ttk.Frame(proxy_frame); proxy_line_frame.pack(fill='x', expand=True, pady=2)
        self.proxy_enabled = tk.BooleanVar(value=False)
        self.proxy_checkbox = ttk.Checkbutton(proxy_line_frame, text="启用代理", variable=self.proxy_enabled); self.proxy_checkbox.pack(side='left', padx=(0, 10))
        ttk.Label(proxy_line_frame, text="地址:").pack(side='left', padx=(0, 5))
        self.proxy_entry = ttk.Entry(proxy_line_frame); self.proxy_entry.pack(side='left', fill='x', expand=True); self.proxy_entry.insert(tk.END, "http://127.0.0.1:10809")
        
        speed_test_frame = ttk.LabelFrame(main_frame, text="测速与筛选", padding="10")
        speed_test_frame.grid(row=3, column=0, sticky="ew", pady=(0, 5))
        speed_test_frame.columnconfigure(1, weight=1)

        self.speed_test_enabled = tk.BooleanVar(value=True) 
        # (修改) 更新 Sing-box 支持的标签文本
        self.speed_test_checkbox = ttk.Checkbutton(speed_test_frame, text="启用测速 (支持 Vmess/Vless/Trojan/SS/Hy/Hy2)", variable=self.speed_test_enabled)
        self.speed_test_checkbox.grid(row=0, column=0, columnspan=3, sticky='w', pady=(0, 5))

        # (修改) 测试模式变量和 Sing-box 的单选按钮
        self.test_mode = tk.StringVar(value="tcp") # 'tcp' or 'singbox'
        mode_frame = ttk.Frame(speed_test_frame)
        mode_frame.grid(row=1, column=0, columnspan=3, sticky='w')
        self.tcp_test_radio = ttk.Radiobutton(mode_frame, text="TCP Ping (测端口延迟)", variable=self.test_mode, value="tcp")
        self.tcp_test_radio.pack(side='left', padx=(0, 15))
        self.sb_test_radio = ttk.Radiobutton(mode_frame, text="Sing-box 检测中...", variable=self.test_mode, value="singbox") # 占位符文本
        self.sb_test_radio.pack(side='left')
        
        ttk.Label(speed_test_frame, text="测速超时 (秒):").grid(row=2, column=0, padx=(0, 10), pady=(5,0), sticky='w')
        self.timeout_spinbox = ttk.Spinbox(speed_test_frame, from_=1, to=15, width=5) # (修改) 允许更长超时
        self.timeout_spinbox.set(5) # (修改) 真实测试默认 5 秒
        self.timeout_spinbox.grid(row=2, column=1, sticky='w', pady=(5,0))

        ttk.Label(speed_test_frame, text="并发数 (搜索/测速):").grid(row=3, column=0, padx=(0, 10), pady=(5,0), sticky='w')
        self.concurrency_spinbox = ttk.Spinbox(speed_test_frame, from_=10, to=100, increment=10, width=5) # (修改) Sing-box 可能需要较低的最大并发数?
        self.concurrency_spinbox.set(20) # (修改) Sing-box 默认 20
        self.concurrency_spinbox.grid(row=3, column=1, sticky='w', pady=(5,0))
        
        # (修改) Sing-box 的测试 URL 标签
        ttk.Label(speed_test_frame, text="测速地址 (Sing-box):").grid(row=4, column=0, padx=(0, 10), pady=(5,0), sticky='w')
        self.test_url_entry = ttk.Entry(speed_test_frame)
        self.test_url_entry.insert(0, "http://www.msftconnecttest.com/connecttest.txt")
        self.test_url_entry.grid(row=4, column=1, columnspan=2, sticky='ew', pady=(5,0))
        
        control_frame = ttk.Frame(main_frame); control_frame.grid(row=5, column=0, pady=10)
        self.run_button = ttk.Button(control_frame, text="执行聚合处理 (含测速)", command=self.run_processing_thread); self.run_button.pack()

        log_frame = ttk.LabelFrame(main_frame, text="处理日志", padding="5"); log_frame.grid(row=7, column=0, sticky="nsew", pady=5)
        self.log_text = scrolledtext.ScrolledText(log_frame, state='disabled', wrap=tk.WORD); self.log_text.pack(fill='both', expand=True)
        
        result_frame = ttk.LabelFrame(main_frame, text="结果预览 (只显示前10条)", padding="5"); result_frame.grid(row=8, column=0, sticky="nsew", pady=5)
        result_header = ttk.Frame(result_frame); result_header.pack(fill='x', anchor='n', pady=(0, 5))
        self.save_button = ttk.Button(result_header, text="保存为文件...", command=self.save_result_to_file); self.save_button.pack(side='right', anchor='ne')
        ttk.Label(result_header, text="通用订阅格式 (Base64)").pack(side='left', anchor='nw')
        self.result_text = scrolledtext.ScrolledText(result_frame, wrap=tk.WORD); self.result_text.pack(fill='both', expand=True)

        self._setup_context_menus()

    def _setup_context_menus(self):
        TextWidgetContextMenu(self.github_token_entry)
        TextWidgetContextMenu(self.search_query_entry)
        TextWidgetContextMenu(self.sub_links_text)
        TextWidgetContextMenu(self.proxy_entry)
        TextWidgetContextMenu(self.result_text)
        TextWidgetContextMenu(self.test_url_entry) 

    def process_gui_queue(self):
        try:
            while not self.gui_queue.empty():
                msg_type, data = self.gui_queue.get_nowait()
                if msg_type == 'log':
                    self._append_log(data)
                elif msg_type == 'found_links':
                    self.found_links.update(data)
                elif msg_type == 'store_and_display_preview':
                    self.full_result_text = data
                    self._display_preview()
                elif msg_type == 'task_done':
                    was_stopped = self.stop_task_event.is_set()
                    self.set_buttons_state(is_running=False)

                    if data == 'search':
                        if self.found_links:
                            self._update_sub_links_text()
                            msg = f"搜索已中止。已将找到的 {len(self.found_links)} 个链接输出。" if was_stopped else f"搜索完成，已将 {len(self.found_links)} 个链接填入上方。请点击“执行聚合处理”。"
                            self._append_log(f"\n>>> {msg} <<<")
                        elif was_stopped:
                            self._append_log("\n>>> 搜索已中止，未找到任何链接。<<<")
                        
                    elif data == 'process':
                        msg = "聚合处理已中止" if was_stopped else "聚合处理完成"
                        self._append_log(f"\n=== {msg} ===")
        finally:
            self.root.after(100, self.process_gui_queue)

    def _display_preview(self):
        self.result_text.delete('1.0', tk.END)
        if not self.full_result_text:
            return
            
        try:
            decoded_bytes = base64.b64decode(self.full_result_text)
            full_text = decoded_bytes.decode('utf-8', errors='ignore') # 预览时忽略错误
            
            lines = full_text.splitlines()
            preview_lines = lines[:10]
            
            preview_text_content = "\n".join(preview_lines)
            # 仅重新编码预览部分以供显示
            preview_b64 = base64.b64encode(preview_text_content.encode('utf-8')).decode('utf-8')
            
            display_text = (
                f"{preview_b64}\n\n"
                f"--- (以上为前 {len(preview_lines)} 条节点预览) ---\n"
                f"--- (共 {len(lines)} 条完整结果已保存，请点击“保存为文件...”) ---"
            )
            
            self.result_text.insert('1.0', display_text)
            
        except Exception as e:
            self.result_text.insert('1.0', f"生成预览失败: {e}")

    def _append_log(self, message):
        # 确保日志追加是线程安全的（通过队列间接实现）
        # 添加try-except块以增加健壮性
        try:
            # 检查控件是否存在
            if hasattr(self, 'log_text') and self.log_text.winfo_exists():
                self.log_text.config(state='normal')
                self.log_text.insert(tk.END, message + "\n")
                self.log_text.config(state='disabled')
                self.log_text.see(tk.END) # 滚动到底部
        except tk.TclError:
             pass # 忽略写入已销毁控件的错误

    # (v6.1.10) 优化 GUI 卡顿
    def _update_sub_links_text(self):
        # 如果列表非常大，这可能会有点慢，但对于几千个链接应该没问题
        # 先清除现有内容
        try:
            if hasattr(self, 'sub_links_text') and self.sub_links_text.winfo_exists():
                 self.sub_links_text.delete('1.0', tk.END)
                 
                 total_links = len(self.found_links)
                 if total_links == 0:
                     return

                 sorted_links = sorted(list(self.found_links))
                 
                 # (新增) GUI 冻结保护
                 MAX_DISPLAY_LINKS = 5000 
                 content_to_insert = ""
                 
                 if total_links > MAX_DISPLAY_LINKS:
                     display_links = sorted_links[:MAX_DISPLAY_LINKS]
                     content_to_insert = "\n".join(display_links)
                     content_to_insert += f"\n\n... (警告: 仅显示前 {MAX_DISPLAY_LINKS} 条 / 共 {total_links} 条，以防 GUI 冻结) ..."
                     self._append_log(f"警告: 找到 {total_links} 个链接，输入框中仅显示前 {MAX_DISPLAY_LINKS} 条。")
                 else:
                     content_to_insert = "\n".join(sorted_links)

                 self.sub_links_text.insert('1.0', content_to_insert)
        except tk.TclError:
            self._append_log("!! 错误：更新订阅链接文本框时出错 (控件可能已关闭)")


    def set_buttons_state(self, is_running):
        state = 'disabled' if is_running else 'normal'
        # 检查按钮是否存在，以防 UI 关闭时出错
        try:
            if hasattr(self, 'search_button') and self.search_button.winfo_exists():
                 self.search_button.config(state=state)
            if hasattr(self, 'run_button') and self.run_button.winfo_exists():
                 self.run_button.config(state=state)
            if hasattr(self, 'stop_task_button') and self.stop_task_button.winfo_exists():
                 self.stop_task_button.config(state='normal' if is_running else 'disabled')
        except tk.TclError:
             pass # 忽略设置已销毁控件状态的错误


    def start_task(self, target_func, task_name):
        self.set_buttons_state(is_running=True)
        self._append_log(f"--- {task_name}任务开始 ---")
        
        self.stop_task_event.clear()
        
        thread = threading.Thread(target=target_func, daemon=True)
        thread.start()

    def run_search_thread(self):
        # 在开始新搜索前清除日志和结果
        try:
            if hasattr(self, 'log_text') and self.log_text.winfo_exists():
                 self.log_text.config(state='normal'); self.log_text.delete('1.0', tk.END); self.log_text.config(state='disabled')
            if hasattr(self, 'sub_links_text') and self.sub_links_text.winfo_exists():
                 self.sub_links_text.delete('1.0', tk.END)
            if hasattr(self, 'result_text') and self.result_text.winfo_exists():
                 self.result_text.delete('1.0', tk.END)
        except tk.TclError: pass # 忽略清除已销毁控件的错误
        self.found_links.clear()
        self.full_result_text = ""
        self.start_task(self._search_worker, "搜索")

    def run_processing_thread(self):
         # 在处理前仅清除日志和结果预览
        try:
            if hasattr(self, 'log_text') and self.log_text.winfo_exists():
                 self.log_text.config(state='normal'); self.log_text.delete('1.0', tk.END); self.log_text.config(state='disabled')
            if hasattr(self, 'result_text') and self.result_text.winfo_exists():
                 self.result_text.delete('1.0', tk.END)
        except tk.TclError: pass
        self.full_result_text = ""
        self.start_task(self._processing_worker, "聚合")

    def stop_task(self):
        self._append_log("\n正在发送中止信号... 请等待当前线程结束。")
        self.stop_task_event.set()
        # 如果有执行器，尝试更强制地关闭
        if self.executor:
            # cancel_futures 仅在 3.9+ 可用
            cancel_futures_flag = sys.version_info >= (3, 9)
            # 使用 wait=False 立即发送信号，后台进行清理
            self.executor.shutdown(wait=False, cancel_futures=cancel_futures_flag)
        try: # 捕获设置已销毁按钮状态的错误
            if hasattr(self, 'stop_task_button') and self.stop_task_button.winfo_exists():
                 self.stop_task_button.config(state='disabled') # 立即禁用停止按钮
        except tk.TclError: pass


    def _search_worker(self):
        try:
            user_token = self.github_token_entry.get_real_text().strip()
            
            # 简化 token 逻辑
            github_token = user_token if user_token else self.internal_github_token.strip()
            token_source = "您提供的" if user_token else "内置"
            # 处理 token 为空的情况
            token_display = f"{github_token[:5]}..." if github_token else "空"
            self.gui_queue.put(('log', f'使用{token_source}Token (长度: {len(github_token)}): {token_display}'))


            queries = [q.strip() for q in self.search_query_entry.get().strip().split(',') if q.strip()]
            pages = int(self.pages_spinbox.get())
            proxy = self.proxy_entry.get().strip() if self.proxy_enabled.get() else None
            
            concurrency = int(self.concurrency_spinbox.get())
            # 确保为搜索任务创建执行器
            # 关闭可能存在的旧执行器
            if self.executor: self.executor.shutdown(wait=False)
            self.executor = ThreadPoolExecutor(max_workers=concurrency)


            if not queries:
                self.gui_queue.put(('log', "错误：请至少输入一个搜索关键字。")); return

            finder = DynamicSubscriptionFinder(self.gui_queue, proxy, self.stop_task_event, self.thread_lock)
            finder.find(self.executor, github_token, queries, pages)

        except Exception as e:
            self.gui_queue.put(('log', f"搜索过程中发生严重错误: {e}\n{traceback.format_exc()}"))
        finally:
             # 确保执行器总是被干净地关闭
            if self.executor:
                 self.executor.shutdown(wait=True)
                 self.executor = None # 重置执行器状态
            self.gui_queue.put(('task_done', 'search')) # 发送搜索任务完成信号

    # (v6.1.10) 整合“中止时保存结果”逻辑
    def _processing_worker(self):
        try:
            input_lines = [u.strip() for u in self.sub_links_text.get('1.0', tk.END).strip().split('\n') if u.strip()]
            proxy = self.proxy_entry.get().strip() if self.proxy_enabled.get() else None
            
            enable_speed_test = self.speed_test_enabled.get()
            test_mode = self.test_mode.get() # 'tcp' or 'singbox'
            test_timeout = int(self.timeout_spinbox.get())
            test_concurrency = int(self.concurrency_spinbox.get())
            test_url = self.test_url_entry.get().strip() 
            
            direct_nodes = {line for line in input_lines if line.strip().startswith(NexavorUtils.NODE_PROTOCOLS)}
            http_links = [line for line in input_lines if line.strip().startswith(('http://', 'https'))]


            if not direct_nodes and not http_links:
                self.gui_queue.put(('log', "错误：请至少输入一个订阅链接或节点。")); return
                
            # (修改) 检查 Sing-box 可执行文件和测试 URL
            if enable_speed_test and test_mode == 'singbox':
                if not self.aggregator.singbox_exists:
                    self.gui_queue.put(('log', f"错误: 找不到 {self.aggregator.SINGBOX_EXECUTABLE}。请禁用测速，或切换到 TCP Ping 模式。"))
                    return
                if not test_url:
                    self.gui_queue.put(('log', "错误: Sing-box 测速模式下，[测速地址] 不能为空。"))
                    return


            unique_nodes = set(direct_nodes)
            if unique_nodes:
                self.gui_queue.put(('log', f"已直接识别到 {len(unique_nodes)} 个节点。"))

            if http_links:
                self.gui_queue.put(('log', f"开始并发下载 {len(http_links)} 个订阅链接..."))
                
                # 确保为下载任务 (重新) 创建执行器
                if self.executor: self.executor.shutdown(wait=False) 
                self.executor = ThreadPoolExecutor(max_workers=test_concurrency)

                
                future_to_url = {self.executor.submit(self.aggregator.fetch_and_parse_url, url, proxy): url for url in http_links}
                
                completed_count = 0
                total_links = len(future_to_url)
                for future in as_completed(future_to_url):
                    completed_count += 1
                    if self.stop_task_event.is_set(): break
                    try:
                        nodes_from_url = future.result()
                        if nodes_from_url:
                            # 如果集合操作很慢，可以逐个添加节点？
                            # 为简单起见，update 即可。
                            unique_nodes.update(nodes_from_url)
                        # 在下载期间更频繁地记录进度
                        if completed_count % 5 == 0 or completed_count == total_links:
                            self.gui_queue.put(('log', f"聚合进度: {completed_count}/{total_links} | 当前唯一节点: {len(unique_nodes)}"))
                    except Exception as e:
                         url_failed = future_to_url.get(future, "未知URL")
                         self.gui_queue.put(('log', f"一个聚合子任务失败 ({url_failed[:50]}...): {e}"))
            
            # 在可能启动测试执行器之前关闭下载执行器
            if self.executor:
                self.executor.shutdown(wait=True)
                self.executor = None

            # (*** 修改点 1 ***)
            # 移除此处的 'if self.stop_task_event.is_set(): return'
            # 即使下载被中止，也要继续处理已收集的 unique_nodes
            if self.stop_task_event.is_set():
                 self.gui_queue.put(('log', "下载任务已中止。继续处理已下载的节点..."))


            self.gui_queue.put(('log', f"\n下载完成。总共找到 {len(unique_nodes)} 个唯一节点。"))
            
            final_node_list = sorted(list(unique_nodes)) # 保留排序

            # --- (重构) 测速逻辑 ---
            if enable_speed_test and final_node_list:
                
                future_to_node = {} 

                # 为测试专门重新创建执行器
                # 关闭可能存在的旧执行器
                if self.executor: self.executor.shutdown(wait=False)
                self.executor = ThreadPoolExecutor(max_workers=test_concurrency)


                # (修改) Sing-box 模式逻辑
                if test_mode == 'singbox':
                    # 可选：如果需要，添加诊断锚点 (稍后可以注释掉)
                    # self.gui_queue.put(('log', "[诊断]: 正在添加 '测试锚点' 节点..."))
                    # ANCHOR_NODE = "vless://a341050e-80f0-4501-9aa2-3b854a6c11c5@cf.090227.xyz:443?encryption=none&security=tls&sni=ed.090227.xyz&type=ws&host=ed.090227.xyz&path=%2f#TestAnchor"
                    # final_node_list.insert(0, ANCHOR_NODE) 
                    
                    self.gui_queue.put(('log', f"=== 开始 Sing-box 真人测速（并发: {test_concurrency}, 超时: {test_timeout}s） ==="))
                    self.gui_queue.put(('log', f"测速目标: {test_url}")) 
                    
                    self.local_ports_queue = queue.Queue()
                    # (修改) 使用类常量
                    base_port = self.aggregator.SINGBOX_BASE_PORT
                    for i in range(test_concurrency):
                        self.local_ports_queue.put(base_port + i)
                    
                    self.gui_queue.put(('log', f"步骤 1: 正在提交 {len(final_node_list)} 个节点到线程池..."))
                    
                    
                    # 提交所有任务，让工作线程获取端口
                    for node_link in final_node_list:
                        if self.stop_task_event.is_set(): break
                        future = self.executor.submit(self._singbox_test_runner, node_link, test_timeout, test_url) # (修改) 调用 singbox runner
                        future_to_node[future] = node_link
                    
                    self.gui_queue.put(('log', "步骤 2: 提交完成。开始等待结果并监测死锁..."))


                else: # test_mode == 'tcp'
                    self.gui_queue.put(('log', f"=== 开始 TCP Ping 测速（并发: {test_concurrency}, 超时: {test_timeout}s） ==="))
                    nodes_to_test = []
                    parsing_errors = 0
                    
                    self.gui_queue.put(('log', "步骤 1: 正在解析节点链接..."))
                    for node_link in final_node_list:
                        if self.stop_task_event.is_set(): break # 在解析期间检查停止事件
                        parsed = self.aggregator._parse_node_link_for_tcp(node_link)
                        if parsed and parsed[0] and parsed[1]:
                            nodes_to_test.append({'link': node_link, 'addr': parsed[0], 'port': parsed[1]})
                        else:
                            parsing_errors += 1
                    
                    self.gui_queue.put(('log', f"解析完成: {len(nodes_to_test)} 个可测速, {parsing_errors} 个解析失败/不支持。"))
                    if not nodes_to_test:
                        self.gui_queue.put(('log', "没有可 TCP Ping 的节点。"));
                        # (*** 修改点 ***) 如果没有可测速的，也应该继续处理 final_node_list
                    else:
                        self.gui_queue.put(('log', f"步骤 2: 开始并发 TCP Ping {len(nodes_to_test)} 个节点..."))
                        
                        
                        future_to_node = {
                            self.executor.submit(self.aggregator._tcp_ping, node['addr'], node['port'], test_timeout): node['link']
                            for node in nodes_to_test
                        }

                # --- (通用) 结果收集 (使用 wait() 代替 as_completed) ---
                results = [] 
                tested_count = 0
                total_to_test = len(future_to_node)
                
                # 为真实测试增加稍长的超时缓冲，以应对 Sing-box 启动/关闭？
                task_timeout = test_timeout + 15 # (修改) 增加缓冲时间
                deadlock_strikes = 0
                max_strikes = 3 

                active_futures_set = set(future_to_node.keys()) # 使用集合以便更快地移除

                # (*** 修改点 ***) 仅在有任务时才进入循环
                while active_futures_set:
                    if self.stop_task_event.is_set():
                         break # 如果已停止则退出循环

                    # 使用带超时的 concurrent.futures.wait
                    done, not_done = wait(
                        active_futures_set,
                        timeout=task_timeout,
                        return_when=FIRST_COMPLETED
                    )

                    if not done: # 发生超时，没有 future 完成
                        deadlock_strikes += 1
                        self.gui_queue.put(('log', f"!! 警告: {task_timeout} 秒内没有任何节点完成测速... (第 {deadlock_strikes}/{max_strikes} 次尝试)"))
                        if deadlock_strikes >= max_strikes:
                            self.gui_queue.put(('log', f"!! 严重错误: 连续 {max_strikes} 次检测到死锁。"))
                            self.gui_queue.put(('log', "!! 可能原因: 杀毒软件/防火墙拦截, 或所有节点均超时/配置错误。"))
                            self.gui_queue.put(('log', "!! 正在强行中止所有测速任务..."))
                            self.stop_task() # 触发停止
                            break # 退出循环
                        continue # 继续等待

                    deadlock_strikes = 0 # 如果有任务完成则重置计数

                    for future in done:
                        link = future_to_node[future]
                        try:
                            delay = future.result() # 从完成的 future 获取结果
                            if delay != float('inf'):
                                results.append((delay, link))
                        except Exception as e:
                            # 捕获 future.result() 本身可能抛出的异常
                            self.gui_queue.put(('log', f"测速 {link[:30]}... 结果处理出错: {e}"))

                        tested_count += 1
                        active_futures_set.remove(future) # 从活动集合中移除

                        # 记录进度
                        if tested_count % 10 == 0 or tested_count == total_to_test: # (修改) 或许更频繁地记录日志？
                            self.gui_queue.put(('log', f"测速进度: {tested_count}/{total_to_test} | 存活: {len(results)}"))
                
                # --- 结果收集循环结束 ---

                # (*** 修改点 2 ***)
                # 如果循环因 stop_task() 退出，取消剩余的 future，但不再 'return'
                if self.stop_task_event.is_set():
                    self.gui_queue.put(('log', "测速任务被中止。"))
                    for future in active_futures_set: # 取消任何剩余的活动 future
                        future.cancel()
                    # (移除 'return') -> 继续处理已收集的 'results'

                self.gui_queue.put(('log', "\n测速完成。正在按延迟排序..."))
                
                if not results: 
                    self.gui_queue.put(('log', "未找到任何测速成功的节点。将使用原始列表（如果存在）。"));
                    # (*** 修改点 ***) 不要在这里 return，让函数继续处理 final_node_list
                else:
                    results.sort(key=lambda x: x[0])
                    final_node_list = [link for delay, link in results] # (*** 修改点 ***) 仅当有结果时才覆盖
                
                    # 可选：如果添加了锚点并且它存活，则移除
                    anchor_was_alive = False
                    anchor_removed = False
                    # if test_mode == 'singbox' and final_node_list and "#TestAnchor" in final_node_list[0]:
                    #     anchor_was_alive = True
                    #     self.gui_queue.put(('log', f"[诊断]: '测试锚点' 存活，延迟: {results[0][0]:.2f} ms"))
                    #     if len(final_node_list) > 1:
                    #          final_node_list.pop(0)
                    #          results.pop(0)
                    #          anchor_removed = True
                    # elif test_mode == 'singbox':
                    #     self.gui_queue.put(('log', "[诊断]: '测试锚点' 测速失败。"))


                    if final_node_list:
                         # 检查在移除锚点后列表是否变为空
                        if not results and anchor_removed:
                             self.gui_queue.put(('log', "筛选完毕！但移除锚点后无剩余节点。")); 
                             final_node_list = [] # (*** 修改点 ***) 明确设置为空列表
                        elif not results: # 如果 final_node_list 非空，则不应发生，但做安全检查
                             self.gui_queue.put(('log', "未找到任何测速成功的节点。"));
                             final_node_list = [] # (*** 修改点 ***) 明确设置为空列表
                        else:
                             self.gui_queue.put(('log', f"筛选完毕！共 {len(final_node_list)} 个存活节点。最快延迟: {results[0][0]:.2f} ms"))
                    else:
                        # Occurs if only anchor was alive and removed
                        self.gui_queue.put(('log', "未找到任何测速成功的节点。"));
                        final_node_list = [] # (*** 修改点 ***) 明确设置为空列表


            # --- 测速逻辑结束 ---

            elif not final_node_list:
                 self.gui_queue.put(('log', "未找到任何有效节点。")); return
            else:
                self.gui_queue.put(('log', "测速未启用。按默认顺序生成结果..."))
            
            # 最终结果处理
            # (*** 修改点 ***) 即使中止了，也会执行到这里
            if not final_node_list:
                 self.gui_queue.put(('log', "最终列表为空，无法生成结果。")); return

            self.gui_queue.put((('log', f"总共 {len(final_node_list)} 个节点。正在进行Base64编码...")))
            
            final_text = "\n".join(final_node_list)
            
            self.gui_queue.put(('log', f"用于编码的文本前50个字符: {repr(final_text[:50])}"))
            
            final_result = base64.b64encode(final_text.encode('utf-8')).decode('utf-8')
            
            self.gui_queue.put(('log', "编码完成！正在更新结果预览..."))
            self.gui_queue.put(('store_and_display_preview', final_result))

        except Exception as e:
            self.gui_queue.put(('log', f"聚合过程中发生严重错误: {e}\n{traceback.format_exc()}"))
        finally:
             # 确保用于测试的执行器被关闭
            if self.executor:
                 self.executor.shutdown(wait=True)
                 self.executor = None
            self.gui_queue.put(('task_done', 'process')) # 发送处理任务完成信号


    # (修改) Sing-box 的新运行器
    def _singbox_test_runner(self, node_link, timeout, test_url): 
        """Sing-box 线程池的包装器, 自动从实例队列中获取/归还端口"""
        local_port = None
        try:
            # 从队列获取端口，添加超时以防止在队列逻辑失败时无限等待
            local_port = self.local_ports_queue.get(timeout=20) # 增加获取端口的超时时间
            
            return self.aggregator._singbox_real_test(node_link, local_port, timeout, test_url) # 调用 singbox 测试
        
        except queue.Empty:
            self.gui_queue.put(('log', "!! 警告: Sing-box 端口池获取超时，一个测速任务被跳过。"))
            return float('inf')
        except Exception as e:
            # 记录运行器本身的异常 (例如调用 _singbox_real_test 期间的异常)
            self.gui_queue.put(('log', f"Sing-box 运行器异常 ({node_link[:20]}...): {e}"))
            return float('inf') 

        finally:
            # 仅在成功获取端口后才将其归还到队列
            if local_port is not None:
                try:
                    self.local_ports_queue.put(local_port, block=False) # 使用非阻塞 put
                except queue.Full:
                     self.gui_queue.put(('log', "!! 警告: Sing-box 端口队列已满，无法归还端口。"))


    def save_result_to_file(self):
        content = self.full_result_text.strip()
        if not content:
            messagebox.showwarning("内容为空", "没有可以保存的内容。")
            return
        
        # 使用 asksaveasfilename 以获得更好的用户体验
        file_path = filedialog.asksaveasfilename(
            title="保存订阅文件",
            defaultextension=".txt",
            filetypes=[("文本文件", "*.txt"), ("所有文件", "*.*")]
        )
        if not file_path: return # 用户取消
        
        try:
            with open(file_path, 'w', encoding='utf-8') as f:
                f.write(content)
            messagebox.showinfo("保存成功", f"订阅文件已成功保存至：\n{file_path}")
        except Exception as e:
            messagebox.showerror("保存失败", f"保存文件时发生错误：\n{e}")

if __name__ == "__main__":
    # 确保 concurrent.futures 被隐式导入以供 PyInstaller 使用
    import concurrent.futures
    root = tk.Tk()
    app = AggregatorApp(root)
    root.mainloop()
