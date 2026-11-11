/**
 * crypto.js — 微信小程序环境下的纯 JS 加密工具
 *
 * 小程序没有 Node 的 crypto 模块，这里手写 MD5 / HMAC-MD5 / Base64。
 * 供 OneNET Studio REST API 的 access_key 安全鉴权签名使用。
 */

/* ───────────────────────── Base64 ───────────────────────── */

var B64_CHARS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

/** 字节数组 -> Base64 字符串 */
function bytesToBase64(bytes) {
  var out = '';
  var i = 0;
  var len = bytes.length;
  while (i < len) {
    var b1 = bytes[i++];
    var b2 = i < len ? bytes[i++] : -1;
    var b3 = i < len ? bytes[i++] : -1;
    var n = (b1 << 16) | ((b2 === -1 ? 0 : b2) << 8) | (b3 === -1 ? 0 : b3);
    out += B64_CHARS[(n >> 18) & 63];
    out += B64_CHARS[(n >> 12) & 63];
    out += b2 === -1 ? '=' : B64_CHARS[(n >> 6) & 63];
    out += b3 === -1 ? '=' : B64_CHARS[n & 63];
  }
  return out;
}

/** Base64 字符串 -> 字节数组 */
function base64ToBytes(str) {
  str = String(str).replace(/[^A-Za-z0-9+/=]/g, '');
  var out = [];
  var i = 0;
  while (i < str.length) {
    var c1 = B64_CHARS.indexOf(str.charAt(i++));
    var c2 = B64_CHARS.indexOf(str.charAt(i++));
    var c3 = B64_CHARS.indexOf(str.charAt(i++));
    var c4 = B64_CHARS.indexOf(str.charAt(i++));
    var n = (c1 << 18) | (c2 << 12) | ((c3 & 63) << 6) | (c4 & 63);
    out.push((n >> 16) & 255);
    if (c3 !== -1) out.push((n >> 8) & 255);   // c3 不是 '='（'=' 的 indexOf 返回 -1）
    if (c4 !== -1) out.push(n & 255);          // c4 不是 '='
  }
  return out;
}

/** UTF-8 字符串 -> 字节数组 */
function utf8ToBytes(str) {
  var bytes = [];
  str = String(str);
  for (var i = 0; i < str.length; i++) {
    var c = str.charCodeAt(i);
    if (c < 0x80) {
      bytes.push(c);
    } else if (c < 0x800) {
      bytes.push(0xc0 | (c >> 6), 0x80 | (c & 0x3f));
    } else if (c < 0x10000) {
      bytes.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f));
    } else {
      bytes.push(0xf0 | (c >> 18), 0x80 | ((c >> 12) & 0x3f), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f));
    }
  }
  return bytes;
}

/** 字节数组 -> 十六进制字符串 */
function bytesToHex(bytes) {
  var s = '';
  for (var i = 0; i < bytes.length; i++) {
    s += (bytes[i] < 16 ? '0' : '') + bytes[i].toString(16);
  }
  return s;
}

/* ───────────────────────── MD5 ───────────────────────── */

/**
 * 字节数组的 MD5，返回 16 字节数组。
 * 采用 blueimp-md5 的经典结构（带符号常量 + 32 位无符号加法）。
 */
