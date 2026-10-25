// Round 92 W3: README anchor completeness. Not part of ctest - run it with
// `node tests/readme_anchor_check.js`. It reads README.md and every source file
// under src/, tests/ and web/, and answers three questions:
//   1. does each `README:NNN` / `README:NNN-NNN` citation in a comment still point
//      at a line that exists and carries text, and at what text;
//   2. does the comment's own wording (backticked or quoted spans, CJK runs) appear
//      on the lines it cites - printed as hints for a reader, never as a verdict;
//   3. is the `replay` frame method enum written at README:529-531 (the paragraph
//      this script reads, so the number here is deliberately hardcoded) the same
//      set of labels the code can actually put into that frame;
//   4. the same completeness question for every `file.ext:NNN` citation of a source
//      line - README.md, the two live design docs and every comment under src/,
//      tests/, tools/ and web/ - resolved by path, else by unique basename. Those
//      numbers are counted on the tree as checked in; docs/ENGINE_REDESIGN.md and
//      REVIEW_LOG.md are read-only history and keep the numbers they were written
//      with, so neither is scanned as a citer.
// Exit 1 on an out-of-range citation, a citation onto a blank line, a citation that
// resolves to no file or to more than one, or a documented engine path that no
// source file emits.
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

// ---- Source-line citations ------------------------------------------------
// Round 194: `file.ext:NNN` and `file.ext:NNN-NNN` appear in the docs, in headers and
// in test comments, and nothing used to check them - eleven deleted lines in
// server.cpp were enough to make every later server.cpp number in the tree wrong.
// A citation resolves against the repo root first; a bare basename is accepted only
// when exactly one file in the tree carries it. Line numbers are counted on the files
// as checked in, with the final trailing newline not counted as a line.
const CITER_DIRS = ['src', 'tests', 'tools', 'web'];
const CITER_DOCS = ['README.md', 'docs/IMPLEMENTATION_GUIDE.md', 'docs/ENGINE_REDESIGN_V2.md'];
// Read-only history: their numbers were true when written and are line-pinned by
// tests/test_training.cpp, so they are not re-counted here.
const HISTORY_DOCS = new Set(['docs/ENGINE_REDESIGN.md', 'REVIEW_LOG.md']);
const CITABLE_EXT = new Set(['.cpp', '.h', '.js', '.md', '.json', '.sh', '.py', '.txt', '.html', '.css']);
const SOURCE_CITATION = /([A-Za-z0-9_./-]+\.(?:cpp|h|js|md|json|sh|py|txt|html|css)):(\d+)(?:\s*-\s*(\d+))?/g;
const SKIP_DIRS = new Set(['.git', 'deps', 'node_modules', '.qoder']);

function walk_all(dir, out) {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
        if (entry.isDirectory()) {
            if (SKIP_DIRS.has(entry.name) || entry.name.startsWith('build')) continue;
            walk_all(path.join(dir, entry.name), out);
        } else out.push(path.join(dir, entry.name));
    }
    return out;
}

const repo_files = walk_all(ROOT, []);
const rel_of = (f) => path.relative(ROOT, f).split(path.sep).join('/');
const by_basename = new Map();
for (const f of repo_files) {
    if (!CITABLE_EXT.has(path.extname(f))) continue;
    const base = path.basename(f);
    if (!by_basename.has(base)) by_basename.set(base, []);
    by_basename.get(base).push(rel_of(f));
}

const cite_files = [];
for (const dir of CITER_DIRS) walk(path.join(ROOT, dir), cite_files);
const cite_set = new Set(cite_files
    .filter((f) => ['.cpp', '.h', '.js'].includes(path.extname(f)))
    .map(rel_of));
for (const d of CITER_DOCS) cite_set.add(d);
for (const d of HISTORY_DOCS) cite_set.delete(d);
cite_set.delete(rel_of(self));

