#!/usr/bin/env python3
"""接收 ESP32-P4 上传的 H.264 实时帧，并保存裸码流及提供预览。"""

from __future__ import annotations

import argparse
import functools
import json
import queue
import shutil
import struct
import threading
import time
from collections import deque
from datetime import datetime
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse


MAX_JPEG_SIZE = 2 * 1024 * 1024
MAX_H264_FRAME_SIZE = 1024 * 1024
SAVE_LOCK = threading.Lock()
# 广播命令时串行化发送，避免多个 HTTP 请求线程同时写同一条 WebSocket 连接。
WS_SEND_LOCK = threading.Lock()
PREVIEW_QUEUE_SIZE = 16
PREVIEW_JPEG_QUALITY = 85
MJPEG_BOUNDARY = "lummiss-frame"

# P4 每帧前的 16 字节自描述头，H.264 与 MJPEG 直通分别使用 LMV1/LMJ1：
#   [0..3] magic    [4..5] width    [6..7] height
#   [8..9] fps      [10] frame_type [11] 保留
#   [12..15] sequence
# frame_type 沿用 esp_h264_frame_type_t 原值：0=IDR、1=I、2=P；
# 只有 0/1 可随机接入，preview 的重同步依赖这个判断。
WS_FRAME_HEADER_SIZE = 16
WS_H264_MAGIC = b"LMV1"
WS_MJPEG_MAGIC = b"LMJ1"


class CameraHTTPServer(ThreadingHTTPServer):
    """让浏览器的长期 MJPEG 连接不阻止服务器退出。"""

    daemon_threads = True
    allow_reuse_address = True


def enqueue_preview_frame(server: CameraHTTPServer, frame: bytes, is_keyframe: bool) -> None:
    """把编码帧送入预览队列；积压时等待下一个 IDR 重新同步。

    H.264 的 P 帧依赖前面的参考帧，不能简单丢掉队首后继续解码。队列满时
    清空积压并等待下一关键帧，使浏览器快速回到最新画面且不会持续花屏。
    """
    if not server.preview_enabled:  # type: ignore[attr-defined]
        return

    with server.preview_queue_lock:  # type: ignore[attr-defined]
        if server.preview_wait_idr:  # type: ignore[attr-defined]
            if not is_keyframe:
                server.preview_dropped += 1  # type: ignore[attr-defined]
                return
            server.preview_wait_idr = False  # type: ignore[attr-defined]
            reset_decoder = True
        else:
            reset_decoder = False

        try:
            server.preview_queue.put_nowait((frame, reset_decoder))  # type: ignore[attr-defined]
            return
        except queue.Full:
            dropped = 0
            while True:
                try:
                    server.preview_queue.get_nowait()  # type: ignore[attr-defined]
                    dropped += 1
                except queue.Empty:
                    break
            server.preview_dropped += dropped + 1  # type: ignore[attr-defined]

            if is_keyframe:
                server.preview_wait_idr = False  # type: ignore[attr-defined]
                server.preview_queue.put_nowait((frame, True))  # type: ignore[attr-defined]
            else:
                server.preview_wait_idr = True  # type: ignore[attr-defined]


