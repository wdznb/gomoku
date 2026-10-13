// Round 92 W3: README anchor completeness. Not part of ctest - run it with
// `node tests/readme_anchor_check.js`. It reads README.md and every source file
// under src/, tests/ and web/, and answers three questions:
//   1. does each `README:NNN` / `README:NNN-NNN` citation in a comment still point
//      at a line that exists and carries text, and at what text;
//   2. does the comment's own wording (backticked or quoted spans, CJK runs) appear
//      on the lines it cites - printed as hints for a reader, never as a verdict;
//   3. is the `replay` frame method enum written at README:529-531 (the paragraph
//      this script reads, so the number here is deliberately hardcoded) the same
//      set of labels the code can actually put into that frame.
// Exit 1 on an out-of-range citation, a citation onto a blank line, or a documented
// engine path that no source file emits.
'use strict';

const fs = require('fs');
const path = require('path');

const ROOT = path.join(__dirname, '..');
const readme = fs.readFileSync(path.join(ROOT, 'README.md'), 'utf8').split('\n');
const lines = readme.length;
// A trailing newline makes the final split element empty; that is not a line.
if (readme[lines - 1] === '') readme.pop();
const total = readme.length;

const CITATION = /README:(\d+)(?:\s*-\s*(\d+))?/g;
const SCAN_DIRS = ['src', 'tests', 'web'];
const SCAN_EXT = new Set(['.cpp', '.h', '.js']);

function walk(dir, out) {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
        const full = path.join(dir, entry.name);
        if (entry.isDirectory()) walk(full, out);
        else if (SCAN_EXT.has(path.extname(entry.name))) out.push(full);
    }
    return out;
}

const files = [];
for (const dir of SCAN_DIRS) walk(path.join(ROOT, dir), files);
// The script's own comments quote the very lines it checks; leave them out.
const self = path.join(ROOT, 'tests', 'readme_anchor_check.js');

const citations = [];
for (const file of files) {
    if (file === self) continue;
    const text = fs.readFileSync(file, 'utf8').split('\n');
    text.forEach((raw, index) => {
        for (const match of raw.matchAll(CITATION)) {
            citations.push({
                where: `${path.relative(ROOT, file).split(path.sep).join('/')}:${index + 1}`,
                from: Number(match[1]),
                to: match[2] ? Number(match[2]) : Number(match[1]),
                comment: raw.trim().replace(/^\/\/\s*/, '').slice(0, 58),
                file,
                index,
            });
        }
    });
}

const show = (n) => (readme[n - 1] || '').trim().replace(/\s+/g, ' ').slice(0, 64);
// The text a blank anchor was reaching for: the nearest written lines either side.
const neighbours = (n) => {
    let up = n - 1;
    while (up > 0 && readme[up - 1].trim() === '') up--;
    let down = n + 1;
    while (down <= total && readme[down - 1].trim() === '') down++;
    return `nearest text at ${up > 0 ? up : '-'}/${down <= total ? down : '-'}`;
};

