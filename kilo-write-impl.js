#!/usr/bin/env bun
/**
 * kilo-write-impl.js - 模拟 kilo 插件 write 工具的核心实现
 * 完全复刻 packages/opencode/src/tool/write.ts + encoding.ts 的逻辑
 */

const { readFileSync, existsSync } = require("fs");
const { resolve, dirname } = require("path");
const { mkdirSync } = require("fs");

// 使用 kilo 源码中相同的库
const chardet = require(__dirname + "/packages/opencode/node_modules/chardet");
const iconv = require(__dirname + "/packages/opencode/node_modules/iconv-lite");

const DEFAULT = "utf-8";
const UTF8_BOM = "utf-8-bom";
const BOM_CODE = 0xfeff;
const BOM_CHAR = String.fromCharCode(BOM_CODE);

const BOMS = {
  "utf-8-bom": Buffer.from([0xef, 0xbb, 0xbf]),
  "utf-16le": Buffer.from([0xff, 0xfe]),
  "utf-16be": Buffer.from([0xfe, 0xff]),
  "utf-32le": Buffer.from([0xff, 0xfe, 0x00, 0x00]),
  "utf-32be": Buffer.from([0x00, 0x00, 0xfe, 0xff]),
};

// ====== 编码检测 (复刻 encoding.ts) ======

function startsWith(bytes, bom, limit) {
  return limit >= bom.length && bytes.subarray(0, bom.length).equals(bom);
}

function hasUtf8Bom(bytes) {
  return startsWith(bytes, BOMS["utf-8-bom"], bytes.length);
}

function isUtf8(bytes) {
  try {
    new TextDecoder("utf-8", { fatal: true }).decode(bytes);
    return true;
  } catch {
    return false;
  }
}

function normalize(name) {
  const lower = name.toLowerCase().replace(/[^a-z0-9]/g, "");
  const map = {
    utf8: "utf-8",
    utf16le: "utf-16le",
    utf16be: "utf-16be",
    utf32le: "utf-32le",
    utf32be: "utf-32be",
    iso88591: "iso-8859-1",
    iso88592: "iso-8859-2",
    iso88595: "iso-8859-5",
    iso88597: "iso-8859-7",
    iso88598: "iso-8859-8",
    iso88599: "iso-8859-9",
    windows1250: "windows-1250",
    windows1251: "windows-1251",
    windows1252: "windows-1252",
    windows1253: "windows-1253",
    windows1255: "windows-1255",
    shiftjis: "Shift_JIS",
    eucjp: "euc-jp",
    iso2022jp: "iso-2022-jp",
    euckr: "euc-kr",
    iso2022kr: "iso-2022-kr",
    big5: "big5",
    gb18030: "gb18030",
    koi8r: "koi8-r",
  };
  return map[lower] || name;
}

function detect(bytes) {
  if (bytes.length === 0) return DEFAULT;
  if (isUtf8(bytes)) return hasUtf8Bom(bytes) ? UTF8_BOM : DEFAULT;
  const result = chardet.detect(bytes);
  if (!result) return DEFAULT;
  const enc = normalize(result);
  if (!iconv.encodingExists(enc)) return DEFAULT;
  return enc;
}

function decode(bytes, encoding) {
  if (encoding === UTF8_BOM) return iconv.decode(bytes, "utf-8");
  return iconv.decode(bytes, encoding);
}

function encode(text, encoding) {
  const body = text.charCodeAt(0) === 0xfeff ? text.slice(1) : text;
  const key = encoding === UTF8_BOM ? UTF8_BOM : encoding.toLowerCase();
  const bom = BOMS[key];
  if (bom) return Buffer.concat([bom, iconv.encode(body, key === UTF8_BOM ? "utf-8" : key)]);
  return iconv.encode(text, encoding);
}

function readSync(path) {
  const bytes = readFileSync(path);
  const encoding = detect(bytes);
  return { text: decode(bytes, encoding), encoding };
}

function writeSync(path, text, encoding) {
  mkdirSync(dirname(path), { recursive: true });
  const { writeFileSync } = require("fs");
  writeFileSync(path, encode(text, encoding));
}

// ====== BOM 处理 (复刻 bom.ts) ======

function bomSplit(text) {
  if (text.charCodeAt(0) !== BOM_CODE) return { bom: false, text };
  return { bom: true, text: text.slice(1) };
}

function bomJoin(text, bom) {
  const stripped = bomSplit(text).text;
  if (!bom) return stripped;
  return BOM_CHAR + stripped;
}

// ====== 主流程 ======

const jsonPath = process.argv[2];
if (!jsonPath) {
  console.error("错误: 缺少 JSON 文件参数");
  process.exit(1);
}

if (!existsSync(jsonPath)) {
  console.error("错误: 找不到文件:", jsonPath);
  process.exit(1);
}

const jsonStr = readFileSync(jsonPath, "utf-8");
let params;
try {
  params = JSON.parse(jsonStr);
} catch (e) {
  console.error("错误: JSON 解析失败:", e.message);
  process.exit(1);
}

if (!params.filePath || params.content === undefined) {
  console.error("错误: JSON 必须包含 filePath 和 content 字段");
  process.exit(1);
}

let filePath = params.filePath;
if (!filePath.startsWith("/")) {
  filePath = resolve(process.cwd(), filePath);
}

const exists = existsSync(filePath);
let sourceEncoding = DEFAULT;
let sourceBom = false;

if (exists) {
  const pre = readSync(filePath);
  sourceEncoding = pre.encoding;
  sourceBom = sourceEncoding === UTF8_BOM;
}

const next = bomSplit(params.content);
const desiredBom = sourceBom || next.bom;
const contentNew = next.text;

writeSync(filePath, bomJoin(contentNew, desiredBom), exists ? sourceEncoding : DEFAULT);

console.log("OK:", filePath);
