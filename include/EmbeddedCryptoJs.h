#ifndef EMBEDDED_CRYPTO_JS_H
#define EMBEDDED_CRYPTO_JS_H

#include <Arduino.h>

const char EMBEDDED_CRYPTO_JS[] PROGMEM = R"CRYPTOJS__(
/**
 * Minimal AES-CBC Crypto Library for ESP8266 Portal
 * Stripped down from CryptoJS - only includes what's needed:
 * - AES encrypt/decrypt with CBC mode and PKCS7 padding
 * - Hex, Base64, UTF8 encoding
 * - Random IV generation
 *
 * Size: ~15KB vs 202KB full CryptoJS
 */
(function (root) {
  "use strict";

  // Core WordArray
  var WordArray = {
    create: function (words, sigBytes) {
      var wa = Object.create(WordArray);
      wa.words = words || [];
      wa.sigBytes = sigBytes !== undefined ? sigBytes : wa.words.length * 4;
      return wa;
    },
    concat: function (wordArray) {
      var thisWords = this.words;
      var thatWords = wordArray.words;
      var thisSigBytes = this.sigBytes;
      var thatSigBytes = wordArray.sigBytes;

      // PRE-ALLOCATE array dengan ukuran pasti
      var requiredLength = Math.ceil((thisSigBytes + thatSigBytes) / 4);
      if (thisWords.length < requiredLength) {
        var newWords = new Array(requiredLength);
        for (var j = 0; j < thisWords.length; j++) {
          newWords[j] = thisWords[j];
        }
        for (var j = thisWords.length; j < requiredLength; j++) {
          newWords[j] = 0;
        }
        this.words = thisWords = newWords;
      }

      // Copy bytes - sekarang aman
      for (var i = 0; i < thatSigBytes; i++) {
        var thatByte = (thatWords[i >>> 2] >>> (24 - (i % 4) * 8)) & 0xff;
        var index = (thisSigBytes + i) >>> 2;
        thisWords[index] |= thatByte << (24 - ((thisSigBytes + i) % 4) * 8);
      }

      this.sigBytes += thatSigBytes;
      this.clamp();
      return this;
    },
    clamp: function () {
      var words = this.words;
      var sigBytes = this.sigBytes;
      words[sigBytes >>> 2] &= 0xffffffff << (32 - (sigBytes % 4) * 8);
      words.length = Math.ceil(sigBytes / 4);
    },
    clone: function () {
      return WordArray.create(this.words.slice(0), this.sigBytes);
    },
    random: function (nBytes) {
      var words = [];
      for (var i = 0; i < nBytes; i += 4) {
        words.push((Math.random() * 0x100000000) | 0);
      }
      return WordArray.create(words, nBytes);
    },
    toString: function (encoder) {
      return (encoder || Hex).stringify(this);
    },
  };

  // Hex encoder
  var Hex = {
    stringify: function (wordArray) {
      var words = wordArray.words;
      var sigBytes = wordArray.sigBytes;
      var hexChars = [];
      for (var i = 0; i < sigBytes; i++) {
        var bite = (words[i >>> 2] >>> (24 - (i % 4) * 8)) & 0xff;
        hexChars.push((bite >>> 4).toString(16));
        hexChars.push((bite & 0x0f).toString(16));
      }
      return hexChars.join("");
    },
    parse: function (hexStr) {
      var hexStrLength = hexStr.length;
      var words = [];
      for (var i = 0; i < hexStrLength; i += 2) {
        words[i >>> 3] |=
          parseInt(hexStr.substr(i, 2), 16) << (24 - (i % 8) * 4);
      }
      return WordArray.create(words, hexStrLength / 2);
    },
  };

  // Base64 encoder
  var Base64 = {
    _map: "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=",
    stringify: function (wordArray) {
      var words = wordArray.words;
      var sigBytes = wordArray.sigBytes;
      var map = this._map;
      wordArray.clamp();
      var base64Chars = [];
      for (var i = 0; i < sigBytes; i += 3) {
        var byte1 = (words[i >>> 2] >>> (24 - (i % 4) * 8)) & 0xff;
        var byte2 = (words[(i + 1) >>> 2] >>> (24 - ((i + 1) % 4) * 8)) & 0xff;
        var byte3 = (words[(i + 2) >>> 2] >>> (24 - ((i + 2) % 4) * 8)) & 0xff;
        var triplet = (byte1 << 16) | (byte2 << 8) | byte3;
        for (var j = 0; j < 4 && i + j * 0.75 < sigBytes; j++) {
          base64Chars.push(map.charAt((triplet >>> (6 * (3 - j))) & 0x3f));
        }
      }
      var paddingChar = map.charAt(64);
      if (paddingChar) {
        while (base64Chars.length % 4) {
          base64Chars.push(paddingChar);
        }
      }
      return base64Chars.join("");
    },
    parse: function (base64Str) {
      var base64StrLength = base64Str.length;
      var map = this._map;
      var reverseMap = this._reverseMap;
      if (!reverseMap) {
        reverseMap = this._reverseMap = [];
        for (var j = 0; j < map.length; j++) {
          reverseMap[map.charCodeAt(j)] = j;
        }
      }
      var paddingChar = map.charAt(64);
      if (paddingChar) {
        var paddingIndex = base64Str.indexOf(paddingChar);
        if (paddingIndex !== -1) {
          base64StrLength = paddingIndex;
        }
      }
      var words = [];
      var nBytes = 0;
      for (var i = 0; i < base64StrLength; i++) {
        if (i % 4) {
          var bits1 = reverseMap[base64Str.charCodeAt(i - 1)] << ((i % 4) * 2);
          var bits2 = reverseMap[base64Str.charCodeAt(i)] >>> (6 - (i % 4) * 2);
          var bitsCombined = bits1 | bits2;
          words[nBytes >>> 2] |= bitsCombined << (24 - (nBytes % 4) * 8);
          nBytes++;
        }
      }
      return WordArray.create(words, nBytes);
    },
  };

  // UTF8 encoder
  var Utf8 = {
    stringify: function (wordArray) {
      var words = wordArray.words;
      var sigBytes = wordArray.sigBytes;
      var utf8Chars = [];
      for (var i = 0; i < sigBytes; i++) {
        var bite = (words[i >>> 2] >>> (24 - (i % 4) * 8)) & 0xff;
        utf8Chars.push(String.fromCharCode(bite));
      }
      try {
        return decodeURIComponent(escape(utf8Chars.join("")));
      } catch (e) {
        return utf8Chars.join("");
      }
    },
    parse: function (utf8Str) {
      var latin1Str = unescape(encodeURIComponent(utf8Str));
      var words = [];
      for (var i = 0; i < latin1Str.length; i++) {
        words[i >>> 2] |=
          (latin1Str.charCodeAt(i) & 0xff) << (24 - (i % 4) * 8);
      }
      return WordArray.create(words, latin1Str.length);
    },
  };

  // Latin1 encoder (needed internally)
  var Latin1 = {
    stringify: function (wordArray) {
      var words = wordArray.words;
      var sigBytes = wordArray.sigBytes;
      var latin1Chars = [];
      for (var i = 0; i < sigBytes; i++) {
        var bite = (words[i >>> 2] >>> (24 - (i % 4) * 8)) & 0xff;
        latin1Chars.push(String.fromCharCode(bite));
      }
      return latin1Chars.join("");
    },
    parse: function (latin1Str) {
      var words = [];
      for (var i = 0; i < latin1Str.length; i++) {
        words[i >>> 2] |=
          (latin1Str.charCodeAt(i) & 0xff) << (24 - (i % 4) * 8);
      }
      return WordArray.create(words, latin1Str.length);
    },
  };

  // PKCS7 padding
  var Pkcs7 = {
    pad: function (data, blockSize) {
      var blockSizeBytes = blockSize * 4;
      var nPaddingBytes = blockSizeBytes - (data.sigBytes % blockSizeBytes);
      var paddingWord =
        (nPaddingBytes << 24) |
        (nPaddingBytes << 16) |
        (nPaddingBytes << 8) |
        nPaddingBytes;
      var paddingWords = [];
      for (var i = 0; i < nPaddingBytes; i += 4) {
        paddingWords.push(paddingWord);
      }
      var padding = WordArray.create(paddingWords, nPaddingBytes);
      data.concat(padding);
    },
    unpad: function (data) {
      var nPaddingBytes = data.words[(data.sigBytes - 1) >>> 2] & 0xff;
      data.sigBytes -= nPaddingBytes;
    },
  };

  // CBC mode
  var CBC = {
    encrypt: function (cipher, message, key, cfg) {
      var iv = cfg.iv;
      var prevBlock = iv;
      cipher._doEncryptBlock(message.words, 0, key, prevBlock);
      prevBlock = { words: message.words.slice(0, 4) };
      for (var offset = 4; offset < message.words.length; offset += 4) {
        cipher._doEncryptBlock(message.words, offset, key, prevBlock);
        prevBlock = { words: message.words.slice(offset, offset + 4) };
      }
    },
    decrypt: function (cipher, message, key, cfg) {
      var iv = cfg.iv;
      var prevBlock = iv.words.slice(0);
      for (var offset = 0; offset < message.words.length; offset += 4) {
        var thisBlock = message.words.slice(offset, offset + 4);
        cipher._doDecryptBlock(message.words, offset, key);
        for (var i = 0; i < 4; i++) {
          message.words[offset + i] ^= prevBlock[i];
        }
        prevBlock = thisBlock;
      }
    },
  };

  // AES S-box
  var SBOX = [];
  var INV_SBOX = [];
  var SUB_MIX_0 = [],
    SUB_MIX_1 = [],
    SUB_MIX_2 = [],
    SUB_MIX_3 = [];
  var INV_SUB_MIX_0 = [],
    INV_SUB_MIX_1 = [],
    INV_SUB_MIX_2 = [],
    INV_SUB_MIX_3 = [];

  (function () {
    var d = [];
    for (var i = 0; i < 256; i++) {
      if (i < 128) {
        d[i] = i << 1;
      } else {
        d[i] = (i << 1) ^ 0x11b;
      }
    }
    var x = 0,
      xi = 0;
    for (var i = 0; i < 256; i++) {
      var sx = xi ^ (xi << 1) ^ (xi << 2) ^ (xi << 3) ^ (xi << 4);
      sx = (sx >>> 8) ^ (sx & 0xff) ^ 0x63;
      SBOX[x] = sx;
      INV_SBOX[sx] = x;
      var x2 = d[x],
        x4 = d[x2],
        x8 = d[x4];
      var t = (d[sx] * 0x101) ^ (sx * 0x1010100);
      SUB_MIX_0[x] = (t << 24) | (t >>> 8);
      SUB_MIX_1[x] = (t << 16) | (t >>> 16);
      SUB_MIX_2[x] = (t << 8) | (t >>> 24);
      SUB_MIX_3[x] = t;
      t = (x8 * 0x1010101) ^ (x4 * 0x10001) ^ (x2 * 0x101) ^ (x * 0x1010100);
      INV_SUB_MIX_0[sx] = (t << 24) | (t >>> 8);
      INV_SUB_MIX_1[sx] = (t << 16) | (t >>> 16);
      INV_SUB_MIX_2[sx] = (t << 8) | (t >>> 24);
      INV_SUB_MIX_3[sx] = t;
      if (!x) {
        x = xi = 1;
      } else {
        x = x2 ^ d[d[d[x8 ^ x2]]];
        xi ^= d[d[xi]];
      }
    }
  })();

  var RCON = [0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36];

  // AES cipher
  var AES = {
    _keySchedule: null,
    _invKeySchedule: null,
    _nRounds: 0,

    _createKeySchedule: function (key) {
      var keyWords = key.words;
      var keySize = key.sigBytes / 4;
      var nRounds = (this._nRounds = keySize + 6);
      var ksRows = (nRounds + 1) * 4;
      var keySchedule = (this._keySchedule = []);
      for (var ksRow = 0; ksRow < ksRows; ksRow++) {
        if (ksRow < keySize) {
          keySchedule[ksRow] = keyWords[ksRow];
        } else {
          var t = keySchedule[ksRow - 1];
          if (!(ksRow % keySize)) {
            t = (t << 8) | (t >>> 24);
            t =
              (SBOX[t >>> 24] << 24) |
              (SBOX[(t >>> 16) & 0xff] << 16) |
              (SBOX[(t >>> 8) & 0xff] << 8) |
              SBOX[t & 0xff];
            t ^= RCON[(ksRow / keySize) | 0] << 24;
          } else if (keySize > 6 && ksRow % keySize == 4) {
            t =
              (SBOX[t >>> 24] << 24) |
              (SBOX[(t >>> 16) & 0xff] << 16) |
              (SBOX[(t >>> 8) & 0xff] << 8) |
              SBOX[t & 0xff];
          }
          keySchedule[ksRow] = keySchedule[ksRow - keySize] ^ t;
        }
      }
      var invKeySchedule = (this._invKeySchedule = []);
      for (var invKsRow = 0; invKsRow < ksRows; invKsRow++) {
        var ksRow = ksRows - invKsRow;
        var t = keySchedule[ksRow - (invKsRow % 4 ? 0 : 4)];
        if (invKsRow < 4 || ksRow <= 4) {
          invKeySchedule[invKsRow] = t;
        } else {
          invKeySchedule[invKsRow] =
            INV_SUB_MIX_0[SBOX[t >>> 24]] ^
            INV_SUB_MIX_1[SBOX[(t >>> 16) & 0xff]] ^
            INV_SUB_MIX_2[SBOX[(t >>> 8) & 0xff]] ^
            INV_SUB_MIX_3[SBOX[t & 0xff]];
        }
      }
    },

    _doEncryptBlock: function (words, offset, key, prevBlock) {
      if (!this._keySchedule) this._createKeySchedule(key);
      var keySchedule = this._keySchedule;
      var nRounds = this._nRounds;
      var pb = prevBlock.words || prevBlock;
      var s0 = words[offset] ^ pb[0] ^ keySchedule[0];
      var s1 = words[offset + 1] ^ pb[1] ^ keySchedule[1];
      var s2 = words[offset + 2] ^ pb[2] ^ keySchedule[2];
      var s3 = words[offset + 3] ^ pb[3] ^ keySchedule[3];
      var ksRow = 4;
      for (var round = 1; round < nRounds; round++) {
        var t0 =
          SUB_MIX_0[s0 >>> 24] ^
          SUB_MIX_1[(s1 >>> 16) & 0xff] ^
          SUB_MIX_2[(s2 >>> 8) & 0xff] ^
          SUB_MIX_3[s3 & 0xff] ^
          keySchedule[ksRow++];
        var t1 =
          SUB_MIX_0[s1 >>> 24] ^
          SUB_MIX_1[(s2 >>> 16) & 0xff] ^
          SUB_MIX_2[(s3 >>> 8) & 0xff] ^
          SUB_MIX_3[s0 & 0xff] ^
          keySchedule[ksRow++];
        var t2 =
          SUB_MIX_0[s2 >>> 24] ^
          SUB_MIX_1[(s3 >>> 16) & 0xff] ^
          SUB_MIX_2[(s0 >>> 8) & 0xff] ^
          SUB_MIX_3[s1 & 0xff] ^
          keySchedule[ksRow++];
        var t3 =
          SUB_MIX_0[s3 >>> 24] ^
          SUB_MIX_1[(s0 >>> 16) & 0xff] ^
          SUB_MIX_2[(s1 >>> 8) & 0xff] ^
          SUB_MIX_3[s2 & 0xff] ^
          keySchedule[ksRow++];
        s0 = t0;
        s1 = t1;
        s2 = t2;
        s3 = t3;
      }
      var t0 =
        ((SBOX[s0 >>> 24] << 24) |
          (SBOX[(s1 >>> 16) & 0xff] << 16) |
          (SBOX[(s2 >>> 8) & 0xff] << 8) |
          SBOX[s3 & 0xff]) ^
        keySchedule[ksRow++];
      var t1 =
        ((SBOX[s1 >>> 24] << 24) |
          (SBOX[(s2 >>> 16) & 0xff] << 16) |
          (SBOX[(s3 >>> 8) & 0xff] << 8) |
          SBOX[s0 & 0xff]) ^
        keySchedule[ksRow++];
      var t2 =
        ((SBOX[s2 >>> 24] << 24) |
          (SBOX[(s3 >>> 16) & 0xff] << 16) |
          (SBOX[(s0 >>> 8) & 0xff] << 8) |
          SBOX[s1 & 0xff]) ^
        keySchedule[ksRow++];
      var t3 =
        ((SBOX[s3 >>> 24] << 24) |
          (SBOX[(s0 >>> 16) & 0xff] << 16) |
          (SBOX[(s1 >>> 8) & 0xff] << 8) |
          SBOX[s2 & 0xff]) ^
        keySchedule[ksRow++];
      words[offset] = t0;
      words[offset + 1] = t1;
      words[offset + 2] = t2;
      words[offset + 3] = t3;
    },

    _doDecryptBlock: function (words, offset, key) {
      if (!this._invKeySchedule) this._createKeySchedule(key);
      var invKeySchedule = this._invKeySchedule;
      var nRounds = this._nRounds;
      var t = words[offset + 1];
      words[offset + 1] = words[offset + 3];
      words[offset + 3] = t;
      var s0 = words[offset] ^ invKeySchedule[0];
      var s1 = words[offset + 1] ^ invKeySchedule[1];
      var s2 = words[offset + 2] ^ invKeySchedule[2];
      var s3 = words[offset + 3] ^ invKeySchedule[3];
      var ksRow = 4;
      for (var round = 1; round < nRounds; round++) {
        var t0 =
          INV_SUB_MIX_0[s0 >>> 24] ^
          INV_SUB_MIX_1[(s1 >>> 16) & 0xff] ^
          INV_SUB_MIX_2[(s2 >>> 8) & 0xff] ^
          INV_SUB_MIX_3[s3 & 0xff] ^
          invKeySchedule[ksRow++];
        var t1 =
          INV_SUB_MIX_0[s1 >>> 24] ^
          INV_SUB_MIX_1[(s2 >>> 16) & 0xff] ^
          INV_SUB_MIX_2[(s3 >>> 8) & 0xff] ^
          INV_SUB_MIX_3[s0 & 0xff] ^
          invKeySchedule[ksRow++];
        var t2 =
          INV_SUB_MIX_0[s2 >>> 24] ^
          INV_SUB_MIX_1[(s3 >>> 16) & 0xff] ^
          INV_SUB_MIX_2[(s0 >>> 8) & 0xff] ^
          INV_SUB_MIX_3[s1 & 0xff] ^
          invKeySchedule[ksRow++];
        var t3 =
          INV_SUB_MIX_0[s3 >>> 24] ^
          INV_SUB_MIX_1[(s0 >>> 16) & 0xff] ^
          INV_SUB_MIX_2[(s1 >>> 8) & 0xff] ^
          INV_SUB_MIX_3[s2 & 0xff] ^
          invKeySchedule[ksRow++];
        s0 = t0;
        s1 = t1;
        s2 = t2;
        s3 = t3;
      }
      var t0 =
        ((INV_SBOX[s0 >>> 24] << 24) |
          (INV_SBOX[(s1 >>> 16) & 0xff] << 16) |
          (INV_SBOX[(s2 >>> 8) & 0xff] << 8) |
          INV_SBOX[s3 & 0xff]) ^
        invKeySchedule[ksRow++];
      var t1 =
        ((INV_SBOX[s1 >>> 24] << 24) |
          (INV_SBOX[(s2 >>> 16) & 0xff] << 16) |
          (INV_SBOX[(s3 >>> 8) & 0xff] << 8) |
          INV_SBOX[s0 & 0xff]) ^
        invKeySchedule[ksRow++];
      var t2 =
        ((INV_SBOX[s2 >>> 24] << 24) |
          (INV_SBOX[(s3 >>> 16) & 0xff] << 16) |
          (INV_SBOX[(s0 >>> 8) & 0xff] << 8) |
          INV_SBOX[s1 & 0xff]) ^
        invKeySchedule[ksRow++];
      var t3 =
        ((INV_SBOX[s3 >>> 24] << 24) |
          (INV_SBOX[(s0 >>> 16) & 0xff] << 16) |
          (INV_SBOX[(s1 >>> 8) & 0xff] << 8) |
          INV_SBOX[s2 & 0xff]) ^
        invKeySchedule[ksRow++];
      words[offset] = t0;
      words[offset + 1] = t1;
      words[offset + 2] = t2;
      words[offset + 3] = t3;
      var swap = words[offset + 1];
      words[offset + 1] = words[offset + 3];
      words[offset + 3] = swap;
    },

    encrypt: function (message, key, cfg) {
      cfg = cfg || {};
      var data =
        typeof message === "string" ? Utf8.parse(message) : message.clone();
      Pkcs7.pad(data, 4);
      this._keySchedule = null;
      this._invKeySchedule = null;
      CBC.encrypt(this, data, key, cfg);
      return { ciphertext: data, iv: cfg.iv, key: key };
    },

    decrypt: function (cipherParams, key, cfg) {
      cfg = cfg || {};
      var ciphertext = cipherParams.ciphertext.clone();
      this._keySchedule = null;
      this._invKeySchedule = null;
      this._createKeySchedule(key);
      CBC.decrypt(this, ciphertext, key, cfg);
      Pkcs7.unpad(ciphertext);
      return ciphertext;
    },
  };

  // CipherParams helper
  var CipherParams = {
    create: function (cfg) {
      return { ciphertext: cfg.ciphertext, iv: cfg.iv, key: cfg.key };
    },
  };

  // Export CryptoJS-compatible API
  root.CryptoJS = {
    lib: {
      WordArray: WordArray,
      CipherParams: CipherParams,
    },
    enc: { Hex: Hex, Base64: Base64, Utf8: Utf8, Latin1: Latin1 },
    mode: { CBC: CBC },
    pad: { Pkcs7: Pkcs7 },
    AES: AES,
  };
})(typeof window !== "undefined" ? window : this);
)CRYPTOJS__";

#endif // EMBEDDED_CRYPTO_JS_H
