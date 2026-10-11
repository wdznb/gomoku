// Regression probes for the round-73 (A6-A10) and round-74 (C6/C7) web/app.js
// fixes. The real file is loaded read-only into a vm context with DOM/browser
// stubs, then the methods the fixes touch are driven directly. No browser and
// no server are needed; run it with `node tests/web_frontend_check.js`.
'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const results = [];

function check(label, fn) {
    try {
        fn();
        results.push({ label, ok: true, detail: '' });
    } catch (err) {
        results.push({ label, ok: false, detail: String((err && err.message) || err) });
    }
}

function assert(condition, message) {
    if (!condition) throw new Error(message || 'assertion failed');
}

function makeElement(id) {
    return {
        id,
        value: '',
        disabled: false,
        hidden: false,
        textContent: '',
        innerHTML: '',
        className: '',
        min: '',
        max: '',
        width: 0,
        height: 0,
        scrollTop: 0,
        scrollHeight: 0,
        dataset: {},
        style: {},
        classList: { add() {}, remove() {}, toggle() {}, contains() { return false; } },
        addEventListener() {},
        removeEventListener() {},
        setAttribute() {},
        appendChild() {},
        getBoundingClientRect() { return { left: 0, top: 0, width: 760, height: 760 }; },
        getContext() {
            if (!this._ctx) {
                const target = {};
                this._ctx = new Proxy(target, {
                    get(t, prop) {
                        if (typeof prop !== 'string') return undefined;
                        if (!(prop in t)) t[prop] = () => {};
                        return t[prop];
                    },
                    set(t, prop, value) { t[prop] = value; return true; },
                });
            }
            return this._ctx;
        },
    };
}

function buildSandbox() {
    const elements = new Map();
    const sockets = [];
    const elementFor = (id) => {
        if (!elements.has(id)) elements.set(id, makeElement(id));
        return elements.get(id);
    };

    class FakeWebSocket {
        constructor(url) {
            this.url = url;
            this.readyState = FakeWebSocket.OPEN;
            this.sent = [];
            sockets.push(this);
        }
        send(data) { this.sent.push(data); }
        close() { this.readyState = FakeWebSocket.CLOSED; }
    }
    FakeWebSocket.OPEN = 1;
    FakeWebSocket.CLOSED = 3;

    const sandbox = {
        console: { log() {}, error() {}, warn() {} },
        document: {
            getElementById: elementFor,
            createElement: () => makeElement('created'),
            createElementNS: () => makeElement('created-ns'),
            querySelectorAll: () => [],
            addEventListener() {},
            body: { classList: { add() {}, remove() {}, toggle() {} } },
        },
        window: {
            innerWidth: 1400,
            innerHeight: 900,
            devicePixelRatio: 1,
            addEventListener() {},
            location: { protocol: 'http:', host: 'localhost:8080' },
        },
        localStorage: {
            store: {},
            getItem(key) {
                return Object.prototype.hasOwnProperty.call(this.store, key)
                    ? this.store[key] : null;
            },
            setItem(key, value) { this.store[key] = String(value); },
            removeItem(key) { delete this.store[key]; },
        },
        WebSocket: FakeWebSocket,
        setTimeout: () => 0,
        clearTimeout: () => {},
        setInterval: () => 0,
        clearInterval: () => {},
    };
    return { sandbox, elementFor, sockets };
}

const source = fs.readFileSync(path.join(__dirname, '..', 'web', 'app.js'), 'utf8');
const bundle = buildSandbox();
vm.createContext(bundle.sandbox);
vm.runInContext(source, bundle.sandbox, { filename: 'app.js' });
vm.runInContext('globalThis.__GomokuGame = GomokuGame;', bundle.sandbox);

const BLACK = 1;
const WHITE = 2;
const emptyBoard = () =>
    Array(15).fill(null).map(() => Array(15).fill(0));

function newGame() {
    return new bundle.sandbox.__GomokuGame();
}

const el = bundle.elementFor;

// A6: a resume that lands while 摆棋 is open leaves setup unconditionally, and
// the resumed side/token are applied. Old code restored the board without
// dropping the setup working copy.
check('A6 session-during-setup exits setup', () => {
    const game = newGame();
    game.setup.active = true;
    const exits = [];
    const original = game.exitSetup.bind(game);
    game.exitSetup = (confirmed) => {
        exits.push(confirmed);
        original(confirmed);
    };
    game.handleServerMessage({
        type: 'session',
        token: 'a'.repeat(16),
        resumed: true,
        player_color: 'black',
        game_status: 'playing',
        current_turn: 'black',
        move_count: 0,
        max_depth: 6,
        timeout_ms: 5000,
        winner: '',
        reason: '',
        board: emptyBoard(),
    });
    assert(exits.length === 1, 'exitSetup must run exactly once');
    assert(exits[0] === false, 'the setup must be dropped, not confirmed');
    assert(game.setup.active === false, 'setup must be closed');
    assert(game.playerColor === BLACK, 'the resumed side must be applied');
    assert(game.sessionToken === 'a'.repeat(16), 'the token must be kept');
});