def receive_h264_frame(server: CameraHTTPServer, frame: bytes, frame_type: str,
                       sequence: str) -> None:
    """落盘 + 统计 + 送预览队列。

    WebSocket 与 HTTP POST 两条入口共用，保证两条路的落盘文件、统计口径
    和预览行为完全一致。frame 必须是剥掉协议头之后的裸 Annex-B 码流。
    """
    now = time.monotonic()
    with SAVE_LOCK:
        with server.h264_path.open("ab") as stream:  # type: ignore[attr-defined]
            stream.write(frame)
        with server.latest_h264_path.open("ab") as stream:  # type: ignore[attr-defined]
            stream.write(frame)

        if server.stream_frames == 0:  # type: ignore[attr-defined]
            # 统计从第一帧开始，避免服务器提前启动的等待时间拉低平均帧率。
            server.stream_started = now  # type: ignore[attr-defined]
            server.last_report = now  # type: ignore[attr-defined]
        server.stream_frames += 1  # type: ignore[attr-defined]
        server.stream_bytes += len(frame)  # type: ignore[attr-defined]
        server.last_frame_time = now  # type: ignore[attr-defined]
        frames = server.stream_frames  # type: ignore[attr-defined]
        total_bytes = server.stream_bytes  # type: ignore[attr-defined]
        last_report = server.last_report  # type: ignore[attr-defined]
        report = now - last_report >= 10.0
        if report:
            interval = max(now - last_report, 0.001)
            interval_frames = frames - server.report_frames  # type: ignore[attr-defined]
            interval_bytes = total_bytes - server.report_bytes  # type: ignore[attr-defined]
            server.recent_fps = interval_frames / interval  # type: ignore[attr-defined]
            server.recent_kbps = interval_bytes * 8 / interval / 1000  # type: ignore[attr-defined]
            server.report_frames = frames  # type: ignore[attr-defined]
            server.report_bytes = total_bytes  # type: ignore[attr-defined]
            server.last_report = now  # type: ignore[attr-defined]
            recent_fps = server.recent_fps  # type: ignore[attr-defined]
            recent_kbps = server.recent_kbps  # type: ignore[attr-defined]

    enqueue_preview_frame(server, frame, frame_type in {"0", "1"})
    if report:
        print(
            "H.264 接收："
            f"累计 {frames} 帧，最近 {recent_fps:.1f} fps，"
            f"{recent_kbps:.0f} kbps，"
            f"最新序号 {sequence}",
            flush=True,
        )


def receive_mjpeg_frame(server: CameraHTTPServer, frame: bytes, sequence: str) -> None:
    """接收摄像头原始 JPEG，直接发布给浏览器，不经过 PyAV/OpenCV。"""
    if len(frame) < 4 or not frame.startswith(b"\xff\xd8") or not frame.endswith(b"\xff\xd9"):
        server.ws_bad_frames += 1  # type: ignore[attr-defined]
        return

    now = time.monotonic()
    if server.stream_frames == 0:  # type: ignore[attr-defined]
        server.stream_started = now  # type: ignore[attr-defined]
        server.last_report = now  # type: ignore[attr-defined]
    server.stream_frames += 1  # type: ignore[attr-defined]
    server.stream_bytes += len(frame)  # type: ignore[attr-defined]
    server.last_frame_time = now  # type: ignore[attr-defined]
    with server.preview_condition:  # type: ignore[attr-defined]
        server.preview_jpeg = frame  # type: ignore[attr-defined]
        server.preview_sequence += 1  # type: ignore[attr-defined]
        server.preview_frames += 1  # type: ignore[attr-defined]
        server.preview_last_frame_time = now  # type: ignore[attr-defined]
        server.preview_times.append(now)  # type: ignore[attr-defined]
        server.preview_condition.notify_all()  # type: ignore[attr-defined]

    if now - server.last_report >= 10.0:  # type: ignore[attr-defined]
        interval = max(now - server.last_report, 0.001)  # type: ignore[attr-defined]
        interval_frames = server.stream_frames - server.report_frames  # type: ignore[attr-defined]
        interval_bytes = server.stream_bytes - server.report_bytes  # type: ignore[attr-defined]
        server.recent_fps = interval_frames / interval  # type: ignore[attr-defined]
        server.recent_kbps = interval_bytes * 8 / interval / 1000  # type: ignore[attr-defined]
        server.report_frames = server.stream_frames  # type: ignore[attr-defined]
        server.report_bytes = server.stream_bytes  # type: ignore[attr-defined]
        server.last_report = now  # type: ignore[attr-defined]
        print(f"MJPEG 直通接收：累计 {server.stream_frames} 帧，最近 "
              f"{server.recent_fps:.1f} fps，{server.recent_kbps:.0f} kbps，"
              f"最新序号 {sequence}", flush=True)


def parse_ws_frame(message: bytes) -> tuple[str, int, int, int, int, int, bytes] | None:
    """拆开 P4 的「16 字节帧头 + 视频载荷」消息。

    返回 (kind, width, height, fps, frame_type, sequence, payload)。
    分辨率随每帧携带，所以 P4 改分辨率时这里不需要任何改动。
    """
    if len(message) <= WS_FRAME_HEADER_SIZE:
        return None
    if message[:4] == WS_H264_MAGIC:
        kind = "h264"
    elif message[:4] == WS_MJPEG_MAGIC:
        kind = "mjpeg"
    else:
        return None
    width, height, fps = struct.unpack_from("<HHH", message, 4)
    sequence = struct.unpack_from("<I", message, 12)[0]
    payload = message[WS_FRAME_HEADER_SIZE:]
    if kind == "h264" and not (payload.startswith(b"\x00\x00\x00\x01") or payload.startswith(b"\x00\x00\x01")):
        return None
    if kind == "mjpeg" and not (payload.startswith(b"\xff\xd8") and payload.endswith(b"\xff\xd9")):
        return None
    return kind, width, height, fps, message[10], sequence, payload


