#!/usr/bin/env python3
"""接收 ESP32-P4 上传的 H.264 实时帧，并保存裸码流及提供预览。"""

from __future__ import annotations

import argparse
import json
import shutil
import threading
import time
from datetime import datetime
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse


MAX_JPEG_SIZE = 2 * 1024 * 1024
MAX_H264_FRAME_SIZE = 1024 * 1024
SAVE_LOCK = threading.Lock()
PREVIEW_EVENT = threading.Event()


class CameraRequestHandler(BaseHTTPRequestHandler):
    """接收 H.264 帧；保留旧 JPEG 接口用于兼容性检查。"""

    server_version = "LummissCameraServer/2.0"
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

            # ESP H.264 的 0/1 分别代表 IDR/I 帧；从关键帧开始重建可独立解码的 GOP。
            gop_mode = "wb" if frame_type in {"0", "1"} else "ab"
            with self.server.gop_path.open(gop_mode) as stream:  # type: ignore[attr-defined]
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

        PREVIEW_EVENT.set()
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
        }

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        path = urlparse(self.path).path
        if path == "/latest.jpg":
            with SAVE_LOCK:
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
  <img id="camera" alt="等待 ESP32-P4 上传" style="max-width:95vw;border:1px solid #555">
  <script>
    const image = document.getElementById('camera');
    const status = document.getElementById('status');
    async function refresh() {
      image.src = '/latest.jpg?t=' + Date.now();
      try {
        const data = await (await fetch('/status?t=' + Date.now())).json();
        status.textContent = `已收 ${data.frames} 帧，最近 ${data.recent_fps} fps，${data.recent_kbps} kbps`;
      } catch (_) { status.textContent = '服务器状态读取失败'; }
    }
    refresh(); setInterval(refresh, 500);
  </script>
</body></html>""".encode("utf-8")
        self._send_bytes(HTTPStatus.OK, "text/html; charset=utf-8", html)

    def log_message(self, fmt: str, *args: object) -> None:
        # 每帧 HTTP 访问日志会淹没统计信息，只输出 4xx/5xx 请求。
        status = str(args[1]) if len(args) > 1 else ""
        if status.startswith(("4", "5")):
            print(f"HTTP {self.client_address[0]} - {fmt % args}", flush=True)


def preview_worker(server: ThreadingHTTPServer) -> None:
    """低频解码当前 GOP，生成浏览器预览；失败不影响 H.264 原始流保存。"""
    try:
        import cv2  # type: ignore[import-not-found]
    except ImportError:
        print("未安装 OpenCV：仍会保存 H.264，浏览器暂不显示画面", flush=True)
        return
    if hasattr(cv2, "setLogLevel"):
        cv2.setLogLevel(0)

    output_dir = server.output_dir  # type: ignore[attr-defined]
    gop_path = output_dir / "current_gop.h264"
    decode_path = output_dir / "preview_decode.h264"
    output_path = output_dir / "latest_h264.jpg"
    output_tmp = output_dir / "latest_h264.tmp.jpg"
    last_decode = 0.0
    while True:
        PREVIEW_EVENT.wait()
        PREVIEW_EVENT.clear()
        delay = 0.5 - (time.monotonic() - last_decode)
        if delay > 0:
            time.sleep(delay)
        with SAVE_LOCK:
            if not gop_path.exists() or gop_path.stat().st_size < 64:
                continue
            shutil.copyfile(gop_path, decode_path)

        capture = cv2.VideoCapture(str(decode_path))
        latest_frame = None
        while True:
            ok, frame = capture.read()
            if not ok:
                break
            latest_frame = frame
        capture.release()
        if latest_frame is not None:
            encoded_ok, encoded = cv2.imencode(".jpg", latest_frame)
            if encoded_ok:
                preview_jpeg = encoded.tobytes()
                # 浏览器从内存取最新图，即使 Windows 看图程序锁住磁盘文件也能继续更新。
                with SAVE_LOCK:
                    server.preview_jpeg = preview_jpeg  # type: ignore[attr-defined]
                try:
                    output_tmp.write_bytes(preview_jpeg)
                    output_tmp.replace(output_path)
                except PermissionError:
                    try:
                        output_tmp.unlink(missing_ok=True)
                    except PermissionError:
                        pass
        last_decode = time.monotonic()


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
        help="不启动 cv2 预览解码线程（用于确认该线程的 GIL/IO 是否拖慢 HTTP 上传）",
    )
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    server = ThreadingHTTPServer((args.host, args.port), CameraRequestHandler)
    server.output_dir = args.output  # type: ignore[attr-defined]
    server.h264_path = args.output / f"camera_{timestamp}.h264"  # type: ignore[attr-defined]
    server.latest_h264_path = args.output / "latest_stream.h264"  # type: ignore[attr-defined]
    server.latest_h264_path.write_bytes(b"")  # type: ignore[attr-defined]
    server.gop_path = args.output / "current_gop.h264"  # type: ignore[attr-defined]
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
