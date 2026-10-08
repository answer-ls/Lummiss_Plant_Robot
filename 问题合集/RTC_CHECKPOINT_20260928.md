# RTC推流工作点

ALWAYSINTERNAL=0，RESERVE_INTERNAL=200000。两构建已通过；实机前三轮H264成功，首轮推流79秒。第四轮设备持续发送但浏览器无画面，JPEG/RST错误待查。

第三方依赖及实际sdkconfig另存本地logs/rtc_checkpoint_20260928，不包含在Git恢复范围内；恢复此工作点需结合该快照。

```json
{
  "managed_components_archive": "logs\\rtc_checkpoint_20260928\\managed_components.zip",
  "sha256": "58f6bf46a90c02be0cd239c0d18dad1da2e853314869db4bdbcf512888dda360",
  "note": "本地依赖快照；源码工作点包含RTC可推流，但JPEG错误和第四轮黑屏尚未解决。未宣称端到端稳定。"
}
```
