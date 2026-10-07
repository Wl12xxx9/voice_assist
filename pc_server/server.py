from fastapi import FastAPI, Request
from pydantic import BaseModel
import base64
import logging
import uuid
import time
import base64
import numpy as np
from funasr import AutoModel
import re
from fastapi import Response
import json
import threading
import io
import hashlib
import hmac
from datetime import datetime
import wave
import os
import zlib


# 首次运行会自动下载SenseVoiceSmall模型(~1.2G)，耐心等待
asr_model = AutoModel(model="iic/SenseVoiceSmall", device="cpu")
logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")


# =========配置区 修改为你自己的密钥=========
VOLC_ACCESS_KEY = "AKLTNzNjZDY4MDkzZTU4NDc2MzljN2QyODFiZTUxOWI5YmU"
VOLC_SECRET_KEY = "WXpGaFlUSm1NR00yWkdZeE5HRTFZVGswTW1KalpqYzRNek00WmpReVpqUQ=="
# VOLC_ACCESS_KEY = "AKLTMWMyMDk1NGY4NzlkNDQwYmJlOGQzOWIwM2UwOTE2MjEy"
# VOLC_SECRET_KEY = "WW1Nd05EY3daRGxoTkRZNE5HUXdOemcyTURrd01qVmpZVEkwTUdObE1tRQ=="
VOLC_ASR_APP_ID = "4639957539"
VOLC_ASR_ACCESS_TOKEN = "fNnlrOiBgcbauh0as7XB95WDfmvwIj7u"
VOLC_X_API_KEY = "f2f3d1a0-b206-4dfd-9493-4d44675a4aba"
VOLC_ASR_RESOURCE_ID = "volc.seedasr.auc"
DOUBAO_API_KEY = "ark-86d3c4c3-ce15-4186-aa97-e4034d33d396-7b7c8"
DOUBAO_URL = "https://ark.cn-beijing.volces.com/api/v3/chat/completions"
# DOUBAO_MODEL = "doubao-seed-2.0-lite"
DOUBAO_MODEL = "ep-20260905193629-lvd8v"
# =========================================

# ============百度TTS配置区============
BAIDU_API_KEY = "hkSWny7VCbS49Z807Uj6Z1BK"
BAIDU_SECRET_KEY = "tonLyObT9eyBQckIWXMA4S6CBUtMQNAK"

baidu_access_token = None
baidu_token_expire = 0

def get_baidu_token():
    """获取access_token，内存缓存，过期自动刷新"""
    global baidu_access_token, baidu_token_expire
    now = int(time.time())
    if baidu_access_token is not None and now < baidu_token_expire - 120:
        return baidu_access_token
    try:
        resp = requests.post(
            "https://aip.baidubce.com/oauth/2.0/token",
            data={
                "grant_type":"client_credentials",
                "client_id":BAIDU_API_KEY,
                "client_secret":BAIDU_SECRET_KEY
            },
            timeout=15
        )
        js = resp.json()
        token = js.get("access_token")
        expires_in = js.get("expires_in",2592000)
        if not token:
            logging.error(f"获取百度token失败:{js}")
            return None
        baidu_access_token = token
        baidu_token_expire = now + expires_in
        return token
    except Exception as e:
        logging.exception("获取百度token异常")
        return None
# =====================================

app = FastAPI(max_request_size=3 * 1024 * 1024)

from starlette.middleware.gzip import GZipMiddleware
from starlette.config import Config
from starlette.datastructures import CommaSeparatedStrings

# =========全局请求中间件，所有http请求都会打印日志，方便排查不知道程序跑到哪的问题========
@app.middleware("http")
async def log_all_request(request: Request, call_next):
    start = time.time()
    logging.info(f"【收到HTTP请求】 method={request.method} url={request.url.path}")
    resp = await call_next(request)
    cost_ms = (time.time() - start)*1000
    logging.info(f"【HTTP响应完成】 status={resp.status_code}, cost={cost_ms:.1f} ms")
    return resp
# =====================================================================================

import requests

#接收ESP32上传JSON格式
class AudioReq(BaseModel):
    # audio_b64: str
    asr_text: str

# 内存缓存 task_id
task_cache = {}
task_pcm_cache = dict() 

class SubmitAudioReq(BaseModel):
    audio_b64: str

class QueryTaskReq(BaseModel):
    task_id: str

