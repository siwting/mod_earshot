# 豆包 Seeduplex 全双工直连

本分支基于 2026-10-19 查询到的上游 main 最新提交 `c5847cce61fe8a1b9f4f6bdafcf034c816a3b449`（0.4.1），新增 `proto=doubao`。FreeSWITCH 直接连接豆包，无需桥接服务。此前部署锁定版本为 `ee10e1d8e1eeb06ce8089e84c10984efb8009d66`。

## 协议范围

- 接口：`wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue`。
- 模型：`1.2.6.1`，Seeduplex 3.0 JSON 文本事件协议，不适用于旧版二进制 S2S 接口。
- 鉴权：通过通道变量 `EARSHOT_DOUBAO_API_KEY` 设置 `X-Api-Key` 请求头；不使用 `EARSHOT_AUTH`，不添加 OpenAI 子协议。
- 上行：单声道 PCM16 小端，16 kHz，每包 20 ms / 640 字节；等待 `session.created` 后开始发送，包括自然静音。
- 下行：明确请求 `pcm_s16le`、24 kHz，自动重采样至电话通道采样率。豆包下行的 `pcm` 是 32 位格式，不能替代 `pcm_s16le`；本适配器不解码 Ogg Opus。
- 收音与播放并行。默认保留模块关闭本地 VAD 的行为，依据豆包 `conversation.item.input_audio_transcription.started` 清空播放队列并屏蔽上一响应的后续音频；新响应可继续播放。
- `flush` 同时发送 `response.cancel`；等取消确认前不播放新收到的音频。可选择开启本地 VAD/DTMF 打断，但通常无需开启 `vad_barge`。
- `pause` / PCI `mask` 在媒体回调中发出 `input_audio_mute.commit`，恢复时发送 `input_audio_unmute.commit`；暂停不丢弃已排队的下行音频。
- 重连清除旧发送队列，重新创建会话并携带此前返回的 `session.id`。不补发断线期间的语音。
- 服务端 5xx 协议错误触发重连，4xx/未知协议错误及 HTTP 4xx 握手错误停止重试。错误详情通过 ESL JSON 事件提供；日志仅记录数字错误码。
- 挂断时发送 `session.close`，最多等待 3 秒的 `session.closed` 确认，再释放连接。网络故障时超时退出。

## 最小接入

正式接入推荐设置 `EARSHOT_DOUBAO_API_KEY_FILE=/etc/callcenter/doubao-api-key`，由 FreeSWITCH 本机读取密钥，避免密钥进入 ESL 命令及通道事件。文件必须为普通文件（非符号链接），不可允许组/其他用户写入或其他用户读取；推荐 root:callcenter 0640，确保 FS 用户可读。仅支持一行 1–1024 字节可见 ASCII 密钥，允许结尾换行。文件读取失败不会回退。兼容旧的 `EARSHOT_DOUBAO_API_KEY` 输入，模块读取后立即清除，启动失败也清除；正式环境不要使用这个旧方式。传输层凭证副本用于重连，释放时擦除。随后在已接听的通话上执行：

```text
earshot <uuid> start wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue proto=doubao
```

拨号计划也可直接使用：

```xml
<action application="answer"/>
<action application="earshot" data="start wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue proto=doubao"/>
<action application="playback" data="silence_stream://-1"/>
```

默认音色 `zh_female_vv_jupiter_bigtts`。如需提示词、音色或工具声明，在启动前将完整 JSON 写入 `EARSHOT_SESSION_CONFIG`：

```json
{
  "type": "session.create",
  "session": {
    "model": "1.2.6.1",
    "instructions": "你是电话客服，请用简洁自然的中文回答。",
    "audio": {
      "input": {"format": {"type": "pcm", "rate": 16000}},
      "output": {
        "format": {"type": "pcm_s16le", "rate": 24000},
        "voice": "zh_female_vv_jupiter_bigtts"
      }
    }
  }
}
```