const text_of = new Map();
const target_lines = (rel) => {
    if (!text_of.has(rel)) {
        const t = fs.readFileSync(path.join(ROOT, rel), 'utf8').split('\n');
        if (t.length && t[t.length - 1] === '') t.pop();
        text_of.set(rel, t);
    }
    return text_of.get(rel);
};
const source_phrases = (block) => [...new Set([...block.matchAll(/`([^`\n]{2,60})`/g)]
    .map((m) => norm(m[1])))].filter((p) => /[A-Za-z_]{3,}/.test(p) && !/^[\w./-]+\.(?:cpp|h|js|md|json|sh|py|txt|html|css)/.test(p));
const needle_of = (p) => p.split('(')[0].split('{')[0].split('[')[0].split(':')[0].trim();

const source_problems = [];
const source_hints = [];
let source_total = 0;
let source_ok = 0;
for (const rel of [...cite_set].sort()) {
    const citer = text_of.has(rel) ? text_of.get(rel) : target_lines(rel);
    const is_doc = rel === 'README.md' || rel.startsWith('docs/');
    citer.forEach((raw, index) => {
        for (const match of raw.matchAll(SOURCE_CITATION)) {
            if (/https?:\/\//.test(raw.slice(Math.max(0, match.index - 12), match.index + match[1].length))) continue;
            const where = `${rel}:${index + 1}`;
            const from = Number(match[2]);
            const to = match[3] ? Number(match[3]) : from;
            let target = null;
            const direct = path.join(ROOT, match[1]);
            if (fs.existsSync(direct) && fs.statSync(direct).isFile()) target = rel_of(direct);
            else {
                const list = by_basename.get(path.basename(match[1])) || [];
                if (list.length > 1) {
                    source_problems.push(`${where} -> ${match[0]}: ambiguous basename (${list.join(', ')})`);
                    continue;
                }
                if (list.length === 1) target = list[0];
            }
            if (target === null) {
                source_problems.push(`${where} -> ${match[0]}: no such file`);
                continue;
            }
            ++source_total;
            const lines = target_lines(target);
            if (from < 1 || to > lines.length || from > to) {
                source_problems.push(`${where} -> ${match[0]}: range ${from}-${to} outside ${target} (${lines.length} lines)`);
                continue;
            }
            if (lines[from - 1].trim() === '') {
                let down = from;
                while (down <= to && lines[down - 1].trim() === '') down++;
                source_problems.push(`${where} -> ${match[0]}: blank anchor ${from} in ${target}` +
                                     `${down <= to ? ` (first text at ${down})` : ''}`);
                continue;
            }
            ++source_ok;
            const block = is_doc ? raw : comment_block({ file: path.join(ROOT, rel), index });
            const phrases = source_phrases(block);
            if (phrases.length === 0) continue;
            const cited = norm(lines.slice(from - 1, to).join(' '));
            if (phrases.some((p) => cited.includes(needle_of(p)))) continue;
            const elsewhere = phrases.map((p) => ({ p, n: lines.findIndex((l) => l.includes(needle_of(p))) }))
                .find((h) => h.n >= 0);
            source_hints.push(`${where} -> ${match[0]}${elsewhere
                ? `: no phrase on the cited lines, first "${elsewhere.p}" at ${target}:${elsewhere.n + 1}`
                : `: none of the cited phrases appears in ${target}`}`);
        }
    });
}
console.log('');
console.log(`source citations resolved: ${source_total}, ok: ${source_ok}, ` +
            `problems: ${source_problems.length + (source_total - source_ok)}`);
for (const p of source_problems) problems.push('source ' + p);
console.log(`source topic hints (review by hand, not enforced): ${source_hints.length}`);
for (const h of source_hints) console.log('  HINT ' + h);

if (problems.length) {
    console.log('');
    for (const p of problems) console.log('PROBLEM ' + p);
    process.exit(1);
}
console.log('');
console.log(`readme_anchor_check: ${citations.length} citations, 0 problems`);