def background_asr_task(audio_b64: str, tid: str):
    task_result = {"status":"pending", "text":"", "msg":"", "ts":time.time()}
    try:
        pcm_bytes = base64.b64decode(audio_b64)
        pcm_np = np.frombuffer(pcm_bytes, dtype=np.int16)
        pcm_float = pcm_np.astype(np.float32) / 32768.0
        res = asr_model.generate(input=pcm_float, fs=16000, language="zh")
        raw_text = res[0]["text"].strip()
        text = re.sub(r"<\|.*?\|>", "", raw_text).strip()
        logging.info(f"【本地ASR识别结果】: {text}")
        task_result["status"] = "success"
        task_result["text"] = text
    except Exception as e:
        logging.error(f"本地ASR异常:{e}")
        task_result["status"] = "error"
        task_result["msg"] = str(e)
    task_cache[tid] = task_result


def submit_audio(audio_b64: str):
    tid = str(uuid.uuid4())
    # 先标记任务pending，后台线程去做识别
    # task_cache[tid] = {"status":"pending", "text":"", "msg":""}
    task_cache[tid] = {"status":"pending", "text":"", "msg":"", "ts":time.time()}
    th = threading.Thread(target=background_asr_task, args=(audio_b64, tid), daemon=True)
    th.start()
    logging.info(f"[DEBUG] 已经启动ASR后台线程 tid={tid}")
    return tid

def query_task(task_id: str):
    """本地模式直接读缓存, 永远不要返回None"""
    if task_id not in task_cache:
        return {"status":"error","msg":"task不存在","text":""}
    return task_cache[task_id]

def background_asr_from_bytes(pcm_bytes:bytes, tid:str):
    task_result = {"status":"pending", "text":"", "msg":"", "ts":time.time()}
    try:
        pcm_np = np.frombuffer(pcm_bytes, dtype=np.int16)
        pcm_float = pcm_np.astype(np.float32) / 32768.0
        res = asr_model.generate(input=pcm_float, fs=16000, language="zh")
        raw_text = res[0]["text"].strip()
        text = re.sub(r"<\|.*?\|>", "", raw_text).strip()
        logging.info(f"【本地ASR识别结果】：{text}")
        task_result["status"] = "success"
        task_result["text"] = text
    except Exception as e:
        logging.error(f"本地ASR异常:{e}")
        task_result["status"] = "error"
        task_result["msg"] = str(e)
    task_cache[tid] = task_result

@app.post("/audio_submit")
def audio_submit(req: SubmitAudioReq):
    tid = submit_audio(req.audio_b64)
    if tid is not None:
        return Response(content=json.dumps({"task_id": tid}), media_type="application/json")
    return Response(content=json.dumps({"status":"error","msg":"火山submit调用失败","task_id":""}), media_type="application/json")

@app.post("/audio_query")
async def audio_query(req: QueryTaskReq):
    now = time.time()
    #清理30秒过期任务
    del_keys = [k for k,v in task_cache.items() if v.get("ts",0)+30 < now]
    for k in del_keys:
        del task_cache[k]
        if k in task_pcm_cache:
            del task_pcm_cache[k]
    task_id = req.task_id
    if task_id not in task_cache:
        payload = {"status":"error","msg":"task不存在","text":""}
    else:
        payload = task_cache[task_id]
    # 强制输出固定content-length，禁用chunked
    return Response(content=json.dumps(payload), media_type="application/json")

@app.post("/audio_infer")
async def audio_infer(req: AudioReq):
    asr_text = req.asr_text
    logging.info(f"【audio_infer接口被调用】asr_text={asr_text}")

    if not asr_text:
        logging.warning("asr_text为空, 返回err:asr_empty")
        return {"asr_text":"","llm_text":"","err":"asr_empty"}

    #2.调用豆包LLM
    payload = {
        "model": DOUBAO_MODEL,
        "messages": [{"role":"user","content": asr_text}]
    }
    headers = {
        "Content-Type":"application/json",
        "Authorization": f"Bearer {DOUBAO_API_KEY}"
    }
    llm_ret = requests.post(DOUBAO_URL, json=payload, headers=headers, timeout=12)
    llm_text = ""
    if llm_ret.status_code == 200:
        j = llm_ret.json()
        llm_text = j["choices"][0]["message"]["content"]
        logging.info(f"LLM调用成功,返回内容：|{llm_text}|")
    else:
        logging.error(f"LLM请求失败 code={llm_ret.status_code}, resp={llm_ret.text}")
    logging.info(f"接口返回给ESP32 asr_text={asr_text}, llm_text={llm_text}")
    return {"asr_text": asr_text, "llm_text": llm_text, "err":""}

@app.post("/upload_pcm_bin")
async def upload_pcm_bin(request: Request):
    tid = str(uuid.uuid4())
    raw_pcm = await request.body()
    task_pcm_cache[tid] = raw_pcm
    task_cache[tid] = {"status":"pending", "text":"", "msg":"", "ts":time.time()}
    th = threading.Thread(target=background_asr_from_bytes, args=(raw_pcm, tid), daemon=True)
    th.start()
    return Response(content=json.dumps({"task_id":tid}), media_type="application/json")

