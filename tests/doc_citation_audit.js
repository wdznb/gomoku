#!/usr/bin/env node
// 轮次221 #117：历史文档（docs/ENGINE_REDESIGN.md 这份 v1）里带行号的引用，
// readme_anchor_check.js 是**故意不扫**它的（历史账不当当前的合约）。但「不扫」不等于
// 「错了我不知道」—— 这一台就是给历史文档做一次漂移审计：把每条 `file.cpp:NNN` 对着
// 当前的树解一遍，报三种结果：
//   ok      —— 那一行还在，且内容看着就像引用所指的东西（只报行数，不判语义）
//   moved   —— 那一行还在，但内容已经换成别的东西（**要靠人判**，脚本只把两侧内容打出来）
//   missing —— 引用的文件找不到，或行号超出文件长度（这是硬伤）
// 它默认不作为门禁（exit 0）；带 --strict 时 missing 才让退出码非 0，用来接进流程时
// 只卡「文件/行根本不存在」这一类，不卡语义漂移。
'use strict';
const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const strict = process.argv.includes('--strict');
const targets = (process.argv.find(a => a.startsWith('--docs=')) || '--docs=docs/ENGINE_REDESIGN.md')
  .replace('--docs=', '').split(',');

const CITE = /`([A-Za-z0-9_./-]+\.(?:h|cpp|hpp|cc|js|md|sh|py))(:\d+(?:-\d+)?)?`/g;

function findFile(name) {
  const direct = path.join(ROOT, name);
  if (fs.existsSync(direct)) return direct;
  const base = path.basename(name);
  const dirs = ['src', 'src/core', 'src/search', 'src/server', 'src/optimization',
                'src/training', 'tools', 'tests', 'web', 'docs'];
  for (const d of dirs) {
    const p = path.join(ROOT, d, base);
    if (fs.existsSync(p)) return p;
  }
  return null;
}

const seen = new Set();
let ok = 0, moved = 0, missing = 0;
for (const doc of targets) {
  const docPath = path.join(ROOT, doc);
  if (!fs.existsSync(docPath)) { console.log(`DOC-MISSING ${doc}`); continue; }
  const text = fs.readFileSync(docPath, 'utf8');
  const docLines = text.split('\n');
  let m;
  while ((m = CITE.exec(text)) !== null) {
    const [, file, range] = m;
    if (!range) continue;                       // 只点名文件、不带行号：没有可漂移的东西
    const key = `${doc}|${m[0]}`;
    if (seen.has(key)) continue;
    seen.add(key);
    const lineNo = text.slice(0, m.index).split("\n").length;
    const p = findFile(file);
    const [aStr, bStr] = range.slice(1).split('-');
    const a = parseInt(aStr, 10), b = bStr ? parseInt(bStr, 10) : a;
    if (!p) { console.log(`MISSING ${m[0]}  (在 ${doc}:${lineNo} —— 文件找不到)`); missing++; continue; }
    const lines = fs.readFileSync(p, 'utf8').split('\n');
    if (a > lines.length || b > lines.length) {
      console.log(`MISSING ${m[0]}  (在 ${doc}:${lineNo} —— ${path.relative(ROOT, p)} 只有 ${lines.length} 行)`);
      missing++; continue;
    }
    const at = lines[a - 1].trim();
    const emptyish = at === '' || at.startsWith('//') || at.startsWith('*') || at.startsWith('/*');
    if (emptyish) {
      console.log(`MOVED   ${m[0]}  (在 ${doc}:${lineNo} —— 那一行现在是「${at || '(空行)'}」)`);
      moved++;
    } else {
      ok++;
      if (process.argv.includes('--verbose')) console.log(`ok      ${m[0]}  → ${at.slice(0, 90)}`);
    }
  }
}
console.log(`历史文档引用审计：ok=${ok} moved=${moved} missing=${missing}（共 ${seen.size} 条带行号的引用）`);
process.exit(strict && missing > 0 ? 1 : 0);