// A7: the replay cluster (nav buttons and 载入回放) is disabled while 摆棋 is
// open, and entering setup drops any historical replay view.
check('A7 replay cluster guarded during setup', () => {
    const game = newGame();
    game.moveHistory = [
        { row: 7, col: 7, color: BLACK },
        { row: 7, col: 8, color: WHITE },
    ];
    game.analysis = [];
    game.gameStatus = 'playing';
    game.playerColor = BLACK;
    game.aiThinking = false;
    game.setup.active = false;

    game.refreshReplayControls();
    assert(el('btn-replay').disabled === false, 'stale record: 载入回放 stays enabled');
    assert(el('btn-replay-next').disabled === false, 'stale record: nav stays enabled');

    game.setup.active = true;
    game.refreshReplayControls();
    for (const id of ['btn-replay-first', 'btn-replay-prev', 'btn-replay-play',
                      'btn-replay-next', 'btn-replay-last', 'btn-replay']) {
        assert(el(id).disabled === true, id + ' must be disabled during setup');
    }
    game.setup.active = false;

    game.replayIndex = 3;
    game.enterSetup();
    assert(game.replayIndex === null, 'enterSetup must drop the replay view');
    assert(game.setup.active === true, 'setup must be open');
});

// A8: 执黑/执白 claims the side only when choose_side actually left the socket;
// a send failure leaves the buttons and the colour untouched.
check('A8 chooseSide only claims after a successful send', () => {
    const failed = newGame();
    let failed_sent = [];
    failed.sendMessage = (msg) => { failed_sent.push(msg); return false; };
    el('btn-black').disabled = false;
    failed.chooseSide('white');
    assert(failed_sent.some((m) => m.type === 'choose_side'), 'the click must be sent');
    assert(failed.playerColor === null, 'a failed send must not claim a side');
    assert(el('btn-black').disabled === false, 'the side buttons must stay live');

    const ok = newGame();
    const ok_sent = [];
    ok.sendMessage = (msg) => { ok_sent.push(msg); return true; };
    ok.chooseSide('black');
    assert(ok_sent.some((m) => m.type === 'choose_side'), 'the click must be sent');
    assert(ok.playerColor === BLACK, 'a delivered click claims the side');
    assert(el('btn-black').disabled === true, 'black button must be spent');
    assert(el('btn-white').disabled === true, 'white button must be spent');
    assert(el('btn-undo').disabled === false, 'playing controls must open');
});

// A9: 提示 refuses an emptied or non-numeric settings box instead of sending a
// request the server would fill with its own defaults.
check('A9 requestHint guards non-finite settings', () => {
    const game = newGame();
    const sent = [];
    game.sendMessage = (msg) => { sent.push(msg); return true; };
    let status = null;
    const original = game.updateStatus.bind(game);
    game.updateStatus = (text, kind) => { status = text; original(text, kind); };

    el('timeout').value = '';
    el('depth').value = '6';
    game.requestHint();
    assert(sent.length === 0, 'an emptied timeout must not be sent');
    assert(status !== null && status.indexOf('超时和深度需要先填好数字') !== -1,
           'the refusal must be shown in the status line');

    el('timeout').value = '3';
    game.requestHint();
    assert(sent.length === 1, 'a valid pair must be sent');
    assert(sent[0].type === 'request_hint', 'request_hint must go out');
    assert(sent[0].timeout_ms === 3000, 'seconds must be converted to ms');
    assert(sent[0].max_depth === 6, 'the depth must be passed through');
});

// A10: an undo that lands on an empty board the player holds a side in mirrors
// the server's playable state instead of dead-ending at waiting.
check('A10 undo to empty board promotes with a chosen side', () => {
    const playing = newGame();
    playing.playerColor = BLACK;
    playing.baseStones = [];
    playing.moveHistory = [{ row: 7, col: 7, color: BLACK }];
    playing.handleUndoSuccess({ move_count: 0, game_status: 'waiting' });
    assert(playing.gameStatus === 'playing', 'a chosen side keeps the game playable');

    const unclaimed = newGame();
    unclaimed.playerColor = null;
    unclaimed.baseStones = [];
    unclaimed.moveHistory = [];
    unclaimed.handleUndoSuccess({ move_count: 0, game_status: 'waiting' });
    assert(unclaimed.gameStatus === 'waiting', 'no side chosen stays waiting');
});