import edge_tts
import io
import wave
import numpy as np
from scipy import signal

# @app.post("/tts_text2pcm")
# async def tts_text2pcm(req: dict):
#     """
#     输入json {"text":"xxx"}
#     返回裸pcm二进制：16000Hz 16bit 单声道 little-endian，不带wav头部
#     """
#     text = req.get("text","")
#     if not text:
#         return Response(content=b"", status_code=400)
#     try:
#         # 注意：语音名称横杠必须是英文短横杠
#         communicate = edge_tts.Communicate(text, voice="zh-CN-XiaoxiaoNeural", rate="+0%")
#         audio_chunks = []
#         # 只收集type=="audio"，丢弃WordBoundary时间戳元数据
#         async for chunk in communicate.stream():
#             if chunk["type"] == "audio":
#                 audio_chunks.append(chunk["data"])
#         if not audio_chunks:
#             logging.warning("edge-tts没有返回音频数据")
#             return Response(content=b"", status_code=200)
#         wav_bytes = b"".join(audio_chunks)

#         # --- wav_bytes 是完整合法wav文件 ---
#         wav_io = io.BytesIO(wav_bytes)
#         with wave.open(wav_io,"rb") as wf:
#             n_channels = wf.getnchannels()
#             sampwidth = wf.getsampwidth()
#             framerate = wf.getframerate()
#             frames = wf.readframes(wf.getnframes())
#             audio_np = np.frombuffer(frames, dtype=np.int16)

#         # 转单声道
#         if n_channels == 2:
#             audio_np = audio_np.reshape(-1, 2).mean(axis=1).astype(np.int16)

#         # edge-tts默认输出24000Hz，重采样到16000Hz
#         orig_sr = framerate
#         target_sr = 16000
#         import scipy.signal
#         num = int(len(audio_np) * target_sr / orig_sr)
#         resampled = scipy.signal.resample(audio_np, num).astype(np.int16)
#         pcm_raw = resampled.tobytes()

#         return Response(content=pcm_raw, media_type="application/octet-stream")

#     except Exception as e:
#         logging.exception("TTS合成异常")
#         return Response(content=b"", status_code=200)

# 测试成功代码，🈲删
# @app.post("/tts_text2pcm")
# async def tts_text2pcm(req: dict):
#     import math
#     import base64
#     text = req.get("text","")
#     sample_rate = 16000
#     duration = 2
#     volume = 12000
#     freq = 440
#     samples = sample_rate * duration
#     buf = []
#     for i in range(samples):
#         v = int(volume * math.sin(2*math.pi*freq*i/sample_rate))
#         buf.append(v.to_bytes(2, byteorder='little', signed=True))
#     pcm_bytes = b"".join(buf)
#     # 二进制转为base64字符串，放入json返回，彻底规避chunked二进制bug
#     b64_str = base64.b64encode(pcm_bytes).decode('ascii')
#     return {"pcm_b64": b64_str}
# 测试成功代码，🈲删

def volc_signature(access_key: str, secret_key: str, method: str, path: str, params: dict, body: str):
    """火山OpenAPI v3签名"""
    service = "speech_sv"
    region = "cn-north-2"
    now = datetime.utcnow()
    date_utc = now.strftime("%Y%m%dT%H%M%SZ")
    date_short = now.strftime("%Y%m%d")
    host = "speech-sv.volcengineapi.com"

    # canonical query
    sorted_q = sorted(params.items())
    cano_query = "&".join([f"{k}={v}" for k, v in sorted_q])

    # hash body
    body_bytes = body.encode("utf-8") if body else b""
    body_sha256 = hashlib.sha256(body_bytes).hexdigest()

    cano_req = "\n".join([
        method,
        path,
        cano_query,
        f"host:{host}",
        f"x-date:{date_utc}",
        "",
        "host;x-date",
        body_sha256
    ])
    cano_req_sha256 = hashlib.sha256(cano_req.encode("utf-8")).hexdigest()

    k_date = hmac.new(secret_key.encode("utf-8"), date_short.encode("utf-8"), hashlib.sha256).digest()
    k_region = hmac.new(k_date, region.encode("utf-8"), hashlib.sha256).digest()
    k_service = hmac.new(k_region, service.encode("utf-8"), hashlib.sha256).digest()
    k_signing = hmac.new(k_service, b"request", hashlib.sha256).digest()

    sig = hmac.new(k_signing, cano_req_sha256.encode("utf-8"), hashlib.sha256).hexdigest()
    auth = f"HMAC-SHA256 Credential={access_key}/{date_short}/{region}/{service}/request, SignedHeaders=host;x-date, Signature={sig}"
    headers = {
        "Host": host,
        "X-Date": date_utc,
        "Authorization": auth,
        "Content-Type": "application/json"
    }
    return headers

