#!/usr/bin/env bun
/**
 * kilo-edit-impl.js - 模拟 kilo 插件 edit 工具的核心实现
 * 完全复刻 packages/opencode/src/tool/edit.ts + encoding.ts 的逻辑
 */

const { readFileSync, existsSync } = require("fs");
const { resolve, dirname } = require("path");
const { mkdirSync } = require("fs");

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
    utf8: "utf-8", utf16le: "utf-16le", utf16be: "utf-16be",
    utf32le: "utf-32le", utf32be: "utf-32be",
    iso88591: "iso-8859-1", iso88592: "iso-8859-2",
    iso88595: "iso-8859-5", iso88597: "iso-8859-7",
    iso88598: "iso-8859-8", iso88599: "iso-8859-9",
    windows1250: "windows-1250", windows1251: "windows-1251",
    windows1252: "windows-1252", windows1253: "windows-1253",
    windows1255: "windows-1255",
    shiftjis: "Shift_JIS", eucjp: "euc-jp",
    iso2022jp: "iso-2022-jp", euckr: "euc-kr",
    iso2022kr: "iso-2022-kr", big5: "big5",
    gb18030: "gb18030", koi8r: "koi8-r",
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

// ====== 换行符处理 ======

function normalizeLineEndings(text) {
  return text.replaceAll("\r\n", "\n");
}

function detectLineEnding(text) {
  return text.includes("\r\n") ? "\r\n" : "\n";
}

function convertToLineEnding(text, ending) {
  if (ending === "\n") return text;
  return text.replaceAll("\n", "\r\n");
}

// ====== Levenshtein 距离 ======

function levenshtein(a, b) {
  if (a === "" || b === "") return Math.max(a.length, b.length);
  const matrix = Array.from({ length: a.length + 1 }, (_, i) =>
    Array.from({ length: b.length + 1 }, (_, j) => (i === 0 ? j : j === 0 ? i : 0))
  );
  for (let i = 1; i <= a.length; i++) {
    for (let j = 1; j <= b.length; j++) {
      const cost = a[i - 1] === b[j - 1] ? 0 : 1;
      matrix[i][j] = Math.min(matrix[i - 1][j] + 1, matrix[i][j - 1] + 1, matrix[i - 1][j - 1] + cost);
    }
  }
  return matrix[a.length][b.length];
}

// ====== 9 种 Replacer (复刻 edit.ts) ======

function* simpleReplacer(content, find) {
  yield find;
}

function* lineTrimmedReplacer(content, find) {
  const originalLines = content.split("\n");
  const searchLines = find.split("\n");
  if (searchLines[searchLines.length - 1] === "") searchLines.pop();
  for (let i = 0; i <= originalLines.length - searchLines.length; i++) {
    let matches = true;
    for (let j = 0; j < searchLines.length; j++) {
      if (originalLines[i + j].trim() !== searchLines[j].trim()) {
        matches = false;
        break;
      }
    }
    if (matches) {
      let matchStartIndex = 0;
      for (let k = 0; k < i; k++) matchStartIndex += originalLines[k].length + 1;
      let matchEndIndex = matchStartIndex;
      for (let k = 0; k < searchLines.length; k++) {
        matchEndIndex += originalLines[i + k].length;
        if (k < searchLines.length - 1) matchEndIndex += 1;
      }
      yield content.substring(matchStartIndex, matchEndIndex);
    }
  }
}

const SINGLE_CANDIDATE_SIMILARITY_THRESHOLD = 0.0;
const MULTIPLE_CANDIDATES_SIMILARITY_THRESHOLD = 0.3;

