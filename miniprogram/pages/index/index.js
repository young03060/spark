var onenet = require('../../utils/onenet.js');

/* ── 空气质量等级（国标 HJ 633-2012 PM2.5 分级，按瞬时读数近似） ──
 * color: 可读文字/线色；line: 图表 AQI 参考线用色；bg: 徽章底色 */
function aqiLevel(pm) {
  if (pm === null || pm === undefined || isNaN(pm)) return null;
  if (pm <= 35)  return { label: '优',     color: '#00a84b', line: '#00e400', bg: '#e6f7ee' };
  if (pm <= 75)  return { label: '良',     color: '#c99900', line: '#e6c700', bg: '#fdf6e3' };
  if (pm <= 115) return { label: '轻度污染', color: '#f07000', line: '#ff7e00', bg: '#fff0e0' };
  if (pm <= 150) return { label: '中度污染', color: '#e02020', line: '#ff0000', bg: '#fdeaea' };
  if (pm <= 250) return { label: '重度污染', color: '#99004c', line: '#99004c', bg: '#f7e6f0' };
  return { label: '严重污染', color: '#7e0023', line: '#7e0023', bg: '#f4e6ea' };
}

/* ── 室内舒适度（用板子自身温湿度算，无需外部天气 API） ── */
function comfortLevel(tem, hum) {
  if (tem === null || tem === undefined || isNaN(tem)) return null;
  var hasHum = (hum !== null && hum !== undefined && !isNaN(hum));
  if (tem < 16)  return { label: '偏冷', emoji: '❄️', color: '#38bdf8', tip: '温度偏低，注意保暖' };
  if (tem < 18)  return { label: '微凉', emoji: '☁️', color: '#60a5fa', tip: '体感偏凉' };
  if (tem <= 26) {
    if (hasHum && hum < 40) return { label: '干燥', emoji: '🌵', color: '#f59e0b', tip: '湿度偏低，注意补水' };
    if (hasHum && hum > 70) return { label: '潮湿', emoji: '💧', color: '#0ea5e9', tip: '湿度偏高，注意防潮' };
    return { label: '舒适', emoji: '😊', color: '#16a34a', tip: '温湿度适宜' };
  }
  if (tem <= 30)  return { label: '微热', emoji: '☀️', color: '#f59e0b', tip: '体感偏暖' };
  if (tem <= 35)  return { label: '偏热', emoji: '🌡️', color: '#f97316', tip: '注意防暑降温' };
  return { label: '炎热', emoji: '🔥', color: '#dc2626', tip: '高温，注意防暑' };
}

/* AQI 分界阈值 + 参考线色（用于趋势图横向虚线） */
var AQI_BANDS = [
  { value: 35,  color: '#00e400' },
  { value: 75,  color: '#e6c700' },
  { value: 115, color: '#ff7e00' },
  { value: 150, color: '#ff0000' },
  { value: 250, color: '#99004c' }
];

/* 时间范围选项：bucketMs=聚合桶宽(均值)，refreshMs=该范围的历史自动刷新间隔 */
var RANGES = {
  '1h':  { ms: 1 * 3600 * 1000,      bucketMs: 30 * 1000,       refreshMs: 60 * 1000 },
  '6h':  { ms: 6 * 3600 * 1000,      bucketMs: 2 * 60 * 1000,   refreshMs: 3 * 60 * 1000 },
  '24h': { ms: 24 * 3600 * 1000,     bucketMs: 10 * 60 * 1000,  refreshMs: 5 * 60 * 1000 }
};

function hexToRgba(hex, alpha) {
  var h = String(hex).replace('#', '');
  if (h.length === 3) h = h[0] + h[0] + h[1] + h[1] + h[2] + h[2];
  var r = parseInt(h.substring(0, 2), 16);
  var g = parseInt(h.substring(2, 4), 16);
  var b = parseInt(h.substring(4, 6), 16);
  return 'rgba(' + r + ',' + g + ',' + b + ',' + alpha + ')';
}

/* 把原始点按时间桶聚合成均值，得到覆盖全窗口的平滑序列 */
function aggregate(list, start, bucketMs) {
  if (!list || !list.length) return list;
  var buckets = {};
  var keys = [];
  list.forEach(function (p) {
    var k = Math.floor((p.time - start) / bucketMs);
    if (k < 0) k = 0;
    if (!buckets[k]) { buckets[k] = { sum: 0, n: 0, t: 0 }; keys.push(k); }
    buckets[k].sum += p.value;
    buckets[k].n++;
    if (p.time > buckets[k].t) buckets[k].t = p.time;
  });
  keys.sort(function (a, b) { return a - b; });
  return keys.map(function (k) {
    var b = buckets[k];
    return { time: b.t, value: Math.round(b.sum / b.n) };
  });
}

/* 把毫秒跨度格式化成「X 分钟」/「X.X 小时」 */
function fmtSpan(ms) {
  var min = Math.round(ms / 60000);
  if (min < 1) return '不足 1 分钟';
  if (min < 60) return min + ' 分钟';
  return (Math.round(min / 6) / 10) + ' 小时';
}