function md5Bytes(bytes) {
  function add32(a, b) { return (a + b) & 0xFFFFFFFF; }
  function cmn(q, a, b, x, s, t) {
    a = add32(add32(a, q), add32(x, t));
    return add32((a << s) | (a >>> (32 - s)), b);
  }
  function ff(a, b, c, d, x, s, t) { return cmn((b & c) | ((~b) & d), a, b, x, s, t); }
  function gg(a, b, c, d, x, s, t) { return cmn((b & d) | (c & (~d)), a, b, x, s, t); }
  function hh(a, b, c, d, x, s, t) { return cmn(b ^ c ^ d, a, b, x, s, t); }
  function ii(a, b, c, d, x, s, t) { return cmn(c ^ (b | (~d)), a, b, x, s, t); }

  // 填充
  var length = bytes.length;
  var padded = bytes.slice();
  padded.push(0x80);
  while (padded.length % 64 !== 56) padded.push(0);
  var bitLen = length * 8;            // 我们的输入都很小，bitLen < 2^32
  var lo = bitLen >>> 0;
  var hi = (length > 268435455) ? 0 : 0;   // 高 32 位恒为 0（消息 < 2^29 字节）
  for (var k = 0; k < 4; k++) padded.push((lo >>> (8 * k)) & 0xff);
  for (var k = 0; k < 4; k++) padded.push((hi >>> (8 * k)) & 0xff);

  var a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;

  for (var i = 0; i < padded.length; i += 64) {
    var M = new Array(16);
    for (var j = 0; j < 16; j++) {
      M[j] = padded[i + 4 * j] |
        (padded[i + 4 * j + 1] << 8) |
        (padded[i + 4 * j + 2] << 16) |
        (padded[i + 4 * j + 3] << 24);
    }
    var aa = a, bb = b, cc = c, dd = d;

    /* Round 1 */
    a = ff(a, b, c, d, M[0], 7, -680876936);
    d = ff(d, a, b, c, M[1], 12, -389564586);
    c = ff(c, d, a, b, M[2], 17, 606105819);
    b = ff(b, c, d, a, M[3], 22, -1044525330);
    a = ff(a, b, c, d, M[4], 7, -176418897);
    d = ff(d, a, b, c, M[5], 12, 1200080426);
    c = ff(c, d, a, b, M[6], 17, -1473231341);
    b = ff(b, c, d, a, M[7], 22, -45705983);
    a = ff(a, b, c, d, M[8], 7, 1770035416);
    d = ff(d, a, b, c, M[9], 12, -1958414417);
    c = ff(c, d, a, b, M[10], 17, -42063);
    b = ff(b, c, d, a, M[11], 22, -1990404162);
    a = ff(a, b, c, d, M[12], 7, 1804603682);
    d = ff(d, a, b, c, M[13], 12, -40341101);
    c = ff(c, d, a, b, M[14], 17, -1502002290);
    b = ff(b, c, d, a, M[15], 22, 1236535329);

    /* Round 2 */
    a = gg(a, b, c, d, M[1], 5, -165796510);
    d = gg(d, a, b, c, M[6], 9, -1069501632);
    c = gg(c, d, a, b, M[11], 14, 643717713);
    b = gg(b, c, d, a, M[0], 20, -373897302);
    a = gg(a, b, c, d, M[5], 5, -701558691);
    d = gg(d, a, b, c, M[10], 9, 38016083);
    c = gg(c, d, a, b, M[15], 14, -660478335);
    b = gg(b, c, d, a, M[4], 20, -405537848);
    a = gg(a, b, c, d, M[9], 5, 568446438);
    d = gg(d, a, b, c, M[14], 9, -1019803690);
    c = gg(c, d, a, b, M[3], 14, -187363961);
    b = gg(b, c, d, a, M[8], 20, 1163531501);
    a = gg(a, b, c, d, M[13], 5, -1444681467);
    d = gg(d, a, b, c, M[2], 9, -51403784);
    c = gg(c, d, a, b, M[7], 14, 1735328473);
    b = gg(b, c, d, a, M[12], 20, -1926607734);

    /* Round 3 */
    a = hh(a, b, c, d, M[5], 4, -378558);
    d = hh(d, a, b, c, M[8], 11, -2022574463);
    c = hh(c, d, a, b, M[11], 16, 1839030562);
    b = hh(b, c, d, a, M[14], 23, -35309556);
    a = hh(a, b, c, d, M[1], 4, -1530992060);
    d = hh(d, a, b, c, M[4], 11, 1272893353);
    c = hh(c, d, a, b, M[7], 16, -155497632);
    b = hh(b, c, d, a, M[10], 23, -1094730640);
    a = hh(a, b, c, d, M[13], 4, 681279174);
    d = hh(d, a, b, c, M[0], 11, -358537222);
    c = hh(c, d, a, b, M[3], 16, -722521979);
    b = hh(b, c, d, a, M[6], 23, 76029189);
    a = hh(a, b, c, d, M[9], 4, -640364487);
    d = hh(d, a, b, c, M[12], 11, -421815835);
    c = hh(c, d, a, b, M[15], 16, 530742520);
    b = hh(b, c, d, a, M[2], 23, -995338651);

    /* Round 4 */
    a = ii(a, b, c, d, M[0], 6, -198630844);
    d = ii(d, a, b, c, M[7], 10, 1126891415);
    c = ii(c, d, a, b, M[14], 15, -1416354905);
    b = ii(b, c, d, a, M[5], 21, -57434055);
    a = ii(a, b, c, d, M[12], 6, 1700485571);
    d = ii(d, a, b, c, M[3], 10, -1894986606);
    c = ii(c, d, a, b, M[10], 15, -1051523);
    b = ii(b, c, d, a, M[1], 21, -2054922799);
    a = ii(a, b, c, d, M[8], 6, 1873313359);
    d = ii(d, a, b, c, M[15], 10, -30611744);
    c = ii(c, d, a, b, M[6], 15, -1560198380);
    b = ii(b, c, d, a, M[13], 21, 1309151649);
    a = ii(a, b, c, d, M[4], 6, -145523070);
    d = ii(d, a, b, c, M[11], 10, -1120210379);
    c = ii(c, d, a, b, M[2], 15, 718787259);
    b = ii(b, c, d, a, M[9], 21, -343485551);

    a = add32(a, aa); b = add32(b, bb); c = add32(c, cc); d = add32(d, dd);
  }

  var out = [];
  [a, b, c, d].forEach(function (w) {
    out.push(w & 0xff, (w >>> 8) & 0xff, (w >>> 16) & 0xff, (w >>> 24) & 0xff);
  });
  return out;
}

/** 字符串（UTF-8）的 MD5 十六进制 */
function md5Hex(str) {
  return bytesToHex(md5Bytes(utf8ToBytes(String(str))));
}

/* ───────────────────────── HMAC-MD5 ───────────────────────── */

/**
 * HMAC-MD5
 * @param keyBytes 密钥字节数组（即 base64 解码后的 access_key）
 * @param msgBytes 消息字节数组
 * @returns 16 字节数组
 */
function hmacMd5(keyBytes, msgBytes) {
  var blockSize = 64;
  var key = keyBytes.slice();
  if (key.length > blockSize) {
    key = md5Bytes(key);
  }
  while (key.length < blockSize) key.push(0);

  var iPad = [], oPad = [];
  for (var i = 0; i < blockSize; i++) {
    iPad.push(key[i] ^ 0x36);
    oPad.push(key[i] ^ 0x5c);
  }
  var inner = md5Bytes(iPad.concat(msgBytes));
  return md5Bytes(oPad.concat(inner));
}

/* ───────────────────────── 导出 ───────────────────────── */

module.exports = {
  bytesToBase64: bytesToBase64,
  base64ToBytes: base64ToBytes,
  utf8ToBytes: utf8ToBytes,
  bytesToHex: bytesToHex,
  md5Bytes: md5Bytes,
  md5Hex: md5Hex,
  hmacMd5: hmacMd5
};