function* blockAnchorReplacer(content, find) {
  const originalLines = content.split("\n");
  const searchLines = find.split("\n");
  if (searchLines.length < 3) return;
  if (searchLines[searchLines.length - 1] === "") searchLines.pop();

  const firstLineSearch = searchLines[0].trim();
  const lastLineSearch = searchLines[searchLines.length - 1].trim();
  const searchBlockSize = searchLines.length;

  const candidates = [];
  for (let i = 0; i < originalLines.length; i++) {
    if (originalLines[i].trim() !== firstLineSearch) continue;
    for (let j = i + 2; j < originalLines.length; j++) {
      if (originalLines[j].trim() === lastLineSearch) {
        candidates.push({ startLine: i, endLine: j });
        break;
      }
    }
  }
  if (candidates.length === 0) return;

  if (candidates.length === 1) {
    const { startLine, endLine } = candidates[0];
    const actualBlockSize = endLine - startLine + 1;
    let similarity = 0;
    let linesToCheck = Math.min(searchBlockSize - 2, actualBlockSize - 2);
    if (linesToCheck > 0) {
      for (let j = 1; j < searchBlockSize - 1 && j < actualBlockSize - 1; j++) {
        const originalLine = originalLines[startLine + j].trim();
        const searchLine = searchLines[j].trim();
        const maxLen = Math.max(originalLine.length, searchLine.length);
        if (maxLen === 0) continue;
        const distance = levenshtein(originalLine, searchLine);
        similarity += (1 - distance / maxLen) / linesToCheck;
        if (similarity >= SINGLE_CANDIDATE_SIMILARITY_THRESHOLD) break;
      }
    } else {
      similarity = 1.0;
    }
    if (similarity >= SINGLE_CANDIDATE_SIMILARITY_THRESHOLD) {
      let matchStartIndex = 0;
      for (let k = 0; k < startLine; k++) matchStartIndex += originalLines[k].length + 1;
      let matchEndIndex = matchStartIndex;
      for (let k = startLine; k <= endLine; k++) {
        matchEndIndex += originalLines[k].length;
        if (k < endLine) matchEndIndex += 1;
      }
      yield content.substring(matchStartIndex, matchEndIndex);
    }
    return;
  }

  let bestMatch = null;
  let maxSimilarity = -1;
  for (const candidate of candidates) {
    const { startLine, endLine } = candidate;
    const actualBlockSize = endLine - startLine + 1;
    let similarity = 0;
    let linesToCheck = Math.min(searchBlockSize - 2, actualBlockSize - 2);
    if (linesToCheck > 0) {
      for (let j = 1; j < searchBlockSize - 1 && j < actualBlockSize - 1; j++) {
        const originalLine = originalLines[startLine + j].trim();
        const searchLine = searchLines[j].trim();
        const maxLen = Math.max(originalLine.length, searchLine.length);
        if (maxLen === 0) continue;
        const distance = levenshtein(originalLine, searchLine);
        similarity += 1 - distance / maxLen;
      }
      similarity /= linesToCheck;
    } else {
      similarity = 1.0;
    }
    if (similarity > maxSimilarity) {
      maxSimilarity = similarity;
      bestMatch = candidate;
    }
  }
  if (maxSimilarity >= MULTIPLE_CANDIDATES_SIMILARITY_THRESHOLD && bestMatch) {
    const { startLine, endLine } = bestMatch;
    let matchStartIndex = 0;
    for (let k = 0; k < startLine; k++) matchStartIndex += originalLines[k].length + 1;
    let matchEndIndex = matchStartIndex;
    for (let k = startLine; k <= endLine; k++) {
      matchEndIndex += originalLines[k].length;
      if (k < endLine) matchEndIndex += 1;
    }
    yield content.substring(matchStartIndex, matchEndIndex);
  }
}

function* whitespaceNormalizedReplacer(content, find) {
  const normalizeWhitespace = (text) => text.replace(/\s+/g, " ").trim();
  const normalizedFind = normalizeWhitespace(find);
  const lines = content.split("\n");
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    if (normalizeWhitespace(line) === normalizedFind) {
      yield line;
    } else {
      const normalizedLine = normalizeWhitespace(line);
      if (normalizedLine.includes(normalizedFind)) {
        const words = find.trim().split(/\s+/);
        if (words.length > 0) {
          const pattern = words.map((word) => word.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")).join("\\s+");
          try {
            const regex = new RegExp(pattern);
            const match = line.match(regex);
            if (match) yield match[0];
          } catch { /* invalid regex, skip */ }
        }
      }
    }
  }
  const findLines = find.split("\n");
  if (findLines.length > 1) {
    for (let i = 0; i <= lines.length - findLines.length; i++) {
      const block = lines.slice(i, i + findLines.length);
      if (normalizeWhitespace(block.join("\n")) === normalizedFind) {
        yield block.join("\n");
      }
    }
  }
}

function* indentationFlexibleReplacer(content, find) {
  const removeIndentation = (text) => {
    const lines = text.split("\n");
    const nonEmptyLines = lines.filter((line) => line.trim().length > 0);
    if (nonEmptyLines.length === 0) return text;
    const minIndent = Math.min(
      ...nonEmptyLines.map((line) => {
        const match = line.match(/^(\s*)/);
        return match ? match[0].length : 0;
      })
    );
    return lines.map((line) => (line.trim().length === 0 ? line : line.slice(minIndent))).join("\n");
  };
  const normalizedFind = removeIndentation(find);
  const contentLines = content.split("\n");
  const findLines = find.split("\n");
  for (let i = 0; i <= contentLines.length - findLines.length; i++) {
    const block = contentLines.slice(i, i + findLines.length).join("\n");
    if (removeIndentation(block) === normalizedFind) {
      yield block;
    }
  }
}

function* escapeNormalizedReplacer(content, find) {
  const unescapeString = (str) => {
    return str.replace(/\\(n|t|r|'|"|`|\\|\n|\$)/g, (match, capturedChar) => {
      switch (capturedChar) {
        case "n": return "\n";
        case "t": return "\t";
        case "r": return "\r";
        case "'": return "'";
        case '"': return '"';
        case "`": return "`";
        case "\\": return "\\";
        case "\n": return "\n";
        case "$": return "$";
        default: return match;
      }
    });
  };
  const unescapedFind = unescapeString(find);
  if (content.includes(unescapedFind)) yield unescapedFind;
  const lines = content.split("\n");
  const findLines = unescapedFind.split("\n");
  for (let i = 0; i <= lines.length - findLines.length; i++) {
    const block = lines.slice(i, i + findLines.length).join("\n");
    const unescapedBlock = unescapeString(block);
    if (unescapedBlock === unescapedFind) yield block;
  }
}