/* 设备在线判定：最近一次上报在 30s 内视为在线（板子 5s 上报一次） */
var ONLINE_TIMEOUT_MS = 30000;

Page({
  data: {
    tem: '--',
    hum: '--',
    pm: '--',
    pmLevel: null,          // { label, color, line, bg } 或 null
    comfort: null,          // { label, emoji, color, tip } 或 null
    online: null,           // true=在线 / false=离线 / null=检测中
    led1: false,
    buzzer1: false,
    alarmTh: 70,
    loading: true,
    lastError: '',
    ranges: [
      { key: '1h',  label: '1 小时' },
      { key: '6h',  label: '6 小时' },
      { key: '24h', label: '24 小时' }
    ],
    historyRange: '1h',
    historyHint: '加载中…'
  },

  onLoad: function () {
    this.refresh();
    this.timer = setInterval(this.refresh.bind(this), 3000);
    // 历史趋势按所选范围定时刷新（长窗口拉取更重，间隔相应拉长）
    this.histTimer = setInterval(this._tickHistory.bind(this), 30000);
  },

  onReady: function () {
    this.loadHistory();
  },

  onUnload: function () {
    if (this.timer) clearInterval(this.timer);
    if (this.histTimer) clearInterval(this.histTimer);
  },

  refresh: function () {
    var that = this;
    onenet.fetchAll().then(function (data) {
      var upd = { loading: false, lastError: '' };
      var temNum = null, humNum = null;
      if (data.tem !== undefined && data.tem !== null) { temNum = Number(data.tem); upd.tem = temNum.toFixed(1); }
      if (data.hum !== undefined && data.hum !== null) { humNum = Number(data.hum); upd.hum = Math.round(humNum); }
      if (data.PM !== undefined && data.PM !== null) {
        upd.pm = Math.round(data.PM);
        upd.pmLevel = aqiLevel(upd.pm);
      }
      upd.comfort = comfortLevel(temNum, humNum);
      // 最近 30s 内有上报 → 在线
      upd.online = data.__lastTime ? (Date.now() - data.__lastTime < ONLINE_TIMEOUT_MS) : false;
      // 若平台回传了控制状态，同步本地（板子每 5s 上报一次真实状态）
      if (data.LED1 !== undefined && data.LED1 !== null) upd.led1 = !!data.LED1;
      if (data.buzzer1 !== undefined && data.buzzer1 !== null) upd.buzzer1 = !!data.buzzer1;
      // 拖动滑杆期间不回写，避免 3s 轮询把拖到一半的值拽回旧值
      if (!that._draggingAlarm && data.alarm_th !== undefined && data.alarm_th !== null) {
        upd.alarmTh = Math.round(data.alarm_th);
      }
      that.setData(upd);
    }).catch(function (err) {
      that.setData({ loading: false, lastError: '读取失败：' + (err && err.errMsg || err) });
    });
  },

  /* ───────────────────── 历史趋势 ───────────────────── */

  onRangeChange: function (e) {
    var range = e.currentTarget.dataset.range;
    if (!RANGES[range] || range === this.data.historyRange) return;
    this.setData({ historyRange: range, historyHint: '加载中…' });
    this.loadHistory();
  },

  _tickHistory: function () {
    var range = RANGES[this.data.historyRange] || RANGES['1h'];
    if (!this._lastHistFetch || Date.now() - this._lastHistFetch >= range.refreshMs) {
      this.loadHistory();
    }
  },

  loadHistory: function () {
    var that = this;
    var range = RANGES[this.data.historyRange] || RANGES['1h'];
    var end = Date.now();
    var start = end - range.ms;
    this._lastHistFetch = Date.now();
    onenet.queryDevicePropertyHistoryRange(onenet.DEV_PM, 'PM', start, end)
      .then(function (list) {
        that.drawChart(aggregate(list, start, range.bucketMs));
      })
      .catch(function (err) {
        console.error('history fail', err);
        that.setData({ historyHint: '趋势加载失败' });
      });
  },

  drawChart: function (list) {
    if (!list || !list.length) {
      this.setData({ historyHint: '暂无历史数据，等待设备上报…' });
      return;
    }
    // 提示实际覆盖范围 + 采样点数（list 是聚合后的点）
    var spanMs = list[list.length - 1].time - list[0].time;
    this.setData({
      historyHint: '实际覆盖 ' + fmtSpan(spanMs) + ' · ' + list.length + ' 个采样点'
    });

    // 降采样到最多 ~160 点，减轻 canvas 绘制负担
    var pts = list;
    if (list.length > 160) {
      var step = list.length / 160;
      pts = [];
      for (var i = 0; i < list.length; i += step) pts.push(list[Math.floor(i)]);
      if (pts[pts.length - 1] !== list[list.length - 1]) pts.push(list[list.length - 1]);
    }

    var query = wx.createSelectorQuery().in(this);
    query.select('#pmChart').fields({ node: true, size: true }).exec(function (res) {
      if (!res || !res[0] || !res[0].node || !res[0].width) return;
      var canvas = res[0].node;
      var ctx = canvas.getContext('2d');
      var info = wx.getSystemInfoSync();
      var dpr = (info && info.pixelRatio) || 2;
      var w = res[0].width;
      var h = res[0].height;
      canvas.width = w * dpr;
      canvas.height = h * dpr;
      ctx.scale(dpr, dpr);

      var padL = 34, padR = 12, padT = 12, padB = 26;
      var plotW = w - padL - padR;
      var plotH = h - padT - padB;

      var values = pts.map(function (p) { return p.value; });
      var vMax = Math.max.apply(null, values);
      var yMax = Math.max(vMax, 150) * 1.1;
      var yMin = 0;

      function y(v) { return padT + plotH * (1 - (v - yMin) / (yMax - yMin)); }
      function x(i) { return padL + plotW * (i / (pts.length - 1)); }

      ctx.clearRect(0, 0, w, h);
      ctx.font = '10px sans-serif';

      // AQI 参考线（虚线）+ 阈值标签
      AQI_BANDS.forEach(function (band) {
        if (band.value > yMax) return;
        var yy = y(band.value);
        ctx.globalAlpha = 0.35;
        ctx.strokeStyle = band.color;
        ctx.setLineDash([4, 4]);
        ctx.beginPath();
        ctx.moveTo(padL, yy);
        ctx.lineTo(padL + plotW, yy);
        ctx.stroke();
        ctx.setLineDash([]);
        ctx.globalAlpha = 1;
        ctx.fillStyle = band.color;
        ctx.textAlign = 'right';
        ctx.fillText(String(band.value), padL - 4, yy + 3);
      });

      // Y=0 基线
      ctx.strokeStyle = '#d9dce1';
      ctx.beginPath();
      ctx.moveTo(padL, y(0));
      ctx.lineTo(padL + plotW, y(0));
      ctx.stroke();

      // 折线 + 渐变填充（颜色取最新值所属等级）
      if (pts.length >= 1) {
        var level = aqiLevel(pts[pts.length - 1].value) || { color: '#1aad19' };

        if (pts.length >= 2) {
          ctx.strokeStyle = level.color;
          ctx.lineWidth = 2;
          ctx.beginPath();
          pts.forEach(function (p, i) {
            if (i === 0) ctx.moveTo(x(i), y(p.value));
            else ctx.lineTo(x(i), y(p.value));
          });
          ctx.stroke();

          var grad = ctx.createLinearGradient(0, padT, 0, padT + plotH);
          grad.addColorStop(0, hexToRgba(level.color, 0.35));
          grad.addColorStop(1, hexToRgba(level.color, 0.03));
          ctx.fillStyle = grad;
          ctx.lineTo(x(pts.length - 1), y(0));
          ctx.lineTo(x(0), y(0));
          ctx.closePath();
          ctx.fill();
        }

        // 最新点圆点
        ctx.fillStyle = level.color;
        ctx.beginPath();
        ctx.arc(x(pts.length - 1), y(pts[pts.length - 1].value), 3, 0, 2 * Math.PI);
        ctx.fill();
      }

      // X 轴时间标签（首/中/尾）
      function tLabel(ts) {
        var d = new Date(ts);
        return ('0' + d.getHours()).slice(-2) + ':' + ('0' + d.getMinutes()).slice(-2);
      }
      ctx.fillStyle = '#8a9099';
      ctx.textAlign = 'center';
      ctx.fillText(tLabel(pts[0].time), padL, padT + plotH + 16);
      ctx.fillText(tLabel(pts[Math.floor(pts.length / 2)].time), padL + plotW / 2, padT + plotH + 16);
      ctx.fillText(tLabel(pts[pts.length - 1].time), padL + plotW, padT + plotH + 16);
    });
  },

  /* ───────────────────── 设备控制 ───────────────────── */

  /* 灯开关 */
  onLedChange: function (e) {
    var val = e.detail.value;
    this.setData({ led1: val });
    this.sendControl({ LED1: val });
  },

  /* 蜂鸣器开关 */
  onBuzzerChange: function (e) {
    var val = e.detail.value;
    this.setData({ buzzer1: val });
    this.sendControl({ buzzer1: val });
  },

  /* 报警阈值滑杆：拖动中只更新显示 */
  onAlarmChanging: function (e) {
    this._draggingAlarm = true;
    this.setData({ alarmTh: e.detail.value });
  },

  /* 报警阈值滑杆：松手后下发 */
  onAlarmChange: function (e) {
    this._draggingAlarm = false;
    var val = e.detail.value;
    this.setData({ alarmTh: val });
    this.sendControl({ alarm_th: val });
  },

  sendControl: function (params) {
    var that = this;
    onenet.setDeviceProperty(onenet.DEV_DW, params).then(function (res) {
      console.log('控制已下发', params, res);
      wx.showToast({ title: '已下发', icon: 'success', duration: 800 });
    }).catch(function (err) {
      console.error('控制失败', err);
      wx.showToast({ title: '下发失败', icon: 'none', duration: 1500 });
      that.refresh();
    });
  }
});
