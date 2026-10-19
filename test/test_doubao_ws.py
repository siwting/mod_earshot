"""Local Seeduplex peer: validates real LWS framing, auth, ack gating and recovery.
Requires websockets==15.0.1; never contacts Doubao or uses a real credential.
"""
import asyncio
import base64
from http import HTTPStatus
import json
import sys
from websockets.legacy.server import serve

AUDIO = base64.b64encode(b'\x34\x12' * 480).decode()

async def case(driver, mode):
    attempts = 0
    completed = asyncio.Event()
    failures = []

    async def reject(path, headers):
        nonlocal attempts
        attempts += 1
        return HTTPStatus.UNAUTHORIZED, [], b'local test rejection'

    async def handler(ws, path):
        nonlocal attempts
        attempts += 1
        try:
            assert ws.request_headers['X-Api-Key'] == 'local-test-key'
            assert 'Authorization' not in ws.request_headers
            first = json.loads(await ws.recv())
            assert first['type'] == 'session.create', first
            assert first['session']['model'] == '1.2.6.1'
            assert first['session']['audio']['input']['format'] == {'type': 'pcm', 'rate': 16000}
            assert first['session']['audio']['output']['format'] == {'type': 'pcm_s16le', 'rate': 24000}
            if attempts > 1:
                assert first['session']['id'] == 'local-dialog'
            try:
                extra = await asyncio.wait_for(ws.recv(), .15)
                raise AssertionError(f'audio before session.created: {extra}')
            except asyncio.TimeoutError:
                pass
            await ws.send(json.dumps({'type': 'session.created', 'session': {'id': 'local-dialog'}}))
            frame = json.loads(await ws.recv())
            assert frame['type'] == 'input_audio_buffer.append'
            assert base64.b64decode(frame['audio']) == b'\x34\x12' * 320
            if mode == 'retry' and attempts == 1:
                await ws.send(json.dumps({'type': 'error', 'error': {'code': 55000001}}))
                await ws.wait_closed()
                return
            # Fragment one text message across WebSocket fragments, preserving one JSON event.
            message = json.dumps({'type': 'response.output_audio.delta', 'response_id': 'r1', 'delta': AUDIO})
            await ws.send([message[:47], message[47:]])
            await ws.send(json.dumps({'type': 'conversation.item.input_audio_transcription.started'}))
            await ws.send(message)  # stale r1 tail must not be played
            await ws.send(json.dumps({'type': 'response.output_audio.delta', 'response_id': 'r2', 'delta': AUDIO}))
            await ws.send(json.dumps({'type': 'response.done'}))
            while True:
                frame = json.loads(await ws.recv())
                if frame['type'] == 'session.close':
                    await ws.send(json.dumps({'type': 'session.closed'}))
                    completed.set()
                    await ws.wait_closed()
                    return
                assert frame['type'] == 'input_audio_buffer.append'
        except Exception as exc:
            failures.append(repr(exc))
            completed.set()

    async with serve(handler, '127.0.0.1', 0, process_request=reject if mode == 'reject' else None) as server:
        port = server.sockets[0].getsockname()[1]
        process = await asyncio.create_subprocess_exec(driver, f'ws://127.0.0.1:{port}/duplex', mode,
                                                      stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
        try:
            stdout, stderr = await asyncio.wait_for(process.communicate(), 15)
        except asyncio.TimeoutError:
            process.kill()
            await process.wait()
            raise AssertionError(f'{mode}: driver timed out')
        assert process.returncode == 0, stderr.decode()
        assert not failures, failures
        assert attempts == (2 if mode == 'retry' else 1), attempts
        assert mode == 'reject' or completed.is_set()
        print(stderr.decode().strip())

async def main():
    for mode in ('normal', 'retry', 'reject'):
        await case(sys.argv[1], mode)

asyncio.run(main())