function* trimmedBoundaryReplacer(content, find) {
  const trimmedFind = find.trim();
  if (trimmedFind === find) return;
  if (content.includes(trimmedFind)) yield trimmedFind;
  const lines = content.split("\n");
  const findLines = find.split("\n");
  for (let i = 0; i <= lines.length - findLines.length; i++) {
    const block = lines.slice(i, i + findLines.length).join("\n");
    if (block.trim() === trimmedFind) yield block;
  }
}

function* contextAwareReplacer(content, find) {
  const findLines = find.split("\n");
  if (findLines.length < 3) return;
  if (findLines[findLines.length - 1] === "") findLines.pop();
  const contentLines = content.split("\n");
  const firstLine = findLines[0].trim();
  const lastLine = findLines[findLines.length - 1].trim();
  for (let i = 0; i < contentLines.length; i++) {
    if (contentLines[i].trim() !== firstLine) continue;
    for (let j = i + 2; j < contentLines.length; j++) {
      if (contentLines[j].trim() === lastLine) {
        const blockLines = contentLines.slice(i, j + 1);
        const block = blockLines.join("\n");
        if (blockLines.length === findLines.length) {
          let matchingLines = 0;
          let totalNonEmptyLines = 0;
          for (let k = 1; k < blockLines.length - 1; k++) {
            const blockLine = blockLines[k].trim();
            const findLine = findLines[k].trim();
            if (blockLine.length > 0 || findLine.length > 0) {
              totalNonEmptyLines++;
              if (blockLine === findLine) matchingLines++;
            }
          }
          if (totalNonEmptyLines === 0 || matchingLines / totalNonEmptyLines >= 0.5) {
            yield block;
            break;
          }
        }
        break;
      }
    }
  }
}

function* multiOccurrenceReplacer(content, find) {
  let startIndex = 0;
  while (true) {
    const index = content.indexOf(find, startIndex);
    if (index === -1) break;
    yield find;
    startIndex = index + find.length;
  }
}

// ====== replace 主函数 (复刻 edit.ts:702) ======

const ALL_REPLACERS = [
  simpleReplacer,
  lineTrimmedReplacer,
  blockAnchorReplacer,
  whitespaceNormalizedReplacer,
  indentationFlexibleReplacer,
  escapeNormalizedReplacer,
  trimmedBoundaryReplacer,
  contextAwareReplacer,
  multiOccurrenceReplacer,
];

function replace(content, oldString, newString, replaceAll, level) {
  if (oldString === newString) {
    throw new Error("No changes to apply: oldString and newString are identical.");
  }

  const replacers = ALL_REPLACERS.slice(0, level);
  let notFound = true;

  for (const replacer of replacers) {
    for (const search of replacer(content, oldString)) {
      const index = content.indexOf(search);
      if (index === -1) continue;
      notFound = false;
      if (replaceAll) {
        return content.replaceAll(search, newString);
      }
      const lastIndex = content.lastIndexOf(search);
      if (index !== lastIndex) continue;
      return content.substring(0, index) + newString + content.substring(index + search.length);
    }
  }

  if (notFound) {
    throw new Error(
      "Could not find oldString in the file. It must match exactly, including whitespace, indentation, and line endings."
    );
  }
  throw new Error(
    "Found multiple matches for oldString. Provide more surrounding context to make the match unique."
  );
}

// ====== 主流程 ======

const jsonPath = process.argv[2];
const level = parseInt(process.argv[3]) || 1;
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

if (!params.filePath || params.oldString === undefined || params.newString === undefined) {
  console.error("错误: JSON 必须包含 filePath, oldString 和 newString 字段");
  process.exit(1);
}

if (params.oldString === params.newString) {
  console.error("错误: oldString 和 newString 完全相同");
  process.exit(1);
}

let filePath = params.filePath;
if (!filePath.startsWith("/")) {
  filePath = resolve(process.cwd(), filePath);
}

if (!existsSync(filePath)) {
  console.error("错误: 目标文件不存在:", filePath);
  process.exit(1);
}

const pre = readSync(filePath);
const sourceEncoding = pre.encoding;
const sourceBom = sourceEncoding === UTF8_BOM;
const oldContent = pre.text;

const ending = detectLineEnding(oldContent);
const oldStr = convertToLineEnding(normalizeLineEndings(params.oldString), ending);
const newStr = convertToLineEnding(normalizeLineEndings(params.newString), ending);

const replaced = replace(oldContent, oldStr, newStr, params.replaceAll || false, level);
const toWrite = bomJoin(replaced, sourceBom);
writeSync(filePath, toWrite, sourceEncoding);

console.log("OK:", filePath);