// C6: 重开 must not clear the board while the socket is down - the stored token
// would resume straight back into the game the player just "restarted".
check('C6 restart is refused while the socket is down', () => {
    const game = newGame();
    game.board[7][7] = BLACK;
    game.moveHistory = [{ row: 7, col: 7, color: BLACK }];
    game.gameStatus = 'playing';
    game.playerColor = BLACK;
    game.ws.readyState = bundle.sandbox.WebSocket.CLOSED;

    let resets = 0;
    game.resetGame = () => { resets++; };
    let status = null;
    const original = game.updateStatus.bind(game);
    game.updateStatus = (text, kind) => { status = text; original(text, kind); };

    game.restart();
    assert(resets === 0, 'resetGame must not run while offline');
    assert(game.board[7][7] === BLACK, 'the board must be untouched');
    assert(game.moveHistory.length === 1, 'the move record must be untouched');
    assert(game.playerColor === BLACK, 'the claimed side must be untouched');
    assert(status !== null && status.indexOf('未连接') !== -1,
           'the refusal must be shown in the status line');
});

// C7a: 求招 arms once per click and the hint_result releases it, so a slow
// search cannot be queued twice and the button comes back when it answers.
check('C7 hint is sent once and released by hint_result', () => {
    const game = newGame();
    game.playerColor = BLACK;
    const sent = [];
    game.sendMessage = (msg) => { sent.push(msg); return true; };
    el('timeout').value = '5';
    el('depth').value = '6';

    game.requestHint();
    assert(sent.length === 1, 'the first click must be sent');
    assert(sent[0].type === 'request_hint', 'request_hint must go out');
    assert(game.hintInFlight === true, 'the flag must be armed');
    assert(el('btn-hint').disabled === true, 'the button must be disabled');

    game.requestHint();
    assert(sent.length === 1, 'a second click must not send a second frame');

    game.handleServerMessage({ type: 'hint_result', row: 7, col: 7, score: 100 });
    assert(game.hintInFlight === false, 'hint_result must disarm the flag');
    assert(el('btn-hint').disabled === false, 'the button must come back');
});

// C7b: a hint that was in flight when the page dropped must not stay armed
// forever once the reconnect resumes the session - the resumed session frame
// describes a game that the old search cannot answer.
check('C7 resumed session releases an in-flight hint', () => {
    const game = newGame();
    game.playerColor = BLACK;
    game.gameStatus = 'playing';
    const sent = [];
    game.sendMessage = (msg) => { sent.push(msg); return true; };
    el('timeout').value = '5';
    el('depth').value = '6';

    game.requestHint();
    assert(game.hintInFlight === true, 'armed before the drop');

    game.handleServerMessage({
        type: 'session',
        token: 'b'.repeat(16),
        resumed: true,
        player_color: 'black',
        game_status: 'playing',
        current_turn: 'white',
        move_count: 2,
        max_depth: 6,
        timeout_ms: 5000,
        winner: '',
        reason: '',
        board: emptyBoard(),
    });
    assert(game.hintInFlight === false, 'the resumed session must release the flag');
    assert(el('btn-hint').disabled === false, 'the hint button must come back');

    game.requestHint();
    assert(sent.filter((m) => m.type === 'request_hint').length === 2,
           'a fresh hint must go out after the resume');
});

// C7c: a session that was NOT resumed (token no longer known) leaves the
// request untouched - by design, the click on its way owns the state - and the
// side buttons are the documented way out: choosing a side re-arms 求招.
check('C7 non-resumed session recovers via chooseSide', () => {
    const game = newGame();
    game.playerColor = BLACK;
    game.gameStatus = 'playing';
    const sent = [];
    game.sendMessage = (msg) => { sent.push(msg); return true; };
    el('timeout').value = '5';
    el('depth').value = '6';

    game.requestHint();
    assert(game.hintInFlight === true, 'armed before the drop');

    game.handleServerMessage({
        type: 'session',
        token: 'c'.repeat(16),
        resumed: false,
        player_color: '',
        game_status: 'waiting',
        current_turn: 'black',
        move_count: 0,
        max_depth: 6,
        timeout_ms: 5000,
        winner: '',
        reason: '',
        board: emptyBoard(),
    });
    assert(el('btn-hint').disabled === true, 'still held by the old request');

    game.chooseSide('white');
    assert(game.hintInFlight === false, 'claiming a side must release the flag');
    assert(el('btn-hint').disabled === false, 'the hint button must come back');
});

let failed = 0;
for (const result of results) {
    console.log(`${result.ok ? 'PASS' : 'FAIL'} ${result.label}` +
                (result.ok ? '' : ` -- ${result.detail}`));
    if (!result.ok) failed++;
}
console.log(`web_frontend_check: ${results.length - failed}/${results.length} passed`);
process.exit(failed === 0 ? 0 : 1);
