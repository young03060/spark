/**
 * onenet.js — OneNET Studio (新一代) REST API 封装
 *
 * 小程序直连 OneNET：用产品级 access_key 做安全鉴权（HMAC-MD5 签名），
 * 读设备最新属性 / 下发属性设置。
 *
 * 依赖 utils/crypto.js 提供的 MD5/HMAC-MD5/Base64。
 */

var crypto = require('./crypto.js');
var config = require('./onenet.config.js');

/* ───────────────────── 配置（改成你自己的） ───────────────────── */

var PRODUCT_ID = config.PRODUCT_ID;
var USER_ID = config.USER_ID;
// 产品级 access_key（设备接入页查看），签名时需 base64 解码
var ACCESS_KEY = config.ACCESS_KEY;
// 两个设备：dw = 温湿度 + 控制(LED1/buzzer1/alarm_th)，pm002 = PM2.5
var DEV_DW = config.DEV_DW;
var DEV_PM = config.DEV_PM;
var API_HOST = 'https://iot-api.heclouds.com';
var SIGN_METHOD = 'md5';          // 签名算法：md5
var SIGN_VERSION = '2018-10-31';

/* ───────────────────── 签名 ───────────────────── */

/**
 * 生成 Authorization 头
 * @param {string} accessKey 产品 access_key（base64 形式）
 * @param {number} et 过期时间戳（秒，unix）
 * @param {string} method 签名算法 md5/sha1/sha256
 * @param {string} res 访问资源，如 products/{pid}/devices/{dev}
 */
function buildAuthorization(accessKey, et, method, res) {
  // StringForSignature = et + '\n' + method + '\n' + res + '\n' + version
  var msg = et + '\n' + method + '\n' + res + '\n' + SIGN_VERSION;
  // access_key 需先 base64 解码再作为 HMAC 密钥
  var key = crypto.base64ToBytes(accessKey);
  var msgBytes = crypto.utf8ToBytes(msg);
  var sigBytes = crypto.hmacMd5(key, msgBytes);
  var sign = crypto.bytesToBase64(sigBytes);

  return 'version=' + SIGN_VERSION +
    '&res=' + encodeURIComponent(res) +
    '&et=' + et +
    '&method=' + method +
    '&sign=' + encodeURIComponent(sign);
}

/** 生成当前请求的 Authorization（et = 当前时间 + 24 小时） */
function makeAuth(res) {
  var et = Math.floor(Date.now() / 1000) + 24 * 3600;
  return buildAuthorization(ACCESS_KEY, et, SIGN_METHOD, res);
}

/* ───────────────────── 读：查询设备最新属性 ───────────────────── */

/**
 * 查询设备最新属性
 * @param {string} deviceName 设备名
 * @returns Promise，resolve 为解析后的属性对象 { tem:30.2, hum:69, PM:40, LED1:true, ... }
 */
function queryDeviceProperty(deviceName) {
  var res = 'userid/' + USER_ID;
  var url = API_HOST + '/thingmodel/query-device-property?product_id=' + PRODUCT_ID +
    '&device_name=' + deviceName;

  return new Promise(function (resolve, reject) {
    wx.request({
      url: url,
      method: 'GET',
      header: {
        'Authorization': makeAuth(res)
      },
      success: function (r) {
        resolve(parseProperties(r.data));
      },
      fail: function (err) {
        reject(err);
      }
    });
  });
}

/**
 * 把 query-device-property 的返回解析成 { identifier: value } 形式。
 * 兼容数组 / 对象两种返回结构，方便后续按真实响应微调。
 */
function parseProperties(body) {
  var result = {};
  var lastTime = 0;
  if (!body || body.code !== 0) return result;
  var data = body.data;
  if (!Array.isArray(data)) return result;

  // data 是 [{identifier, value, time, data_type, ...}, ...]
  // 只有上报过的属性才有 value 字段；value 是字符串，需转成数字/bool
  data.forEach(function (it) {
    if (!it || it.identifier === undefined) return;
    if (it.value === undefined || it.value === null) return;   // 未上报，跳过
    var v = it.value;
    if (typeof v === 'string') {
      if (v === 'true') v = true;
      else if (v === 'false') v = false;
      else if (v !== '' && !isNaN(Number(v))) v = Number(v);
    }
    result[it.identifier] = v;
    // 记录最近一次上报时间，用于判断设备在线/离线
    var t = Number(it.time);
    if (it.time !== undefined && it.time !== null && !isNaN(t) && t > lastTime) lastTime = t;
  });
  result.__lastTime = lastTime;   // 该设备最近一次上报时间（毫秒），0 = 从未上报
  return result;
}

/* ───────────────────── 读：查询属性历史（趋势图） ───────────────────── */

/**
 * 查询设备属性历史数据。
 * 实测返回 data.list = [{ time: 毫秒时间戳, value: "字符串数值" }, ...]，
 * 默认倒序（新→旧）；加 sort=1 返回正序（旧→新），limit 实测可到 1000+。
 *
 * @param {string} deviceName 设备名（PM 在 pm002）
 * @param {string} identifier 属性标识，如 "PM"
 * @param {number} startTime 起始毫秒时间戳
 * @param {number} endTime   结束毫秒时间戳
 * @param {number} limit     记录数
 * @returns Promise，resolve 为按时间正序的 [{ time, value }]
 */