# @app.post("/tts_text2pcm")
# async def tts_text2pcm(req: dict):
#     import base64
#     text = req.get("text", "")
#     if not text:
#         return {"pcm_b64": ""}

#     host = "speech-sv.volcengineapi.com"
#     path = "/"
#     params = {
#         "Action": "SynthesisSpeech",
#         "Version": "2024-01-01"
#     }
#     payload = {
#         "App": {
#             "AppId": VOLC_ASR_APP_ID
#         },
#         "Audio": {
#             "AudioFormat": "pcm",
#             "SampleRate": 16000,
#             "BitDepth": 16
#         },
#         "Speaker": "zh_female_qingxin",
#         "Text": text
#     }
#     import json
#     body_json = json.dumps(payload, ensure_ascii=False)
#     try:
#         headers = volc_signature(VOLC_ACCESS_KEY, VOLC_SECRET_KEY, "POST", path, params, body_json)
#         import requests
#         resp = requests.post(f"https://{host}{path}", params=params, headers=headers, data=body_json.encode("utf-8"), timeout=15)
#         if resp.status_code != 200:
#             logging.error(f"火山TTS http code={resp.status_code}, resp={resp.text}")
#             return {"pcm_b64": ""}
#         j_resp = resp.json()
#         if j_resp.get("ResponseMetadata", {}).get("Error"):
#             logging.error(f"火山TTS业务错误:{j_resp}")
#             return {"pcm_b64": ""}
#         # 获取返回的base64音频，火山返回pcm的base64字符串
#         audio_b64 = j_resp["Result"]["AudioData"]
#         # 直接透传返回给ESP32，不需要再解码再编码
#         return {"pcm_b64": audio_b64}
#     except Exception as e:
#         logging.exception("火山TTS调用异常")
#         return {"pcm_b64": ""}

@app.post("/tts_text2pcm")
async def tts_text2pcm(req: dict):
    text = req.get("text","")
    if not text.strip():
        return {"pcm_b64": ""}

    MAX_TTS_TEXT_LEN = 40
    if len(text) > MAX_TTS_TEXT_LEN:
        text = text[:MAX_TTS_TEXT_LEN]
        logging.warning(f"TTS文本超长，已截断到{MAX_TTS_TEXT_LEN}字符，截断后text=[{text}]")

    token = get_baidu_token()
    if not token:
        return {"pcm_b64": ""}
    try:
        tts_url = "https://tsn.baidu.com/text2audio"
        payload = {
            "tex": text,
            "tok": token,
            "cuid":"esp32_voice_assist",
            "ctp":1,
            "lan":"zh",
            "spd":5,
            "pit":5,
            "vol":6,
            "per":0,     # per=0度小美免费基础音色，不要填其他数字，避免扣费
            "aue":4      # aue=4 输出pcm‑16k裸流，关键参数
        }
        resp = requests.post(tts_url, data=payload, timeout=20)
        content_type = resp.headers.get("Content-Type","")
        if "audio" not in content_type.lower():
            logging.error(f"百度TTS返回错误:{resp.text}")
            return {"pcm_b64":""}
        pcm_bytes = resp.content

        # =========新增计算校验信息========
        pcm_len = len(pcm_bytes)
        crc_val = zlib.crc32(pcm_bytes)   # 计算原始pcm的crc32
        crc_val = crc_val & 0xFFFFFFFF
        b64_str = base64.b64encode(pcm_bytes).decode("ascii")

        ret_json = {
            "pcm_b64": b64_str,
            "pcm_len": pcm_len,
            "crc32": crc_val
        }
        # =================================

        # =========新增：把裸PCM保存为可直接播放的wav文件=========
        def save_pcm_to_wav(out_path:str, raw_pcm:bytes):
            wf = wave.open(out_path, "wb")
            wf.setnchannels(1)      #单声道
            wf.setsampwidth(2)      #16bit
            wf.setframerate(16000)  #16000hz
            wf.writeframes(raw_pcm)
            wf.close()

        import time
        fname = f"tts_out_{int(time.time())}.wav"
        # save_pcm_to_wav(fname, pcm_bytes)
        logging.info(f"✅TTS音频已保存到: {os.path.abspath(fname)}")
        # ======================================================

        # b64_str = base64.b64encode(pcm_bytes).decode("ascii")
        # return {"pcm_b64": b64_str}
        return ret_json
    except Exception as e:
        logging.exception("百度TTS调用异常")
        # return {"pcm_b64": ""}
        return ret_json



if __name__ == "__main__":
    # host="0.0.0.0" 允许局域网其他设备访问，不要写127.0.0.1！
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000)