def broadcast_command(server: CameraHTTPServer, command: str) -> int:
    """向所有已连接的 P4 广播一条文本命令，返回实际发送到的连接数。

    命令是异步生效的：P4 的回复要等它下一条 WebSocket 消息回来，
    落在 server.last_command_reply，通过 /status 读取。
    """
    payload = json.dumps({"cmd": command}, ensure_ascii=False)
    with server.ws_clients_lock:  # type: ignore[attr-defined]
        clients = list(server.ws_clients)  # type: ignore[attr-defined]

    sent = 0
    with WS_SEND_LOCK:
        for connection in clients:
            try:
                connection.send(payload)
                sent += 1
            except Exception:
                # 连接刚好断开：由 ws_client_handler 的收尾逻辑负责摘除。
                pass
    return sent


def ws_client_handler(server: CameraHTTPServer, connection: object) -> None:
    """处理一条来自 P4 的 WebSocket 连接。

    二进制消息 = H.264 帧（16 字节头 + Annex-B 码流）；
    文本消息 = P4 对下行命令的回复。
    """
    with server.ws_clients_lock:  # type: ignore[attr-defined]
        server.ws_clients.add(connection)  # type: ignore[attr-defined]
    print(
        f"P4 WebSocket 已连接：{getattr(connection, 'remote_address', '?')}",
        flush=True,
    )
    try:
        while True:
            try:
                message = connection.recv()
            except Exception as error:
                # 保留异常类型和文本，才能和 P4 的 transport_poll_write(0)
                # 时间点对齐，区分服务器主动关闭、超时和底层网络断链。
                print(
                    f"P4 WebSocket 接收结束：{type(error).__name__}: {error}",
                    flush=True,
                )
                break

            if isinstance(message, bytes):
                parsed = parse_ws_frame(message)
                if parsed is None:
                    server.ws_bad_frames += 1  # type: ignore[attr-defined]
                    continue
                kind, width, height, fps, frame_type, sequence, payload = parsed
                server.ws_width = width  # type: ignore[attr-defined]
                server.ws_height = height  # type: ignore[attr-defined]
                server.ws_fps = fps  # type: ignore[attr-defined]
                if kind == "mjpeg":
                    receive_mjpeg_frame(server, payload, str(sequence))
                else:
                    receive_h264_frame(server, payload, str(frame_type), str(sequence))
            else:
                server.last_command_reply = message  # type: ignore[attr-defined]
                print(f"P4 命令回复：{message}", flush=True)
    finally:
        with server.ws_clients_lock:  # type: ignore[attr-defined]
            server.ws_clients.discard(connection)  # type: ignore[attr-defined]
        # 新连接/重连后的第一批数据可能从 P 帧开始。清空旧预览队列并
        # 等待下一个 IDR，避免把断线前的参考帧和恢复后的 P 帧拼在一起。
        with server.preview_queue_lock:  # type: ignore[attr-defined]
            server.preview_wait_idr = True  # type: ignore[attr-defined]
            while True:
                try:
                    server.preview_queue.get_nowait()  # type: ignore[attr-defined]
                except queue.Empty:
                    break
        print("P4 WebSocket 已断开", flush=True)


def start_ws_server(server: CameraHTTPServer, host: str, port: int) -> bool:
    """在后台线程里启动 WebSocket 服务端。

    用 websockets 的线程式 API（websockets.sync.server），与现有
    ThreadingHTTPServer 的线程模型一致，不需要引入 asyncio。
    """
    try:
        from websockets.sync.server import serve
    except ImportError:
        print(
            "缺少 websockets：请运行 python -m pip install -r "
            "tools/camera_server_requirements.txt",
            flush=True,
        )
        return False

    server.ws_server = serve(  # type: ignore[attr-defined]
        functools.partial(ws_client_handler, server), host, port)
    threading.Thread(
        target=server.ws_server.serve_forever,  # type: ignore[attr-defined]
        name="ws-server",
        daemon=True,
    ).start()
    print(f"WebSocket 监听：ws://{host}:{port}/ws", flush=True)
    return True