function queryDevicePropertyHistory(deviceName, identifier, startTime, endTime, limit) {
  var res = 'userid/' + USER_ID;
  var url = API_HOST + '/thingmodel/query-device-property-history' +
    '?product_id=' + PRODUCT_ID +
    '&device_name=' + deviceName +
    '&identifier=' + encodeURIComponent(identifier) +
    '&start_time=' + startTime +
    '&end_time=' + endTime +
    '&limit=' + (limit || 100) +
    '&sort=1';

  return new Promise(function (resolve, reject) {
    wx.request({
      url: url,
      method: 'GET',
      header: { 'Authorization': makeAuth(res) },
      success: function (r) {
        resolve(parseHistory(r.data));
      },
      fail: function (err) {
        reject(err);
      }
    });
  });
}

/** 解析历史返回 → 正序 [{ time: number, value: number }] */
function parseHistory(body) {
  var out = [];
  if (!body || body.code !== 0 || !body.data || !Array.isArray(body.data.list)) return out;
  body.data.list.forEach(function (it) {
    if (!it || it.time === undefined || it.value === undefined) return;
    var v = it.value;
    if (typeof v === 'string') v = Number(v);
    if (isNaN(v)) return;
    out.push({ time: Number(it.time), value: v });
  });
  out.sort(function (a, b) { return a.time - b.time; });
  return out;
}

/**
 * 分页拉取 start~end 区间内全部原始点（每次 limit=1000），返回正序 [{time,value}]。
 * 用于长窗口趋势图：历史接口 limit 硬上限 1000 且不支持服务端聚合，
 * 只能分页取全量原始点、再由调用方按时间桶聚合成均值。
 *
 * @param {string} deviceName
 * @param {string} identifier
 * @param {number} startTime 毫秒
 * @param {number} endTime   毫秒
 * @returns Promise，resolve 为正序 [{time, value}]（去重合并各页）
 */
function queryDevicePropertyHistoryRange(deviceName, identifier, startTime, endTime) {
  var PAGE_SIZE = 1000;
  var MAX_POINTS = 20000;   // ≈27h @5s，覆盖 24h 窗口
  var res = 'userid/' + USER_ID;

  function fetchPage(offset) {
    var url = API_HOST + '/thingmodel/query-device-property-history' +
      '?product_id=' + PRODUCT_ID +
      '&device_name=' + deviceName +
      '&identifier=' + encodeURIComponent(identifier) +
      '&start_time=' + startTime +
      '&end_time=' + endTime +
      '&limit=' + PAGE_SIZE +
      '&offset=' + offset +
      '&sort=1';
    return new Promise(function (resolve, reject) {
      wx.request({
        url: url,
        method: 'GET',
        header: { 'Authorization': makeAuth(res) },
        success: function (r) { resolve(parseHistory(r.data)); },
        fail: function (err) { reject(err); }
      });
    });
  }

  // 顺序分页：某页不满 PAGE_SIZE 即到末尾，停止；兜底上限 MAX_POINTS。
  var all = [];
  function next(offset) {
    if (offset >= MAX_POINTS) return Promise.resolve(all);
    return fetchPage(offset).then(function (list) {
      all = all.concat(list);
      if (list.length >= PAGE_SIZE) return next(offset + PAGE_SIZE);
      return all;
    });
  }
  return next(0);
}

/* ───────────────────── 写：下发属性设置 ───────────────────── */

/**
 * 设置设备属性（下发控制到设备）
 * @param {string} deviceName 设备名
 * @param {object} params 要设置的属性，如 { LED1: true } 或 { alarm_th: 75 }
 * @returns Promise
 */
function setDeviceProperty(deviceName, params) {
  var res = 'userid/' + USER_ID;
  var url = API_HOST + '/thingmodel/set-device-property';
  var body = {
    product_id: PRODUCT_ID,
    device_name: deviceName,
    params: params
  };

  return new Promise(function (resolve, reject) {
    wx.request({
      url: url,
      method: 'POST',
      data: body,
      header: {
        'Content-Type': 'application/json',
        'Authorization': makeAuth(res)
      },
      success: function (r) {
        resolve(r.data);
      },
      fail: function (err) {
        reject(err);
      }
    });
  });
}

/* ───────────────────── 业务封装 ───────────────────── */

/** 读取全部数据：dw(温湿度+控制状态) + pm002(PM2.5)，合并成一个对象 */
function fetchAll() {
  return new Promise(function (resolve, reject) {
    var dw = null, pm = null;
    var done = 0;
    function finish(err) {
      if (err) return reject(err);
      done++;
      if (done === 2) {
        var merged = {};
        if (dw) Object.assign(merged, dw);
        if (pm) Object.assign(merged, pm);
        // 两台设备的最近上报时间取最大值
        merged.__lastTime = Math.max(dw && dw.__lastTime || 0, pm && pm.__lastTime || 0);
        resolve(merged);
      }
    }
    queryDeviceProperty(DEV_DW).then(function (d) { dw = d; finish(); }, finish);
    queryDeviceProperty(DEV_PM).then(function (d) { pm = d; finish(); }, finish);
  });
}

module.exports = {
  PRODUCT_ID: PRODUCT_ID,
  ACCESS_KEY: ACCESS_KEY,
  DEV_DW: DEV_DW,
  DEV_PM: DEV_PM,
  buildAuthorization: buildAuthorization,
  queryDeviceProperty: queryDeviceProperty,
  queryDevicePropertyHistory: queryDevicePropertyHistory,
  queryDevicePropertyHistoryRange: queryDevicePropertyHistoryRange,
  setDeviceProperty: setDeviceProperty,
  fetchAll: fetchAll
};
