# -*- coding: utf-8 -*-
"""
test_onenet.py — 直接用 Python 验证 OneNET Studio REST API（已调通）。

鉴权要点（已验证）：
  - 密钥 = base64decode(产品 access_key)
  - res   = userid/{用户ID}（不是 products/...）
  - version = 2018-10-31
  - method  = md5
  - 签名   = base64( HMAC-md5(密钥, et+"\n"+method+"\n"+res+"\n"+version) )

运行：python test_onenet.py   （会查 dw/pm002 数据，并可选下发控制）
"""
import hashlib, hmac, base64, time, json
import os
import urllib.request, urllib.parse, urllib.error

PRODUCT_ID = os.environ["ONENET_PRODUCT_ID"]
USER_ID = os.environ["ONENET_USER_ID"]
ACCESS_KEY = os.environ["ONENET_ACCESS_KEY"]
API_HOST = 'https://iot-api.heclouds.com'
SIGN_METHOD = 'md5'
SIGN_VERSION = '2018-10-31'

DEV_DW = 'dw'
DEV_PM = 'pm002'


def make_auth():
    et = int(time.time()) + 24 * 3600
    res = 'userid/%s' % USER_ID
    msg = "%s\n%s\n%s\n%s" % (et, SIGN_METHOD, res, SIGN_VERSION)
    key = base64.b64decode(ACCESS_KEY)
    sig = base64.b64encode(hmac.new(key, msg.encode(), hashlib.md5).digest()).decode()
    return ("version=%s&res=%s&et=%s&method=%s&sign=%s" % (
        SIGN_VERSION,
        urllib.parse.quote(res, safe=''),
        et, SIGN_METHOD,
        urllib.parse.quote(sig, safe='')))


def _request(method, url, body=None):
    headers = {'Authorization': make_auth()}
    data = None
    if body is not None:
        headers['Content-Type'] = 'application/json'
        data = json.dumps(body).encode('utf-8')
    req = urllib.request.Request(url, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return json.loads(r.read().decode('utf-8'))
    except urllib.error.HTTPError as e:
        return {'_http_error': e.code, '_body': e.read().decode('utf-8', 'replace')}
    except Exception as e:
        return {'_exception': str(e)}


def query_device(device):
    url = '%s/thingmodel/query-device-property?product_id=%s&device_name=%s' % (
        API_HOST, PRODUCT_ID, device)
    return _request('GET', url)


def set_device(device, params):
    url = '%s/thingmodel/set-device-property' % API_HOST
    body = {'product_id': PRODUCT_ID, 'device_name': device, 'params': params}
    return _request('POST', url, body=body)


def query_history(device, identifier, start_time, end_time, limit=100, sort=1):
    """属性历史查询 (用于趋势图)。
    返回 data.list = [{time: 毫秒时间戳, value: "字符串"}], sort=1 正序, limit 实测可到 1000+。"""
    url = ('%s/thingmodel/query-device-property-history'
           '?product_id=%s&device_name=%s&identifier=%s'
           '&start_time=%d&end_time=%d&limit=%d&sort=%d'
           % (API_HOST, PRODUCT_ID, device, identifier, start_time, end_time, limit, sort))
    return _request('GET', url)


if __name__ == '__main__':
    print('=== query dw ===')
    print(json.dumps(query_device(DEV_DW), ensure_ascii=False, indent=2))
    print('\n=== query pm002 ===')
    print(json.dumps(query_device(DEV_PM), ensure_ascii=False, indent=2))

    print('\n=== PM2.5 历史 (最近 1 小时) ===')
    end = int(time.time() * 1000)
    start = end - 3600 * 1000
    hist = query_history(DEV_PM, 'PM', start, end, limit=50)
    lst = (hist.get('data') or {}).get('list') or [] if isinstance(hist, dict) else []
    print('points:', len(lst))
    print(json.dumps(lst[:5], ensure_ascii=False, indent=2))

    # 下发控制测试（取消注释即可）
    # print('\n=== set LED1 = true ===')
    # print(json.dumps(set_device(DEV_DW, {'LED1': True}), ensure_ascii=False, indent=2))
