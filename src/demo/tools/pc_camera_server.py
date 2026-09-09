#!/usr/bin/env python3
"""接收 ESP32-P4 上传的 H.264 实时帧，并保存裸码流及提供预览。"""

from __future__ import annotations

import argparse
import json
import queue
import shutil
import threading
import time
from collections import deque
from datetime import datetime
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse


MAX_JPEG_SIZE = 2 * 1024 * 1024
MAX_H264_FRAME_SIZE = 1024 * 1024
SAVE_LOCK = threading.Lock()
PREVIEW_QUEUE_SIZE = 16
PREVIEW_JPEG_QUALITY = 85
MJPEG_BOUNDARY = "lummiss-frame"


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
        frame = self._read_body(MAX_H264_FRAME_SIZE)
        if frame is None:
            return
        if not (frame.startswith(b"\x00\x00\x00\x01") or frame.startswith(b"\x00\x00\x01")):
            self._send_json(
                HTTPStatus.BAD_REQUEST,
                {"ok": False, "error": "not an Annex-B H.264 frame"},
            )
            return

        frame_type = self.headers.get("X-Frame-Type", "-1")
        sequence = self.headers.get("X-Frame-Sequence", "unknown")
        now = time.monotonic()
        with SAVE_LOCK:
            with self.server.h264_path.open("ab") as stream:  # type: ignore[attr-defined]
                stream.write(frame)
            with self.server.latest_h264_path.open("ab") as stream:  # type: ignore[attr-defined]
                stream.write(frame)

            if self.server.stream_frames == 0:  # type: ignore[attr-defined]
                # 统计从第一帧开始，避免服务器提前启动的等待时间拉低平均帧率。
                self.server.stream_started = now  # type: ignore[attr-defined]
                self.server.last_report = now  # type: ignore[attr-defined]
            self.server.stream_frames += 1  # type: ignore[attr-defined]
            self.server.stream_bytes += len(frame)  # type: ignore[attr-defined]
            self.server.last_frame_time = now  # type: ignore[attr-defined]
            frames = self.server.stream_frames  # type: ignore[attr-defined]
            total_bytes = self.server.stream_bytes  # type: ignore[attr-defined]
            last_report = self.server.last_report  # type: ignore[attr-defined]
            report = now - last_report >= 10.0
            if report:
                interval = max(now - last_report, 0.001)
                interval_frames = frames - self.server.report_frames  # type: ignore[attr-defined]
                interval_bytes = total_bytes - self.server.report_bytes  # type: ignore[attr-defined]
                self.server.recent_fps = interval_frames / interval  # type: ignore[attr-defined]
                self.server.recent_kbps = interval_bytes * 8 / interval / 1000  # type: ignore[attr-defined]
                self.server.report_frames = frames  # type: ignore[attr-defined]
                self.server.report_bytes = total_bytes  # type: ignore[attr-defined]
                self.server.last_report = now  # type: ignore[attr-defined]
                recent_fps = self.server.recent_fps  # type: ignore[attr-defined]
                recent_kbps = self.server.recent_kbps  # type: ignore[attr-defined]

        enqueue_preview_frame(self.server, frame, frame_type in {"0", "1"})
        if report:
            print(
                "H.264 接收："
                f"累计 {frames} 帧，最近 {recent_fps:.1f} fps，"
                f"{recent_kbps:.0f} kbps，"
                f"最新序号 {sequence}",
                flush=True,
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
        return {
            "ok": True,
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
  <script>
    const status = document.getElementById('status');
    async function refreshStatus() {
      try {
        const data = await (await fetch('/status?t=' + Date.now())).json();
        status.textContent = `接收 ${data.recent_fps} fps / ${data.recent_kbps} kbps，网页预览 ${data.preview_fps} fps，预览丢帧 ${data.preview_dropped}`;
      } catch (_) { status.textContent = '服务器状态读取失败'; }
    }
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

    if args.no_preview:
        print("预览解码线程已禁用（--no-preview）", flush=True)
    else:
        threading.Thread(target=preview_worker, args=(server,), daemon=True).start()
    print(f"监听：http://{args.host}:{args.port}/h264", flush=True)
    print(f"状态与预览：http://127.0.0.1:{args.port}/", flush=True)
    print(f"H.264 保存文件：{server.h264_path}", flush=True)  # type: ignore[attr-defined]
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n服务器已停止", flush=True)
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