class CameraRequestHandler(BaseHTTPRequestHandler):
    """接收 H.264 帧；保留旧 JPEG 接口用于兼容性检查。"""

    server_version = "LummissCameraServer/3.0"
    protocol_version = "HTTP/1.1"

    @property
    def output_dir(self) -> Path:
        return self.server.output_dir  # type: ignore[attr-defined]

    def _send_bytes(self, status: int, content_type: str, data: bytes) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if data:
            self.wfile.write(data)

    def _send_json(self, status: int, payload: dict[str, object]) -> None:
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self._send_bytes(status, "application/json; charset=utf-8", data)

    def _read_body(self, maximum: int) -> bytes | None:
        try:
            content_length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            content_length = 0
        if content_length <= 0 or content_length > maximum:
            self._send_json(
                HTTPStatus.BAD_REQUEST,
                {"ok": False, "error": "invalid content length"},
            )
            return None
        body = self.rfile.read(content_length)
        if len(body) != content_length:
            self._send_json(
                HTTPStatus.BAD_REQUEST,
                {"ok": False, "error": "incomplete request body"},
            )
            return None
        return body

    def _handle_h264(self) -> None:
        """HTTP 上传入口，作为 WebSocket 主通道的回退保留。"""
        frame = self._read_body(MAX_H264_FRAME_SIZE)
        if frame is None:
            return
        if not (frame.startswith(b"\x00\x00\x00\x01") or frame.startswith(b"\x00\x00\x01")):
            self._send_json(
                HTTPStatus.BAD_REQUEST,
                {"ok": False, "error": "not an Annex-B H.264 frame"},
            )
            return

        receive_h264_frame(
            self.server,
            frame,
            self.headers.get("X-Frame-Type", "-1"),
            self.headers.get("X-Frame-Sequence", "unknown"),
        )
        self._send_bytes(HTTPStatus.NO_CONTENT, "text/plain", b"")

    def _handle_jpeg(self) -> None:
        jpeg = self._read_body(MAX_JPEG_SIZE)
        if jpeg is None:
            return
        if not (jpeg.startswith(b"\xff\xd8") and jpeg.endswith(b"\xff\xd9")):
            self._send_json(
                HTTPStatus.BAD_REQUEST,
                {"ok": False, "error": "incomplete jpeg"},
            )
            return

        sequence = self.headers.get("X-Frame-Sequence", "unknown")
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")[:-3]
        image_path = self.output_dir / f"camera_{timestamp}_{sequence}.jpg"
        latest_path = self.output_dir / "latest.jpg"
        latest_tmp = self.output_dir / "latest.tmp"
        with SAVE_LOCK:
            image_path.write_bytes(jpeg)
            shutil.copyfile(image_path, latest_tmp)
            latest_tmp.replace(latest_path)
        self._send_json(HTTPStatus.OK, {"ok": True, "file": image_path.name})

    def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        path = urlparse(self.path).path
        if path == "/h264":
            self._handle_h264()
        elif path == "/upload":
            self._handle_jpeg()
        else:
            self._send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "not found"})

    def _stream_status(self) -> dict[str, object]:
        with SAVE_LOCK:
            frames = self.server.stream_frames  # type: ignore[attr-defined]
            total_bytes = self.server.stream_bytes  # type: ignore[attr-defined]
            started = self.server.stream_started  # type: ignore[attr-defined]
            last_frame = self.server.last_frame_time  # type: ignore[attr-defined]
            stream_name = self.server.h264_path.name  # type: ignore[attr-defined]
            recent_fps = self.server.recent_fps  # type: ignore[attr-defined]
            recent_kbps = self.server.recent_kbps  # type: ignore[attr-defined]
            report_frames = self.server.report_frames  # type: ignore[attr-defined]
        now = time.monotonic()
        elapsed = max(now - started, 0.001)
        if last_frame == 0 or now - last_frame > 2.0:
            recent_fps = 0.0
            recent_kbps = 0.0
        elif report_frames == 0:
            # 第一轮 10 秒汇总前也显示实时估算，避免页面有帧却仍显示 0 FPS。
            recent_fps = frames / elapsed
            recent_kbps = total_bytes * 8 / elapsed / 1000

        with self.server.preview_condition:  # type: ignore[attr-defined]
            preview_times = tuple(self.server.preview_times)  # type: ignore[attr-defined]
            preview_frames = self.server.preview_frames  # type: ignore[attr-defined]
            preview_dropped = self.server.preview_dropped  # type: ignore[attr-defined]
            preview_last_frame = self.server.preview_last_frame_time  # type: ignore[attr-defined]
        if len(preview_times) >= 2 and now - preview_last_frame <= 2.0:
            preview_fps = (len(preview_times) - 1) / max(preview_times[-1] - preview_times[0], 0.001)
        else:
            preview_fps = 0.0

        with self.server.ws_clients_lock:  # type: ignore[attr-defined]
            ws_clients = len(self.server.ws_clients)  # type: ignore[attr-defined]
        return {
            "ok": True,
            "ws_clients": ws_clients,
            "ws_bad_frames": self.server.ws_bad_frames,  # type: ignore[attr-defined]
            # 分辨率由 P4 每帧随帧头带上来，这里只是显示最新值。
            "video_width": self.server.ws_width,  # type: ignore[attr-defined]
            "video_height": self.server.ws_height,  # type: ignore[attr-defined]
            "video_fps": self.server.ws_fps,  # type: ignore[attr-defined]
            "last_command_reply": self.server.last_command_reply,  # type: ignore[attr-defined]
            "video_enabled": self.server.video_enabled,  # type: ignore[attr-defined]
            "frames": frames,
            "bytes": total_bytes,
            "average_fps": round(frames / elapsed, 2),
            "average_kbps": round(total_bytes * 8 / elapsed / 1000, 1),
            "recent_fps": round(recent_fps, 2),
            "recent_kbps": round(recent_kbps, 1),
            "seconds_since_last_frame": None if last_frame == 0 else round(now - last_frame, 2),
            "stream_file": stream_name,
            "preview_frames": preview_frames,
            "preview_fps": round(preview_fps, 2),
            "preview_dropped": preview_dropped,
        }

    def _handle_command(self) -> None:
        """把命令广播给所有已连接的 P4（下行控制通道）。

        命令是异步生效的：这里只返回广播到的连接数，P4 的回复要等它下一条
        WebSocket 消息回来，通过 /status 的 last_command_reply 读取。
        例：/cmd?cmd=ping、/cmd?cmd=status。
        """
        query = parse_qs(urlparse(self.path).query)
        command = query.get("cmd", [""])[0].strip()
        if not command:
            self._send_json(
                HTTPStatus.BAD_REQUEST,
                {"ok": False, "error": "missing cmd, e.g. /cmd?cmd=ping"},
            )
            return

        sent = broadcast_command(self.server, command)
        if sent > 0 and command in {"video_on", "video_off"}:
            self.server.video_enabled = command == "video_on"  # type: ignore[attr-defined]
        self._send_json(HTTPStatus.OK, {
            "ok": True,
            "command": command,
            "clients": sent,
            "last_reply": self.server.last_command_reply,  # type: ignore[attr-defined]
        })

    def _handle_mjpeg_stream(self) -> None:
        """用一个长期 HTTP 连接持续推送最新 JPEG 帧。"""
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", f"multipart/x-mixed-replace; boundary={MJPEG_BOUNDARY}")
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
        self.send_header("Pragma", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

        # 页面在首帧到达前连接时，从当前序号开始等待，避免空转占满一个 CPU 核；
        # 已有画面时则让循环立即发送当前最新帧。
        with self.server.preview_condition:  # type: ignore[attr-defined]
            if self.server.preview_jpeg is None:  # type: ignore[attr-defined]
                last_sequence = self.server.preview_sequence  # type: ignore[attr-defined]
            else:
                last_sequence = self.server.preview_sequence - 1  # type: ignore[attr-defined]
        try:
            while True:
                with self.server.preview_condition:  # type: ignore[attr-defined]
                    self.server.preview_condition.wait_for(  # type: ignore[attr-defined]
                        lambda: self.server.preview_sequence != last_sequence,  # type: ignore[attr-defined]
                        timeout=5.0,
                    )
                    sequence = self.server.preview_sequence  # type: ignore[attr-defined]
                    jpeg = self.server.preview_jpeg  # type: ignore[attr-defined]
                if jpeg is None or sequence == last_sequence:
                    continue
                last_sequence = sequence
                header = (
                    f"--{MJPEG_BOUNDARY}\r\n"
                    "Content-Type: image/jpeg\r\n"
                    f"Content-Length: {len(jpeg)}\r\n"
                    f"X-Frame-Sequence: {sequence}\r\n\r\n"
                ).encode("ascii")
                self.wfile.write(header)
                self.wfile.write(jpeg)
                self.wfile.write(b"\r\n")
                self.wfile.flush()
        except OSError:
            # 浏览器刷新或关闭页面会断开长期连接，属于正常行为。
            return

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        path = urlparse(self.path).path
        if path == "/preview.mjpg":
            self._handle_mjpeg_stream()
            return
        if path == "/cmd":
            self._handle_command()
            return
        if path == "/latest.jpg":
            with self.server.preview_condition:  # type: ignore[attr-defined]
                preview_jpeg = self.server.preview_jpeg  # type: ignore[attr-defined]
            if preview_jpeg is not None:
                self._send_bytes(HTTPStatus.OK, "image/jpeg", preview_jpeg)
                return
            latest_path = self.output_dir / "latest_h264.jpg"
            if not latest_path.exists():
                latest_path = self.output_dir / "latest.jpg"
            if not latest_path.exists():
                self._send_bytes(HTTPStatus.NOT_FOUND, "text/plain", b"No image yet")
                return
            try:
                latest_data = latest_path.read_bytes()
            except PermissionError:
                self._send_bytes(HTTPStatus.SERVICE_UNAVAILABLE, "text/plain", b"Preview file is locked")
                return
            self._send_bytes(HTTPStatus.OK, "image/jpeg", latest_data)
            return
        if path in {"/health", "/status", "/h264"}:
            self._send_json(HTTPStatus.OK, self._stream_status())
            return
        if path != "/":
            self._send_bytes(HTTPStatus.NOT_FOUND, "text/plain", b"Not found")
            return

        html = """<!doctype html>
<html lang="zh-CN">
<head><meta charset="utf-8"><title>Lummiss H.264 实时流</title></head>
<body style="background:#111;color:#eee;text-align:center;font-family:sans-serif">
  <h1>ESP32-P4 H.264 实时回传</h1>
  <p id="status">等待视频帧</p>
  <img id="camera" src="/preview.mjpg" alt="等待 ESP32-P4 上传" style="max-width:95vw;border:1px solid #555">
  <p>
    <button id="video-toggle" onclick="toggleVideo()">关闭视频流</button>
    <button onclick="sendCmd('ping')">ping</button>
    <button onclick="sendCmd('status')">status</button>
    <span id="cmdreply"></span>
  </p>
  <script>
    const status = document.getElementById('status');
    const cmdreply = document.getElementById('cmdreply');
    const videoToggle = document.getElementById('video-toggle');
    let videoEnabled = true;
    function updateVideoButton() {
      videoToggle.textContent = videoEnabled ? '关闭视频流' : '开启视频流';
    }
    async function refreshStatus() {
      try {
        const data = await (await fetch('/status?t=' + Date.now())).json();
        if (typeof data.video_enabled === 'boolean') {
          videoEnabled = data.video_enabled;
          updateVideoButton();
        }
        const size = data.video_width ? `，${data.video_width}×${data.video_height}@${data.video_fps}fps` : '';
        const streamState = data.video_enabled ? '视频流开启' : '视频流关闭';
        status.textContent = `${streamState}｜接收 ${data.recent_fps} fps / ${data.recent_kbps} kbps${size}，网页预览 ${data.preview_fps} fps，预览丢帧 ${data.preview_dropped}，WebSocket 连接 ${data.ws_clients}`;
        if (data.last_command_reply) {
          cmdreply.textContent = `｜P4 回复：${data.last_command_reply}`;
        }
      } catch (_) { status.textContent = '服务器状态读取失败'; }
    }
    async function sendCmd(cmd) {
      try {
        const data = await (await fetch('/cmd?cmd=' + encodeURIComponent(cmd))).json();
        cmdreply.textContent = `｜已向 ${data.clients} 条连接下发 ${data.command}`;
        return data;
      } catch (_) { cmdreply.textContent = '｜命令下发失败'; }
    }
    async function toggleVideo() {
      videoToggle.disabled = true;
      const command = videoEnabled ? 'video_off' : 'video_on';
      const data = await sendCmd(command);
      if (data && data.clients > 0) {
        videoEnabled = command === 'video_on';
        updateVideoButton();
      }
      videoToggle.disabled = false;
    }
    updateVideoButton();
    refreshStatus(); setInterval(refreshStatus, 1000);
  </script>
</body></html>""".encode("utf-8")
        self._send_bytes(HTTPStatus.OK, "text/html; charset=utf-8", html)

    def log_message(self, fmt: str, *args: object) -> None:
        # 每帧 HTTP 访问日志会淹没统计信息，只输出 4xx/5xx 请求。
        status = str(args[1]) if len(args) > 1 else ""
        if status.startswith(("4", "5")):
            print(f"HTTP {self.client_address[0]} - {fmt % args}", flush=True)


def preview_worker(server: CameraHTTPServer) -> None:
    """持续解码新收到的 H.264 访问单元，并发布 MJPEG 预览帧。"""
    try:
        import av  # type: ignore[import-not-found]
        import cv2  # type: ignore[import-not-found]
    except ImportError:
        server.preview_enabled = False  # type: ignore[attr-defined]
        print(
            "缺少 PyAV 或 OpenCV：请运行 python -m pip install -r "
            "tools/camera_server_requirements.txt",
            flush=True,
        )
        return
    if hasattr(cv2, "setLogLevel"):
        cv2.setLogLevel(0)

    output_dir = server.output_dir  # type: ignore[attr-defined]
    output_path = output_dir / "latest_h264.jpg"
    output_tmp = output_dir / "latest_h264.tmp.jpg"
    decoder = av.CodecContext.create("h264", "r")
    last_disk_save = 0.0

    while True:
        encoded_frame, reset_decoder = server.preview_queue.get()  # type: ignore[attr-defined]
        if reset_decoder:
            decoder = av.CodecContext.create("h264", "r")

        try:
            decoded_frames = decoder.decode(av.Packet(encoded_frame))
        except Exception as error:  # PyAV 各版本的 FFmpeg 异常基类名称不同
            with server.preview_queue_lock:  # type: ignore[attr-defined]
                server.preview_wait_idr = True  # type: ignore[attr-defined]
            print(f"预览解码失败，等待下一个 IDR：{error}", flush=True)
            continue

        for decoded_frame in decoded_frames:
            bgr_frame = decoded_frame.to_ndarray(format="bgr24")
            encoded_ok, encoded = cv2.imencode(
                ".jpg",
                bgr_frame,
                [cv2.IMWRITE_JPEG_QUALITY, PREVIEW_JPEG_QUALITY],
            )
            if encoded_ok:
                preview_jpeg = encoded.tobytes()
                now = time.monotonic()
                # 条件变量唤醒所有网页客户端；慢客户端只会跳到最新帧。
                with server.preview_condition:  # type: ignore[attr-defined]
                    server.preview_jpeg = preview_jpeg  # type: ignore[attr-defined]
                    server.preview_sequence += 1  # type: ignore[attr-defined]
                    server.preview_frames += 1  # type: ignore[attr-defined]
                    server.preview_last_frame_time = now  # type: ignore[attr-defined]
                    server.preview_times.append(now)  # type: ignore[attr-defined]
                    server.preview_condition.notify_all()  # type: ignore[attr-defined]

                # 磁盘快照只作调试，每秒最多写一次；网页直接读取内存。
                if now - last_disk_save >= 1.0:
                    try:
                        output_tmp.write_bytes(preview_jpeg)
                        output_tmp.replace(output_path)
                    except PermissionError:
                        try:
                            output_tmp.unlink(missing_ok=True)
                        except PermissionError:
                            pass
                    last_disk_save = now


def main() -> None:
    parser = argparse.ArgumentParser(description="Lummiss ESP32-P4 H.264 接收服务器")
    parser.add_argument("--host", default="0.0.0.0", help="监听地址")
    parser.add_argument("--port", type=int, default=8000, help="监听端口")
    parser.add_argument(
        "--output",
        type=Path,
        default=Path(__file__).resolve().parent / "camera_captures",
        help="H.264 和预览图保存目录",
    )
    parser.add_argument(
        "--no-preview",
        action="store_true",
        help="不启动持久 H.264 解码和 MJPEG 预览（用于只测试接收与保存）",
    )
    parser.add_argument(
        "--ws-port",
        type=int,
        default=8001,
        help="P4 的 WebSocket 视频/命令通道端口（默认 8001）",
    )
    parser.add_argument(
        "--no-ws",
        action="store_true",
        help="不启动 WebSocket 服务端，只用 HTTP POST 回退通道接收",
    )
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    server = CameraHTTPServer((args.host, args.port), CameraRequestHandler)
    server.output_dir = args.output  # type: ignore[attr-defined]
    server.h264_path = args.output / f"camera_{timestamp}.h264"  # type: ignore[attr-defined]
    server.latest_h264_path = args.output / "latest_stream.h264"  # type: ignore[attr-defined]
    server.latest_h264_path.write_bytes(b"")  # type: ignore[attr-defined]
    server.stream_frames = 0  # type: ignore[attr-defined]
    server.stream_bytes = 0  # type: ignore[attr-defined]
    server.stream_started = time.monotonic()  # type: ignore[attr-defined]
    server.last_frame_time = 0.0  # type: ignore[attr-defined]
    server.last_report = time.monotonic()  # type: ignore[attr-defined]
    server.report_frames = 0  # type: ignore[attr-defined]
    server.report_bytes = 0  # type: ignore[attr-defined]
    server.recent_fps = 0.0  # type: ignore[attr-defined]
    server.recent_kbps = 0.0  # type: ignore[attr-defined]
    server.preview_jpeg = None  # type: ignore[attr-defined]
    server.preview_enabled = not args.no_preview  # type: ignore[attr-defined]
    server.preview_queue = queue.Queue(maxsize=PREVIEW_QUEUE_SIZE)  # type: ignore[attr-defined]
    server.preview_queue_lock = threading.Lock()  # type: ignore[attr-defined]
    server.preview_wait_idr = True  # type: ignore[attr-defined]
    server.preview_condition = threading.Condition()  # type: ignore[attr-defined]
    server.preview_sequence = 0  # type: ignore[attr-defined]
    server.preview_frames = 0  # type: ignore[attr-defined]
    server.preview_dropped = 0  # type: ignore[attr-defined]
    server.preview_last_frame_time = 0.0  # type: ignore[attr-defined]
    server.preview_times = deque(maxlen=60)  # type: ignore[attr-defined]

    # WebSocket 状态：已连接的 P4 集合、下行命令的最新回复、最近一帧的几何参数。
    server.ws_server = None  # type: ignore[attr-defined]
    server.ws_clients = set()  # type: ignore[attr-defined]
    server.ws_clients_lock = threading.Lock()  # type: ignore[attr-defined]
    server.ws_bad_frames = 0  # type: ignore[attr-defined]
    server.ws_width = 0  # type: ignore[attr-defined]
    server.ws_height = 0  # type: ignore[attr-defined]
    server.ws_fps = 0  # type: ignore[attr-defined]
    server.last_command_reply = None  # type: ignore[attr-defined]
    server.video_enabled = True  # type: ignore[attr-defined]

    if args.no_preview:
        print("预览解码线程已禁用（--no-preview）", flush=True)
    else:
        threading.Thread(target=preview_worker, args=(server,), daemon=True).start()

    if args.no_ws:
        print("WebSocket 服务端已禁用（--no-ws）", flush=True)
    else:
        start_ws_server(server, args.host, args.ws_port)

    print(f"HTTP 回退入口：http://{args.host}:{args.port}/h264", flush=True)
    print(f"状态与预览：http://127.0.0.1:{args.port}/", flush=True)
    print(f"下发命令：http://127.0.0.1:{args.port}/cmd?cmd=ping", flush=True)
    print(f"H.264 保存文件：{server.h264_path}", flush=True)  # type: ignore[attr-defined]
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n服务器已停止", flush=True)
    finally:
        if server.ws_server is not None:  # type: ignore[attr-defined]
            server.ws_server.shutdown()  # type: ignore[attr-defined]
        server.server_close()


if __name__ == "__main__":
    main()
