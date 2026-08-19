# Ditto HTTP API

Ditto 启动后会在本机启动一个简单的 HTTP 服务：

- 地址：`http://127.0.0.1:23456/`
- 网页：显示最近 5 条剪贴板文本，并可提交新文本
- `GET /api/latest`：获取最新一条剪贴板数据
- `POST /api/clipboard`：请求体直接发送 UTF-8 文本，写入 Windows 剪贴板；Ditto 会按原有监听流程自动收录

示例：

```powershell
curl http://127.0.0.1:23456/api/latest
curl -X POST http://127.0.0.1:23456/api/clipboard -H "Content-Type: text/plain" --data-binary "hello"
```

默认只监听 `127.0.0.1`，不会直接暴露到局域网；单次 POST 最大 1 MiB。