音频规格和模型字段必须保持上述值；缺少必填字段、格式不符或 JSON 无效会在连接前被拒绝。`extension` 和 `session.tools` 可按官方协议增加。`codec=`、`rate=` 对豆包会固定为 PCM16 / 16 kHz 上行。

## 事件与通话控制

订阅 `CUSTOM earshot::json earshot::transcript earshot::error`。`earshot::json` 携带通道 UUID、`corr`、`stream-id`，正文为豆包非音频 JSON，包括会话、文本回复、函数调用和用量信息；音频 Base64 不进入该事件。

```text
earshot <uuid> flush
earshot <uuid> pause
earshot <uuid> resume
earshot <uuid> send {"type":"speech_text_buffer.commit","speech_id":"greeting-1","text":"您好，请问有什么可以帮您？"}
earshot <uuid> stop
```

`send` 支持 `response.cancel`、`input_audio_buffer.commit`、`speech_text_buffer.commit`、`speech_text_buffer.replacement.append/commit`、`conversation.item.create/update/retrieve/delete`。`session.update` 必须传入包含上述模型和音频规格的完整配置；更新后的配置用于后续重连。

音频上传、会话创建/关闭和静音事件由模块管理，不能用 `send` 绕过状态检查。Function Calling 通过 `earshot::json` 交给现有业务控制器处理，再用 `conversation.item.create` 回传工具结果。模块不会自动把模型工具调用执行为 FreeSWITCH 命令。

## 部署集成

`deployment-modular` 和 `deployment-cluster` 使用 fork `siwting/mod_earshot` 的 `main`，以各部署包 `sources.lock.json` 中的提交和 SHA256 为准，校验源码后直接构建，无需本地豆包补丁。voiceswitch 文件凭证接入要求模块包含文件凭证、事件关联和会话状态查询改动；旧的 `a631b95` 不满足要求。

已有服务器应先在测试环境构建验证，再排空通话并使用对应部署包的 `03-freeswitch.sh --apply-config` 更新。该入口可能按构建记录重建 FreeSWITCH，不能当作仅热加载一个模块的命令。安装路径继续从 `pkg-config --variable=modulesdir freeswitch` 获取。`--check` 仅验证服务和模块加载，不代表豆包实呼验收。

源码发布和部署包更新不代表已部署到现网，也不自动添加实际 DID 路由。回退应恢复升级前的部署包及匹配模块，在排空通话后按原流程重新安装。

## 验证与边界

本地通过 7 组 C 单元/协议测试和 1 组真实 libwebsockets 本地对端测试（正常收发、分片重组、5xx 重连和 HTTP 401 停止重试）；AddressSanitizer / UndefinedBehaviorSanitizer 同样通过。测试使用真实 cJSON 和协议适配器，FreeSWITCH 重采样器由测试替身验证采样率方向与样本数量，不验证音质。

本地缺少 FreeSWITCH 开发环境，尚未完成 Debian 原生 `.so` 构建、加载、真实豆包凭证握手及电话端到端音质/时延验收。需要在测试服务器确认通话不回灌 AI 播放音频、真实插话打断、挂断释放和连续会话表现。

开发者可运行：

```bash
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
# 可选真实 WebSocket 本地对端测试（需要 libwebsockets 开发包）
python3 -m venv .venv
.venv/bin/pip install websockets==15.0.1
cmake -S . -B build -DEARSHOT_WS_TESTS=ON -DPython3_EXECUTABLE="$PWD/.venv/bin/python"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

官方依据（2026-10-19 核对）：[API 文档](https://docs.volcengine.com/docs/DoubaoVoice/endtoend-realtime-voice-full-duplex-version?lang=zh)、[接入必读](https://docs.volcengine.com/docs/DoubaoVoice/access-mustread?lang=zh)、API 页面提供的 `python3.7_duplex_demo.zip`。

## 后端状态对接补充

`earshot <uuid> status id=agent` 的 `session_ready` 表示豆包会话是否已经确认；`ready` 只是媒体播放门控，不能替代会话确认。错误/断连/ready/connected 事件带 `stream-id` 和 `corr`，后端可按每次启动的独立 corr 排除旧事件。
