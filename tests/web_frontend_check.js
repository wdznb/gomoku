// Regression probes for the round-73 (A6-A10), round-74 (C6/C7), round-75
// (F3/F4) and round-76 (J3-J8) web/app.js fixes. The real file is loaded
// read-only into a vm context with DOM/browser stubs, then the methods the
// fixes touch are driven directly. No browser and no server are needed; run it
// with `node tests/web_frontend_check.js`.
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
    const element = {
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
        // Round 102 GG2: the 摆棋 guards live in the click handlers themselves, so
        // a no-op addEventListener left those entries unreachable. The record is
        // passive - no existing case fires a listener - and elements are shared
        // across games, so fireClick() runs the handler registered last, which is
        // the most recently constructed game's.
        _listeners: [],
        // Round 103 HH3: the 落子记录 rows are throwaway createElement('div')s, so
        // recording what was appended is the only way to reach their click handlers.
        _children: [],
        addEventListener(type, handler) { element._listeners.push({ type, handler }); },
        removeEventListener() {},
        setAttribute() {},
        // Round 103 HH1: openImportModal focuses the paste box; a stub without it
        // threw before the case could reach anything it asserts on.
        focus() {},
        appendChild(child) { element._children.push(child); },
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
    return element;
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

// F3: an undo stops the hint search in flight, and the aborted search answers
// with neither hint_result nor an error - the undo handler is where the button
// comes back. Old code left hintInFlight armed and 求招 greyed for the game.
// (C7a below is the control that the hint_result path still releases normally.)
check('F3 undo releases an in-flight hint', () => {
    const game = newGame();
    game.playerColor = BLACK;
    game.gameStatus = 'playing';
    const sent = [];
    game.sendMessage = (msg) => { sent.push(msg); return true; };
    el('timeout').value = '5';
    el('depth').value = '6';

    game.requestHint();
    assert(game.hintInFlight === true, 'armed before the undo');
    assert(el('btn-hint').disabled === true, 'the button must be held');

    game.handleUndoSuccess({ move_count: 0, game_status: 'waiting' });
    assert(game.hintInFlight === false, 'the undo must release the flag');
    assert(el('btn-hint').disabled === false, 'the hint button must come back');

    // No side chosen: the flag is released but the button stays disabled.
    const idle = newGame();
    idle.playerColor = null;
    idle.gameStatus = 'playing';
    idle.hintInFlight = true;
    el('btn-hint').disabled = true;
    idle.handleUndoSuccess({ move_count: 0, game_status: 'waiting' });
    assert(idle.hintInFlight === false, 'the flag is released regardless');
    assert(el('btn-hint').disabled === true, 'no side chosen leaves 求招 disabled');

    // A finished game keeps it disabled too.
    const over = newGame();
    over.playerColor = BLACK;
    over.gameStatus = 'game_over';
    over.hintInFlight = true;
    el('btn-hint').disabled = true;
    over.handleUndoSuccess({ move_count: 0, game_status: 'game_over' });
    assert(over.hintInFlight === false, 'the flag is released on a finished game too');
    assert(el('btn-hint').disabled === true, 'a finished game leaves 求招 disabled');
});

// F4: a live stone ends the replay view instead of painting history while the
// game moves on, and a board click on a historical ply says why it is refused.
check('F4 live moves exit replay and replay clicks warn', () => {
    const game = newGame();
    game.playerColor = BLACK;
    game.gameStatus = 'playing';
    game.aiThinking = false;
    game.moveHistory = [
        { row: 7, col: 7, color: BLACK },
        { row: 7, col: 8, color: WHITE },
    ];
    game.board[7][7] = BLACK;
    game.board[7][8] = WHITE;
    game.replayIndex = 0;

    game.handleAIMove({ row: 5, col: 5, color: 'white', score: 0, thinking_time_ms: 1 });
    assert(game.replayIndex === null, 'ai_move must exit the replay view');
    assert(game.board[5][5] === WHITE, 'the live stone must be placed');
    assert(game.moveHistory.length === 3, 'the live stone joins the record');
    assert(game.board[7][7] === BLACK && game.board[7][8] === WHITE,
           'the whole live position stays on the board');

    const human = newGame();
    human.playerColor = WHITE;
    human.gameStatus = 'playing';
    human.aiThinking = false;
    human.replayIndex = 2;
    human.handleMoveMade({ row: 4, col: 4, color: 'black' });
    assert(human.replayIndex === null, 'move_made must exit the replay view');
    assert(human.board[4][4] === BLACK, 'the human stone must be placed');

    // A click on a historical ply: no move leaves, the refusal is a warning.
    const viewing = newGame();
    viewing.playerColor = BLACK;
    viewing.gameStatus = 'playing';
    viewing.aiThinking = false;
    viewing.moveHistory = [
        { row: 7, col: 7, color: BLACK },
        { row: 7, col: 8, color: WHITE },
    ];
    viewing.board[7][7] = BLACK;
    viewing.board[7][8] = WHITE;
    viewing.replayIndex = 0;
    let statusText = null;
    let statusKind = null;
    const original = viewing.updateStatus.bind(viewing);
    viewing.updateStatus = (text, kind) => { statusText = text; statusKind = kind; original(text, kind); };
    let sentMoves = 0;
    viewing.sendMove = () => { sentMoves++; };
    const x = viewing.geom.pad + 7 * viewing.geom.cell;
    viewing.handleCanvasClick({ clientX: x, clientY: x });
    assert(sentMoves === 0, 'no move may leave from a replay view');
    assert(statusKind === 'warn' && statusText !== null && statusText.indexOf('回放') !== -1,
           'the refusal must surface as a warning, got: ' + statusText);
});

// J3: a resumed session frame resets the per-game residue (move list, engine
// numbers, base stones, replay view) before painting the server's board. Old
// code left the previous game's move list on screen while the board already
// showed the resumed position.
check('J3 resumed session drops the previous move record', () => {
    const game = newGame();
    game.moveHistory = [
        { row: 0, col: 0, color: BLACK },
        { row: 0, col: 1, color: WHITE },
    ];
    game.analysis = [{ depth: 1, nodes: 10, score: 0, elapsed_ms: 1, method: 'alphabeta' }];
    game.baseStones = [{ row: 1, col: 1, color: BLACK }];
    game.replayIndex = 1;
    game.sendMessage = () => true;

    const board = emptyBoard();
    board[7][7] = BLACK;
    game.handleServerMessage({
        type: 'session',
        token: 'd'.repeat(16),
        resumed: true,
        player_color: 'black',
        game_status: 'playing',
        current_turn: 'white',
        move_count: 1,
        max_depth: 6,
        timeout_ms: 5000,
        winner: '',
        reason: '',
        board: board,
    });

    assert(game.moveHistory.length === 0, 'the old move record must be dropped');
    assert(game.analysis.length === 0, 'the old engine numbers must be dropped');
    assert(game.baseStones.length === 0, 'the old base stones must be dropped');
    assert(game.replayIndex === null, 'the replay view must be closed');
    assert(game.board[7][7] === BLACK, 'the server board must be painted');
});

// J4: the resumed status line reports the server's ply count, falls back to the
// local list when the frame carries no usable number, and describes an
// installed position instead of "0 手" under four stones.
check('J4 resumed status reports the server ply count', () => {
    // move_count=7 -> the server's number, not whatever the local list holds
    const server = newGame();
    let status = null;
    const original = server.updateStatus.bind(server);
    server.updateStatus = (text, kind) => { status = text; original(text, kind); };
    server.sendMessage = () => true;
    server.handleServerMessage({
        type: 'session',
        token: 'e'.repeat(16),
        resumed: true,
        player_color: 'black',
        game_status: 'playing',
        current_turn: 'white',
        move_count: 7,
        max_depth: 6,
        timeout_ms: 5000,
        winner: '',
        reason: '',
        board: emptyBoard(),
    });
    // Round 112 PP3: the unit on this line follows README:580, which defines the
    // session frame's move_count as 盘上子数, so the server's number is counted in 子.
    // What the assertion is doing is unchanged - it still says "the server's number,
    // not the local list" (see the :547 comment).
    assert(status !== null && status.indexOf('已恢复对局：7 子') !== -1,
           'the server count must be reported, got: ' + status);

    // missing or non-numeric -> the local record is the only count left
    const local = newGame();
    local.gameStatus = 'playing';
    local.moveHistory = [
        { row: 7, col: 7, color: BLACK },
        { row: 7, col: 8, color: WHITE },
        { row: 6, col: 8, color: BLACK },
    ];
    local.currentTurn = WHITE;
    assert(local.resumedStatusText(undefined).indexOf('已恢复对局：3 手') !== -1,
           'a missing count must fall back to the local list');
    assert(local.resumedStatusText('7').indexOf('已恢复对局：3 手') !== -1,
           'a string count must not be trusted');

    // 0 plies with stones on the board -> describe the installed position
    const installed = newGame();
    const board = emptyBoard();
    board[7][7] = BLACK;
    board[7][8] = WHITE;
    board[8][8] = BLACK;
    board[8][7] = WHITE;
    installed.board = board;
    installed.currentTurn = BLACK;
    // A hand-set position whose side has been taken is "playing" with an empty
    // move list; a fresh game would be "waiting" and take the waiting branch,
    // which would leave the 0-ply wording below untested.
    installed.gameStatus = 'playing';
    const text = installed.resumedStatusText(0);
    assert(text.indexOf('已恢复摆好的局面：4 子') !== -1, 'got: ' + text);
});

// J5: 悔棋/求招 follow one rule everywhere - held while no side is chosen or
// the game is over - and every exit path (session, thinking, ai_move, stop,
// undo_success) restores that rule instead of leaving the wholesale greying
// behind.
check('J5 undo/hint buttons follow the chosen-side rule on every path', () => {
    // session, game over -> both held
    const over = newGame();
    over.sendMessage = () => true;
    el('btn-undo').disabled = false;
    el('btn-hint').disabled = false;
    over.handleServerMessage({
        type: 'session',
        token: 'f'.repeat(16),
        resumed: true,
        player_color: 'black',
        game_status: 'game_over',
        current_turn: 'white',
        move_count: 3,
        max_depth: 6,
        timeout_ms: 5000,
        winner: 'white',
        reason: 'five',
        board: emptyBoard(),
    });
    assert(el('btn-undo').disabled === true, 'a finished game holds 悔棋');
    assert(el('btn-hint').disabled === true, 'a finished game holds 求招');

    // session, playing with a side -> both open
    const play = newGame();
    play.sendMessage = () => true;
    el('btn-undo').disabled = true;
    el('btn-hint').disabled = true;
    play.handleServerMessage({
        type: 'session',
        token: 'g'.repeat(16),
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
    assert(el('btn-undo').disabled === false, 'playing with a side opens 悔棋');
    assert(el('btn-hint').disabled === false, 'playing with a side opens 求招');

    // thinking -> both held, 停止 comes up
    const thinking = newGame();
    thinking.playerColor = BLACK;
    thinking.gameStatus = 'playing';
    thinking.currentTurn = WHITE;
    thinking.handleThinkingProgress({ depth: 1, nodes: 1, elapsed_ms: 1 });
    assert(thinking.aiThinking === true, 'the progress frame arms thinking');
    assert(el('btn-stop').disabled === false, '停止 comes up with the search');
    assert(el('btn-undo').disabled === true && el('btn-hint').disabled === true,
           'thinking holds both live controls');

    // ai_move -> the finished search owes them back by the rule
    thinking.handleAIMove({ row: 5, col: 5, color: 'white', score: 0, thinking_time_ms: 1 });
    assert(thinking.aiThinking === false, 'ai_move ends thinking');
    assert(el('btn-stop').disabled === true, '停止 goes back down');
    assert(el('btn-undo').disabled === false && el('btn-hint').disabled === false,
           'ai_move restores both by the rule');

    // stop_thinking -> same rule; an unclaimed side keeps them held
    const stopped = newGame();
    stopped.playerColor = BLACK;
    stopped.gameStatus = 'playing';
    stopped.aiThinking = true;
    el('btn-undo').disabled = true;
    el('btn-hint').disabled = true;
    stopped.sendMessage = () => true;
    stopped.stopThinking();
    assert(stopped.aiThinking === false, 'stop_thinking disarms locally');
    assert(el('btn-undo').disabled === false && el('btn-hint').disabled === false,
           'stop restores both by the rule');

    const idle = newGame();
    idle.playerColor = null;
    idle.gameStatus = 'playing';
    idle.sendMessage = () => true;
    idle.stopThinking();
    assert(el('btn-undo').disabled === true && el('btn-hint').disabled === true,
           'no side chosen keeps them held');

    // undo_success -> releases an in-flight hint and follows the rule
    const undone = newGame();
    undone.playerColor = BLACK;
    undone.gameStatus = 'playing';
    undone.baseStones = [];
    undone.moveHistory = [];
    undone.board = emptyBoard();
    undone.hintInFlight = true;
    el('btn-hint').disabled = true;
    undone.handleUndoSuccess({ move_count: 0, game_status: 'playing' });
    assert(undone.hintInFlight === false, 'the undo releases an in-flight hint');
    assert(el('btn-hint').disabled === false, 'and the button comes back by the rule');

    const undoneOver = newGame();
    undoneOver.playerColor = BLACK;
    undoneOver.gameStatus = 'playing';
    undoneOver.baseStones = [];
    undoneOver.moveHistory = [];
    undoneOver.board = emptyBoard();
    undoneOver.hintInFlight = true;
    el('btn-hint').disabled = true;
    undoneOver.handleUndoSuccess({ move_count: 0, game_status: 'game_over' });
    assert(el('btn-hint').disabled === true, 'a finished game keeps 求招 held');
});

// J6: the label ring outside the grid is edge information, not a cell. Its
// pixels round into row/column 0 (and 14), so a click half-way to the first
// intersection used to land a stone there; the grid guard drops them before
// any mapping instead.
check('J6 clicks on the label ring never reach the board', () => {
    const game = newGame();
    game.playerColor = BLACK;
    game.gameStatus = 'playing';
    game.aiThinking = false;
    game.currentTurn = BLACK;
    const sent = [];
    game.sendMove = (row, col) => { sent.push([row, col]); };
    let statusCalls = 0;
    const original = game.updateStatus.bind(game);
    game.updateStatus = (text, kind) => { statusCalls++; original(text, kind); };

    const pad = game.geom.pad;
    const cell = game.geom.cell;
    const gridMax = game.geom.size - pad;

    // left of the grid, level with a row
    game.handleCanvasClick({ clientX: pad - 6, clientY: pad + 7 * cell });
    assert(sent.length === 0 && statusCalls === 0,
           'the ring must be ignored, not answered');

    // below the grid, level with a column
    game.handleCanvasClick({ clientX: pad + 7 * cell, clientY: gridMax + 6 });
    assert(sent.length === 0 && statusCalls === 0, 'the ring must be ignored vertically');

    // inside the grid the corner cell still lands normally
    game.handleCanvasClick({ clientX: pad + 2, clientY: pad + 2 });
    assert(sent.length === 1 && sent[0][0] === 0 && sent[0][1] === 0,
           'an in-grid click must still send its cell, got: ' + JSON.stringify(sent));

    // in 摆棋 the ring pixels must not paint the working board either
    game.setup.active = true;
    let painted = 0;
    game.paintSetupCell = () => { painted++; };
    game.handleCanvasClick({ clientX: pad - 6, clientY: pad + 7 * cell });
    assert(painted === 0, 'the ring must not paint the setup board');
    game.handleCanvasClick({ clientX: pad + 2, clientY: pad + 2 });
    assert(painted === 1, 'an in-grid click does paint the setup board');
    game.setup.active = false;
});

// J7: every way out of the replay view leaves it properly. Auto-play reaching
// the last ply exits instead of stranding the index there, a click on the
// historical board explains itself, and live messages (ai_move, game_over)
// pull the view back to the live position.
check('J7 replay view exits on end-of-play and live moves', () => {
    const sandbox = bundle.sandbox;
    const realSetInterval = sandbox.setInterval;
    const realClearInterval = sandbox.clearInterval;
    const timers = [];
    sandbox.setInterval = (fn) => { timers.push(fn); return timers.length; };
    sandbox.clearInterval = (id) => { if (id) timers[id - 1] = null; };
    try {
        // (i) auto-play at the last ply exits the view
        const auto = newGame();
        auto.playerColor = BLACK;
        auto.gameStatus = 'playing';
        auto.aiThinking = false;
        auto.moveHistory = [
            { row: 7, col: 7, color: BLACK },
            { row: 7, col: 8, color: WHITE },
            { row: 6, col: 8, color: BLACK },
        ];
        auto.replayIndex = 1;
        const before = timers.length;
        auto.toggleReplayAuto();
        assert(timers.length === before + 1, '自动 must arm one interval');
        const tick = timers[timers.length - 1];
        tick();
        assert(auto.replayIndex === 2, 'the tick must advance to the last ply');
        tick();
        assert(auto.replayIndex === null, 'the end of the record must exit the replay');
        assert(el('btn-replay-play').textContent === '自动', '自动 must be labelled back');
    } finally {
        sandbox.setInterval = realSetInterval;
        sandbox.clearInterval = realClearInterval;
    }

    // (ii) a click on the historical board refuses and says why
    const viewing = newGame();
    viewing.playerColor = BLACK;
    viewing.gameStatus = 'playing';
    viewing.aiThinking = false;
    viewing.currentTurn = BLACK;
    viewing.moveHistory = [
        { row: 7, col: 7, color: BLACK },
        { row: 7, col: 8, color: WHITE },
    ];
    viewing.replayIndex = 1;
    let statusText = null;
    let statusKind = null;
    const original = viewing.updateStatus.bind(viewing);
    viewing.updateStatus = (text, kind) => {
        statusText = text;
        statusKind = kind;
        original(text, kind);
    };
    let sentMoves = 0;
    viewing.sendMove = () => { sentMoves++; };
    viewing.handleCanvasClick({ clientX: viewing.geom.pad + 5 * viewing.geom.cell,
                                clientY: viewing.geom.pad + 5 * viewing.geom.cell });
    assert(sentMoves === 0, 'no move may leave from a replay view');
    assert(statusKind === 'warn' && statusText !== null && statusText.indexOf('回放') !== -1,
           'the refusal must surface as a warning, got: ' + statusText);

    // (iii) an ai_move pulls the view back to the live position
    const live = newGame();
    live.playerColor = BLACK;
    live.gameStatus = 'playing';
    live.aiThinking = false;
    live.moveHistory = [
        { row: 7, col: 7, color: BLACK },
        { row: 7, col: 8, color: WHITE },
    ];
    live.board[7][7] = BLACK;
    live.board[7][8] = WHITE;
    live.replayIndex = 0;
    live.handleAIMove({ row: 5, col: 5, color: 'white', score: 0, thinking_time_ms: 1 });
    assert(live.replayIndex === null, 'ai_move must exit the replay view');
    assert(live.board[5][5] === WHITE, 'the live stone must land');

    // (iv) game_over pulls it back too
    const ended = newGame();
    ended.playerColor = BLACK;
    ended.gameStatus = 'playing';
    ended.aiThinking = false;
    ended.moveHistory = [
        { row: 7, col: 7, color: BLACK },
        { row: 7, col: 8, color: WHITE },
    ];
    ended.board[7][7] = BLACK;
    ended.board[7][8] = WHITE;
    ended.replayIndex = 1;
    ended.handleGameOver({ winner: 'white', reason: 'five' });
    assert(ended.replayIndex === null, 'game_over must exit the replay view');
    assert(ended.gameStatus === 'game_over', 'the game must be over');
});

// J8: a hint answer describes the position the search began on. Every path that
// changes the position pushes onto moveHistory, so a reply whose ply count no
// longer matches the request is stale: release the control, say so, and leave
// the board alone.
check('J8 stale hint replies are refused', () => {
    const stale = newGame();
    stale.playerColor = BLACK;
    stale.gameStatus = 'playing';
    stale.aiThinking = false;
    stale.moveHistory = [
        { row: 7, col: 7, color: BLACK },
        { row: 7, col: 8, color: WHITE },
    ];
    stale.board[7][7] = BLACK;
    stale.board[7][8] = WHITE;
    stale.hintInFlight = true;
    stale.hintMoveCount = 2;
    el('btn-hint').disabled = true;
    let highlight = null;
    stale.highlightMove = (row, col) => { highlight = [row, col]; };
    let status = null;
    const original = stale.updateStatus.bind(stale);
    stale.updateStatus = (text, kind) => { status = text; original(text, kind); };

    // a stone landed while 求招 was out
    stale.moveHistory.push({ row: 5, col: 5, color: BLACK });
    stale.handleHintResult({ row: 0, col: 0, score: 42 });

    assert(status === '建议已失效', 'the stale answer must say so, got: ' + status);
    assert(highlight === null, 'the ring must not move for a dead position');
    assert(stale.hintInFlight === false, 'the flag must be released');
    assert(el('btn-hint').disabled === false, 'the button comes back by the rule');

    // the same answer against the position it was asked for lands normally
    const fresh = newGame();
    fresh.playerColor = BLACK;
    fresh.gameStatus = 'playing';
    fresh.aiThinking = false;
    fresh.moveHistory = [
        { row: 7, col: 7, color: BLACK },
        { row: 7, col: 8, color: WHITE },
    ];
    fresh.hintInFlight = true;
    fresh.hintMoveCount = 2;
    el('btn-hint').disabled = true;
    let landed = null;
    fresh.highlightMove = (row, col) => { landed = [row, col]; };
    let freshStatus = null;
    const originalFresh = fresh.updateStatus.bind(fresh);
    fresh.updateStatus = (text, kind) => { freshStatus = text; originalFresh(text, kind); };

    fresh.handleHintResult({ row: 7, col: 7, score: 100 });
    assert(landed !== null && landed[0] === 7 && landed[1] === 7,
           'the ring must mark the advised cell, got: ' + JSON.stringify(landed));
    assert(freshStatus === '建议 H8 · 分值 100', 'got: ' + freshStatus);
});

// L7: 终止 stops the AI search, not a hint search - a hint already in flight
// still answers after stop_thinking (the ply-count check is what voids a stale
// one). The old code restored 求招 by the plain rule at the end of
// stopThinking, so the button claimed idle while hintInFlight was still armed.
check('L7 stop_thinking keeps 求招 held while a hint is in flight', () => {
    const game = newGame();
    game.playerColor = BLACK;
    game.gameStatus = 'playing';
    game.sendMessage = () => true;
    game.hintInFlight = true;
    el('btn-hint').disabled = true;

    game.stopThinking();
    assert(game.aiThinking === false, '终止 disarms the throwaway flag');
    assert(el('btn-hint').disabled === true,
           'an in-flight hint keeps 求招 held through stop_thinking');

    // Control: with no hint out, 终止 restores the ordinary rule as before.
    const idle = newGame();
    idle.playerColor = BLACK;
    idle.gameStatus = 'playing';
    idle.sendMessage = () => true;
    idle.hintInFlight = false;
    el('btn-hint').disabled = true;
    idle.stopThinking();
    assert(el('btn-hint').disabled === false, 'a plain stop hands 求招 back');

    // The other two corners of the old rule still hold.
    const unclaimed = newGame();
    unclaimed.playerColor = null;
    unclaimed.gameStatus = 'playing';
    unclaimed.sendMessage = () => true;
    unclaimed.hintInFlight = false;
    el('btn-hint').disabled = false;
    unclaimed.stopThinking();
    assert(el('btn-hint').disabled === true, 'no side chosen keeps 求招 held');

    const over = newGame();
    over.playerColor = BLACK;
    over.gameStatus = 'game_over';
    over.sendMessage = () => true;
    over.hintInFlight = false;
    el('btn-hint').disabled = false;
    over.stopThinking();
    assert(el('btn-hint').disabled === true, 'a finished game keeps 求招 held');

    // The undo path is unchanged: it releases the flag and hands the control
    // back by the plain rule (F3 above pins the same shape).
    const undone = newGame();
    undone.playerColor = BLACK;
    undone.gameStatus = 'playing';
    undone.baseStones = [];
    undone.moveHistory = [];
    undone.board = emptyBoard();
    undone.hintInFlight = true;
    el('btn-hint').disabled = true;
    undone.handleUndoSuccess({ move_count: 0, game_status: 'playing' });
    assert(undone.hintInFlight === false, 'the undo releases the flag');
    assert(el('btn-hint').disabled === false, 'the undo hands 求招 back by the rule');
});

// B2: 摆棋 installs a position without dropping the previous game's 落子记录.
// While nobody has chosen a side, that list belongs to the game the position
// replaced, so no replay entry point may step into it.
check('B2 unclaimed position locks replay', () => {
    const game = newGame();
    game.playerColor = BLACK;
    game.gameStatus = 'playing';
    game.setup.active = false;
    game.aiThinking = false;
    game.moveHistory = [
        { row: 7, col: 7, color: BLACK },
        { row: 7, col: 8, color: WHITE },
    ];
    game.analysis = [
        { method: 'alphabeta', score: 40, depth: 4, nodes: 100, elapsed_ms: 5 },
        { method: 'alphabeta', score: -40, depth: 4, nodes: 100, elapsed_ms: 5 },
    ];
    game.analysisEnded = true;
    game.replayIndex = null;

    const board = emptyBoard();
    board[10][10] = WHITE;
    game.handlePositionSet({ board, current_turn: 'black' });
    assert(game.gameStatus === 'waiting', 'position_set leaves the game waiting');
    assert(game.playerColor === null, 'position_set clears the chosen side');
    assert(game.moveHistory.length === 2, 'the old 落子记录 stays until a side is chosen');
    assert(game.replayIndex === null, 'position_set exits any replay view');

    // The 落子记录 row click handler is showReplayMove(index); the nav buttons
    // go through stepReplay -> showReplayMove as well.
    game.showReplayMove(0);
    assert(game.replayIndex === null, 'an unclaimed position must not enter replay');
    game.stepReplay(1);
    assert(game.replayIndex === null, 'the nav buttons must not enter replay either');
    const nav = ['btn-replay-first', 'btn-replay-prev', 'btn-replay-play',
                 'btn-replay-next', 'btn-replay-last'];
    game.refreshReplayControls();
    for (const id of nav) {
        assert(el(id).disabled === true, id + ' must stay disabled on an unclaimed position');
    }
    assert(el('btn-replay').disabled === true, '载入回放 must stay disabled as well');

    // A frame that hands the position a side unlocks the very same click.
    game.chooseSide('black');
    assert(game.playerColor === BLACK, 'choose_side claims the side');
    game.gameStatus = 'playing';
    game.refreshReplayControls();
    for (const id of nav) {
        assert(el(id).disabled === false, id + ' must be usable once a side is chosen');
    }
    game.showReplayMove(1);
    assert(game.replayIndex === 1, 'the same click enters replay after a side is chosen');
    game.exitReplay();
});

// B2/B3: btn-replay keeps its own stale-record rule, and the three disconnect
// texts are errors while a lost 上一局 stays a warning.
check('B2 btn-replay still follows only the stale judgment', () => {
    const game = newGame();
    game.setup.active = false;
    game.aiThinking = false;
    game.playerColor = BLACK;
    game.gameStatus = 'playing';
    game.moveHistory = [
        { row: 7, col: 7, color: BLACK },
        { row: 7, col: 8, color: WHITE },
    ];

    // Analysis shorter than the record: stale, so 载入回放 is offered.
    game.analysis = [{ method: 'alphabeta', score: 40, depth: 4, nodes: 100, elapsed_ms: 5 }];
    game.analysisEnded = false;
    game.refreshReplayControls();
    assert(el('btn-replay').disabled === false, 'a stale record keeps 载入回放 available');

    // Fully analysed game: not stale, so it is held - independent of the side.
    game.analysis = [
        { method: 'alphabeta', score: 40, depth: 4, nodes: 100, elapsed_ms: 5 },
        { method: 'alphabeta', score: -40, depth: 4, nodes: 100, elapsed_ms: 5 },
    ];
    game.analysisEnded = true;
    game.refreshReplayControls();
    assert(el('btn-replay').disabled === true, 'a fresh record has nothing to reload');

    // No side chosen at all on an empty board: the plain no-moves rule still holds.
    const fresh = newGame();
    fresh.setup.active = false;
    fresh.moveHistory = [];
    fresh.analysis = [];
    fresh.playerColor = null;
    fresh.gameStatus = 'waiting';
    fresh.refreshReplayControls();
    assert(el('btn-replay').disabled === true, 'no moves means no 载入回放');
    assert(el('btn-replay-first').disabled === true, 'no moves means no replay nav');
});

// B3: README:553 wants the reconnect countdown in amber (warn); only refused or
// lost-connection events are errors (red). 上一局找不回来了 stays a warning.
check('B3 disconnect statuses follow README colour rules', () => {
    const game = newGame();

    game.ws.readyState = bundle.sandbox.WebSocket.CLOSED;
    game.wsReconnect = null;
    game.scheduleReconnect();
    // README:553 explicitly prescribes amber for the reconnect countdown.
    assert(el('status').className === 'status warn',
           '重连倒计时 must be warn (amber), got ' + el('status').className);

    game.restart();
    assert(el('status').className === 'status error',
           '未连接，重开没有生效 must be an error, got ' + el('status').className);

    game.sendMessage({ type: 'undo' });
    assert(el('status').className === 'status error',
           '消息未发出 must be an error, got ' + el('status').className);

    const lost = newGame();
    lost.resumeAsked = true;
    lost.handleServerMessage({ type: 'session', token: null, resumed: false });
    assert(el('status').className === 'status warn',
           'a lost previous game is only a warning, got ' + el('status').className);
});

// B4: a typed 0 is out of range, not empty: the depth clamps to 2 and the
// timeout range follows it, instead of silently jumping to the default 8.
check('B4 typed depth zero clamps to two', () => {
    const game = newGame();
    const depthEl = el('depth');
    const timeoutEl = el('timeout');

    depthEl.value = '0';
    game.syncTimeoutBounds('min');
    assert(depthEl.value === '2', 'a typed 0 clamps to the minimum depth 2, got ' + depthEl.value);
    assert(timeoutEl.min === '4' && timeoutEl.max === '8',
           `the range must follow depth 2, got ${timeoutEl.min}..${timeoutEl.max}`);
    assert(timeoutEl.value === '4', 'min mode lands on the new minimum, got ' + timeoutEl.value);

    // Above the top still clamps to 20, and an emptied box still defaults to 8.
    depthEl.value = '25';
    game.syncTimeoutBounds('min');
    assert(depthEl.value === '20', '25 clamps to 20, got ' + depthEl.value);
    assert(timeoutEl.min === '40' && timeoutEl.max === '80',
           `the range must follow depth 20, got ${timeoutEl.min}..${timeoutEl.max}`);

    depthEl.value = '';
    game.syncTimeoutBounds('min');
    assert(depthEl.value === '8', 'an empty box falls back to the default 8, got ' + depthEl.value);
    assert(timeoutEl.min === '16' && timeoutEl.max === '32',
           `the range must follow depth 8, got ${timeoutEl.min}..${timeoutEl.max}`);
});

// B5: the curve clips at +/-2000, so only a proven win/loss earns a red dot.
check('B5 eval graph dots only for mate scores', () => {
    const game = newGame();
    const canvas = el('eval-graph');
    const ctx = canvas.getContext();
    let dots = [];
    let line = [];
    ctx.arc = (x, y, r) => { dots.push({ x, y, r }); };
    ctx.moveTo = (x, y) => { line.push({ x, y }); };
    ctx.lineTo = (x, y) => { line.push({ x, y }); };

    // The stub canvas has no clientWidth/clientHeight, so drawEvalGraph falls back
    // to 200x64: y = 32 - (clamped / 2000) * 26, i.e. 6 on the top clip edge and 58
    // on the bottom one. The mid axis is captured too, so each ply's point is found
    // by its own x = width * (i + 0.5) / n.
    const plot = (scores, colors) => {
        dots = [];
        line = [];
        game.moveHistory = colors.map(color => ({ row: 7, col: 7, color }));
        game.analysis = scores.map(score => ({
            method: 'alphabeta', score, depth: 4, nodes: 10, elapsed_ms: 1,
        }));
        game.drawEvalGraph();
        const ys = scores.map((unused, i) => {
            const want = 200 * (i + 0.5) / scores.length;
            const hit = line.filter(p => Math.abs(p.x - want) < 1e-9);
            assert(hit.length === 1, 'no single curve point at x=' + want);
            return hit[0].y;
        });
        return { dots: dots.slice(), ys };
    };

    const big = plot([3000], [BLACK]);
    assert(big.dots.length === 0, 'a big searched score earns no dot');
    assert(Math.abs(big.ys[0] - 6) < 1e-9, '+3000 sits clipped on the top edge, y=' + big.ys[0]);

    const mate = plot([99999], [BLACK]);
    assert(mate.dots.length === 1, '+99999 earns the win dot');
    assert(Math.abs(mate.ys[0] - 6) < 1e-9,
           'a mated point is pinned to the clip edge, not mid-scale, y=' + mate.ys[0]);

    const loss = plot([-100000], [BLACK]);
    assert(loss.dots.length === 1, '-100000 earns the loss dot');
    assert(Math.abs(loss.ys[0] - 58) < 1e-9,
           'a mated loss sits on the bottom edge, y=' + loss.ys[0]);

    // WHITE is plotted from BLACK's point of view, so a WHITE mate score mirrors
    // onto the opposite edge and still gets its dot.
    const white = plot([99999], [WHITE]);
    assert(white.dots.length === 1, 'a WHITE mate score still gets a dot');
    assert(Math.abs(white.ys[0] - 58) < 1e-9,
           'flipped it sits on the bottom edge, y=' + white.ys[0]);

    const mixed = plot([3000, 99999], [BLACK, BLACK]);
    assert(mixed.dots.length === 1, 'only the mated ply of a mixed run gets a dot');
    assert(Math.abs(mixed.dots[0].x - 150) < 1e-9, 'the dot belongs to the mated ply');
    assert(Math.abs(mixed.ys[0] - 6) < 1e-9 && Math.abs(mixed.ys[1] - 6) < 1e-9,
           'both clipped plies share the top edge, ys=' + mixed.ys.join(','));
});

// D4: README:289 allows 直接落子 after 摆棋 confirmation without claiming a
// side first. An empty waiting board still requires choose_side.
check('D4 click sends make_move on a pending position', () => {
    const game = newGame();
    let lastMsg = null;
    game.sendMessage = (msg) => { lastMsg = msg; return true; };

    // Pending position: waiting, no colour, base stones present.
    game.gameStatus = 'waiting';
    game.playerColor = null;
    game.baseStones = [{ row: 7, col: 7, color: BLACK }];
    // The live board must also carry the base stone - displayView() composes it,
    // but the click guard checks this.board directly.
    game.board[7][7] = BLACK;
    game.aiThinking = false;
    game.replayIndex = null;  // not in replay mode
    game.currentTurn = BLACK;  // server owns the turn until a side is claimed

    // Simulate a canvas click at cell (7,8) using the geometry constants.
    const pad = game.geom.pad;
    const cell = game.geom.cell;
    const x = pad + 8 * cell;  // column 8
    const y = pad + 7 * cell;  // row 7
    game.handleCanvasClick({ clientX: x, clientY: y });
    
    assert(lastMsg && lastMsg.type === 'make_move',
           'a pending position must send make_move, got ' + JSON.stringify(lastMsg));
    assert(lastMsg.row === 7 && lastMsg.col === 8, 'coordinates must match the click');

    // Empty waiting board: no base stones → still requires choose_side.
    const fresh = newGame();
    fresh.sendMessage = (msg) => { lastMsg = msg; return true; };
    fresh.gameStatus = 'waiting';
    fresh.playerColor = null;
    fresh.baseStones = [];
    fresh.aiThinking = false;
    fresh.replayIndex = null;
    fresh.currentTurn = BLACK;

    lastMsg = null;
    fresh.handleCanvasClick({ clientX: pad + 7 * cell, clientY: pad + 7 * cell });
    assert(lastMsg === null, 'empty waiting board must not send any move');

    // Guards remain intact: aiThinking blocks clicks.
    const thinking = newGame();
    thinking.sendMessage = (msg) => { lastMsg = msg; return true; };
    thinking.gameStatus = 'playing';
    thinking.playerColor = BLACK;
    thinking.baseStones = [];
    thinking.aiThinking = true;
    thinking.replayIndex = null;
    thinking.currentTurn = BLACK;

    lastMsg = null;
    thinking.handleCanvasClick({ clientX: pad + 7 * cell, clientY: pad + 7 * cell });
    assert(lastMsg === null, 'aiThinking must still block clicks');

    // Replay view blocks clicks.
    const replaying = newGame();
    replaying.sendMessage = (msg) => { lastMsg = msg; return true; };
    replaying.gameStatus = 'playing';
    replaying.playerColor = BLACK;
    replaying.baseStones = [];
    replaying.aiThinking = false;
    replaying.replayIndex = 0;  // in replay mode
    replaying.currentTurn = BLACK;

    lastMsg = null;
    replaying.handleCanvasClick({ clientX: pad + 7 * cell, clientY: pad + 7 * cell });
    assert(lastMsg === null, 'replay view must still block clicks');

    // F5: the first stone placed on a hand-set position claims the colour it was
    // played for. The server names that colour on the move_made frame, and the
    // page must take it up - staying waiting with no colour leaves 悔棋/求招 held
    // for the whole game and reads a black win as a loss.
    const claimed = newGame();
    claimed.sendMessage = () => true;
    claimed.gameStatus = 'waiting';
    claimed.playerColor = null;
    claimed.baseStones = [{ row: 7, col: 7, color: BLACK }];
    claimed.board[7][7] = BLACK;
    claimed.aiThinking = false;
    claimed.replayIndex = null;
    claimed.currentTurn = BLACK;
    claimed.moveHistory = [{ row: 7, col: 7, color: BLACK }];
    // Pre-state set here on purpose: the shared element map keeps whatever the
    // earlier sub-cases left, and this asserts the handler, not those.
    el('btn-undo').disabled = true;
    el('btn-hint').disabled = true;
    el('btn-resign').disabled = true;
    el('btn-black').disabled = false;
    el('btn-white').disabled = false;

    claimed.handleMoveMade({ row: 8, col: 8, color: 'black' });
    assert(claimed.playerColor === BLACK, 'the first stone claims its colour');
    assert(claimed.gameStatus === 'playing', 'and the game is playing');
    assert(el('btn-undo').disabled === false, '悔棋 comes back for the claimed side');
    assert(el('btn-black').disabled === true && el('btn-white').disabled === true,
           'the colour choice is over');

    // Anti-regression: an ordinary game that already holds a colour keeps it. A
    // white reply must not repaint the player's side or the status.
    const settled = newGame();
    settled.sendMessage = () => true;
    settled.gameStatus = 'playing';
    settled.playerColor = BLACK;
    settled.baseStones = [];
    settled.board[9][9] = WHITE;
    settled.aiThinking = true;
    settled.replayIndex = null;
    settled.currentTurn = BLACK;

    settled.handleMoveMade({ row: 9, col: 9, color: 'white' });
    assert(settled.playerColor === BLACK, 'an ordinary game keeps its colour');
    assert(settled.gameStatus === 'playing', 'and its status');
});

// H1: a second tab resuming the same token closes this socket. That close is not
// a dead network - reconnecting re-sends `resume` with the same token and takes
// the session back, once a second, with every handover halting the AI search.
// The takeover notice must stop the retry loop while keeping the token, so a
// manual refresh can still get the game back.
check('H1 a session takeover stops the reconnect loop', () => {
    // The frame the server sends the socket it is replacing. One constant: a
    // rename on the server side is a one-line change here.
    const TAKEOVER = 'session_taken_over';

    const sandbox = bundle.sandbox;
    const realSetTimeout = sandbox.setTimeout;
    const realClearTimeout = sandbox.clearTimeout;
    const armed = [];
    const cleared = [];
    sandbox.setTimeout = (fn, ms) => { armed.push({ fn, ms }); return 4242; };
    sandbox.clearTimeout = (id) => { cleared.push(id); };
    try {
        const game = newGame();  // its constructor already opened one socket
        const sent = [];
        game.sendMessage = (msg) => { sent.push(msg); return true; };
        game.sessionToken = 'a'.repeat(16);
        sandbox.localStorage.setItem('gomoku.session.token', game.sessionToken);
        game.wsAttempts = 0;
        // A retry already pending, so "dropped" below is not vacuous.
        game.wsReconnect = 4242;
        const closeHandler = game.ws.onclose;  // wired by the constructor's openSocket
        assert(typeof closeHandler === 'function', 'the socket must carry a close handler');

        const socketsBefore = bundle.sockets.length;
        game.handleServerMessage({ type: TAKEOVER, token: game.sessionToken });
        assert(game.wsReconnect === null, 'the pending reconnect timer must be dropped');

        // The server closes the socket right after the notice; the page must read
        // that close as the takeover, not as a lost connection.
        closeHandler();
        assert(armed.length === 0, 'no reconnect may be armed after a takeover');
        assert(bundle.sockets.length === socketsBefore, 'the socket must not be reopened');
        assert(sent.filter((m) => m.type === 'resume').length === 0,
               'no resume may go out after a takeover');

        // The token survives: a manual refresh still finds the game.
        assert(sandbox.localStorage.getItem('gomoku.session.token') === game.sessionToken,
               'the token must stay for a manual refresh');

        // Anti-false-positive: a plain drop (no notice) still reconnects.
        armed.length = 0;
        const normal = newGame();
        normal.sendMessage = () => true;
        const socketsBeforeNormal = bundle.sockets.length;
        normal.ws.onclose();
        assert(armed.length === 1, 'a plain drop still arms the reconnect');
        armed[0].fn();
        assert(bundle.sockets.length === socketsBeforeNormal + 1,
               'and the retry really reopens the socket');
    } finally {
        sandbox.setTimeout = realSetTimeout;
        sandbox.clearTimeout = realClearTimeout;
    }
});

// C2a: the side buttons answer "is a colour still owed?", not "is a game
// running?". A resumed frame that ends the game, or one already mid-play, must
// keep them greyed; a resumed frame waiting for a colour must hand them back.
check('C2 side buttons follow the pending-choice rule on every resumed status', () => {
    const session = (status, color) => ({
        type: 'session',
        token: 'd'.repeat(16),
        resumed: true,
        player_color: color,
        game_status: status,
        current_turn: 'black',
        move_count: 2,
        max_depth: 6,
        timeout_ms: 5000,
        winner: status === 'game_over' ? 'white' : '',
        reason: status === 'game_over' ? 'five' : '',
        board: emptyBoard(),
    });

    // waiting with no colour: a choice is pending, the buttons are the way on.
    const opening = newGame();
    opening.sendMessage = () => true;
    assert(opening.gameStatus === 'waiting' && opening.playerColor === null,
           'a fresh game waits for a colour');
    el('btn-black').disabled = true;
    el('btn-white').disabled = true;
    opening.handleServerMessage(session('waiting', ''));
    assert(el('btn-black').disabled === false, 'waiting without a colour opens 执黑');
    assert(el('btn-white').disabled === false, 'waiting without a colour opens 执白');

    // waiting with a colour already claimed: nothing is pending.
    const claimed = newGame();
    claimed.sendMessage = () => true;
    el('btn-black').disabled = false;
    el('btn-white').disabled = false;
    claimed.handleServerMessage(session('waiting', 'black'));
    assert(el('btn-black').disabled === true && el('btn-white').disabled === true,
           'an already claimed side keeps both buttons down');

    // playing with a colour: the ordinary mid-game resume.
    const playing = newGame();
    playing.sendMessage = () => true;
    el('btn-black').disabled = false;
    el('btn-white').disabled = false;
    playing.handleServerMessage(session('playing', 'black'));
    assert(el('btn-black').disabled === true && el('btn-white').disabled === true,
           'a running game takes no colour choice');

    // game over: the settled board takes no colour choice either.
    const over = newGame();
    over.sendMessage = () => true;
    el('btn-black').disabled = false;
    el('btn-white').disabled = false;
    over.handleServerMessage(session('game_over', 'black'));
    assert(el('btn-black').disabled === true && el('btn-white').disabled === true,
           'a finished game takes no colour choice');

    // The predicate the rule is built on, pinned on the two states it decides.
    assert(opening.sideChoicePending() === true, 'the waiting board is still pending');
    assert(over.sideChoicePending() === false, 'a finished game is not pending');
});

// C2b: 摆棋-confirmed awaiting colour is the same pending state, and the live
// ending puts the side buttons down where the resumed frame does.
check('C2 the ending locks the side buttons and 摆棋 keeps them open', () => {
    // After 确认 the page is waiting with no colour claimed: pending.
    const pos = newGame();
    pos.sendMessage = () => true;
    pos.board = emptyBoard();
    pos.board[7][7] = BLACK;
    pos.setup.active = true;
    pos.setup.board = pos.board.map((row) => row.slice());
    pos.handlePositionSet({ board: pos.board, current_turn: 'black' });
    assert(pos.gameStatus === 'waiting' && pos.playerColor === null,
           'position_set lands on waiting with no colour');
    assert(el('btn-black').disabled === false && el('btn-white').disabled === false,
           'the confirmed position shows the colour choice');

    // A live game_over frame must take the pair down. They start up here because
    // that is the state the ending has to clear: the ending is reachable without
    // any resume, and a colour click on a settled board would re-arm the game.
    const over = newGame();
    over.sendMessage = () => true;
    over.playerColor = BLACK;
    over.gameStatus = 'playing';
    el('btn-black').disabled = false;
    el('btn-white').disabled = false;
    over.handleServerMessage({ type: 'game_over', winner: 'white', reason: 'five' });
    assert(el('btn-black').disabled === true && el('btn-white').disabled === true,
           'handleGameOver locks both side buttons');

    // The predicate on the state the server hands back after 确认.
    assert(pos.sideChoicePending() === true, 'the confirmed position is still pending');
});

// C2c: a chooseSide that only arrives after the game settled - a stale frame is
// the only way it can reach the click handler, the ending greys the pair - must
// not re-arm 悔棋/求招/认输 nor drop an in-flight hint flag.
check('C2 chooseSide after game over does not re-arm the game', () => {
    const game = newGame();
    game.sendMessage = () => true;
    game.playerColor = null;
    game.gameStatus = 'game_over';
    game.hintInFlight = true;
    el('btn-undo').disabled = true;
    el('btn-hint').disabled = true;
    el('btn-resign').disabled = true;
    el('btn-black').disabled = false;
    el('btn-white').disabled = false;

    game.chooseSide('black');
    assert(game.playerColor === BLACK, 'the click still claims the colour');
    assert(el('btn-undo').disabled === true, 'a finished game keeps 悔棋 down');
    assert(el('btn-hint').disabled === true, 'a finished game keeps 求招 down');
    assert(el('btn-resign').disabled === true, 'a finished game keeps 认输 down');
    assert(game.hintInFlight === true, 'the in-flight hint is not silently cleared');

    // Control: the ordinary opening is still locally 'waiting' at click time, so
    // the same call must hand all three back.
    const fresh = newGame();
    fresh.sendMessage = () => true;
    fresh.hintInFlight = true;
    el('btn-undo').disabled = true;
    el('btn-hint').disabled = true;
    el('btn-resign').disabled = true;
    fresh.chooseSide('black');
    assert(fresh.gameStatus === 'waiting', 'the client is still waiting when it clicks');
    assert(el('btn-undo').disabled === false && el('btn-hint').disabled === false &&
           el('btn-resign').disabled === false, 'the opening re-arms all three');
    assert(fresh.hintInFlight === false, 'the opening releases the hint flag');
});

// C3a: a hint searched before 摆棋 was opened cannot describe the working copy.
// The answer is dropped whole - no ring, no replay move, no line over 摆棋中 -
// but the control it was holding comes back.
check('C3 a hint result during setup draws nothing and keeps 摆棋中', () => {
    const game = newGame();
    game.sendMessage = () => true;
    game.playerColor = BLACK;
    game.gameStatus = 'playing';
    game.board = emptyBoard();
    game.enterSetup();
    assert(game.setup.active === true, 'enterSetup arms the working copy');
    const setupStatus = el('status').textContent;
    assert(setupStatus.indexOf('摆棋中') === 0, 'the setup line is on screen');

    game.replayIndex = 3;
    game.hintInFlight = true;
    el('btn-hint').disabled = true;
    let rings = 0;
    game.highlightMove = () => { rings += 1; };

    game.handleHintResult({ row: 7, col: 7, score: 100 });
    assert(game.hintInFlight === false, 'the answer still releases the flag');
    assert(el('btn-hint').disabled === false, 'and 求招 comes back for the new position');
    assert(rings === 0, 'no ring may land on the setup board');
    assert(game.replayIndex === 3, 'the replay cursor must not move');
    assert(el('status').textContent === setupStatus, 'the 摆棋中 line stays');
});

// C3b: the control test above must not swallow the normal path - outside 摆棋
// the answer still marks the square and writes the status line.
check('C3 a hint result outside setup still marks the board', () => {
    const game = newGame();
    game.sendMessage = () => true;
    game.playerColor = BLACK;
    game.gameStatus = 'playing';
    game.board = emptyBoard();
    game.moveHistory = [];
    game.hintMoveCount = 0;
    game.replayIndex = null;
    game.hintInFlight = true;
    el('btn-hint').disabled = true;
    const seen = [];
    game.highlightMove = (row, col) => { seen.push([row, col]); };

    game.handleHintResult({ row: 6, col: 8, score: 123 });
    assert(seen.length === 1 && seen[0][0] === 6 && seen[0][1] === 8,
           'the ring still marks the suggested square');
    assert(el('status').textContent.indexOf('建议') === 0 &&
           el('status').textContent.indexOf('123') >= 0,
           'the status line reports the hint, got: ' + el('status').textContent);
    assert(game.hintInFlight === false && el('btn-hint').disabled === false,
           'and the control is released');

    // The stale-ply void still works: a different ply count says the searched
    // position is gone, so no ring and 建议已失效.
    const stale = newGame();
    stale.sendMessage = () => true;
    stale.playerColor = BLACK;
    stale.gameStatus = 'playing';
    stale.moveHistory = [{ row: 7, col: 7, color: BLACK }];
    stale.hintMoveCount = 0;
    let staleRings = 0;
    stale.highlightMove = () => { staleRings += 1; };
    stale.handleHintResult({ row: 5, col: 5, score: 9 });
    assert(staleRings === 0, 'a stale answer draws no ring');
    assert(el('status').textContent === '建议已失效', 'and reports the hint as void');
});

// V3: while 摆棋 is open its guidance owns the top bar (README:360). Rotating
// the view redraws the board and the view buttons, but must leave the amber
// 摆棋中 line exactly as 摆棋 wrote it.
check('V3 rotating the view during setup keeps the 摆棋中 guidance', () => {
    const game = newGame();
    game.enterSetup();
    assert(game.setup.active === true, 'setup must be open');
    const text = el('status').textContent;
    const kind = el('status').className;
    assert(kind === 'status warn', '摆棋 opens with the amber guidance, got: ' + kind);
    assert(text.indexOf('摆棋中') === 0, 'and the line is the 摆棋 guidance, got: ' + text);

    game.view.rot = 90;
    game.applyView();

    assert(el('status').textContent === text, 'a rotation must not rewrite the top bar');
    assert(el('status').className === kind, 'nor change its colour');
    assert(el('btn-view-rotate').textContent === '旋转 90°',
           'the rotate button still wears the angle it turned to, got: ' +
           el('btn-view-rotate').textContent);
});

// Anti-overreach for V3: outside setup the view line still writes the top bar,
// so the early return did not take the only feedback the rotation has.
check('V3 rotating the view outside setup still writes the 视角 line', () => {
    const game = newGame();
    game.setup.active = false;
    game.view.rot = 0;
    game.view.flipH = false;
    game.applyView();
    assert(el('status').textContent.indexOf('视角：') === 0,
           'a plain rotation narrates the view, got: ' + el('status').textContent);
    assert(el('status').className === 'status idle', 'in the narration colour');

    game.view.rot = 90;
    game.applyView();
    assert(el('status').textContent.indexOf('视角：') === 0 &&
           el('status').textContent.indexOf('旋转 90°') >= 0,
           'and it follows the new angle, got: ' + el('status').textContent);
    assert(el('status').className === 'status idle', 'still narration, not a warning');
    assert(el('btn-view-rotate').textContent === '旋转 90°', 'the button matches the view');
});

// W1: README:529-531 says the three pre-search fast paths forced / greedy_five /
// must_block answer without a depth or a node count, exactly like book / vcf /
// vct / greedy. formatAnalysis omits both suffixes whenever the field is falsy,
// so all seven labels must render as "<method> <score>" and keep only the
// timing suffix in the replay line.
check('W1 fast-path methods render without depth or node suffixes', () => {
    const game = newGame();
    const quick = ['book', 'vcf', 'vct', 'greedy', 'forced', 'greedy_five', 'must_block'];
    for (const method of quick) {
        const text = game.formatAnalysis(
            { method, score: 120, depth: 0, nodes: 0, elapsed_ms: 0 }, true);
        assert(text === `${method} 120`,
               `${method} must print as "<method> <score>", got: ${text}`);
        assert(!/\d+d(\s|$)/.test(text), `${method} carries no depth suffix, got: ${text}`);
        assert(!/\dk/.test(text), `${method} carries no node suffix, got: ${text}`);
    }

    // The searched path is the control: fill depth and nodes and both suffixes
    // appear, so the seven absences above come from the fast paths themselves.
    const searched = game.formatAnalysis(
        { method: 'alphabeta', score: 120, depth: 4, nodes: 1500, elapsed_ms: 0 }, true);
    assert(searched === 'alphabeta 120 4d 1.5k',
           'the searched path prints both suffixes, got: ' + searched);

    // Same through the replay line, where the timing suffix is shown: the method
    // name and its time arrive while the two fast-path suffixes stay missing.
    game.moveHistory = [{ row: 7, col: 7, color: BLACK }];
    game.analysis = [{ method: 'must_block', score: 120, depth: 0, nodes: 0, elapsed_ms: 7 }];
    game.replayIndex = 0;
    game.updateReplayInfo();
    const line = el('replay-info').textContent;
    assert(line.indexOf('must_block 120 7ms') > 0,
           'the replay line names the method and the time, got: ' + line);
    assert(!/\d+d(\s|$)/.test(line) && !/\dk/.test(line),
           'no depth or node suffix in the replay line, got: ' + line);
});

// W2: README:531 calls 99000 a defensive mark score, below the |score| >= 99999
// threshold that turns a number into a verdict. The must_block label itself is
// unaffected either way.
check('W2 the 99000 defensive mark stays a number, 100000 reads as a win', () => {
    const game = newGame();
    const defend = game.formatAnalysis(
        { method: 'must_block', score: 99000, depth: 0, nodes: 0, elapsed_ms: 0 }, true);
    assert(defend.indexOf('must_block') === 0, 'the label is printed, got: ' + defend);
    assert(defend.indexOf('99000') >= 0, 'and the score stays a number, got: ' + defend);
    assert(defend.indexOf('胜') < 0 && defend.indexOf('负') < 0,
           '99000 is not a proven win or loss, got: ' + defend);
    assert(defend === 'must_block 99000', 'exact line, got: ' + defend);

    const win = game.formatAnalysis(
        { method: 'must_block', score: 100000, depth: 0, nodes: 0, elapsed_ms: 0 }, true);
    assert(win === 'must_block 胜', '100000 clears the mate threshold, got: ' + win);
    const loss = game.formatAnalysis(
        { method: 'must_block', score: -100000, depth: 0, nodes: 0, elapsed_ms: 0 }, true);
    assert(loss === 'must_block 负', 'the mirrored score reads as a loss, got: ' + loss);
});

// W3: the curve is clipped at +/-2000 and only a saturated score earns the red
// dot (README:531), so the 99000 defensive mark sits pinned to the edge with no
// dot, while a mate score past the threshold draws one.
check('W3 the curve dots 100000 but not the 99000 defensive mark', () => {
    const game = newGame();
    const ctx = el('eval-graph').getContext();
    let dots = [];
    let line = [];
    ctx.arc = (x, y, r) => { dots.push({ x, y, r }); };
    ctx.moveTo = (x, y) => { line.push({ x, y }); };
    ctx.lineTo = (x, y) => { line.push({ x, y }); };

    // The stub canvas is 200x64, so y = 32 - (clamped / 2000) * 26: a score past
    // the clip sits on y = 6. Each ply's point is found by its own x.
    const plot = (scores, colors) => {
        dots = [];
        line = [];
        game.moveHistory = colors.map(color => ({ row: 7, col: 7, color }));
        game.analysis = scores.map(score => ({
            method: 'must_block', score, depth: 0, nodes: 0, elapsed_ms: 1,
        }));
        game.drawEvalGraph();
        const ys = scores.map((unused, i) => {
            const want = 200 * (i + 0.5) / scores.length;
            const hit = line.filter(p => Math.abs(p.x - want) < 1e-9);
            assert(hit.length === 1, 'no curve point at x=' + want);
            return hit[0].y;
        });
        return { dots: dots.slice(), ys };
    };

    const defend = plot([99000], [BLACK]);
    assert(defend.dots.length === 0, 'the defensive mark score earns no red dot');
    assert(Math.abs(defend.ys[0] - 6) < 1e-9,
           'and it sits clipped on the top edge, y=' + defend.ys[0]);

    const win = plot([100000], [BLACK]);
    assert(win.dots.length === 1, 'a score past the mate threshold earns the dot');
    assert(Math.abs(win.ys[0] - 6) < 1e-9, 'at the same clipped edge, y=' + win.ys[0]);

    const mixed = plot([99000, 100000], [BLACK, BLACK]);
    assert(mixed.dots.length === 1, 'only the mated ply of the pair is dotted');
    assert(Math.abs(mixed.dots[0].x - 150) < 1e-9, 'the dot belongs to the mated ply');
    assert(Math.abs(mixed.ys[0] - 6) < 1e-9 && Math.abs(mixed.ys[1] - 6) < 1e-9,
           'both clipped plies share the top edge, ys=' + mixed.ys.join(','));
});

// AA3: README:57 lists 耗时 as one of the five per-ply analysis fields, so a
// measure of 0 ms is information, not an absence. formatAnalysis gated the suffix
// on truthiness, which dropped every honest sub-millisecond 0 while the two
// round-92 pins above show that depth / nodes stay hidden when they are 0 - those
// two gates are deliberately unchanged, only the elapsed one reads != null.
check('AA3 a measured 0ms shows in the full line, depth and nodes still hide', () => {
    const game = newGame();

    const fast = game.formatAnalysis(
        { method: 'greedy_five', score: 100000, depth: 0, nodes: 0, elapsed_ms: 0 });
    assert(fast === 'greedy_five 胜 0ms', 'a 0 ms measurement is printed, got: ' + fast);

    const searched = game.formatAnalysis(
        { method: 'alphabeta', score: 120, depth: 4, nodes: 1500, elapsed_ms: 0 });
    assert(searched === 'alphabeta 120 4d 1.5k 0ms',
           'the searched line keeps both suffixes and gains 0ms, got: ' + searched);

    // An analysis that never measured anything at all keeps no suffix: the field
    // is absent, not zero.
    const unmeasured = game.formatAnalysis(
        { method: 'alphabeta', score: 120, depth: 4, nodes: 1500 });
    assert(unmeasured === 'alphabeta 120 4d 1.5k',
           'a missing elapsed_ms stays unprinted, got: ' + unmeasured);

    // The compact replay line has never shown the time, and does not start to.
    const compact = game.formatAnalysis(
        { method: 'greedy_five', score: 100000, depth: 0, nodes: 0, elapsed_ms: 0 }, true);
    assert(compact === 'greedy_five 胜', 'the compact line stays without a time, got: ' + compact);

    // A real measurement still reads the same as before the fix.
    const timed = game.formatAnalysis(
        { method: 'must_block', score: 120, depth: 0, nodes: 0, elapsed_ms: 7 });
    assert(timed === 'must_block 120 7ms', 'a nonzero time is unchanged, got: ' + timed);
});

// BB1: a resume the server could not honour still answers with the whole fresh
// session - README:486-488 says a missed resume opens a new game and that a
// session frame comes back either way, and README:502 shows that frame carrying
// the board, the side and the config. handleSession used to treat every
// resumed:false as "nothing to apply" and return, so the page kept painting the
// dead game: its stones, its side, its in-flight hint and its 摆棋. Only a frame
// the page asked for is authoritative; the echo a server sends when it opens a
// session on its own must still be ignored, and the resumed:true branch must
// behave exactly as before.
check('BB1 a missed resume lands the fresh session it was answered with', () => {
    const deadGame = (token) => {
        const game = newGame();
        const sent = [];
        game.sendMessage = (msg) => { sent.push(msg); return true; };
        const board = emptyBoard();
        board[7][7] = BLACK;
        board[8][8] = WHITE;
        game.board = board;
        game.moveHistory = [
            { row: 7, col: 7, color: BLACK },
            { row: 8, col: 8, color: WHITE },
        ];
        game.analysis = [{ method: 'alphabeta', score: 40, depth: 4, nodes: 100, elapsed_ms: 5 }];
        game.baseStones = [{ row: 2, col: 2, color: BLACK }];
        game.lastMove = { row: 8, col: 8 };
        game.playerColor = BLACK;
        game.gameStatus = 'playing';
        game.currentTurn = BLACK;
        game.aiThinking = true;
        game.hintInFlight = true;
        game.resumeAsked = true;
        game.setup.active = true;
        game.setup.confirmInFlight = true;
        el('btn-undo').disabled = false;
        el('btn-hint').disabled = false;
        el('btn-resign').disabled = false;
        el('btn-stop').disabled = false;
        game.handleServerMessage({
            type: 'session',
            token,
            resumed: false,
            player_color: '',
            game_status: 'waiting',
            current_turn: 'black',
            move_count: 0,
            max_depth: 8,
            timeout_ms: 5000,
            winner: '',
            reason: '',
            board: emptyBoard(),
        });
        return { game, sent };
    };

    const { game, sent } = deadGame('b'.repeat(16));
    assert(game.board.flat().every(v => v === 0), 'the dead board must be replaced');
    assert(game.moveHistory.length === 0, 'the dead move record must be dropped');
    assert(game.analysis.length === 0, 'the dead engine numbers must be dropped');
    assert(game.baseStones.length === 0, 'the dead base stones must be dropped');
    assert(game.lastMove === null, 'the last move of the dead game must go');
    assert(game.aiThinking === false, 'the dead game has no search running');
    assert(game.hintInFlight === false, 'a hint asked for on the dead game is off');
    assert(game.setup.active === false, '摆棋 cannot outlive the game it was opened on');
    assert(game.setup.confirmInFlight === false, 'the setup 确认 must be released too');
    assert(game.playerColor === null, 'the fresh session has no side claimed');
    assert(game.gameStatus === 'waiting', 'the fresh session is waiting');
    assert(game.sessionToken === 'b'.repeat(16), 'the new token is the one to keep');
    assert(game.resumeAsked === false, 'the ask is answered, so it is consumed');
    assert(game.resumeApplied === false, 'nothing was resumed, so no replay is owed');
    assert(el('btn-undo').disabled === true && el('btn-hint').disabled === true,
           'an unclaimed waiting game holds 悔棋 and 求招');
    assert(el('btn-resign').disabled === true, 'waiting is not a game to resign');
    assert(el('btn-stop').disabled === true, 'and has no search to stop');
    assert(el('status').textContent === '上一局找不回来了，已开新局',
           'the line still says the old game is gone, got: ' + el('status').textContent);
    assert(el('status').className === 'status warn',
           'at the warn level, got: ' + el('status').className);
    // The DOM hands an assigned number back as text, and the stub keeps whatever
    // type was written, so both are read as numbers.
    assert(Number(el('depth').value) === 8 && Number(el('timeout').value) === 5,
           'the fresh session carries its own AI settings, got: ' +
           JSON.stringify(el('depth').value) + '/' + JSON.stringify(el('timeout').value));
    assert(!sent.some(m => m && m.type === 'request_replay'),
           'an empty board has nothing to replay, so nothing is asked for');

    // The other half of the door: a session frame the page never asked for stays
    // an announcement about the server, not a rewrite of what the player is doing.
    const echo = newGame();
    const echoSent = [];
    echo.sendMessage = (msg) => { echoSent.push(msg); return true; };
    const live = emptyBoard();
    live[7][7] = BLACK;
    echo.board = live;
    echo.moveHistory = [{ row: 7, col: 7, color: BLACK }];
    echo.playerColor = BLACK;
    echo.gameStatus = 'playing';
    echo.resumeAsked = false;
    echo.handleServerMessage({
        type: 'session',
        token: 'c'.repeat(16),
        resumed: false,
        player_color: '',
        game_status: 'waiting',
        current_turn: 'black',
        move_count: 0,
        max_depth: 8,
        timeout_ms: 5000,
        winner: '',
        reason: '',
        board: emptyBoard(),
    });
    assert(echo.board[7][7] === BLACK, 'the echo must not wipe the local board');
    assert(echo.moveHistory.length === 1, 'nor the local move record');
    assert(echo.playerColor === BLACK, 'nor the side the player is committed to');
    assert(echo.gameStatus === 'playing', 'nor the status of the running game');
    assert(echoSent.length === 0, 'and it sends nothing in reply');

    // The resumed:true branch is untouched by the shared helper: it still asks
    // for the replay and flags the answer as a resume report.
    const resumed = newGame();
    const resumedSent = [];
    resumed.sendMessage = (msg) => { resumedSent.push(msg); return true; };
    const stones = emptyBoard();
    stones[7][7] = BLACK;
    resumed.handleServerMessage({
        type: 'session',
        token: 'd'.repeat(16),
        resumed: true,
        player_color: 'black',
        game_status: 'playing',
        current_turn: 'white',
        move_count: 1,
        max_depth: 6,
        timeout_ms: 5000,
        winner: '',
        reason: '',
        board: stones,
    });
    assert(resumed.board[7][7] === BLACK, 'a resumed board is still painted');
    assert(resumed.resumeApplied === true, 'the resume still flags its replay answer');
    assert(resumedSent.some(m => m && m.type === 'request_replay'),
           'and still asks for the replay');
    // BB1 (round 97) pinned 手 here; README:580 defines a session frame's move_count
    // as the stones on the board, so PP3 corrected the unit to 子 for this frame too.
    assert(el('status').textContent.indexOf('已恢复对局：1 子') >= 0,
           'with the resumed status line, got: ' + el('status').textContent);
});

// CC4 (round 98): 摆棋 owns the top bar while it is open, so a live frame that
// arrives underneath it must paint the board without overwriting the guidance.
// The writers here are ai_move and undo_success. README:379 states the rule with
// no exception: 摆棋期间顶栏归那条琥珀色指引, 不会把指引冲掉 - so game_over is held
// to it too (round 105 III3): an ending stores its text in gameOverText and
// exitSetup paints that back on the way out of the panel.
check('CC4 an ai_move during setup keeps 摆棋中 and still paints', () => {
    const game = newGame();
    game.sendMessage = () => true;
    game.playerColor = BLACK;
    game.gameStatus = 'playing';
    game.board = emptyBoard();
    game.moveHistory = [{ row: 7, col: 7, color: BLACK }];
    game.board[7][7] = BLACK;
    game.currentTurn = WHITE;
    game.enterSetup();
    assert(game.setup.active === true, 'enterSetup arms the panel');
    const setupStatus = el('status').textContent;
    assert(setupStatus.indexOf('摆棋中') === 0, 'the setup line is on screen');

    game.aiThinking = true;
    game.handleAIMove({ row: 8, col: 8, color: 'white', score: 1234, thinking_time_ms: 42 });
    assert(el('status').textContent === setupStatus,
           'the 摆棋中 line must survive an ai_move, got: ' + el('status').textContent);
    assert(el('status').className === 'status warn', 'and its level stays the setup one');
    assert(game.board[8][8] === WHITE, 'the stone still lands on the live board');
    assert(game.currentTurn === BLACK, 'the turn still flips');
    assert(game.moveHistory.length === 2, 'and the record still grows');
    assert(game.aiThinking === false, 'the flag still releases');
    assert(el('btn-stop').disabled === true, 'and 终止 goes back to sleep');
});

check('CC4 an undo_success during setup keeps 摆棋中 and still rebuilds', () => {
    const game = newGame();
    game.sendMessage = () => true;
    game.playerColor = BLACK;
    game.gameStatus = 'playing';
    game.baseStones = [];
    game.moveHistory = [
        { row: 7, col: 7, color: BLACK },
        { row: 8, col: 8, color: WHITE },
    ];
    game.board = emptyBoard();
    game.board[7][7] = BLACK;
    game.board[8][8] = WHITE;
    game.currentTurn = BLACK;
    game.enterSetup();
    const setupStatus = el('status').textContent;
    assert(setupStatus.indexOf('摆棋中') === 0, 'the setup line is on screen');

    game.handleUndoSuccess({ move_count: 0, game_status: 'waiting' });
    assert(el('status').textContent === setupStatus,
           'the 摆棋中 line must survive an undo, got: ' + el('status').textContent);
    assert(game.board[7][7] === 0 && game.board[8][8] === 0, 'the board still rewinds');
    assert(game.moveHistory.length === 0, 'and the record');
    assert(game.currentTurn === BLACK, 'and the turn comes back to black');
    assert(game.hintInFlight === false, 'and the hint control is handed back');
});

// The controls: with no panel open both writers still speak for themselves.
check('CC4 an ai_move outside setup still writes its own line', () => {
    const game = newGame();
    game.sendMessage = () => true;
    game.playerColor = BLACK;
    game.gameStatus = 'playing';
    game.board = emptyBoard();
    game.moveHistory = [{ row: 7, col: 7, color: BLACK }];
    game.currentTurn = WHITE;
    game.setup.active = false;

    game.handleAIMove({ row: 8, col: 8, color: 'white', score: 1234, thinking_time_ms: 42 });
    const line = el('status').textContent;
    assert(line.indexOf('AI ') === 0, 'the ai_move line is written, got: ' + line);
    assert(line.indexOf('1234') >= 0 && line.indexOf('42ms') >= 0,
           'with the score and the measured time, got: ' + line);
});

check('CC4 an undo_success outside setup still writes its own line', () => {
    const game = newGame();
    game.sendMessage = () => true;
    game.playerColor = BLACK;
    game.gameStatus = 'playing';
    game.baseStones = [];
    game.moveHistory = [{ row: 7, col: 7, color: BLACK }];
    game.board = emptyBoard();
    game.board[7][7] = BLACK;
    game.setup.active = false;

    game.handleUndoSuccess({ move_count: 0, game_status: 'waiting' });
    const line = el('status').textContent;
    assert(line.indexOf('已悔至空盘') === 0, 'the undo line is written, got: ' + line);
});

// GG2 (round 102): README:355/357 makes 确认 the only step that writes to the
// server and 取消 the way to drop the working copy, and a second 摆棋 click is
// that cancel; README:366 limits 清空 to the copy; README:377 keeps a refusal
// inside 摆棋 for a retry. While a 确认 round trip was in flight those three
// companions used to stay clickable, and handlePositionSet installed whatever
// came back - so a copy its player had already abandoned still landed on the
// board.
//
// The guards live in the click handlers, so this needs the recorded listeners
// (see makeElement). They are DOM-state and call-consequence assertions only:
// the harness sees no CSS and receives no real server frame, so nothing here
// speaks for the wire pipeline.
function fireClick(id) {
    const listeners = el(id)._listeners;
    for (let i = listeners.length - 1; i >= 0; --i) {
        if (listeners[i].type === 'click') {
            listeners[i].handler({ target: el(id), preventDefault() {} });
            return true;
        }
    }
    return false;
}

check('GG2 an in-flight 确认 holds its three companions too', () => {
    const game = newGame();
    game.board = emptyBoard();
    game.enterSetup();
    game.setup.confirmInFlight = true;
    game.refreshSetupControls();
    for (const id of ['btn-setup-confirm', 'btn-setup-cancel', 'btn-setup-clear',
                      'btn-position-toggle']) {
        assert(el(id).disabled === true, id + ' must be held while 确认 is in flight');
    }
});

check('GG2 the three click entries refuse to drop a copy 确认 is holding', () => {
    const game = newGame();
    game.board = emptyBoard();
    game.board[3][3] = BLACK;
    game.enterSetup();
    game.setup.board[5][5] = WHITE;
    game.setup.record.push({ row: 5, col: 5, color: WHITE });
    game.setup.confirmInFlight = true;
    game.refreshSetupControls();

    const copyBefore = JSON.stringify(game.setup.board);
    const recordBefore = JSON.stringify(game.setup.record);
    const sentBefore = game.ws ? game.ws.sent.length : 0;

    for (const id of ['btn-setup-cancel', 'btn-setup-clear', 'btn-position-toggle']) {
        assert(fireClick(id), id + ' registers a click handler');
        assert(game.setup.active === true, id + ' must leave 摆棋 open');
        assert(game.setup.confirmInFlight === true, id + ' must not release the flag');
        assert(JSON.stringify(game.setup.board) === copyBefore, id + ' must not touch the copy');
        assert(JSON.stringify(game.setup.record) === recordBefore, id + ' must not touch it');
        assert(game.board[3][3] === BLACK, id + ' must not touch the live board');
    }
    assert((game.ws ? game.ws.sent.length : 0) === sentBefore,
           'and none of the three writes to the server');
});

check('GG2 leaving setup hands the three companions back', () => {
    const game = newGame();
    game.board = emptyBoard();
    game.enterSetup();
    game.setup.confirmInFlight = true;
    game.refreshSetupControls();
    assert(el('btn-setup-cancel').disabled === true, 'held first');

    game.exitSetup(false);
    assert(game.setup.active === false, '摆棋 left');
    assert(game.setup.confirmInFlight === false, 'and the flag cleared');
    for (const id of ['btn-setup-cancel', 'btn-setup-clear', 'btn-position-toggle']) {
        assert(el(id).disabled === false, id + ' must not stay locked once it is over');
    }
});

check('GG2 with no round trip in flight the companions do their old jobs', () => {
    const game = newGame();
    game.board = emptyBoard();
    game.board[3][3] = BLACK;
    game.enterSetup();
    game.setup.board[5][5] = WHITE;
    game.setup.record.push({ row: 5, col: 5, color: WHITE });
    game.refreshSetupControls();
    for (const id of ['btn-setup-cancel', 'btn-setup-clear', 'btn-position-toggle']) {
        assert(el(id).disabled === false, id + ' must be clickable with nothing in flight');
    }
    const sentBefore = game.ws ? game.ws.sent.length : 0;

    // 清空 empties the copy only (README:366).
    assert(fireClick('btn-setup-clear'), 'btn-setup-clear registers a click handler');
    assert(game.setup.active === true, '清空 keeps 摆棋 open');
    assert(game.setup.board.every((row) => row.every((v) => v === 0)), 'the copy is emptied');
    assert(game.setup.record.length === 0, 'and its record with it');
    assert(game.board[3][3] === BLACK, 'the live board is untouched');

    // A second 摆棋 click is the cancel (README:357).
    assert(fireClick('btn-position-toggle'), 'btn-position-toggle registers a click handler');
    assert(game.setup.active === false, 'the toggle closes the panel it opened');
    assert(game.board[3][3] === BLACK, 'and the live board is what comes back');
    assert((game.ws ? game.ws.sent.length : 0) === sentBefore, 'neither click wrote to the server');
});

// HH (round 103): three more round-trip / lock rows. README:369 says 确认 writes
// *this reading* into the copy, so a 识别 answer for text the player has since
// edited must not become it; README:307 gives the top bar 轮到谁, which a finished
// game no longer has; README:310 keeps every 落子记录 row clickable, so the lock
// the replay cluster gets while the AI thinks has to be checked at the row too.
// Same caveat as GG2 - the harness sees no CSS and receives no real server frame,
// so everything below is DOM state and call consequence, never a claim about the
// wire pipeline.
function fireOn(element, type) {
    const listeners = element._listeners;
    for (let i = listeners.length - 1; i >= 0; --i) {
        if (listeners[i].type === type) {
            listeners[i].handler({ target: element, preventDefault() {} });
            return true;
        }
    }
    return false;
}

function fireEvent(id, type) {
    return fireOn(el(id), type);
}

// A recognised reading the server could answer with; only its ok flag and the
// shape renderImportState() reads matter here.
const okReading = () => {
    const board = emptyBoard();
    board[7][7] = BLACK;
    board[7][8] = WHITE;
    return { ok: true, board, encoding: { black: 'B', white: 'W', empty: '.' } };
};

check('HH1 an edited paste box drops the 识别 answer it was waiting for', () => {
    const game = newGame();
    game.openImportModal();
    el('position-text').value = '  BBBB  ';
    game.checkPosition();
    assert(game.setup.checkedText === 'BBBB', '识别 records the payload it actually sent');

    el('position-text').value = 'WWWW';
    assert(fireEvent('position-text', 'input'), 'the paste box has an input handler');
    const note = el('import-status').textContent;

    game.handlePositionPreview(okReading());
    assert(game.setup.preview === null, 'a reading for text nobody is looking at stays dropped');
    assert(el('btn-import-apply').disabled === true, 'and never re-arms 确认');
    assert(el('import-status').textContent === note, 'and never overwrites the changed-content line');
});

check('HH1 the answer for the text still in the box still writes the reading', () => {
    const game = newGame();
    game.openImportModal();
    el('position-text').value = 'BBBBWWWW';
    game.checkPosition();

    const reading = okReading();
    game.handlePositionPreview(reading);
    assert(game.setup.preview === reading, 'the current reading lands');
    assert(el('btn-import-apply').disabled === false, 'and 确认 is armed for it');
    assert(el('import-status').textContent !== '', 'with the panel line rewritten');
});

check('HH1 a stale refusal is dropped, a current one is shown', () => {
    const game = newGame();
    game.openImportModal();
    el('position-text').value = 'BBBB';
    game.checkPosition();

    el('position-text').value = 'WWWW';
    fireEvent('position-text', 'input');
    const note = el('import-status').textContent;

    game.handlePositionPreview({ ok: false, error: 'nope' });
    assert(game.setup.preview === null, 'the dropped refusal writes no preview');
    assert(el('import-status').textContent === note, 'and leaves the changed-content line alone');
    assert(el('btn-import-apply').disabled === true, 'and leaves 确认 disarmed');

    // Back to the text the request was sent with: the same refusal is now current
    // and must reach the panel, so the guard is a staleness test, not a mute.
    el('position-text').value = 'BBBB';
    game.handlePositionPreview({ ok: false, error: 'nope' });
    assert(el('import-status').textContent !== note, 'the current refusal is shown');
});

check('HH2 the ending rewrites the sub-line instead of keeping it', () => {
    const game = newGame();
    game.sendMessage = () => true;
    game.playerColor = BLACK;
    game.gameStatus = 'playing';
    game.baseStones = [];
    game.board = emptyBoard();
    game.moveHistory = [];
    game.currentTurn = BLACK;

    game.handleMoveMade({ row: 7, col: 7, color: 'black' });
    const before = el('turn-info').textContent;
    assert(before.indexOf('轮到') >= 0 && before.indexOf('思考中') >= 0,
           'the move_made line claims a turn and a thinking AI, got: ' + before);

    game.handleGameOver({ winner: 'white', reason: 'five' });
    const after = el('turn-info').textContent;
    assert(after !== before, 'the ending must rewrite it, got: ' + after);
    assert(after.indexOf('轮到') < 0 && after.indexOf('思考中') < 0,
           'and no longer name a side to move or a thinking AI, got: ' + after);
    assert(after === '', 'it is emptied, got: ' + after);
});

check('HH3 a 落子记录 row click is locked while the AI thinks', () => {
    const game = newGame();
    game.playerColor = BLACK;
    game.gameStatus = 'playing';
    game.board = emptyBoard();
    game.moveHistory = [{ row: 7, col: 7, color: BLACK }, { row: 8, col: 8, color: WHITE }];
    game.analysis = [];
    game.replayIndex = null;
    game.aiThinking = true;
    el('move-list')._children.length = 0;
    game.updateMoveList();
    const rows = el('move-list')._children;
    assert(rows.length === game.moveHistory.length, 'one row per ply is rendered');

    assert(fireOn(rows[0], 'click'), 'the row carries a click handler');
    assert(game.replayIndex === null, 'the click must not drop the live view');
    assert(el('replay-info').textContent === '', 'and the sub-line still shows no replayed ply');
    assert(el('btn-replay-exit').disabled === true, 'the way out still says there is nothing to exit');
});

check('HH3 with the AI not thinking the same row click replays ply N', () => {
    const game = newGame();
    game.playerColor = BLACK;
    game.gameStatus = 'playing';
    game.board = emptyBoard();
    game.moveHistory = [{ row: 7, col: 7, color: BLACK }, { row: 8, col: 8, color: WHITE }];
    game.analysis = [];
    game.replayIndex = null;
    game.aiThinking = false;
    el('move-list')._children.length = 0;
    game.updateMoveList();
    const rows = el('move-list')._children;

    assert(fireOn(rows[0], 'click'), 'the row carries a click handler');
    assert(game.replayIndex === 0, 'the click enters the first ply');
    assert(el('replay-info').textContent !== '', 'and the sub-line names it');
    assert(el('btn-replay-exit').disabled === false, 'with the way out armed');

    // The nav cluster shares the same entry point, so prove the lock did not take
    // it away either while nothing is thinking.
    assert(fireClick('btn-replay-last'), 'btn-replay-last registers a click handler');
    assert(game.replayIndex === game.moveHistory.length - 1, 'and still steps to the last ply');
});

check('HH3 the lock leaves the way out of a replay open', () => {
    const game = newGame();
    game.playerColor = BLACK;
    game.gameStatus = 'playing';
    game.board = emptyBoard();
    game.moveHistory = [{ row: 7, col: 7, color: BLACK }, { row: 8, col: 8, color: WHITE }];
    game.analysis = [];
    game.aiThinking = true;
    game.replayIndex = 1;

    game.exitReplay();
    assert(game.replayIndex === null, 'exitReplay is not the locked path');
    assert(el('replay-info').textContent === '', 'and the live view comes back');
    assert(fireClick('btn-replay-exit'), 'btn-replay-exit registers a click handler');
    assert(game.replayIndex === null, 'staying out of the replay is fine');
});

// III (round 105): the 确认 lock had three doors that could never unlock it.
// README:567 says a takeover sends the frame to the old socket and then CLOSES it,
// so the error frame that normally releases the lock can never reach this page
// (III1), and the board itself stayed paintable while 确认 was in flight (III2).
// README:379 gives the top line to the 摆棋 guidance for as long as the copy is
// open, and README:378 keeps the player inside 摆棋 after a refusal, so an ending
// that lands mid-setup is stored, not painted over the guidance (III3).
// Same caveat as GG2/HH - no CSS and no real server frame here, only DOM state
// and the consequence of a call.
const setupDuo = ['btn-setup-confirm', 'btn-setup-cancel', 'btn-setup-clear',
                  'btn-position-toggle'];

check('III1 a takeover releases the 确认 lock its closed socket cannot clear', () => {
    const game = newGame();
    game.board = emptyBoard();
    game.enterSetup();
    game.setup.board[5][5] = BLACK;
    game.setup.record.push({ row: 5, col: 5, color: BLACK });
    game.setup.preview = okReading();
    game.setup.confirmInFlight = true;
    game.refreshSetupControls();
    for (const id of setupDuo) {
        assert(el(id).disabled === true, id + ' is held before the frame');
    }

    game.handleServerMessage({ type: 'session_taken_over', token: 'ffffffffffffffff' });

    assert(game.setup.confirmInFlight === false, 'the round trip is over');
    for (const id of setupDuo) {
        assert(el(id).disabled === false, id + ' must not stay grey with no socket to answer');
    }
    // Parked, not discarded: the copy the player was holding is still there.
    assert(game.setup.active === true, 'the takeover does not leave 摆棋');
    assert(game.setup.board[5][5] === BLACK, 'the copy keeps its stone');
    assert(game.setup.record.length === 1, 'and its record');
    assert(game.setup.preview !== null, 'and its reading');
});

check('III1 with no round trip in flight the same frame changes no setup control', () => {
    const game = newGame();
    game.board = emptyBoard();
    game.enterSetup();
    game.setup.confirmInFlight = false;
    game.refreshSetupControls();
    for (const id of setupDuo) {
        assert(el(id).disabled === false, id + ' is clickable with nothing in flight');
    }

    game.handleServerMessage({ type: 'session_taken_over', token: 'eeeeeeeeeeeeeeee' });

    assert(game.setup.confirmInFlight === false, 'the flag stays cleared');
    for (const id of setupDuo) {
        assert(el(id).disabled === false, id + ' stays clickable');
    }
    assert(game.setup.active === true, 'and 摆棋 is still open');
});

check('III2 the board cannot be edited while 确认 holds the copy', () => {
    const game = newGame();
    game.board = emptyBoard();
    game.enterSetup();
    const pad = game.geom.pad;
    const cell = game.geom.cell;
    const spot = (row, col) => ({ clientX: pad + col * cell, clientY: pad + row * cell });

    // The entry point really reaches the copy before it is locked, so the assert
    // below is about the lock and not about a click that does nothing anyway.
    game.handleCanvasClick(spot(2, 2));
    assert(game.setup.board[2][2] === BLACK, 'a free click paints the copy');
    assert(game.setup.record.length === 1, 'and records it');

    game.setup.confirmInFlight = true;
    game.refreshSetupControls();
    const copyBefore = JSON.stringify(game.setup.board);
    const recordBefore = JSON.stringify(game.setup.record);

    game.handleCanvasClick(spot(4, 4));
    assert(game.setup.board[4][4] === 0, 'the locked click paints nothing');
    assert(JSON.stringify(game.setup.board) === copyBefore, 'the copy is untouched, cell by cell');
    assert(JSON.stringify(game.setup.record) === recordBefore, 'and so is its record');
    assert(game.setup.confirmInFlight === true, 'the click does not release the flag either');

    // The cell already painted is still editable-looking data, unchanged: the guard
    // adds a lock, it does not rewrite what the player set up.
    assert(game.setup.board[2][2] === BLACK, 'the earlier stone is still there');
});

check('III2 with the copy free the same click still paints', () => {
    const game = newGame();
    game.board = emptyBoard();
    game.enterSetup();
    const pad = game.geom.pad;
    const cell = game.geom.cell;
    game.setup.confirmInFlight = false;

    game.handleCanvasClick({ clientX: pad + 4 * cell, clientY: pad + 4 * cell });
    assert(game.setup.board[4][4] === BLACK, 'an unlocked click still edits the copy');
    assert(game.setup.record.length === 1, 'and its record');
});

check('III3 an ending during 摆棋 waits in gameOverText instead of the guidance', () => {
    const game = newGame();
    game.sendMessage = () => true;
    game.playerColor = BLACK;
    game.gameStatus = 'playing';
    game.currentTurn = BLACK;
    game.board = emptyBoard();
    game.moveHistory = [];
    game.baseStones = [];
    game.replayIndex = null;
    game.enterSetup();

    const guidance = el('status').textContent;
    assert(guidance !== '', '摆棋 put its own line on the top bar');
    assert(game.gameOverText === '', 'and nothing is stored yet');

    game.handleGameOver({ winner: 'white', reason: 'five' });
    assert(game.gameStatus === 'game_over', 'the ending still lands in the state');
    assert(game.gameOverText !== '', 'and is stored for the way out');
    assert(el('status').textContent === guidance, 'the guidance is not overwritten, got: ' +
           el('status').textContent);
    assert(el('status').textContent !== game.gameOverText, 'the top bar is still not the result');

    game.exitSetup(false);
    assert(el('status').textContent === game.gameOverText,
           'dropping the copy paints the result it owed, got: ' + el('status').textContent);
});

check('III3 an ending with no 摆棋 open still writes the top bar at once', () => {
    const game = newGame();
    game.sendMessage = () => true;
    game.playerColor = BLACK;
    game.gameStatus = 'playing';
    game.currentTurn = BLACK;
    game.board = emptyBoard();
    game.moveHistory = [];
    game.baseStones = [];
    game.replayIndex = null;
    game.setup.active = false;
    el('status').textContent = '';

    game.handleGameOver({ winner: 'white', reason: 'five' });
    assert(game.setup.active === false, 'no setup was opened by the frame');
    assert(el('status').textContent === game.gameOverText,
           'the result is on screen immediately, got: ' + el('status').textContent);

    // And a plain cancel with no ending waiting keeps its own line: the new branch
    // only speaks when a result was stored.
    const fresh = newGame();
    fresh.board = emptyBoard();
    fresh.gameStatus = 'playing';
    fresh.enterSetup();
    assert(fresh.gameOverText === '', 'this one never ended');
    fresh.exitSetup(false);
    assert(el('status').textContent !== '', 'cancel still says something');
    assert(el('status').textContent !== fresh.gameOverText, 'and it is not an empty result line');
});

// JJ1 (round 106): README:326-327 promises the 求招 answer is 标在棋盘上, and that a
// position which gains another stone retires that answer. The ring used to be painted
// by highlightMove alone, one frame deep in the canvas, so any later redraw - 旋转 /
// 镜像, a stone landing, a status repaint - dropped it while the top bar still read
// 建议 H8. The fix keeps it as display state (this.recommendation) painted inside
// drawBoard, so the only honest probe is a repaint that never re-runs highlightMove.
// The harness cannot see CSS, but the board ctx is observable: the ring is the one
// stroke whose style is #c0392b at radius geom.stone + 3.
const ringRecorder = () => {
    const ctx = el('board').getContext();
    const marks = [];
    let pending = null;
    ctx.beginPath = () => { pending = null; };
    ctx.arc = (x, y, r) => { pending = { x, y, r }; };
    ctx.stroke = () => {
        if (pending) {
            marks.push({ r: pending.r, style: String(ctx.strokeStyle), width: ctx.lineWidth });
        }
        pending = null;
    };
    return {
        marks,
        clear: () => { marks.length = 0; pending = null; },
        restore: () => {
            ctx.beginPath = () => {};
            ctx.arc = () => {};
            ctx.stroke = () => {};
        },
    };
};

const adviceRings = (rec, game) =>
    rec.marks.filter(m => m.style === '#c0392b' &&
                         Math.abs(m.r - (game.geom.stone + 3)) < 1e-9 &&
                         m.width === 2);

const adviceGame = () => {
    const game = newGame();
    game.sendMessage = () => true;
    game.playerColor = BLACK;
    game.gameStatus = 'playing';
    game.currentTurn = BLACK;
    game.aiThinking = false;
    game.board = emptyBoard();
    game.baseStones = [];
    game.moveHistory = [];
    game.analysis = [];
    game.replayIndex = null;
    game.setup.active = false;
    game.hintInFlight = true;
    game.hintMoveCount = 0;
    return game;
};

check('JJ1 the 建议环 survives a repaint that never re-asks 求招', () => {
    const game = adviceGame();
    const rec = ringRecorder();
    try {
        game.handleServerMessage({ type: 'hint_result', row: 7, col: 7, score: 1234 });
        assert(adviceRings(rec, game).length === 1, 'the answer paints its ring once');
        rec.clear();

        // 旋转 is display-only (README:379) and goes through applyView -> drawBoard,
        // never through highlightMove: this is the repaint that used to lose the ring.
        assert(fireClick('btn-view-rotate'), 'btn-view-rotate registers a click handler');
        const after = adviceRings(rec, game);
        assert(after.length === 1,
               'the ring the player was promised is still on the board after a repaint, got ' +
               after.length);
        assert(el('status').textContent.indexOf('视角') === 0,
               'and the repaint really was the view one, got: ' + el('status').textContent);
    } finally {
        rec.restore();
    }
});

check('JJ1 a repaint with no 求招 in the game draws no ring', () => {
    const game = adviceGame();
    const rec = ringRecorder();
    try {
        rec.clear();
        assert(fireClick('btn-view-rotate'), 'btn-view-rotate registers a click handler');
        assert(adviceRings(rec, game).length === 0, 'a board nobody asked advice about stays plain');
    } finally {
        rec.restore();
    }
});

check('JJ1 the next stone retires the 建议环', () => {
    const game = adviceGame();
    const rec = ringRecorder();
    try {
        game.handleServerMessage({ type: 'hint_result', row: 7, col: 7, score: 1234 });
        assert(adviceRings(rec, game).length === 1, 'the answer is on the board first');
        rec.clear();

        // 局面又落一手，这一份建议作废 (README:327) - move_made is the frame that places it.
        game.handleMoveMade({ row: 8, col: 8, color: 'white' });
        assert(adviceRings(rec, game).length === 0, 'the stone repaints no retired ring');
        rec.clear();
        assert(fireClick('btn-view-rotate'), 'btn-view-rotate registers a click handler');
        assert(adviceRings(rec, game).length === 0, 'and a later repaint does not bring it back');
        assert(game.moveHistory.length === 1, 'the stone itself landed');
    } finally {
        rec.restore();
    }
});

// JJ3 (round 106): README:520-521 makes state_sync the authoritative opening frame -
// the client redraws the board from it and adopts its turn. It used to blank
// replayIndex while leaving the 自动 interval armed, so the replay clock kept running
// against a record that no longer exists; the next tick came back as "past the last
// ply", called exitReplay and overwrote 对局开始 with 已回到当前局面. The harness cannot
// wait for a real 700 ms tick, so it records what setInterval was handed and, exactly
// as a browser would, fires every interval the page has not cleared.
check('JJ3 state_sync releases the replay clock it just invalidated', () => {
    const sandbox = bundle.sandbox;
    const realSetInterval = sandbox.setInterval;
    const realClearInterval = sandbox.clearInterval;
    const timers = [];
    const cleared = [];
    sandbox.setInterval = (fn) => { timers.push(fn); return timers.length; };
    sandbox.clearInterval = (id) => {
        if (id) { cleared.push(id); timers[id - 1] = null; }
    };
    try {
        const game = newGame();
        game.playerColor = BLACK;
        game.gameStatus = 'playing';
        game.aiThinking = false;
        game.board = emptyBoard();
        game.baseStones = [];
        game.analysis = [];
        game.moveHistory = [
            { row: 7, col: 7, color: BLACK },
            { row: 8, col: 8, color: WHITE },
            { row: 6, col: 6, color: BLACK },
        ];
        game.replayIndex = 0;
        game.toggleReplayAuto();
        const armed = game.replayTimer;
        assert(typeof armed === 'number' && armed > 0, '自动 armed one interval');
        const tick = timers[armed - 1];
        assert(typeof tick === 'function', 'and its tick is on the clock');
        assert(el('btn-replay-play').textContent === '暂停', 'the control reads as playing');

        game.handleServerMessage({ type: 'state_sync', board: emptyBoard(), current_turn: 'black' });

        assert(el('status').textContent.indexOf('对局开始') === 0,
               'the authoritative frame wrote its own line, got: ' + el('status').textContent);

        // A browser can only fire an interval the page has not cleared, so the orphan
        // tick is driven exactly when it is still on the clock - which is the whole
        // defect: the old frame left it armed against a record that no longer exists.
        if (cleared.indexOf(armed) === -1) tick();
        assert(el('status').textContent.indexOf('对局开始') === 0,
               'no orphaned replay tick rewrites the opening line, got: ' +
               el('status').textContent);

        assert(cleared.indexOf(armed) >= 0, 'state_sync must clear the interval it orphaned');
        assert(game.replayTimer === null, 'and the page must stop owning a replay clock');
        assert(el('btn-replay-play').textContent === '自动', 'and hand 自动 back to the player');
    } finally {
        sandbox.setInterval = realSetInterval;
        sandbox.clearInterval = realClearInterval;
    }
});

check('JJ3 a state_sync with no replay running needs no release', () => {
    const sandbox = bundle.sandbox;
    const realSetInterval = sandbox.setInterval;
    const realClearInterval = sandbox.clearInterval;
    const timers = [];
    const cleared = [];
    sandbox.setInterval = (fn) => { timers.push(fn); return timers.length; };
    sandbox.clearInterval = (id) => { if (id) { cleared.push(id); timers[id - 1] = null; } };
    try {
        const game = newGame();
        game.playerColor = BLACK;
        game.gameStatus = 'playing';
        game.aiThinking = false;
        game.board = emptyBoard();
        game.baseStones = [];
        game.moveHistory = [{ row: 7, col: 7, color: BLACK }];
        game.analysis = [];
        game.replayIndex = null;
        game.replayTimer = null;
        const before = cleared.length;

        game.handleServerMessage({ type: 'state_sync', board: emptyBoard(), current_turn: 'black' });

        assert(el('status').textContent.indexOf('对局开始') === 0,
               'the opening line still stands with no clock to fight it, got: ' +
               el('status').textContent);
        assert(cleared.length === before, 'nothing was armed, so nothing was cancelled');
        assert(game.replayTimer === null, 'and no replay clock appeared');
    } finally {
        sandbox.setInterval = realSetInterval;
        sandbox.clearInterval = realClearInterval;
    }
});

// Round 108 LL, three frontend rows.
//
// LL3 (B) README:360 makes the bottom bar's second line 摆棋's running count, and 清空 is a
// step inside 摆棋, not an exit from it. clearSetupBoard used to write its note through
// updatePositionStatus, which replaces the whole line, so the player who just emptied the
// board lost the count they are supposed to set the next stone by. The fix routes the note
// through renderSetupPanel(note), which prepends it to the count it recomputes.
//
// LL4 (C/D) README:326-327 places 求招's answer on 当前局面, and round 106 JJ1 made that
// answer display state painted by drawBoard. Two views are not the current position - 摆棋's
// working copy (displayView, :1941) and any replay ply - so the ring must not be painted on
// either; an undo changes the position outright, so the answer is void. The first two cases
// below prove the ring is held back, the third proves holding it back does not throw it away.
//
// LL5 (E) handleThinkingProgress greys the whole replay cluster with aiThinking (:821), and
// stopThinking restored the other three controls but not that one, so 终止 left 逐手分析 and
// its nav dead until the next frame arrived.
check('LL3 清空 keeps 摆棋的计数行在提示之后', () => {
    const game = newGame();
    game.board = emptyBoard();
    game.enterSetup();
    const pad = game.geom.pad;
    const cell = game.geom.cell;
    const spot = (row, col) => ({ clientX: pad + col * cell, clientY: pad + row * cell });

    // A cell cycles 空 → 黑 → 白 → 空 (paintSetupCell, :1185), so two clicks on (3,3) and
    // one on (4,4) is the cheapest position with a stone of each colour.
    game.handleCanvasClick(spot(3, 3));
    game.handleCanvasClick(spot(4, 4));
    game.handleCanvasClick(spot(3, 3));
    const painted = el('position-status').textContent;
    assert(painted.indexOf('摆棋中：黑 1 子、白 1 子') === 0,
           "the count line is the panel's own output before 清空, got: " + painted);

    assert(fireClick('btn-setup-clear'), 'btn-setup-clear registers a click handler');
    const line = el('position-status').textContent;
    assert(line.indexOf('棋盘已清空') === 0, 'the note still reaches the player, got: ' + line);
    assert(line.indexOf('摆棋中：黑 0 子、白 0 子') !== -1,
           "and README:360's count line stays with it, got: " + line);
});

check('LL3 清空 still empties the working copy it counts', () => {
    const game = newGame();
    game.board = emptyBoard();
    game.enterSetup();
    const pad = game.geom.pad;
    const cell = game.geom.cell;
    game.handleCanvasClick({ clientX: pad + 3 * cell, clientY: pad + 3 * cell });
    game.handleCanvasClick({ clientX: pad + 4 * cell, clientY: pad + 4 * cell });

    assert(fireClick('btn-setup-clear'), 'btn-setup-clear registers a click handler');
    assert(game.setup.active === true, 'the panel is still open - this is a step, not an exit');
    assert(game.setup.record.length === 0, "the copy's record is empty");
    const stones = game.setup.board.flat().filter((v) => v !== 0).length;
    assert(stones === 0, 'and no stone is left on it, got ' + stones);

    // The count line is recomputed from the copy, so a stone painted after 清空 reads 1 again.
    game.handleCanvasClick({ clientX: pad + 5 * cell, clientY: pad + 5 * cell });
    assert(el('position-status').textContent.indexOf('摆棋中：黑 1 子、白 0 子') === 0,
           'a later paint restores its own count, got: ' + el('position-status').textContent);
});

check('LL4a 摆棋的副本上不画上一局的建议环', () => {
    const game = adviceGame();
    const rec = ringRecorder();
    try {
        game.handleServerMessage({ type: 'hint_result', row: 7, col: 7, score: 1234 });
        assert(adviceRings(rec, game).length === 1, 'the live board shows the answer');
        rec.clear();

        game.enterSetup();
        const rings = adviceRings(rec, game).length;
        assert(rings === 0, 'the 摆棋 repaint draws no ring over the working copy, got ' + rings);
    } finally {
        rec.restore();
    }
});

check('LL4a a replay ply paints no answer for a position it is not', () => {
    const game = adviceGame();
    game.moveHistory = [
        { row: 7, col: 7, color: BLACK },
        { row: 8, col: 8, color: WHITE },
    ];
    game.hintMoveCount = 2;
    const rec = ringRecorder();
    try {
        game.handleServerMessage({ type: 'hint_result', row: 0, col: 0, score: 5 });
        assert(adviceRings(rec, game).length === 1, 'the live board shows the answer');
        rec.clear();

        game.showReplayMove(0);
        assert(game.replayIndex === 0, 'the view really moved onto the first ply');
        const rings = adviceRings(rec, game).length;
        assert(rings === 0, 'the replay repaint draws no ring on a historical board, got ' + rings);
    } finally {
        rec.restore();
    }
});

check('LL4a holding the ring back keeps the answer, and the live view gets it back', () => {
    const game = adviceGame();
    game.moveHistory = [{ row: 7, col: 7, color: BLACK }];
    game.hintMoveCount = 1;
    const rec = ringRecorder();
    try {
        game.handleServerMessage({ type: 'hint_result', row: 7, col: 7, score: 1234 });
        rec.clear();

        // Suppressed, not destroyed - neither of the two hidden views touches the display
        // state, and returning to the live position repaints it. This half is behaviour the
        // page already had, so it holds on both trees; it is what stops the gates above from
        // being a deletion dressed as a guard.
        game.enterSetup();
        assert(game.recommendation !== null && game.recommendation.row === 7 &&
               game.recommendation.col === 7, '摆棋 does not throw the answer away');
        // Clear after the copy is open: at HEAD that repaint is itself the defect, and this
        // case must count only the repaint that is supposed to bring the ring back.
        rec.clear();
        game.exitSetup(false);
        assert(adviceRings(rec, game).length === 1, 'and leaving 摆棋 brings the ring back');

        game.showReplayMove(0);
        assert(game.recommendation !== null, 'nor does a replay view');
        rec.clear();
        game.exitReplay();
        assert(adviceRings(rec, game).length === 1, 'and exiting the replay paints it again');
    } finally {
        rec.restore();
    }
});

check('LL4b an undo retires the answer whose position it popped', () => {
    const game = adviceGame();
    game.moveHistory = [{ row: 7, col: 7, color: BLACK }];
    game.hintMoveCount = 1;
    const rec = ringRecorder();
    try {
        game.handleServerMessage({ type: 'hint_result', row: 8, col: 8, score: 9 });
        assert(adviceRings(rec, game).length === 1, 'the answer is on the live board first');
        rec.clear();

        game.handleUndoSuccess({ move_count: 0, game_status: 'playing' });
        assert(game.recommendation === null, '悔棋 changes the position, so the answer is void');
        const rings = adviceRings(rec, game).length;
        assert(rings === 0, 'and the repaint it triggers draws no ring, got ' + rings);
    } finally {
        rec.restore();
    }
});

check('LL4b the same undo still leaves everything it does not cover', () => {
    const game = adviceGame();
    game.moveHistory = [
        { row: 7, col: 7, color: BLACK },
        { row: 8, col: 8, color: WHITE },
    ];
    game.analysis = [{ method: 'alphabeta', score: 40, depth: 4, nodes: 100, elapsed_ms: 5 }];
    game.hintMoveCount = 2;
    game.handleServerMessage({ type: 'hint_result', row: 0, col: 0, score: 9 });
    game.handleUndoSuccess({ move_count: 1, game_status: 'playing' });

    assert(game.moveHistory.length === 1, 'one ply was popped');
    assert(game.analysis.length === 1,
           'the loaded record it does not reach survives, got ' + game.analysis.length);
    assert(game.replayIndex === null, 'and the replay view exits the way it always did');
    assert(game.board[8][8] === 0, 'the popped stone left the board');
});

check('LL5 终止 hands the replay cluster back with the other controls', () => {
    const game = newGame();
    game.sendMessage = () => true;
    game.playerColor = BLACK;
    game.gameStatus = 'playing';
    game.setup.active = false;
    game.moveHistory = [
        { row: 7, col: 7, color: BLACK },
        { row: 8, col: 8, color: WHITE },
    ];
    game.analysis = [];
    game.replayIndex = null;
    game.aiThinking = true;
    game.refreshReplayControls();
    assert(el('btn-replay-play').disabled === true, '思考中 the cluster is greyed');

    assert(fireClick('btn-stop'), 'btn-stop registers a click handler');
    assert(game.aiThinking === false, 'and 终止 cleared the flag it was greyed for');
    assert(el('btn-replay-play').disabled === false,
           'so the cluster comes back too, got disabled=' + el('btn-replay-play').disabled);
    assert(el('btn-replay').disabled === false, 'and 载入回放 with it');
    assert(el('btn-replay-first').disabled === false, 'and the whole nav group');
});

check('LL5 the greying 终止 undoes is the one handleThinkingProgress arms', () => {
    const game = newGame();
    game.playerColor = BLACK;
    game.gameStatus = 'playing';
    game.setup.active = false;
    game.moveHistory = [{ row: 7, col: 7, color: BLACK }];
    game.analysis = [];
    game.replayIndex = null;

    // Green on both trees: aiThinking is the switch for this cluster, so the control
    // LL5 above asserts is genuinely the one that was left stuck.
    game.aiThinking = true;
    game.refreshReplayControls();
    assert(el('btn-replay-play').disabled === true, 'thinking greys the cluster');
    game.aiThinking = false;
    game.refreshReplayControls();
    assert(el('btn-replay-play').disabled === false, 'and a false flag releases it');
    game.aiThinking = false;
    game.moveHistory = [];
    game.refreshReplayControls();
    assert(el('btn-replay-play').disabled === true, 'while an empty record still keeps it away');
});

// PP3 (round 112): README:580 defines the session frame's move_count as 盘上子数 - the
// stones on the board, the 摆棋 base included - while 手 counts plies. The resumed line
// printed that server number with 手, so a game that opened from a five-stone installed
// position and then played six plies announced 「11 手」 over eleven stones. The
// authoritative frame reads 子; the ply count keeps 手, because it comes from the replay
// frame, whose path passes nothing to resumedStatusText and falls back to the local list.
check('PP3 the resume frame counts 子 and the replay frame counts 手', () => {
    const game = newGame();
    let status = null;
    const original = game.updateStatus.bind(game);
    game.updateStatus = (text, kind) => { status = text; original(text, kind); };
    game.sendMessage = () => true;

    const base = [
        { row: 6, col: 6, color: 'black' },
        { row: 8, col: 8, color: 'white' },
        { row: 5, col: 7, color: 'black' },
        { row: 9, col: 7, color: 'white' },
        { row: 7, col: 5, color: 'black' },
    ];
    const moves = [
        { row: 7, col: 7, color: 'white' },
        { row: 7, col: 8, color: 'black' },
        { row: 8, col: 6, color: 'white' },
        { row: 6, col: 8, color: 'black' },
        { row: 8, col: 9, color: 'white' },
        { row: 9, col: 8, color: 'black' },
    ];
    const board = emptyBoard();
    for (const stone of base.concat(moves)) {
        board[stone.row][stone.col] = stone.color === 'black' ? BLACK : WHITE;
    }

    game.handleServerMessage({
        type: 'session',
        token: 'a'.repeat(16),
        resumed: true,
        player_color: 'black',
        game_status: 'playing',
        current_turn: 'white',
        move_count: base.length + moves.length,
        max_depth: 6,
        timeout_ms: 5000,
        winner: '',
        reason: '',
        board: board,
    });
    // Discriminator: HEAD writes 「11 手」 here.
    assert(status.indexOf('已恢复对局：11 子') !== -1,
           'the server number counts stones, got: ' + status);
    assert(status.indexOf('11 手') === -1, 'and never plies, got: ' + status);

    // Control, green on both trees: the replay reply is the ply path.
    game.handleServerMessage({ type: 'replay', base: base, moves: moves, game_over: false });
    assert(status.indexOf('已恢复对局：6 手') !== -1,
           'the resumed replay still counts plies, got: ' + status);
});

// PP4 (round 112): handleTakenOver released the reconnect timer and the ping and left
// the replay clock running. A page that lost the game to another tab is not the page
// stepping through its record any more, and the interval outliving that decision is the
// one thing here still acting on the game. Like JJ3, the harness cannot wait for a real
// 700 ms tick, so it records what setInterval was handed and fires only intervals the
// page has not cleared - a browser has no other choice.
check('PP4 a takeover releases the replay clock along with the other timers', () => {
    const sandbox = bundle.sandbox;
    const realSetInterval = sandbox.setInterval;
    const realClearInterval = sandbox.clearInterval;
    const timers = [];
    const cleared = [];
    sandbox.setInterval = (fn) => { timers.push(fn); return timers.length; };
    sandbox.clearInterval = (id) => {
        if (id) { cleared.push(id); timers[id - 1] = null; }
    };
    try {
        const game = newGame();
        game.playerColor = BLACK;
        game.gameStatus = 'playing';
        game.aiThinking = false;
        game.board = emptyBoard();
        game.baseStones = [];
        game.analysis = [];
        game.moveHistory = [
            { row: 7, col: 7, color: BLACK },
            { row: 8, col: 8, color: WHITE },
            { row: 6, col: 6, color: BLACK },
        ];
        game.replayIndex = 0;
        game.toggleReplayAuto();
        const armed = game.replayTimer;
        // Control, green on both trees: the same arming path leaves a live clock.
        assert(typeof armed === 'number' && armed > 0, '自动 armed one interval');
        assert(el('btn-replay-play').textContent === '暂停', 'and the control reads as playing');

        const painted = [];
        game.ctx.fillText = (text) => { painted.push(text); };

        game.handleServerMessage({ type: 'session_taken_over', token: 'a'.repeat(16) });

        assert(game.replayTimer === null, 'the takeover must leave no replay clock owned');
        assert(cleared.indexOf(armed) >= 0, 'and must clear the interval it armed');
        assert(el('status').textContent.indexOf('本局面已在其他标签页打开') === 0,
               'the warning stays on the top bar, got: ' + el('status').textContent);
        // Only an uncleared clock can still paint: on HEAD this is the orphan tick
        // writing 回放 · 第 N 手 over a page that no longer operates the game.
        if (cleared.indexOf(armed) === -1) timers[armed - 1]();
        assert(painted.every((text) => text.indexOf('回放') === -1),
               'no replay overlay is painted after the takeover, got: ' + painted.join(' | '));
        assert(el('btn-replay-play').textContent === '自动', 'and 自动 is handed back');
    } finally {
        sandbox.setInterval = realSetInterval;
        sandbox.clearInterval = realClearInterval;
    }
});

// RR1 (round 114): the ◀ / ▶ pair walks the loaded record one ply at a time
// (README:335 「之后可用 ⏮ ◀ ▶ ⏭ 逐手回看」). The live board already shows the
// last ply of that record, so stepping starts there; HEAD read a null replayIndex as
// "before the first ply" for ◀ too, so back could never back up - it jumped to the
// opening move instead.
check('RR1 实时视角按 ◀ 退回上一手，按 ▶ 停在最后一手', () => {
    const game = adviceGame();
    game.moveHistory = [
        { row: 7, col: 7, color: BLACK },
        { row: 8, col: 8, color: WHITE },
        { row: 9, col: 9, color: BLACK },
    ];
    game.analysis = game.moveHistory.map(() => ({
        score: 10, depth: 4, nodes: 100, elapsed_ms: 5, method: 'alphabeta',
    }));
    game.replayIndex = null;
    game.stepReplay(-1);
    assert(game.replayIndex === 1,
           '◀ from the live view lands on the second-to-last ply, got: ' + game.replayIndex);

    game.replayIndex = null;
    game.stepReplay(1);
    assert(game.replayIndex === game.moveHistory.length - 1,
           '▶ from the live view stays on the last ply the board already shows, got: ' +
           game.replayIndex);
});

check('RR1 stepping inside a replay, over an empty record and over an unclaimed position is unchanged', () => {
    const game = adviceGame();
    game.moveHistory = [
        { row: 7, col: 7, color: BLACK },
        { row: 8, col: 8, color: WHITE },
        { row: 9, col: 9, color: BLACK },
    ];
    game.replayIndex = 1;
    game.stepReplay(-1);
    assert(game.replayIndex === 0, 'inside a replay ◀ still walks back one ply, got: ' + game.replayIndex);
    game.stepReplay(1);
    assert(game.replayIndex === 1, 'and ▶ still walks forward one, got: ' + game.replayIndex);

    game.moveHistory = [];
    game.replayIndex = null;
    game.stepReplay(-1);
    assert(game.replayIndex === null, 'an empty record keeps ◀ out of the replay view');
    game.stepReplay(1);
    assert(game.replayIndex === null, 'and keeps ▶ out of it too');

    // The round-101 lock at tests:1008-1011 - a hand-set position nobody has claimed
    // a side for keeps its leftover move list out of the replay.
    game.moveHistory = [
        { row: 3, col: 3, color: BLACK },
        { row: 4, col: 4, color: WHITE },
    ];
    game.playerColor = null;
    game.gameStatus = 'waiting';
    game.showReplayMove(0);
    assert(game.replayIndex === null, 'an unclaimed position must not enter replay');
    game.stepReplay(-1);
    assert(game.replayIndex === null, 'the new starting point honours that lock for ◀');
    game.stepReplay(1);
    assert(game.replayIndex === null, 'and for ▶');
});

// RR2 (round 114): a 求招 answer is void once the position it was searched on is gone
// (README:327 「这一份建议作废」). The ply-count test alone misses one shape: 悔棋, then
// two stones land at different points, and the count is back to what 求招 recorded -
// over a different board.

check('RR2 悔棋后手数补回也不画旧局面的建议环', () => {
    const game = adviceGame();
    game.hintInFlight = false;
    game.moveHistory = [
        { row: 7, col: 7, color: BLACK },
        { row: 8, col: 8, color: WHITE },
    ];
    game.board[7][7] = BLACK;
    game.board[8][8] = WHITE;
    el('timeout').value = '5';
    el('depth').value = '6';
    game.requestHint();
    assert(game.hintInFlight === true && game.hintMoveCount === 2,
           '求招 is in flight against the two-ply position');

    game.handleUndoSuccess({ move_count: 0, game_status: 'playing' });
    game.handleMoveMade({ row: 2, col: 2, color: 'black' });
    game.handleAIMove({ row: 3, col: 3, color: 'white', score: 12, thinking_time_ms: 4 });
    assert(game.moveHistory.length === 2,
           'the count is back to what 求招 recorded, got ' + game.moveHistory.length);
    assert(game.board[7][7] === 0 && game.board[2][2] === BLACK,
           'over a board that is not the one the answer was searched on');

    const rec = ringRecorder();
    try {
        rec.clear();
        game.handleServerMessage({ type: 'hint_result', row: 7, col: 7, score: 99 });
        const rings = adviceRings(rec, game).length;
        assert(rings === 0, 'the answer for the discarded position draws no ring, got ' + rings);
        assert(game.recommendation === null, 'and it is not kept as display state either');
    } finally {
        rec.restore();
    }
});

check('RR2 the undo voids nothing it does not cover: no undo paints, a fresh 求招 paints, a longer record voids', () => {
    // Control one: the same answer with no undo in the game at all still lands.
    const plain = adviceGame();
    plain.hintInFlight = false;
    plain.moveHistory = [
        { row: 7, col: 7, color: BLACK },
        { row: 8, col: 8, color: WHITE },
    ];
    plain.board[7][7] = BLACK;
    plain.board[8][8] = WHITE;
    el('timeout').value = '5';
    el('depth').value = '6';
    plain.requestHint();
    const rec = ringRecorder();
    try {
        rec.clear();
        plain.handleServerMessage({ type: 'hint_result', row: 0, col: 0, score: 9 });
        assert(adviceRings(rec, plain).length === 1, 'the answer the player asked for is painted');
        assert(plain.recommendation !== null, 'and stays on the board as display state');

        // Control two: after the undo-and-rebuild of the discriminator, a 求招 the
        // player asks for over the *current* board is owed its ring.
        const again = adviceGame();
        again.hintInFlight = false;
        again.moveHistory = [
            { row: 7, col: 7, color: BLACK },
            { row: 8, col: 8, color: WHITE },
        ];
        again.board[7][7] = BLACK;
        again.board[8][8] = WHITE;
        again.requestHint();
        again.handleUndoSuccess({ move_count: 0, game_status: 'playing' });
        again.handleMoveMade({ row: 2, col: 2, color: 'black' });
        again.handleAIMove({ row: 3, col: 3, color: 'white', score: 12, thinking_time_ms: 4 });
        again.requestHint();
        assert(again.hintMoveCount === 2 && again.hintInFlight === true,
               'the second 求招 re-arms against the rebuilt position');
        rec.clear();
        again.handleServerMessage({ type: 'hint_result', row: 4, col: 4, score: 7 });
        assert(adviceRings(rec, again).length === 1,
               'so its answer is drawn, got ' + adviceRings(rec, again).length);

        // Control three: the pre-count guard is untouched - one extra stone with no
        // undo at all already voids the answer by count.
        const longer = adviceGame();
        longer.hintInFlight = false;
        longer.moveHistory = [
            { row: 7, col: 7, color: BLACK },
            { row: 8, col: 8, color: WHITE },
        ];
        longer.board[7][7] = BLACK;
        longer.board[8][8] = WHITE;
        longer.requestHint();
        longer.handleMoveMade({ row: 2, col: 2, color: 'white' });
        rec.clear();
        longer.handleServerMessage({ type: 'hint_result', row: 0, col: 0, score: 5 });
        assert(adviceRings(rec, longer).length === 0,
               'a position that gained a ply retires the answer by count alone, got ' +
               adviceRings(rec, longer).length);
    } finally {
        rec.restore();
    }
});

// RR3 (round 114): README:524-525 makes session_taken_over the last frame the replaced
// socket gets, so a thinking_progress already queued behind it arrives at a page that is
// parked. Re-arming there greys nothing back and no later frame can clear it.
check('RR3 接管之后迟到的 thinking_progress 不再重新武装思考态', () => {
    const game = adviceGame();
    game.takenOver = false;
    game.handleTakenOver();
    assert(game.takenOver === true, 'the takeover landed');

    game.gameStatus = 'playing';
    game.currentTurn = WHITE;
    game.handleServerMessage({ type: 'thinking_progress', depth: 4, nodes: 1200 });
    assert(game.aiThinking === false, 'a late progress frame does not arm 思考中, got: ' + game.aiThinking);
    assert(el('btn-stop').disabled === true, '终止 stays disabled, got: ' + el('btn-stop').disabled);
    assert(el('btn-undo').disabled === true && el('btn-hint').disabled === true,
           'and 悔棋/求招 stay greyed with the rest of the parked page');
    assert(el('status').textContent.indexOf('本局面已在其他标签页打开') === 0,
           'the parked line still stands, got: ' + el('status').textContent);
});

check('RR3 an un-taken page still arms on the very same frame', () => {
    const game = adviceGame();
    assert(game.takenOver === false, 'this page still operates the game');
    game.gameStatus = 'playing';
    game.currentTurn = WHITE;
    game.handleServerMessage({ type: 'thinking_progress', depth: 4, nodes: 1200 });
    assert(game.aiThinking === true, 'the AI turn still arms 思考中');
    assert(el('btn-stop').disabled === false, 'and 终止 becomes clickable');
});

// RR4 (round 114): the resume's replay request has to have left before the page calls
// the next replay answer a resume. A send the closed socket refused gets no reply, and
// the stale flag was then eaten by the 载入本局分析 the player did ask for.
check('RR4 request_replay 被拒时不留恢复标记，随后的 replay 进回放视角', () => {
    const game = adviceGame();
    game.sendMessage = () => false;
    const stones = emptyBoard();
    stones[7][7] = BLACK;
    stones[8][8] = WHITE;
    game.handleServerMessage({
        type: 'session',
        token: 'a'.repeat(16),
        resumed: true,
        player_color: 'black',
        game_status: 'playing',
        current_turn: 'white',
        move_count: 2,
        max_depth: 6,
        timeout_ms: 5000,
        winner: '',
        reason: '',
        board: stones,
    });
    assert(game.resumeApplied === false,
           'a refused send cannot promise a reply, got: ' + game.resumeApplied);

    game.handleServerMessage({
        type: 'replay',
        base: [],
        moves: [
            { row: 7, col: 7, color: 'black' },
            { row: 8, col: 8, color: 'white' },
        ],
        game_over: false,
    });
    assert(game.replayIndex === 0, 'the replay that arrives is the view the player scrubs, got: ' + game.replayIndex);
    assert(el('status').textContent.indexOf('回放已载入') === 0,
           'and it reports as a replay, got: ' + el('status').textContent);
});

check('RR4 a send that left still holds the resume for its replay answer', () => {
    const game = adviceGame();
    game.sendMessage = () => true;
    const stones = emptyBoard();
    stones[7][7] = BLACK;
    stones[8][8] = WHITE;
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
        board: stones,
    });
    assert(game.resumeApplied === true, 'the request left, so its answer is a resume report');

    game.handleServerMessage({
        type: 'replay',
        base: [],
        moves: [
            { row: 7, col: 7, color: 'black' },
            { row: 8, col: 8, color: 'white' },
        ],
        game_over: false,
    });
    assert(game.replayIndex === null, 'and the resumed game stays on the live board, got: ' + game.replayIndex);
    assert(el('status').textContent.indexOf('已恢复对局') === 0,
           'with the resumed line, got: ' + el('status').textContent);
});

// SS1 (round 115): README:524-525 makes session_taken_over the last frame the replaced
// socket gets, and handleTakenOver parks this page - the reconnect timer, the ping and
// the replay clock are gone, and 求招 goes grey with the rest of them. A hint answer
// that was already queued behind that notice used to be the one frame still able to act
// on a page that no longer operates the game: it released the button, rang a square of a
// board it does not own, and painted its own line over the takeover warning, with no
// later frame to put any of that back.
const hintPage = () => {
    const game = adviceGame();
    game.hintInFlight = false;
    game.moveHistory = [
        { row: 7, col: 7, color: BLACK },
        { row: 8, col: 8, color: WHITE },
    ];
    game.board[7][7] = BLACK;
    game.board[8][8] = WHITE;
    el('timeout').value = '5';
    el('depth').value = '6';
    game.requestHint();
    return game;
};

check('SS1a 接管之后迟到的 hint_result 不解冻求招、不画环、不盖接管警告', () => {
    const game = hintPage();
    assert(game.hintInFlight === true && game.hintMoveCount === 2,
           '求招 is in flight against the two-ply position');
    assert(el('btn-hint').disabled === true, 'and 求招 is greyed while it is out');

    game.handleServerMessage({ type: 'session_taken_over', token: 'c'.repeat(16) });
    assert(game.takenOver === true, 'the takeover parked this page');

    const rec = ringRecorder();
    try {
        rec.clear();
        game.handleServerMessage({ type: 'hint_result', row: 3, col: 3, score: 55 });
        assert(el('btn-hint').disabled === true,
               'the parked page keeps 求招 greyed, got: ' + el('btn-hint').disabled);
        const rings = adviceRings(rec, game).length;
        assert(rings === 0, 'no ring lands on a board this page no longer operates, got ' + rings);
        assert(game.recommendation === null, 'and no answer is held as display state, got: ' +
               JSON.stringify(game.recommendation));
        const status = el('status').textContent;
        assert(status.indexOf('建议') === -1, 'the advice line does not replace it, got: ' + status);
        assert(status.indexOf('本局面已在其他标签页打开') === 0,
               'the takeover warning still stands, got: ' + status);
    } finally {
        rec.restore();
    }
});

check('SS1b an un-taken page still gets its answer, and a finished one still holds the button', () => {
    // Control one: the guard is about takenOver alone, not about hint_result in general.
    const live = hintPage();
    assert(live.takenOver === false, 'this page still operates the game');
    const rec = ringRecorder();
    try {
        rec.clear();
        live.handleServerMessage({ type: 'hint_result', row: 3, col: 3, score: 55 });
        assert(el('btn-hint').disabled === false, 'the answer releases 求招 by the plain rule');
        assert(adviceRings(rec, live).length === 1, 'and its ring is the one the player was owed');
        assert(live.recommendation !== null, 'kept as display state through later repaints');
        assert(el('status').textContent.indexOf('建议') === 0,
               'with the advice line on top, got: ' + el('status').textContent);

        // Control two: 已终局 keeps the button greyed - the expression SS1 sits above is
        // untouched, so a game_over page does not gain a clickable 求招 from a late answer.
        const over = hintPage();
        over.gameStatus = 'game_over';
        rec.clear();
        over.handleServerMessage({ type: 'hint_result', row: 4, col: 4, score: 20 });
        assert(el('btn-hint').disabled === true,
               'a finished game keeps 求招 held, got: ' + el('btn-hint').disabled);
    } finally {
        rec.restore();
    }
});

// TT1 (round 116): README:379 gives the top line to 摆棋's amber guidance for as long
// as the editor is open, which is why applyView, handleAIMove, handleUndoSuccess,
// handleHintResult and handleGameOver each write theirs only outside it. state_sync was
// the one handler that did not: its tail line replaced the guidance with 「对局开始」
// while the player was still setting a position, and nothing repaints it -
// renderSetupPanel only ever writes #position-status.
check('TT1a 摆棋中的 state_sync 只让位顶栏一行，其余效果照常落地', () => {
    const game = newGame();
    game.sendMessage = () => true;
    game.playerColor = BLACK;
    game.gameStatus = 'playing';
    game.baseStones = [];
    game.board = emptyBoard();
    game.board[7][7] = BLACK;
    game.moveHistory = [{ row: 7, col: 7, color: BLACK }];
    game.currentTurn = BLACK;
    game.enterSetup();
    const guidance = el('status').textContent;
    assert(guidance.indexOf('摆棋中') === 0, 'the guidance owns the top line, got: ' + guidance);
    game.replayIndex = 3;

    const server = emptyBoard();
    server[2][3] = WHITE;
    game.handleServerMessage({ type: 'state_sync', board: server, current_turn: 'white' });

    assert(el('status').textContent === guidance,
           'the opening line does not replace it, got: ' + el('status').textContent);
    assert(el('status').textContent.indexOf('对局开始') === -1,
           'and is not there at all, got: ' + el('status').textContent);
    assert(el('status').className.indexOf('warn') >= 0,
           'still the amber level 摆棋 wrote, got: ' + el('status').className);

    // The guard gives up one line, not the handler: the authoritative state lands.
    assert(game.gameStatus === 'playing', 'the frame still starts the game');
    assert(game.board[7][7] === 0 && game.board[2][3] === WHITE,
           'the server board still overwrites the live one');
    assert(game.moveHistory.length === 0, 'the old 落子记录 is still dropped');
    assert(game.baseStones.length === 1 && game.baseStones[0].row === 2 && game.baseStones[0].col === 3,
           'and the new stone is still the base, got: ' + JSON.stringify(game.baseStones));
    assert(game.replayIndex === null, 'the historical view is still abandoned');
    assert(el('turn-info').textContent.indexOf('轮到') === 0,
           'the sub-line still names the turn, got: ' + el('turn-info').textContent);
});

// TT1-b: the control that the guard is about 摆棋 alone - outside the editor the
// opening line is still written (the same effect JJ3's 「a state_sync with no replay
// running needs no release」 pins at its :2611 assertion).
check('TT1b 不在摆棋时 state_sync 照旧写开场行', () => {
    const game = newGame();
    game.sendMessage = () => true;
    game.board = emptyBoard();
    game.moveHistory = [];
    game.enterSetup();
    game.exitSetup(false);
    assert(game.setup.active === false, 'the editor is closed again');

    game.handleServerMessage({ type: 'state_sync', board: emptyBoard(), current_turn: 'black' });
    assert(el('status').textContent.indexOf('对局开始') === 0,
           'the opening line still reaches the top bar, got: ' + el('status').textContent);
    assert(el('status').className.indexOf('ok') >= 0,
           'at its own level, got: ' + el('status').className);
});

// The prelude both TT2 halves share: a live game whose sub-line claims a thinking AI,
// then 摆棋 opened over it (handleMoveMade is how HH2 gets that sentence).
const thinkingSetup = () => {
    const game = newGame();
    game.sendMessage = () => true;
    game.playerColor = BLACK;
    game.gameStatus = 'playing';
    game.baseStones = [];
    game.board = emptyBoard();
    game.moveHistory = [];
    game.currentTurn = BLACK;
    game.handleMoveMade({ row: 7, col: 7, color: 'black' });
    const thinking = el('turn-info').textContent;
    assert(thinking.indexOf('轮到') === 0 && thinking.indexOf('思考中') >= 0,
           'the move_made line claims a turn and a thinking AI, got: ' + thinking);
    game.enterSetup();
    game.handleGameOver({ winner: 'white', reason: 'five' });
    return { game, thinking };
};

// TT2 (round 116): handleGameOver skips updateTurnInfo('') together with the status
// line while 摆棋 owns it. exitSetup repaints the status half from gameOverText on the
// 取消 way out, but the turn half had no second owner, so a game settled behind the
// editor came back showing an AI that is no longer thinking.
check('TT2a 摆棋中等来的终局，取消后子行不再谎报思考中', () => {
    const { game, thinking } = thinkingSetup();
    assert(game.setup.active === true, 'the ending arrived while 摆棋 was open');
    assert(game.gameStatus === 'game_over', 'and settled the game');
    assert(el('turn-info').textContent === thinking,
           'while the editor owns the top line the sub-line is left alone, got: '
           + el('turn-info').textContent);
    assert(el('status').textContent.indexOf('摆棋中') === 0,
           'and the guidance is still what the player reads, got: ' + el('status').textContent);

    fireClick('btn-setup-cancel');
    assert(game.setup.active === false, 'the cancel closed the editor');
    assert(el('status').textContent === game.gameOverText,
           'the result is painted back, got: ' + el('status').textContent);
    assert(el('status').textContent.indexOf('摆棋中') !== 0,
           'and it is no longer the guidance, got: ' + el('status').textContent);
    assert(el('turn-info').textContent === '',
           'the settled game keeps no thinking AI, got: ' + el('turn-info').textContent);
});

// TT2-c: 确认 is untouched by the new branch - it is inside the !confirmed half, so on
// both trees exitSetup(true) leaves the sub-line to its caller, and its only caller
// handlePositionSet writes updateTurnInfo('') itself.
check('TT2c 确认路径不经过这句新分支', () => {
    const { game, thinking } = thinkingSetup();
    game.exitSetup(true);
    assert(game.setup.active === false, 'the confirmed way out also closes the editor');
    assert(el('turn-info').textContent === thinking,
           'the branch leaves the sub-line exactly as the editor found it, got: '
           + el('turn-info').textContent);
    assert(el('status').textContent.indexOf('摆棋中') === 0,
           'and the top line to its own writer, got: ' + el('status').textContent);
});

let failed = 0;
for (const result of results) {
    console.log(`${result.ok ? 'PASS' : 'FAIL'} ${result.label}` +
                (result.ok ? '' : ` -- ${result.detail}`));
    if (!result.ok) failed++;
}
console.log(`web_frontend_check: ${results.length - failed}/${results.length} passed`);
process.exit(failed === 0 ? 0 : 1);