// Topic spot-check. A comment that names the doc usually repeats something the doc
// says: a backticked or quoted span, or a run of three or more CJK characters. Any
// such phrase is looked for in the cited lines, then - if absent there - anywhere
// in README, so a drifted anchor comes back with the line that actually carries it.
// This is a hint printed for review, not a verdict: paraphrase and section-level
// references are legitimate, and only a reader can tell those from a stale number.
const norm = (s) => s.replace(/\s+/g, ' ');
const phrase_list = (block) => {
    const out = [];
    for (const m of block.matchAll(/`([^`\n]{2,40})`|「([^」\n]{2,40})」|“([^”\n]{2,40})”/g)) {
        out.push(norm(m[1] || m[2] || m[3]));
    }
    for (const m of block.matchAll(/[一-龥]{3,}/g)) out.push(m[0]);
    return [...new Set(out)].filter((p) => p !== 'README');
};
const comment_block = (c) => {
    const lines = fs.readFileSync(c.file, 'utf8').split('\n');
    const is_comment = (s) => /^\s*(\/\/|\/\*|\*)/.test(s);
    let from = c.index;
    while (from > 0 && is_comment(lines[from - 1])) from--;
    let to = c.index;
    while (to + 1 < lines.length && is_comment(lines[to + 1])) to++;
    return lines.slice(from, to + 1).join(' ');
};
const topic_check = (c) => {
    const phrases = phrase_list(comment_block(c));
    if (phrases.length === 0) return { verdict: 'nothing verbatim to match', where: '' };
    const cited = norm(readme.slice(c.from - 1, c.to).join(' '));
    const hit_in_range = phrases.find((p) => cited.includes(p));
    if (hit_in_range) return { verdict: `phrase on the cited lines (${hit_in_range})`, where: '' };
    const hit_elsewhere = phrases
        .map((p) => ({ p, n: readme.findIndex((line) => norm(line).includes(p)) }))
        .find((h) => h.n >= 0);
    if (hit_elsewhere) {
        return { verdict: 'no phrase on the cited lines',
                 where: `, first line carrying one is README:${hit_elsewhere.n + 1} (${hit_elsewhere.p})` };
    }
    return { verdict: 'paraphrase, no verbatim phrase', where: '' };
};

const problems = [];
const drifts = [];
console.log(`README.md lines: ${total}`);
console.log(`citations found: ${citations.length}`);
for (const c of citations.sort((a, b) => a.from - b.from || a.where.localeCompare(b.where))) {
    const range = c.from === c.to ? `${c.from}` : `${c.from}-${c.to}`;
    const flags = [];
    for (let n = c.from; n <= c.to; ++n) {
        if (n < 1 || n > total) flags.push(`line ${n} out of range`);
        else if (readme[n - 1].trim() === '') flags.push(`line ${n} blank (${neighbours(n)})`);
    }
    if (flags.length) problems.push(`${c.where} -> README:${range}: ${flags.join(', ')}`);
    const topic = topic_check(c);
    if (topic.verdict === 'no phrase on the cited lines') {
        drifts.push(`${c.where} -> README:${range}${topic.where}`);
    }
    console.log(`  ${c.where} -> README:${range}  |  says: ${c.comment}  |  doc: ${show(c.from)}` +
                `  |  topic: ${topic.verdict}${topic.where}${flags.length ? '  << ' + flags.join('; ') : ''}`);
}
console.log('');
console.log(`topic hints (review by hand, not enforced): ${drifts.length} citation(s)`);
for (const d of drifts) console.log('  HINT ' + d);

// The documented replay-frame method enum: the backticked slash-list inside the
// `replay` paragraph at README:529-531, read as written rather than hardcoded.
const docLine = readme.slice(528, 531).join('');
const group = docLine.match(/（((?:`[a-z_]+`(?:\s*\/\s*)?){3,})）/);
const documented = group ? [...new Set([...group[1].matchAll(/`([a-z_]+)`/g)].map((m) => m[1]))] : [];
const human = /`method`[^。]*`human`/.test(docLine) || docLine.includes('`human`');
if (group === null) problems.push('README:529-531: no backticked slash-list found in the replay paragraph');

// Every label the engine assigns, from the assignments in src/search.
const engineFile = path.join(ROOT, 'src', 'search', 'search_engine.cpp');
const emitted = [...new Set([...fs.readFileSync(engineFile, 'utf8')
    .matchAll(/result\.method\s*=\s*"([a-z_]+)"/g)].map((m) => m[1]))].sort();

// The two labels outside the documented list cannot enter the frame anyway:
// game_manager.cpp:395 returns before appending an analysis on result.resigned,
// and :407 appends only when result.best_move.pos.is_valid(), which the
// no-legal-move and resign results never are.
console.log('');
console.log(`README:529-531 names human as the player path: ${human}`);
console.log(`README:529-531 engine paths listed (${documented.length}): ${documented.join(', ')}`);
console.log(`src/search/search_engine.cpp assigns (${emitted.length}): ${emitted.join(', ')}`);
const undocumented = emitted.filter((t) => !documented.includes(t));
const unemitted = documented.filter((t) => !emitted.includes(t));
console.log(`assigned but not in the list: ${undocumented.join(', ') || 'none'}`);
console.log(`in the list but never assigned: ${unemitted.join(', ') || 'none'}`);
console.log(`fast paths the sentence calls 后三条: ${documented.slice(-3).join(', ')}`);
for (const t of unemitted) problems.push(`README lists ${t} but src/search/search_engine.cpp never assigns it`);

if (problems.length) {
    console.log('');
    for (const p of problems) console.log('PROBLEM ' + p);
    process.exit(1);
}
console.log('');
console.log(`readme_anchor_check: ${citations.length} citations, 0 problems`);
