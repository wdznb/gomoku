const BOARD_SIZE = 15;

const EMPTY = 0;
const BLACK = 1;
const WHITE = 2;

// The server keys each game by a token it hands out, and a game outlives the
// socket that played it. Keeping the token here is what lets a reload, a dropped
// connection or a server restart find the same board again.
const TOKEN_KEY = 'gomoku.session.token';

function emptyMatrix() {
    return Array(BOARD_SIZE).fill(null).map(() => Array(BOARD_SIZE).fill(EMPTY));
}

function matrixCounts(matrix) {
    let black = 0;
    let white = 0;
    for (const row of matrix) {
        for (const cell of row) {
            if (cell === BLACK) ++black;
            else if (cell === WHITE) ++white;
        }
    }
    return { black, white };
}

// A new matrix with two of the three marks exchanged; the source is never edited
// in place because the caller compares the result against it.
function swappedMatrix(matrix, from, to) {
    return matrix.map((row) => row.map((cell) =>
        cell === from ? to : (cell === to ? from : cell)));
}

// The board is drawn in the middle of the viewport and the rest of the page is
// cut into four trapezoids by the lines from each board corner to the nearest
// page corner. A band budget keeps those trapezoids tall/wide enough to hold
// their content, and the board never grows past kBoardMax.
const BAND_V = 104;
const BAND_H = 196;
const BAND_V_MAX = 190;
const BAND_H_MAX = 320;
const BOARD_MIN = 300;
const BOARD_MAX = 780;

class GomokuGame {
    constructor() {
        this.canvas = document.getElementById('board');
        this.ctx = this.canvas.getContext('2d');
        this.ws = null;
        // Geometry and view are per-layout: cell size follows the viewport, and
        // the view is a display-only rotation / mirror that every stone, marker,
        // label and click passes through.
        this.geom = { size: BOARD_MAX, cell: 38, pad: 28, stone: 15, font: 11 };
        // One queued resize layout at a time.
        this.layoutPending = false;
        this.view = { rot: 0, flipH: false };
        this.dpr = window.devicePixelRatio || 1;
        
        this.board = emptyMatrix();
        this.moveHistory = [];
        this.playerColor = null;
        this.currentTurn = BLACK;
        this.gameStatus = 'waiting';
        this.aiThinking = false;
        // True from sending request_hint until its reply (or the game is
        // replaced): the server answers a second concurrent hint with an error,
        // so the click still being answered disarms its own button.
        this.hintInFlight = false;
        // Ply count when 求招 left: a reply for a position the player has since
        // changed must not be painted as advice.
        this.hintMoveCount = 0;
        this.lastMove = null;
        // The ring 求招 leaves on a square: display state, so drawBoard repaints it.
        this.recommendation = null;
        // Engine numbers for the current game, index-aligned with moveHistory.
        // Filled by the server's "replay" reply (4.4), not by the live messages.
        this.analysis = [];
        // True when the loaded record already covers the game's ending.
        this.analysisEnded = false;
        // Set when a `session` reply asked for the record of a resumed game: the
        // replay that answers it paints the live board, not a scrubbed one.
        this.resumeApplied = false;
        // Null while viewing the live position; a move index while replaying.
        this.replayIndex = null;
        this.replayTimer = null;
        this.gameOverText = '';
        // The stones a hand-set position installed and this client's own move list
        // did not play. The board is base + moves, and anything that rebuilds a
        // position from the record (scrubbing, the 摆棋 record) needs both halves.
        this.baseStones = [];
        // Position editor (摆棋). Everything happens on this working copy: the live
        // board, the move record and the server session are untouched until 确认.
        this.setup = {
            active: false,
            board: null,
            record: [],
            view: null,
            preview: null,
            confirmInFlight: false,
            // The text the outstanding 识别 request was sent with: a reply for a
            // paste box the player has since edited is not a read they can 确认.
            checkedText: null,
        };
        
        this.initWebSocket();
        this.initUI();
        this.layout();
        window.addEventListener('resize', () => this.scheduleLayout());
    }
    
    initWebSocket() {
        this.wsAttempts = 0;
        this.wsReconnect = null;
        this.wsPing = null;
        this.everConnected = false;
        // Set when the server says another tab resumed this token: no auto
        // reconnect from here on, or the two tabs would evict each other forever.
        this.takenOver = false;
        // The token is the game: it is what the server keys its session table by,
        // so keeping it across a reload is what makes 继续上次那局 possible at all.
        this.sessionToken = localStorage.getItem('gomoku.session.token') || null;
        this.resumeAsked = false;
        this.openSocket();
    }

    openSocket() {
        const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        const wsUrl = `${protocol}//${window.location.host}`;
        
        this.ws = new WebSocket(wsUrl);
        
        this.ws.onopen = () => {
            console.log('Connected to server');
            this.everConnected = true;
            this.wsAttempts = 0;
            this.startPing();
            if (this.sessionToken) {
                // Ask before anything else: the server holds the board, the move
                // list and the AI's turn, and every later message is played in the
                // session this one lands in.
                this.resumeAsked = true;
                this.sendMessage({ type: 'resume', token: this.sessionToken });
                this.updateStatus('正在恢复对局…', 'idle');
            } else {
                this.updateStatus('已连接，请选择执子颜色', 'idle');
            }
        };
        
        this.ws.onclose = () => {
            console.log('Disconnected from server');
            this.stopPing();
            this.scheduleReconnect();
        };
        
        this.ws.onerror = (error) => {
            // onclose always follows an error, and that is where the retry lives.
            console.error('WebSocket error:', error);
        };
        
        this.ws.onmessage = (event) => {
            const msg = JSON.parse(event.data);
            this.handleServerMessage(msg);
        };
    }

    // The server reaps a session that has been silent in both directions for two
    // minutes, and a player thinking over the board sends nothing at all in that
    // time, so the page keeps the socket warm rather than asking for a longer window.
    startPing() {
        this.stopPing();
        this.wsPing = setInterval(() => this.sendMessage({ type: 'ping' }), 30000);
    }

    stopPing() {
        if (this.wsPing) {
            clearInterval(this.wsPing);
            this.wsPing = null;
        }
    }

    scheduleReconnect() {
        // A taken-over tab stays out of the fight: reconnecting would resume the
        // same token and evict the tab that just won it.
        if (this.takenOver) return;
        if (this.wsReconnect) return;
        const wait = Math.min(15000, 1000 * 2 ** this.wsAttempts);
        this.wsAttempts += 1;
        // README:553 wants the reconnect countdown in 琥珀色; only the refused or
        // lost-connection events are errors.
        this.updateStatus(`连接断开，${Math.round(wait / 1000)} 秒后重连（第 ${this.wsAttempts} 次）`, 'warn');
        this.wsReconnect = setTimeout(() => {
            this.wsReconnect = null;
            this.openSocket();
        }, wait);
    }

    // Another tab resumed this token and took the game over (README:567: later
    // wins). This tab is parked, not broken: the reconnect timer goes, the flags
    // settle, and the controls take the same grey-out a settled board leaves
    // behind. The token stays in localStorage - reloading this page is how the
    // player takes the game back.
    handleTakenOver() {
        this.takenOver = true;
        if (this.wsReconnect) {
            clearTimeout(this.wsReconnect);
            this.wsReconnect = null;
        }
        this.stopPing();
        // Taken over means this page is no longer the side operating the game, and
        // the replay clock is the only timer here that was never released. Left
        // running, its interval keeps stepping the replay and writing 「回放 · 第 N 手」
        // back to the top bar over the warning below (README:567).
        this.stopReplayAuto();
        this.aiThinking = false;
        this.hintInFlight = false;
        // The socket is closed behind our back, so no error frame will ever reach
        // this page to release the 确认 lock; without this the four buttons stay grey.
        if (this.setup.confirmInFlight) {
            this.setup.confirmInFlight = false;
            this.refreshSetupControls();
        }
        document.getElementById('btn-undo').disabled = true;
        document.getElementById('btn-hint').disabled = true;
        document.getElementById('btn-resign').disabled = true;
        document.getElementById('btn-stop').disabled = true;
        this.setSideButtons(true, true);
        this.refreshReplayControls();
        this.updateStatus('本局面已在其他标签页打开，刷新页面可取回', 'warn');
    }

    // The server's answer to `resume`, and its announcement whenever it opened a
    // session for this socket on its own. A resumed game is painted from the
    // server's copy of itself: the page reloaded, and what it had on screen is not
    // a position the engine is playing.
    handleSession(msg) {
        this.sessionToken = msg.token || null;
        if (this.sessionToken) {
            localStorage.setItem('gomoku.session.token', this.sessionToken);
        } else {
            localStorage.removeItem('gomoku.session.token');
        }
        const asked = this.resumeAsked;
        this.resumeAsked = false;

        if (!msg.resumed) {
            if (!asked) {
                // Nothing to apply. A session that was not resumed is empty by
                // definition, and the request that made the server open it answers
                // with its own state a moment later: overwriting the board and the
                // chosen side here would undo the click that is already on its way.
                return;
            }
            // A resume answered with resumed:false is different: the server already
            // sent the whole fresh session as the authoritative frame, so the dead
            // game still on screen has to be replaced by it, stones and all.
            this.applyAuthoritativeSession(msg);
            this.updateStatus('上一局找不回来了，已开新局', 'warn');
            return;
        }
        this.applyAuthoritativeSession(msg);

        this.sendMessage({ type: 'request_replay' });
        // The reply to that request redraws the move list, the graph and the status
        // line; flag it so it reports the resume rather than a replay the player
        // never asked for.
        this.resumeApplied = true;
        this.updateStatus(this.resumedStatusText(msg.move_count),
                          this.gameStatus === 'game_over' ? 'idle' : 'ok');
    }

    // Landing the session frame the server treats as authoritative: both the
    // resumed game and the fresh game a missed resume opened are painted from it.
    // A frame with no stones has nothing to replay, so nothing is asked for.
    applyAuthoritativeSession(msg) {
        // The session carries its own AI settings, which outlived the page: show
        // them rather than the form defaults, or the numbers on screen would
        // describe a search the server is not going to run. The deadline is stored
        // in milliseconds and shown in seconds, and the range around it follows the
        // depth it belongs to.
        if (Number.isInteger(msg.max_depth) && msg.max_depth > 0) {
            document.getElementById('depth').value = msg.max_depth;
        }
        if (Number.isInteger(msg.timeout_ms) && msg.timeout_ms > 0) {
            document.getElementById('timeout').value = Math.round(msg.timeout_ms / 1000);
        }
        this.syncTimeoutBounds('bounds');
        // A resume that landed while 摆棋 was open (typically a server restart
        // before the 确认 reply arrived) carries the authoritative board: leave
        // the setup first, unconditionally - the working copy is dropped, no
        // set_position is sent, confirmInFlight and the button bar are released
        // - or the page would keep painting and guarding a position the server
        // no longer has.
        if (this.setup.active) this.exitSetup(false);
        this.playerColor = msg.player_color === 'black' ? BLACK
                         : msg.player_color === 'white' ? WHITE : null;
        this.gameStatus = msg.game_status || 'waiting';
        this.currentTurn = msg.current_turn === 'white' ? WHITE : BLACK;
        this.applyBoardMatrix(msg.board);
        this.lastMove = null;
        this.recommendation = null;
        this.aiThinking = false;
        this.gameOverText = msg.game_status === 'game_over'
            ? `已结束的对局 · ${msg.winner === 'draw' ? '和棋' : (msg.winner === 'black' ? '黑棋' : '白棋')}胜`
              + `（${this.reasonText(msg.reason)}）`
            : '';
        // The move list, the installed stones it sits on and the engine numbers are
        // the server's, so ask for them instead of redrawing a history the board
        // cannot give back.
        this.baseStones = [];
        this.stopReplayAuto();
        this.moveHistory = [];
        this.analysis = [];
        this.analysisEnded = false;
        this.replayIndex = null;
        this.updateMoveList();
        this.updateReplayInfo();
        this.drawEvalGraph();
        this.refreshReplayControls();
        this.setSideButtons(!this.sideChoicePending(), !this.sideChoicePending());
        document.getElementById('btn-undo').disabled =
            this.playerColor === null || this.gameStatus === 'game_over';
        document.getElementById('btn-hint').disabled =
            this.playerColor === null || this.gameStatus === 'game_over';
        // A resumed or re-synced game replaces whatever position the hint was
        // searched on, so an in-flight request from before cannot answer here.
        this.hintInFlight = false;
        document.getElementById('btn-resign').disabled = this.gameStatus !== 'playing';
        document.getElementById('btn-stop').disabled = true;
        document.getElementById('btn-restart').disabled = false;
        this.drawBoard();
        this.updateTurnInfo();
    }

    // What a resumed session is worth saying, from the state the server just
    // described: an ending, a position nobody has taken a side of, an empty board
    // with a side claimed, or a game in progress. The authoritative session frame
    // carries the stones on the board (README:580), the 摆棋 base included, so that
    // number is only ever 「N 子」. The ply count comes with the replay instead: that
    // path passes nothing and falls back to the local move list, and says 「N 手」.
    // The local list belongs to whatever game this page last drew, which is why it
    // is not the resume count.
    resumedStatusText(moveCount) {
        const fromServer = Number.isInteger(moveCount);
        const count = fromServer ? moveCount : this.moveHistory.length;
        const turn = `轮到${this.currentTurn === BLACK ? '黑' : '白'}方`;
        if (this.gameStatus === 'game_over' && this.gameOverText) return this.gameOverText;
        if (this.gameStatus === 'waiting') {
            const installed = this.board.flat().filter(v => v !== 0).length;
            return installed > 0
                ? `已恢复摆好的局面：${installed} 子，${turn}，请选择执子颜色`
                : `已恢复会话：棋盘还是空的，请选择执子颜色`;
        }
        // A game can be "playing" with an empty move list: a hand-set position whose
        // side has been taken but which nobody has added to yet. Reporting that as
        // "0 手" under four stones reads as a bug, so say what is on the board.
        if (count === 0) {
            const installed = this.board.flat().filter(v => v !== 0).length;
            if (installed > 0) return `已恢复摆好的局面：${installed} 子，${turn}`;
        }
        return `已恢复对局：${count} ${fromServer ? '子' : '手'} · ${turn}`;
    }
    
    initUI() {
        document.getElementById('btn-black').addEventListener('click', () => this.chooseSide('black'));
        document.getElementById('btn-white').addEventListener('click', () => this.chooseSide('white'));
        document.getElementById('btn-undo').addEventListener('click', () => this.undo());
        document.getElementById('btn-hint').addEventListener('click', () => this.requestHint());
        document.getElementById('btn-resign').addEventListener('click', () => this.resign());
        document.getElementById('btn-stop').addEventListener('click', () => this.stopThinking());
        document.getElementById('btn-restart').addEventListener('click', () => this.restart());
        
        this.canvas.addEventListener('click', (e) => this.handleCanvasClick(e));

        document.getElementById('btn-replay').addEventListener('click', () => this.requestReplay());
        document.getElementById('btn-replay-first').addEventListener('click', () => this.showReplayMove(0));
        document.getElementById('btn-replay-prev').addEventListener('click', () => this.stepReplay(-1));
        document.getElementById('btn-replay-next').addEventListener('click', () => this.stepReplay(1));
        document.getElementById('btn-replay-last').addEventListener('click', () => this.showReplayMove(this.moveHistory.length - 1));
        document.getElementById('btn-replay-play').addEventListener('click', () => this.toggleReplayAuto());
        document.getElementById('btn-replay-exit').addEventListener('click', () => this.exitReplay());

        // The two AI settings used to reach the server only as hint parameters,
        // so the AI's own moves always ran on its defaults. Push them as config
        // whenever they change. A deeper search needs a longer deadline, so the
        // timeout is bounded by the depth (2× to 4× it, in seconds) and follows a
        // depth change back to that range's minimum.
        document.getElementById('depth').addEventListener('change', () => {
            this.syncTimeoutBounds('min');
            this.sendConfig();
        });
        document.getElementById('timeout').addEventListener('change', () => {
            this.syncTimeoutBounds('clamp');
            this.sendConfig();
        });
        this.syncTimeoutBounds('min');

        document.getElementById('btn-export-position').addEventListener('click', () => this.exportPosition());
        document.getElementById('btn-setup-clear').addEventListener('click', () => {
            if (this.setup.confirmInFlight) return;
            this.clearSetupBoard();
        });
        document.getElementById('btn-setup-confirm').addEventListener('click', () => this.confirmSetup());
        document.getElementById('btn-setup-cancel').addEventListener('click', () => {
            if (this.setup.confirmInFlight) return;
            this.exitSetup(false);
        });

        // 导入 is a modal: the paste box, the file picker and the drop zone all
        // land in the working copy, never on the live board.
        document.getElementById('btn-setup-import').addEventListener('click', () => this.openImportModal());
        document.getElementById('btn-import-close').addEventListener('click', () => this.closeImportModal());
        document.getElementById('btn-check-position').addEventListener('click', () => this.checkPosition());
        document.getElementById('btn-import-apply').addEventListener('click', () => this.applyImport());
        document.getElementById('position-text').addEventListener('input', () => {
            // Anything typed or pasted replaces the file as the payload, and the
            // old recognition no longer describes it.
            this.setup.preview = null;
            this.renderImportState('内容已改动，点“识别”重新检查。');
        });
        const drop = document.getElementById('import-drop');
        const readFile = (file) => {
            if (!file) return;
            const reader = new FileReader();
            reader.onload = () => {
                document.getElementById('position-text').value = String(reader.result);
                this.setup.preview = null;
                this.renderImportState(`已读入 ${file.name}，点“识别”检查局面。`);
            };
            reader.onerror = () => this.updateImportStatus('文件读取失败。', true);
            reader.readAsText(file);
        };
        document.getElementById('position-file').addEventListener('change', (e) => {
            readFile(e.target.files[0]);
            // Reset so picking the same file again still fires change.
            e.target.value = '';
        });
        for (const type of ['dragenter', 'dragover']) {
            drop.addEventListener(type, (e) => {
                e.preventDefault();
                drop.classList.add('dragging');
            });
        }
        for (const type of ['dragleave', 'drop']) {
            drop.addEventListener(type, (e) => {
                e.preventDefault();
                drop.classList.remove('dragging');
            });
        }
        drop.addEventListener('drop', (e) => {
            const files = e.dataTransfer ? e.dataTransfer.files : null;
            if (files && files.length) readFile(files[0]);
        });

        // 交换 has two homes. The bar's trio edits the 摆棋 working copy; the
        // modal's edits the recognised preview before it lands on the copy, which
        // is where a mis-read board is actually worth fixing.
        const swapPairs = { bw: [BLACK, WHITE], be: [BLACK, EMPTY], we: [WHITE, EMPTY] };
        const swapTargets = [['#group-swap', 'board'], ['#import-swap', 'preview']];
        for (const [container, target] of swapTargets) {
            for (const button of document.querySelectorAll(`${container} [data-swap]`)) {
                const pair = swapPairs[button.dataset.swap];
                button.addEventListener('click', () => this.swapMarks(pair[0], pair[1], target));
            }
        }

        document.getElementById('btn-position-toggle').addEventListener('click', () => {
            if (this.setup.confirmInFlight) return;
            this.toggleSetup();
        });
        document.getElementById('btn-view-rotate').addEventListener('click', () => {
            this.view.rot = (this.view.rot + 90) % 360;
            this.applyView();
        });
        document.getElementById('btn-view-mirror').addEventListener('click', () => {
            this.view.flipH = !this.view.flipH;
            this.applyView();
        });
    }

    // The view is display-only: the server keeps playing in real coordinates and
    // only the rendering (and the click that maps back from it) is rotated.
    applyView() {
        // The rotate button wears the angle it last turned to, so the bar keeps
        // showing the current view after the status line has moved on.
        const rotate = document.getElementById('btn-view-rotate');
        rotate.textContent = this.view.rot ? `旋转 ${this.view.rot}°` : '旋转';
        rotate.classList.toggle('toggled', this.view.rot !== 0);
        document.getElementById('btn-view-mirror').classList.toggle('toggled', this.view.flipH);
        this.drawBoard();
        // 摆棋's guidance owns the top bar while the panel is open, and nothing
        // writes it back, so a rotation here would leave the player without it.
        if (this.setup.active) return;
        this.updateStatus(`视角：${this.viewLabel()}`, 'idle');
    }

    viewLabel() {
        const parts = [];
        if (this.view.rot) parts.push(`旋转 ${this.view.rot}°`);
        if (this.view.flipH) parts.push('镜像');
        return parts.length ? parts.join(' · ') : '正常';
    }

    // Logical cell -> cell as displayed.
    displayOf(row, col) {
        let r = row;
        let c = col;
        if (this.view.rot === 90) { r = col; c = BOARD_SIZE - 1 - row; }
        else if (this.view.rot === 180) { r = BOARD_SIZE - 1 - row; c = BOARD_SIZE - 1 - col; }
        else if (this.view.rot === 270) { r = BOARD_SIZE - 1 - col; c = row; }
        if (this.view.flipH) c = BOARD_SIZE - 1 - c;
        return [r, c];
    }

    // Displayed cell -> logical cell. The exact inverse of displayOf: undo the
    // mirror first, then undo the rotation. Both steps need the old pair at the
    // same time, so they swap through destructuring rather than assigning in
    // sequence (which would feed a fresh value into the second half).
    logicalOf(row, col) {
        let r = row;
        let c = col;
        if (this.view.flipH) c = BOARD_SIZE - 1 - c;
        if (this.view.rot === 90) [r, c] = [BOARD_SIZE - 1 - c, r];
        else if (this.view.rot === 180) [r, c] = [BOARD_SIZE - 1 - r, BOARD_SIZE - 1 - c];
        else if (this.view.rot === 270) [r, c] = [c, BOARD_SIZE - 1 - r];
        return [r, c];
    }

    coordName(row, col) {
        return `${String.fromCharCode(65 + col)}${BOARD_SIZE - row}`;
    }

    // Layout: keep a vertical band for the status bar and the control bar, and a
    // horizontal band for the two side panels, then centre the largest square
    // board that is left. The panels take the inscribed rectangle of their
    // trapezoid, so nothing they hold is cut by the diagonal divider.
    // layout() tears the SVG down, resizes both canvas backbuffers and repaints
    // the whole board, so one resize drag would otherwise run it per event.
    // Coalesce the events into a single layout per frame.
    scheduleLayout() {
        if (this.layoutPending) return;
        this.layoutPending = true;
        requestAnimationFrame(() => {
            this.layoutPending = false;
            this.layout();
        });
    }

    layout() {
        const width = window.innerWidth;
        const height = window.innerHeight;
        const bandV = Math.max(56, Math.min(BAND_V_MAX, Math.round(height * 0.2)));
        const bandH = Math.max(96, Math.min(BAND_H_MAX, Math.round(width * 0.2)));
        const size = Math.max(BOARD_MIN,
            Math.min(BOARD_MAX, width - 2 * bandH, height - 2 * bandV));
        const x0 = Math.round((width - size) / 2);
        const y0 = Math.round((height - size) / 2);
        const gap = 8;

        this.dpr = window.devicePixelRatio || 1;
        // The canvas has no border, so its CSS box, its backing store (scaled by
        // dpr) and the drawing coordinates all describe the same square.
        this.geom.pad = Math.max(20, Math.round(size * 0.05));
        this.geom.cell = (size - 2 * this.geom.pad) / (BOARD_SIZE - 1);
        this.geom.stone = Math.max(6, this.geom.cell * 0.45);
        this.geom.font = Math.max(9, Math.round(this.geom.cell * 0.34));
        this.geom.size = size;

        const stage = document.getElementById('stage');
        stage.style.left = `${x0}px`;
        stage.style.top = `${y0}px`;
        stage.style.width = `${size}px`;
        stage.style.height = `${size}px`;
        this.canvas.width = Math.round(size * this.dpr);
        this.canvas.height = Math.round(size * this.dpr);
        this.ctx.setTransform(this.dpr, 0, 0, this.dpr, 0, 0);

        const top = document.getElementById('region-top');
        top.style.left = `${x0}px`;
        top.style.width = `${size}px`;
        top.style.top = '0px';
        top.style.height = `${Math.max(0, y0 - gap)}px`;

        const bottom = document.getElementById('region-bottom');
        bottom.style.left = `${x0}px`;
        bottom.style.width = `${size}px`;
        bottom.style.top = `${y0 + size + gap}px`;
        bottom.style.height = `${Math.max(0, height - y0 - size - gap)}px`;

        const left = document.getElementById('region-left');
        left.style.left = '0px';
        left.style.top = `${y0}px`;
        left.style.width = `${Math.max(0, x0 - gap)}px`;
        left.style.height = `${size}px`;

        const right = document.getElementById('region-right');
        right.style.left = `${x0 + size + gap}px`;
        right.style.top = `${y0}px`;
        right.style.width = `${Math.max(0, width - x0 - size - gap)}px`;
        right.style.height = `${size}px`;

        this.drawDividers(x0, y0, size, width, height);
        document.body.classList.toggle('compact', size < 460);

        this.drawBoard();
        this.drawEvalGraph();
    }

    drawDividers(x0, y0, size, width, height) {
        const svg = document.getElementById('dividers');
        const x1 = x0 + size;
        const y1 = y0 + size;
        const corners = [[0, 0], [width, 0], [0, height], [width, height]];
        const boardCorners = [[x0, y0], [x1, y0], [x0, y1], [x1, y1]];
        svg.setAttribute('viewBox', `0 0 ${width} ${height}`);
        svg.innerHTML = '';
        for (const [bx, by] of boardCorners) {
            // Each board corner joins the page corner it is closest to, which is
            // what carves the page into the four regions.
            let best = corners[0];
            let bestDist = Infinity;
            for (const [px, py] of corners) {
                const d = (px - bx) ** 2 + (py - by) ** 2;
                if (d < bestDist) { bestDist = d; best = [px, py]; }
            }
            const line = document.createElementNS('http://www.w3.org/2000/svg', 'line');
            line.setAttribute('x1', bx);
            line.setAttribute('y1', by);
            line.setAttribute('x2', best[0]);
            line.setAttribute('y2', best[1]);
            svg.appendChild(line);
        }
    }

    sendConfig() {
        const timeout = parseInt(document.getElementById('timeout').value, 10);
        const depth = parseInt(document.getElementById('depth').value, 10);
        if (!Number.isFinite(timeout) || !Number.isFinite(depth)) return;
        this.sendMessage({ type: 'set_config', timeout_ms: timeout * 1000, max_depth: depth });
    }

    // The deadline a search needs grows with the depth it is searching to, so the
    // timeout box is bounded by it: 2× to 4× the depth, in seconds. 'min' is a
    // depth change pulling the timeout to the new floor, 'clamp' is an edited
    // timeout being held inside the range, and 'bounds' refreshes only the range so
    // a resumed session can keep the deadline it was saved with.
    syncTimeoutBounds(mode) {
        const depthEl = document.getElementById('depth');
        const timeoutEl = document.getElementById('timeout');
        // The number inputs accept anything typed, and the bounds are multiples of
        // the depth, so the depth is pinned to its own range before they are read.
        // parseInt turns a typed 0 into a number, and 0 is out of range rather
        // than empty: only a non-number falls back to the default depth.
        const typed = parseInt(depthEl.value, 10);
        const depth = Math.min(20, Math.max(2, Number.isInteger(typed) ? typed : 8));
        depthEl.value = String(depth);
        const min = depth * 2;
        const max = depth * 4;
        timeoutEl.min = String(min);
        timeoutEl.max = String(max);
        const current = parseInt(timeoutEl.value, 10);
        if (mode === 'min' || !Number.isInteger(current)) {
            timeoutEl.value = String(min);
        } else if (mode === 'clamp') {
            timeoutEl.value = String(Math.min(max, Math.max(min, current)));
        }
    }

    requestReplay() {
        this.sendMessage({ type: 'request_replay' });
    }

    handleReplay(msg) {
        // The server's move list is authoritative: it already accounts for the
        // undos the client reconciled by count only. Rebuild the local history
        // from it so the analysis indexes line up.
        const moves = Array.isArray(msg.moves) ? msg.moves : [];
        this.moveHistory = moves.map(m => ({
            row: m.row,
            col: m.col,
            color: m.color === 'black' ? BLACK : WHITE
        }));
        this.analysis = moves.map(m => ({
            score: m.score || 0,
            depth: m.depth || 0,
            nodes: m.nodes || 0,
            elapsed_ms: m.elapsed_ms || 0,
            method: m.method || ''
        }));
        this.board = emptyMatrix();
        // The stones the move list did not play: a game resumed or replayed from a
        // hand-set position would otherwise lose that position on redraw, and both
        // the scrub view and the next 摆棋 seed from them.
        this.baseStones = (Array.isArray(msg.base) ? msg.base : [])
            .filter((stone) => stone && stone.row >= 0 && stone.row < BOARD_SIZE &&
                               stone.col >= 0 && stone.col < BOARD_SIZE)
            .map((stone) => ({ row: stone.row, col: stone.col,
                               color: stone.color === 'black' ? BLACK : WHITE }));
        for (const stone of this.baseStones) {
            this.board[stone.row][stone.col] = stone.color;
        }
        for (const m of this.moveHistory) {
            this.board[m.row][m.col] = m.color;
        }
        // The snapshot may have been taken before an in-flight AI stone was
        // committed, so re-derive the turn and marker from the rebuilt history
        // rather than trusting the local flip count. The installed stones count
        // too: a game played out from a hand-set position does not open with the
        // first stone of its own list.
        const baseStones = Array.isArray(msg.base) ? msg.base.length : 0;
        this.currentTurn = ((baseStones + this.moveHistory.length) % 2 === 0) ? BLACK : WHITE;
        this.lastMove = this.moveHistory.length > 0
            ? this.moveHistory[this.moveHistory.length - 1]
            : null;
        if (msg.game_over) {
            this.gameStatus = 'game_over';
        }
        // A replay that arrived for a resumed game is not one the player asked to
        // scrub: the record and the move list are wanted, the historical view is
        // not, so the board stays on the live position.
        const resuming = this.resumeApplied;
        this.resumeApplied = false;
        this.stopReplayAuto();
        this.replayIndex = (this.moveHistory.length > 0 && !resuming) ? 0 : null;
        this.analysisEnded = !!msg.game_over;
        this.refreshReplayControls();
        this.updateMoveList();
        this.drawBoard();
        this.updateReplayInfo();
        this.updateTurnInfo();
        this.drawEvalGraph();
        if (resuming) {
            this.updateStatus(this.resumedStatusText(),
                              this.gameStatus === 'game_over' ? 'idle' : 'ok');
            return;
        }
        this.updateStatus(this.replaySummary(msg));
    }

    reasonText(reason) {
        const reasons = { five: '五连', resign: '认输', draw: '满盘和棋' };
        return reasons[reason] || reason || '未知';
    }

    replaySummary(msg) {
        if (!msg.game_over) {
            return `回放已载入：${this.moveHistory.length} 手（对局进行中）`;
        }
        const winner = msg.winner === 'draw' ? '和棋'
            : (msg.winner === (this.playerColor === BLACK ? 'black' : 'white') ? '你赢了' : '你输了');
        return `回放已载入：${this.moveHistory.length} 手 · ${winner}（${this.reasonText(msg.reason)}）`;
    }

    // A hand-set position that nobody has claimed a side for: the replay cluster
    // still belongs to the game 摆棋 replaced, and stepping through it here would
    // mix the new base stones with the old plies. Choosing a side is the action
    // the README says drops that list, not any of these buttons.
    replayBelongsToOldGame() {
        return this.gameStatus === 'waiting' && this.playerColor === null &&
            this.moveHistory.length > 0;
    }

    showReplayMove(index) {
        if (this.moveHistory.length === 0) return;
        if (this.replayBelongsToOldGame()) return;
        // The row is always clickable (README:310), so the lock the nav buttons get
        // in refreshReplayControls() has to be checked here too.
        if (this.aiThinking) return;
        const clamped = Math.max(0, Math.min(this.moveHistory.length - 1, index));
        this.replayIndex = clamped;
        this.refreshReplayControls();
        this.updateMoveList();
        this.drawBoard();
        this.updateReplayInfo();
    }

    stepReplay(delta) {
        const from = this.replayIndex === null ? (delta > 0 ? -1 : 0) : this.replayIndex;
        this.showReplayMove(from + delta);
    }

    toggleReplayAuto() {
        if (this.replayTimer) {
            this.stopReplayAuto();
            return;
        }
        if (this.replayIndex === null || this.replayIndex >= this.moveHistory.length - 1) {
            this.showReplayMove(0);
        }
        this.replayTimer = setInterval(() => {
            if (this.replayIndex >= this.moveHistory.length - 1) {
                // Reaching the last ply ends the playback, and the view has to
                // come back with it: leaving replayIndex on the final move looks
                // like the live board while the click guards still reject it.
                this.exitReplay();
                return;
            }
            this.stepReplay(1);
        }, 700);
        document.getElementById('btn-replay-play').textContent = '暂停';
    }

    stopReplayAuto() {
        if (this.replayTimer) {
            clearInterval(this.replayTimer);
            this.replayTimer = null;
        }
        document.getElementById('btn-replay-play').textContent = '自动';
    }

    exitReplay() {
        this.stopReplayAuto();
        this.replayIndex = null;
        this.refreshReplayControls();
        this.updateMoveList();
        this.drawBoard();
        this.updateReplayInfo();
        this.updateStatus(this.gameStatus === 'game_over' && this.gameOverText
            ? this.gameOverText : '已回到当前局面');
    }

    refreshReplayControls() {
        const hasMoves = this.moveHistory.length > 0;
        const nav = ['btn-replay-first', 'btn-replay-prev', 'btn-replay-play',
                     'btn-replay-next', 'btn-replay-last'];
        for (const id of nav) {
            const el = document.getElementById(id);
            // While the AI is thinking, the live board is the thing to watch;
            // while 摆棋 is open, the replay cluster belongs to the game, not
            // the working copy on screen.
            el.disabled = !hasMoves || this.aiThinking || this.setup.active ||
                this.replayBelongsToOldGame();
        }
        document.getElementById('btn-replay-exit').disabled = this.replayIndex === null;
        // Re-enable when the record is behind the game: a move played after the
        // last load has no analysis, and an ending that arrived since then is
        // not in the loaded summary either.
        const stale = this.analysis.length < this.moveHistory.length ||
            (this.gameStatus === 'game_over' && !this.analysisEnded);
        document.getElementById('btn-replay').disabled =
            !hasMoves || this.aiThinking || !stale ||
            this.replayBelongsToOldGame() || this.setup.active;
    }

    // One engine verdict, formatted. The non-AlphaBeta paths (book / vcf / vct /
    // greedy) answer without a depth or node count, so those fields are shown
    // only when the search actually filled them in.
    formatAnalysis(a, compact) {
        if (!a || a.method === 'human') return compact ? '' : '玩家落子';
        const mate = Math.abs(a.score) >= 99999;
        const verdict = mate ? (a.score > 0 ? '胜' : '负') : String(a.score);
        const parts = [a.method, verdict];
        if (a.depth) parts.push(`${a.depth}d`);
        if (a.nodes) parts.push(`${(a.nodes / 1000).toFixed(1)}k`);
        if (!compact && a.elapsed_ms != null) parts.push(`${a.elapsed_ms}ms`);
        return parts.join(' ');
    }

    updateReplayInfo() {
        const el = document.getElementById('replay-info');
        if (this.replayIndex === null) {
            el.textContent = '';
            return;
        }
        const m = this.moveHistory[this.replayIndex];
        const a = this.analysis[this.replayIndex];
        const colorName = m.color === BLACK ? '黑' : '白';
        let text = `第 ${this.replayIndex + 1}/${this.moveHistory.length} 手 · ${colorName} ${this.coordName(m.row, m.col)}`;
        if (a) {
            text += ` · ${this.formatAnalysis(a, false)}`;
        } else {
            text += ' · 尚未载入分析';
        }
        el.textContent = text;
    }

    // The curve lives in the left band, so its box is whatever that band is
    // wide: size the backing store from the element's own client box on every
    // draw instead of fixing a width in markup.
    drawEvalGraph() {
        const canvas = document.getElementById('eval-graph');
        const width = Math.max(60, canvas.clientWidth || 200);
        const height = Math.max(40, canvas.clientHeight || 64);
        canvas.width = Math.round(width * this.dpr);
        canvas.height = Math.round(height * this.dpr);
        const ctx = canvas.getContext('2d');
        ctx.setTransform(this.dpr, 0, 0, this.dpr, 0, 0);
        ctx.clearRect(0, 0, width, height);

        // Mid line and axis.
        ctx.strokeStyle = '#e0e0e0';
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(0, height / 2);
        ctx.lineTo(width, height / 2);
        ctx.stroke();
        if (this.analysis.length === 0) return;

        // Only the engine's own moves carry a verdict; a human ply scored 0 in
        // the record means "not searched", so plotting it would drag the curve
        // to the middle on every one of the player's turns.
        const limit = 2000;
        const points = [];
        this.analysis.forEach((a, i) => {
            if (!a || a.method === 'human') return;
            // The engine scores from the side that just moved, so flip the
            // opponent's numbers to read one continuous BLACK-point curve.
            const blackScore = this.moveHistory[i].color === BLACK ? a.score : -a.score;
            const clamped = Math.max(-limit, Math.min(limit, blackScore));
            const x = width * (i + 0.5) / this.analysis.length;
            const y = height / 2 - (clamped / limit) * (height / 2 - 6);
            points.push({ x, y, saturated: Math.abs(blackScore) >= 99999 });
        });
        if (points.length === 0) return;

        ctx.fillStyle = '#9a9a9a';
        ctx.font = '10px sans-serif';
        ctx.fillText('评估（黑棋视角）', 2, 10);

        ctx.strokeStyle = '#4a4a4a';
        ctx.lineWidth = 1.4;
        ctx.beginPath();
        points.forEach((p, i) => i === 0 ? ctx.moveTo(p.x, p.y) : ctx.lineTo(p.x, p.y));
        ctx.stroke();

        // Only a proven win/loss earns a dot; the curve is clipped at ±2000, so a
        // big searched score sits at the edge without meaning the game is over.
        ctx.fillStyle = '#c0392b';
        for (const p of points) {
            if (p.saturated) {
                ctx.beginPath();
                ctx.arc(p.x, p.y, 2.5, 0, Math.PI * 2);
                ctx.fill();
            }
        }
    }
    
    handleCanvasClick(e) {
        const rect = this.canvas.getBoundingClientRect();
        const x = e.clientX - rect.left;
        const y = e.clientY - rect.top;

        // The label ring outside the grid is edge information, not a cell: its
        // pixels round into row/column 0 (and 14), so a click half-way to the
        // first intersection used to land a stone there. Nothing lives to click
        // out there, so it is ignored rather than answered with an error.
        const grid_max = this.geom.size - this.geom.pad;
        if (x < this.geom.pad || x > grid_max || y < this.geom.pad || y > grid_max) return;

        // The click lands on the displayed grid, so it is mapped back to the
        // logical cell before anything else touches it.
        const shown = this.logicalOf(Math.round((y - this.geom.pad) / this.geom.cell),
                                    Math.round((x - this.geom.pad) / this.geom.cell));
        const row = shown[0];
        const col = shown[1];

        // Painting edits the 摆棋 working copy, so it answers before any of the
        // live-game guards: a board being set up is not a game in progress.
        if (this.setup.active) {
            if (this.setup.confirmInFlight) return;
            if (row >= 0 && row < BOARD_SIZE && col >= 0 && col < BOARD_SIZE) {
                this.paintSetupCell(row, col);
            }
            return;
        }

        // A hand-set position that nobody has claimed a side for starts with a stone
        // just as much as with 执黑/执白 (README:289): the server claims the colour
        // from the move and move_made brings it back, so this board is clickable.
        // An empty waiting board is not - it has no position to start.
        const pendingPosition = this.gameStatus === 'waiting' && this.playerColor === null &&
            this.baseStones.length > 0;
        if (this.aiThinking) return;
        if (this.gameStatus !== 'playing' && !pendingPosition) return;
        // A replayed position is a read-only view of the past; clicking there
        // must not send a move for the live turn. Say so, because the click
        // otherwise vanishes and the board looks like it is ignoring the player.
        if (this.replayIndex !== null) {
            this.updateStatus('正在回放历史，先退出回放才能落子', 'warn');
            return;
        }
        // Whose turn it is stays the server's call while no side is claimed:
        // playerColor is null here and would reject every click.
        if (!pendingPosition && this.currentTurn !== this.playerColor) return;
        
        if (row >= 0 && row < BOARD_SIZE && col >= 0 && col < BOARD_SIZE) {
            if (this.board[row][col] === EMPTY) {
                this.sendMove(row, col);
            }
        }
    }
    
    chooseSide(color) {
        // The config goes out first: if the AI opens the game (human picks
        // WHITE) its first search starts before any later control change could
        // reach the server.
        this.sendConfig();
        // Only claim the side once the choose_side actually left this socket.
        // A click in a disconnect window otherwise paints the optimistic state
        // over a server that never heard it, dead-ending the side buttons until
        // a refresh - the same rollback as the 确认 button.
        if (!this.sendMessage({
            type: 'choose_side',
            color: color
        })) return;

        this.playerColor = color === 'black' ? BLACK : WHITE;
        document.getElementById('btn-black').disabled = true;
        document.getElementById('btn-white').disabled = true;
        // A choice that arrives after the game settled - only a stale frame can
        // deliver it, the ending disables these buttons - must not re-arm them.
        if (this.gameStatus !== 'game_over') {
            document.getElementById('btn-undo').disabled = false;
            document.getElementById('btn-hint').disabled = false;
            this.hintInFlight = false;
            document.getElementById('btn-resign').disabled = false;
        }
        document.getElementById('btn-restart').disabled = false;
    }
    
    sendMove(row, col) {
        this.sendMessage({
            type: 'make_move',
            row: row,
            col: col
        });
    }
    
    undo() {
        this.sendMessage({ type: 'undo' });
    }
    
    requestHint() {
        if (this.hintInFlight) return;
        const timeout = parseInt(document.getElementById('timeout').value, 10);
        const depth = parseInt(document.getElementById('depth').value, 10);
        // Same finite guard as sendConfig: an emptied box would serialise to
        // null, and the server would silently run its own 5000ms/depth-6
        // default instead of the settings the player thinks they asked for.
        if (!Number.isFinite(timeout) || !Number.isFinite(depth)) {
            this.updateStatus('超时和深度需要先填好数字', 'warn');
            return;
        }
        if (!this.sendMessage({
            type: 'request_hint',
            timeout_ms: timeout * 1000,
            max_depth: depth
        })) return;
        this.hintInFlight = true;
        this.hintMoveCount = this.moveHistory.length;
        document.getElementById('btn-hint').disabled = true;
    }
    
    resign() {
        if (confirm('确定要认输吗？')) {
            this.sendMessage({ type: 'resign' });
        }
    }
    
    stopThinking() {
        this.sendMessage({ type: 'stop_thinking' });
        // Disarm immediately so a double-click cannot send a second stop or
        // leave the control armed while the worker winds down.
        this.aiThinking = false;
        document.getElementById('btn-stop').disabled = true;
        // The thinking controls went grey with aiThinking, so they come back with
        // it; the player is owed the ordinary rules, not a stuck board.
        document.getElementById('btn-undo').disabled =
            this.playerColor === null || this.gameStatus === 'game_over';
        // 终止 only stops the search early - the hint still answers, so the control
        // stays disabled while that answer is in flight instead of pretending idle.
        document.getElementById('btn-hint').disabled =
            this.playerColor === null || this.gameStatus === 'game_over' || this.hintInFlight;
        // The replay cluster was greyed with aiThinking, so it comes back with it.
        this.refreshReplayControls();
    }
    
    // ---- 摆棋 (position setup) ---------------------------------------
    //
    // Setup edits a working copy of the board. Entering it seeds that copy from
    // what is on the screen now — the live game's stones and its move record — so
    // the player can either paint on the position they are looking at or 清空 it
    // and start over, and neither touches the session. 确认 is the only thing that
    // talks to the server; 取消 drops the copy, which is what restores the board.

    toggleSetup() {
        if (this.setup.active) {
            this.exitSetup(false);
            return;
        }
        this.enterSetup();
    }

    enterSetup() {
        const setup = this.setup;
        // The left-hand replay cluster is the only control still on screen while a
        // position is being set up, so it is the only one put away by disabling —
        // the bottom bar swaps its game buttons and AI settings out for the setup
        // buttons instead, which is what they were standing in for anyway.
        setup.gameButtons = ['btn-replay', 'btn-replay-first', 'btn-replay-prev',
                             'btn-replay-play', 'btn-replay-next', 'btn-replay-last',
                             'btn-replay-exit']
            .map((id) => [id, document.getElementById(id).disabled]);
        setup.active = true;
        setup.board = this.board.map((row) => row.slice());
        setup.record = this.setupSeedRecord();
        setup.view = null;
        setup.preview = null;
        setup.confirmInFlight = false;
        this.setSetupBar(true);
        document.getElementById('btn-position-toggle').classList.add('toggled');
        document.getElementById('position-text').value = '';
        document.getElementById('import-status').textContent = '';
        for (const [id] of setup.gameButtons) {
            document.getElementById(id).disabled = true;
        }
        this.stopReplayAuto();
        // A setup opened on a historical view would leave that view in place on
        // the way out, so 取消 would restore a replay position instead of the
        // game's current board. Same unconditional exit as an undo.
        this.replayIndex = null;
        this.updateReplayInfo();
        this.renderSetupPanel();
        this.drawBoard();
        this.updateStatus('摆棋中：点击棋盘任意位置后依次标记为 黑 / 白 / 空，'
                          + '右侧摆棋记录可点任意步骤预览。', 'warn');
    }

    // The bottom bar's two faces: the game's controls, or the setup's standing in
    // the slots those controls vacate.
    setSetupBar(on) {
        for (const id of ['group-side', 'group-play', 'group-config']) {
            document.getElementById(id).hidden = on;
        }
        for (const id of ['group-setup', 'group-commit', 'group-swap']) {
            document.getElementById(id).hidden = !on;
        }
    }

    // The record starts as the game's own move list, with the stones that list does
    // not explain — the ones a hand-set position installed — in front of it, so the
    // record and the board always describe the same position.
    setupSeedRecord() {
        const played = new Set(this.moveHistory.map((m) => m.row * BOARD_SIZE + m.col));
        const extras = [];
        for (let row = 0; row < BOARD_SIZE; ++row) {
            for (let col = 0; col < BOARD_SIZE; ++col) {
                const color = this.board[row][col];
                if (color !== EMPTY && !played.has(row * BOARD_SIZE + col)) {
                    extras.push({ row, col, color });
                }
            }
        }
        return [...extras,
                ...this.moveHistory.map((m) => ({ row: m.row, col: m.col, color: m.color }))];
    }

    // Dropping the working copy. `confirmed` only picks the words: the board the
    // player sees afterwards is the live one either way, because setup never wrote
    // to it.
    exitSetup(confirmed) {
        const setup = this.setup;
        if (!setup.active) return;
        setup.active = false;
        setup.board = null;
        setup.record = [];
        setup.view = null;
        setup.preview = null;
        setup.confirmInFlight = false;
        // The round trip is over, so the three companions of 确认 are clickable again.
        document.getElementById('btn-setup-cancel').disabled = false;
        document.getElementById('btn-setup-clear').disabled = false;
        document.getElementById('btn-position-toggle').disabled = false;
        this.setSetupBar(false);
        document.getElementById('btn-position-toggle').classList.remove('toggled');
        this.closeImportModal();
        this.updatePositionStatus('');
        for (const [id, disabled] of setup.gameButtons || []) {
            document.getElementById(id).disabled = disabled;
        }
        setup.gameButtons = null;
        this.renderLists();
        this.refreshReplayControls();
        this.drawBoard();
        if (!confirmed) {
            // An ending that arrived while the copy was open is painted here, the
            // one place the player stops looking at the working board (README:379).
            this.updateStatus(this.gameStatus === 'game_over' && this.gameOverText
                ? this.gameOverText : '已退出摆棋：棋盘已还原，落子记录未清空', 'idle');
        }
    }

    paintSetupCell(row, col) {
        const setup = this.setup;
        const next = (setup.board[row][col] + 1) % 3;
        setup.board[row][col] = next;
        // Editing is about the whole position, so a step preview ends here rather
        // than leaving the player painting onto a board they are not looking at.
        setup.view = null;
        const index = setup.record.findIndex((m) => m.row === row && m.col === col);
        if (next === EMPTY) {
            if (index >= 0) setup.record.splice(index, 1);
        } else if (index >= 0) {
            setup.record[index].color = next;
        } else {
            setup.record.push({ row, col, color: next });
        }
        setup.preview = null;
        this.renderSetupPanel();
        this.drawBoard();
    }

    // 清空 empties the board the player is setting up. The game's own move record
    // is not touched: it is what 取消 goes back to, and what 确认 keeps until a side
    // is chosen.
    clearSetupBoard() {
        const setup = this.setup;
        if (!setup.active) return;
        setup.board = emptyMatrix();
        setup.record = [];
        setup.view = null;
        setup.preview = null;
        // The note rides into renderSetupPanel so README:360's count line stays.
        this.renderSetupPanel('棋盘已清空（落子记录仍在，取消可还原）。');
        this.drawBoard();
    }

    // Taking a recognised board as the working copy. The record is row-major
    // because a grid or bitboard carries no order; the position is the same either
    // way, and the point of the record is to step through stones, not history.
    applySetupBoard(matrix) {
        const setup = this.setup;
        setup.board = matrix.map((row) => row.slice());
        setup.record = [];
        for (let row = 0; row < BOARD_SIZE; ++row) {
            for (let col = 0; col < BOARD_SIZE; ++col) {
                const color = setup.board[row][col];
                if (color !== EMPTY) setup.record.push({ row, col, color });
            }
        }
        setup.view = null;
    }

    // 交换 means "the marks were read the wrong way round". The bar's trio applies
    // it to the 摆棋 copy; the modal's applies it to the recognised preview, which
    // is where a mis-read file is still cheap to fix.
    swapMarks(from, to, target) {
        const names = { [BLACK]: '黑', [WHITE]: '白', [EMPTY]: '空' };
        if (target === 'preview') {
            const preview = this.setup.preview;
            if (!preview || !preview.ok) return;
            preview.board = swappedMatrix(preview.board, from, to);
            // The mapping is a claim about the source symbols, so it has to move
            // with the board or the line under it would describe a different read.
            if (preview.encoding) {
                const keys = { [BLACK]: 'black', [WHITE]: 'white', [EMPTY]: 'empty' };
                const enc = preview.encoding;
                [enc[keys[from]], enc[keys[to]]] = [enc[keys[to]], enc[keys[from]]];
            }
            this.renderImportState(`已交换 ${names[from]} ↔ ${names[to]}。`);
            return;
        }
        const setup = this.setup;
        if (!setup.active) return;
        const matrix = swappedMatrix(setup.board, from, to);
        // Swapping two colours keeps every cell occupied, so the record's order (and
        // with it the move list it was seeded from) survives with the colours
        // recoloured. Involving 空 changes which cells are occupied at all, so the
        // record is rebuilt from the board.
        if (from !== EMPTY && to !== EMPTY) {
            for (const stone of setup.record) {
                if (stone.color === from) stone.color = to;
                else if (stone.color === to) stone.color = from;
            }
            setup.board = matrix;
        } else {
            this.applySetupBoard(matrix);
        }
        this.renderSetupPanel(`已交换 ${names[from]} ↔ ${names[to]}。`);
        this.drawBoard();
    }

    confirmSetup() {
        const setup = this.setup;
        if (!setup.active || setup.confirmInFlight) return;
        const counts = matrixCounts(setup.board);
        if (counts.black + counts.white === 0) {
            this.updatePositionStatus('棋盘是空的，先摆几子再确认。', true);
            return;
        }
        // The side buttons are hidden while 摆棋 is open, so nothing can be clicked
        // in between; holding them would only have to be undone on the way out.
        setup.confirmInFlight = true;
        this.refreshSetupControls();
        const sent = this.sendMessage({
            type: 'set_position',
            position: { version: 1, type: 'gomoku_position',
                        encoding: { black: 1, white: 2, empty: 0 },
                        board: setup.board.map((row) => row.slice()) },
        });
        // Nothing left this socket never has to come back: no set_position was
        // sent, so no position_set/error will ever arrive to release 确认.
        // Roll it back so the player can retry after reconnecting.
        if (!sent) {
            setup.confirmInFlight = false;
            this.refreshSetupControls();
        }
    }

    handlePositionSet(msg) {
        const wasSetup = this.setup.active;
        this.exitSetup(true);
        this.applyBoardMatrix(msg.board);
        // The installed stones are the base the record sits on: the replay view and
        // the next 摆棋 both seed from them.
        this.baseStones = [];
        for (let row = 0; row < BOARD_SIZE; ++row) {
            for (let col = 0; col < BOARD_SIZE; ++col) {
                if (msg.board[row][col] !== EMPTY) {
                    this.baseStones.push({ row, col, color: msg.board[row][col] });
                }
            }
        }
        // 落子记录 stays until a side is chosen: the player is looking at a position
        // they have not committed to yet, and the old list is what 取消 goes back to.
        this.stopReplayAuto();
        this.analysis = [];
        this.analysisEnded = false;
        this.replayIndex = null;
        this.lastMove = null;
        this.recommendation = null;
        this.currentTurn = msg.current_turn === 'black' ? BLACK : WHITE;
        this.gameStatus = 'waiting';
        this.playerColor = null;
        this.renderLists();
        this.updateReplayInfo();
        this.drawEvalGraph();
        this.refreshReplayControls();
        document.getElementById('btn-black').disabled = false;
        document.getElementById('btn-white').disabled = false;
        document.getElementById('btn-undo').disabled = true;
        document.getElementById('btn-hint').disabled = true;
        document.getElementById('btn-resign').disabled = true;
        document.getElementById('btn-stop').disabled = true;
        document.getElementById('btn-restart').disabled = false;
        this.drawBoard();
        this.updateStatus(`局面已摆好：轮到${this.currentTurn === BLACK ? '黑' : '白'}方，请选择执子颜色`,
                          'ok');
        this.updateTurnInfo('');
        if (!wasSetup) this.updatePositionStatus('', false);
    }

    // ---- 导入 (file / paste / drop, auto-recognised) ------------------

    openImportModal() {
        this.setup.preview = null;
        document.getElementById('import-modal').hidden = false;
        document.getElementById('position-text').focus();
        this.renderImportState('');
    }

    closeImportModal() {
        document.getElementById('import-modal').hidden = true;
    }

    // The paste box is the payload: a file or a dragged board lands in it, and
    // sending the text verbatim is what lets the server's auto-recognition run.
    importPayload() {
        return document.getElementById('position-text').value.trim();
    }

    checkPosition() {
        const text = this.importPayload();
        if (!text) {
            this.updateImportStatus('请先粘贴棋盘内容，或选择一个文件。', true);
            return;
        }
        this.setup.preview = null;
        this.setup.checkedText = text;
        this.renderImportState('');
        this.sendMessage({ type: 'check_position', position: text });
    }

    handlePositionPreview(msg) {
        // A stale reply re-arms 确认 with a reading nobody is looking at, so it is
        // dropped before it touches preview or the panel.
        if (this.importPayload() !== this.setup.checkedText) return;
        this.setup.preview = msg.ok ? msg : null;
        this.renderImportState(msg.ok ? '' : (msg.error || '无法识别此局面。'), !msg.ok);
    }

    // One render for the modal's read state: what the marks were taken to be, how
    // many stones that gives, and whether 确认 has anything to apply. `note` is the
    // reason the line is being redrawn — a swap just made, or a rejection to show.
    renderImportState(note, isError) {
        const preview = this.setup.preview;
        const mappingEl = document.getElementById('position-mapping');
        if (!preview) {
            mappingEl.textContent = '识别到的标志：—';
            this.updateImportStatus(note, !!isError);
            this.refreshSetupControls();
            return;
        }
        const enc = preview.encoding;
        // A bitboard or a move list carries no symbols, so there is nothing to
        // report and nothing to correct — the swap trio has no meaning for it.
        mappingEl.textContent = enc
            ? `识别到的标志：黑=${enc.black} 白=${enc.white} 空=${enc.empty}`
            : '此形式没有符号，无需修正标志';
        const counts = matrixCounts(preview.board);
        const summary = `黑 ${counts.black} 子、白 ${counts.white} 子，`
            + `轮到${(counts.black + counts.white) % 2 === 0 ? '黑' : '白'}方`;
        this.updateImportStatus(`${note ? note + ' ' : ''}识别结果：${summary}。`
            + '点“确认”写入棋盘。', false);
        this.refreshSetupControls();
    }

    applyImport() {
        const preview = this.setup.preview;
        if (!preview || !preview.ok) return;
        this.applySetupBoard(preview.board);
        this.setup.preview = null;
        this.closeImportModal();
        document.getElementById('position-text').value = '';
        this.renderSetupPanel('已导入识别出的局面，可继续手动修改后确认。');
        this.drawBoard();
    }

    // ---- the setup bar's own rendering --------------------------------

    renderSetupPanel(note) {
        this.renderLists();
        this.refreshSetupControls();
        const counts = matrixCounts(this.setup.board);
        this.updatePositionStatus(
            (note ? note + ' ' : '') +
            `摆棋中：黑 ${counts.black} 子、白 ${counts.white} 子，` +
            `轮到${(counts.black + counts.white) % 2 === 0 ? '黑' : '白'}方。` +
            `确认后才写入棋局。`, false);
    }

    updateSetupList() {
        const list = document.getElementById('setup-list');
        list.innerHTML = '';
        this.setup.record.forEach((move, index) => {
            const div = document.createElement('div');
            const isLive = this.setup.view === null
                ? index === this.setup.record.length - 1
                : index === this.setup.view;
            div.className = `move-item ${move.color === BLACK ? 'black' : 'white'}`
                + (isLive ? ' active' : '');
            div.textContent = `${index + 1}. ${move.color === BLACK ? '黑' : '白'} `
                + this.coordName(move.row, move.col);
            div.addEventListener('click', () => this.showSetupStep(index));
            list.appendChild(div);
        });
        if (this.setup.record.length === 0) {
            const div = document.createElement('div');
            div.className = 'move-item';
            div.textContent = '（摆棋记录为空）';
            list.appendChild(div);
        }
    }

    // Clicking a step switches the board to that step's stone state; clicking the
    // highlighted step again drops back to the whole position.
    showSetupStep(index) {
        this.setup.view = this.setup.view === index ? null : index;
        this.renderSetupPanel();
        this.drawBoard();
    }

    renderLists() {
        const inSetup = this.setup.active;
        document.getElementById('list-title').textContent = inSetup ? '摆棋记录' : '落子记录';
        document.getElementById('setup-list').hidden = !inSetup;
        document.getElementById('move-list').hidden = inSetup;
        if (inSetup) {
            this.updateSetupList();
        } else {
            this.updateMoveList();
        }
    }

    refreshSetupControls() {
        const setup = this.setup;
        const read = !!(setup.preview && setup.preview.ok);
        document.getElementById('btn-import-apply').disabled = !read;
        // The modal's trio only means something once there is a read to correct,
        // and only while that read came with symbols that can be wrong about.
        for (const button of document.querySelectorAll('#import-swap [data-swap]')) {
            button.disabled = !read || !setup.preview.encoding;
        }
        // 确认 is held while the server has the position in hand, so a double click
        // cannot send two loads.
        document.getElementById('btn-setup-confirm').disabled = setup.confirmInFlight;
        // 确认的往返期间副本不再可弃：取消 / 清空 / 摆棋开关与 确认 一起按住。
        document.getElementById('btn-setup-cancel').disabled = setup.confirmInFlight;
        document.getElementById('btn-setup-clear').disabled = setup.confirmInFlight;
        document.getElementById('btn-position-toggle').disabled = setup.confirmInFlight;
    }

    updatePositionStatus(text, isError) {
        const el = document.getElementById('position-status');
        el.textContent = text;
        el.classList.toggle('error', !!isError);
    }

    updateImportStatus(text, isError) {
        const el = document.getElementById('import-status');
        el.textContent = text;
        el.classList.toggle('error', !!isError);
    }

    // A side can only be claimed on a board that has not started. Once the game is
    // playing - or has ended - the way on is 新对局/再来一局, and leaving these
    // armed would let a finished board take a second colour choice.
    sideChoicePending() {
        return this.gameStatus === 'waiting' && this.playerColor === null;
    }

    setSideButtons(blackDisabled, whiteDisabled) {
        document.getElementById('btn-black').disabled = blackDisabled;
        document.getElementById('btn-white').disabled = whiteDisabled;
    }

    exportPosition() {
        // While 摆棋 is open the player is working on a copy, and that copy is what
        // they mean by "the board" — exporting the live position instead would
        // write out stones they had just painted off.
        const matrix = this.setup.active ? this.setup.board : this.board;
        const doc = {
            version: 1,
            type: 'gomoku_position',
            encoding: { black: 1, white: 2, empty: 0 },
            board: matrix.map(row => row.slice())
        };
        const blob = new Blob([JSON.stringify(doc, null, 2)], { type: 'application/json' });
        const link = document.createElement('a');
        link.href = URL.createObjectURL(blob);
        link.download = 'position.json';
        link.click();
        URL.revokeObjectURL(link.href);
    }

    applyBoardMatrix(matrix) {
        if (!Array.isArray(matrix)) return;
        for (let row = 0; row < BOARD_SIZE; ++row) {
            for (let col = 0; col < BOARD_SIZE; ++col) {
                const value = matrix[row] ? matrix[row][col] : EMPTY;
                this.board[row][col] = Number.isInteger(value) ? value : EMPTY;
            }
        }
        this.drawBoard();
    }

    restart() {
        // 重开 means "settle this game and take a new token", and offline the
        // server never hears it - clearing the board anyway would show an empty
        // game that the stored token resumes back into the old one.
        if (!this.sendMessage({ type: 'restart' })) {
            this.updateStatus('未连接，重开没有生效', 'error');
            return;
        }
        this.resetGame();
    }
    
    resetGame() {
        this.board = emptyMatrix();
        this.moveHistory = [];
        this.currentTurn = BLACK;
        this.gameStatus = 'waiting';
        // The server starts a restarted game with nobody committed to a side, so
        // holding onto the old colour would let a click land before the player has
        // chosen again.
        this.playerColor = null;
        this.aiThinking = false;
        // Whatever hint was in flight belonged to the game that just ended; the
        // button state below is what the new game starts from.
        this.hintInFlight = false;
        this.lastMove = null;
        this.recommendation = null;
        // A new game has no history to replay, and stale analysis would attach
        // the old game's numbers to the new moves.
        this.stopReplayAuto();
        this.analysis = [];
        this.analysisEnded = false;
        this.replayIndex = null;
        this.gameOverText = '';
        // A restarted game has no position to set up: the working copy would paint
        // onto a board that no longer exists, so it goes with it. exitSetup first —
        // it hands the buttons back, and every line below then says what the new
        // game actually does with them.
        this.exitSetup(true);
        this.baseStones = [];
        this.refreshReplayControls();
        this.updateReplayInfo();
        this.drawEvalGraph();
        
        document.getElementById('btn-black').disabled = false;
        document.getElementById('btn-white').disabled = false;
        document.getElementById('btn-undo').disabled = true;
        document.getElementById('btn-hint').disabled = true;
        document.getElementById('btn-resign').disabled = true;
        document.getElementById('btn-stop').disabled = true;
        document.getElementById('btn-restart').disabled = true;
        
        this.renderLists();
        this.drawBoard();
        this.updateStatus('等待开始');
        this.updateTurnInfo('');
    }
    
    sendMessage(msg) {
        if (this.ws && this.ws.readyState === WebSocket.OPEN) {
            this.ws.send(JSON.stringify(msg));
            return true;
        }
        if (msg.type !== 'ping') {
            // Say it out loud: a click that quietly vanished reads as the AI
            // ignoring the player. A taken-over tab is not reconnecting, so it
            // gets its own line instead of a promise of a retry that will not come.
            this.updateStatus(this.takenOver
                                  ? '本局面已在其他标签页打开，刷新页面可取回'
                                  : '连接已断开，正在重连：消息未发出',
                              this.takenOver ? 'warn' : 'error');
        }
        return false;
    }
    
    handleServerMessage(msg) {
        switch (msg.type) {
            case 'session':
                this.handleSession(msg);
                break;
            case 'session_taken_over':
                this.handleTakenOver();
                break;
            case 'state_sync':
                this.handleStateSync(msg);
                break;
            case 'move_made':
                this.handleMoveMade(msg);
                break;
            case 'ai_move':
                this.handleAIMove(msg);
                break;
            case 'undo_success':
                this.handleUndoSuccess(msg);
                break;
            case 'replay':
                this.handleReplay(msg);
                break;
            case 'position_preview':
                this.handlePositionPreview(msg);
                break;
            case 'position_set':
                this.handlePositionSet(msg);
                break;
            case 'hint_result':
                this.handleHintResult(msg);
                break;
            case 'thinking_progress':
                this.handleThinkingProgress(msg);
                break;
            case 'game_over':
                this.handleGameOver(msg);
                break;
            case 'game_restarted':
                this.resetGame();
                break;
            case 'error':
                // A refused 确认 leaves the setup open with its working copy intact:
                // re-arm 确认 so the player can fix what was objected to and retry.
                if (this.setup.confirmInFlight) {
                    this.setup.confirmInFlight = false;
                    this.refreshSetupControls();
                }
                // The hint's failure reply is this same frame, so the button comes
                // back for a retry instead of staying armed on a search that ended.
                if (this.hintInFlight) {
                    this.hintInFlight = false;
                    document.getElementById('btn-hint').disabled =
                        this.playerColor === null || this.gameStatus === 'game_over';
                }
                this.updateStatus(`错误：${msg.message}`, 'error');
                break;
        }
    }
    
    handleStateSync(msg) {
        // state_sync is the server's authoritative board at the moment the game
        // starts (choose_side), so it has to overwrite whatever the client is
        // showing: an un-loaded position preview would otherwise stay on screen
        // while the server plays an empty board.
        this.applyBoardMatrix(msg.board);
        // Choosing a side is what commits a hand-set position: the game now runs as
        // a new one from this board, so every stone on it is base and the old 落子
        //记录 — the game that was played before the position was set — is dropped.
        this.baseStones = [];
        if (Array.isArray(msg.board)) {
            for (let row = 0; row < BOARD_SIZE; ++row) {
                for (let col = 0; col < BOARD_SIZE; ++col) {
                    if (msg.board[row][col] !== EMPTY) {
                        this.baseStones.push({ row, col, color: msg.board[row][col] });
                    }
                }
            }
        }
        this.moveHistory = [];
        this.analysis = [];
        this.analysisEnded = false;
        this.stopReplayAuto();
        this.replayIndex = null;
        this.lastMove = null;
        this.recommendation = null;
        this.currentTurn = msg.current_turn === 'black' ? BLACK : WHITE;
        this.gameStatus = 'playing';
        this.renderLists();
        this.updateReplayInfo();
        this.drawEvalGraph();
        this.refreshReplayControls();
        this.updateTurnInfo();
        this.updateStatus('对局开始', 'ok');
    }
    
    handleMoveMade(msg) {
        // A live stone ends any historical view: staying on the replayed ply would
        // paint the board from the past while the game moved on, and every click
        // would then be dropped by the replay guard.
        if (this.replayIndex !== null) this.exitReplay();
        // A hand-set position starts with a stone just as much as with 执黑/执白
        // (README:289), and this frame's colour is the side the server claimed for
        // it. Without this the colour stayed null and the game stayed waiting:
        // 悔棋/求招 stayed disabled, the 终止 button never armed, and a black win
        // was read as a loss.
        if (this.gameStatus === 'waiting' && this.playerColor === null &&
            this.baseStones.length > 0) {
            this.playerColor = msg.color === 'black' ? BLACK : WHITE;
            document.getElementById('btn-black').disabled = true;
            document.getElementById('btn-white').disabled = true;
            if (this.gameStatus !== 'game_over') {
                document.getElementById('btn-undo').disabled = false;
                document.getElementById('btn-hint').disabled = false;
                this.hintInFlight = false;
                document.getElementById('btn-resign').disabled = false;
            }
            // The 落子记录 held back for the side choice goes the way handleStateSync
            // drops it on 执黑/执白: this stone is what commits the position.
            this.moveHistory = [];
            this.gameStatus = 'playing';
        }
        this.placeStone(msg.row, msg.col, msg.color === 'black' ? BLACK : WHITE);
        this.currentTurn = this.currentTurn === BLACK ? WHITE : BLACK;
        this.updateTurnInfo();
        this.refreshReplayControls();
    }
    
    handleAIMove(msg) {
        if (this.replayIndex !== null) this.exitReplay();
        this.placeStone(msg.row, msg.col, msg.color === 'black' ? BLACK : WHITE);
        this.currentTurn = this.currentTurn === BLACK ? WHITE : BLACK;
        this.aiThinking = false;
        document.getElementById('btn-stop').disabled = true;
        // Thinking greyed these out; the finished search owes them back.
        document.getElementById('btn-undo').disabled =
            this.playerColor === null || this.gameStatus === 'game_over';
        document.getElementById('btn-hint').disabled =
            this.playerColor === null || this.gameStatus === 'game_over';
        // ai_move is the only live message that carries the engine's verdict for
        // the stone it just placed, so surface it in the status line.
        if (!this.setup.active) {
            this.updateStatus(`AI ${this.coordName(msg.row, msg.col)} · 分值 ${msg.score} · ${msg.thinking_time_ms}ms`,
                              'idle');
        }
        this.updateTurnInfo();
        this.refreshReplayControls();
    }
    
    handleUndoSuccess(msg) {
        // The server may remove 1 or 2 stones (its own reply plus the human's,
        // so a single undo reverts a full round), and its count is every stone
        // left on the board — the ones a hand-set position installed included. The
        // player's own list is what lies beyond that base, and once the played
        // stones run out the undo eats into the base from its end, which is what
        // the board's stack pops. Rebuild the whole position from those two halves
        // instead of assuming one local pop, and take the turn from parity (BLACK
        // always opens) rather than a local flip.
        const total = Number.isInteger(msg.move_count)
            ? msg.move_count
            : this.baseStones.length + Math.max(0, this.moveHistory.length - 1);
        const kept = this.baseStones.slice(0, Math.min(total, this.baseStones.length));
        const played = Math.max(0, Math.min(total - kept.length, this.moveHistory.length));
        this.baseStones = kept;
        this.moveHistory = this.moveHistory.slice(0, played);
        this.board = emptyMatrix();
        for (const stone of kept) this.board[stone.row][stone.col] = stone.color;
        for (const m of this.moveHistory) this.board[m.row][m.col] = m.color;
        this.currentTurn = (total % 2 === 0) ? BLACK : WHITE;
        this.lastMove = this.moveHistory.length > 0
            ? this.moveHistory[this.moveHistory.length - 1]
            : null;
        // The popped plies shift every analysis index after them, and a replay
        // position that no longer exists is worse than no replay at all, so the
        // view exits unconditionally; the loaded record itself is dropped only
        // when it actually covers the popped plies.
        this.stopReplayAuto();
        this.recommendation = null;
        this.replayIndex = null;
        this.updateReplayInfo();
        if (this.analysis.length > played) {
            this.analysis = [];
            this.analysisEnded = false;
            this.drawEvalGraph();
        }
        if (msg.game_status === 'waiting') {
            // The server treats a WAITING board with a chosen side as playable
            // (make_move promotes and commits), so mirroring that promotion is
            // what keeps an undo-to-empty-board from locking the player out of
            // a game the server still lets them play in.
            this.gameStatus = this.playerColor !== null ? 'playing' : 'waiting';
            if (this.playerColor !== null) {
                if (!this.setup.active) {
                    this.updateStatus(`已悔至空盘：轮到${this.currentTurn === BLACK ? '黑' : '白'}方，继续落子`, 'ok');
                }
            }
        } else if (msg.game_status === 'game_over') {
            this.gameStatus = 'game_over';
        } else {
            this.gameStatus = 'playing';
        }
        this.updateMoveList();
        this.updateTurnInfo();
        this.refreshReplayControls();
        this.drawBoard();
        // An undo does not abort a hint search that is still running: the search
        // finishes and still answers, so this only hands the control back. The
        // answer for the pre-undo position is voided by the ply-count check.
        this.hintInFlight = false;
        document.getElementById('btn-hint').disabled =
            this.playerColor === null || this.gameStatus === 'game_over';
    }
    
    handleHintResult(msg) {
        this.hintInFlight = false;
        document.getElementById('btn-hint').disabled =
            this.playerColor === null || this.gameStatus === 'game_over';
        // 摆棋 never touches moveHistory, so the ply-count test below cannot see
        // that the board the hint was searched on is gone; and the ring would land
        // on the working copy the display shows. Release the control above, then
        // drop the answer whole - no ring, no line over 摆棋中.
        if (this.setup.active) return;
        // The server searched the position as it was when 求招 left; every path
        // that changes the position pushes onto moveHistory, so a different ply
        // count means the answer can name a square that is occupied now, or one
        // that no longer exists. Release the control, say the hint is void, and
        // leave the board alone.
        if (this.moveHistory.length !== this.hintMoveCount) {
            this.updateStatus('建议已失效', 'idle');
            return;
        }
        // The ring marks a square of the live position; leave any replay view
        // first so it is not drawn on a historical board.
        if (this.replayIndex !== null) {
            this.exitReplay();
        }
        this.highlightMove(msg.row, msg.col);
        this.updateStatus(`建议 ${this.coordName(msg.row, msg.col)} · 分值 ${msg.score}`, 'ok');
    }
    
    handleThinkingProgress(msg) {
        // thinking_progress is shared by the AI's move search and hint searches.
        // Only treat it as "the AI is thinking" when it is actually the AI's
        // turn; a hint runs on the player's turn, and setting aiThinking there
        // would never be cleared (hints emit no ai_move) and would freeze the
        // board. This is the sole place that arms the stop control.
        if (this.gameStatus === 'playing' && this.currentTurn !== this.playerColor) {
            this.aiThinking = true;
            document.getElementById('btn-stop').disabled = false;
            // The server refuses both while it is thinking, so a click there is
            // only a red error line; grey them out until the search is over.
            document.getElementById('btn-undo').disabled = true;
            document.getElementById('btn-hint').disabled = true;
            this.refreshReplayControls();
        }
    }
    
    handleGameOver(msg) {
        this.gameStatus = 'game_over';
        this.aiThinking = false;
        this.hintInFlight = false;
        // The ending belongs to the live board. Left in a replay view the player
        // keeps staring at a historical position with no way back, because every
        // click is still rejected by the replay guard.
        if (this.replayIndex !== null) this.exitReplay();
        let statusText = '游戏结束 - ';
        let kind = 'idle';
        if (msg.winner === 'draw') {
            statusText += '和棋';
        } else {
            const winnerColor = msg.winner === 'black' ? '黑棋' : '白棋';
            if (msg.winner === (this.playerColor === BLACK ? 'black' : 'white')) {
                statusText += `你赢了！(${winnerColor})`;
                kind = 'ok';
            } else {
                statusText += `你输了 (${winnerColor}胜)`;
                kind = 'warn';
            }
        }
        // The reason the engine reports (five / resign / draw) is part of the
        // result the player is owed, not only a status code.
        statusText += ` · ${this.reasonText(msg.reason)}`;
        this.gameOverText = statusText;
        // README:379: 摆棋 owns the top line while it is open; gameOverText keeps
        // the result and exitSetup paints it back when the copy is dropped.
        if (!this.setup.active) {
            this.updateStatus(statusText, kind);
            this.updateTurnInfo('');
        }
        
        document.getElementById('btn-undo').disabled = true;
        document.getElementById('btn-hint').disabled = true;
        document.getElementById('btn-resign').disabled = true;
        document.getElementById('btn-stop').disabled = true;
        // A settled board takes no colour choice: picking one here would re-arm
        // 悔棋/求招/认输 for a game that is already over.
        this.setSideButtons(true, true);
        this.refreshReplayControls();
    }
    
    placeStone(row, col, color) {
        this.board[row][col] = color;
        this.lastMove = { row, col, color };
        this.recommendation = null;
        this.moveHistory.push({ row, col, color });
        this.updateMoveList();
        this.drawBoard();
    }
    
    highlightMove(row, col) {
        this.recommendation = { row, col };
        this.drawBoard();
    }
    
    // The board as the player should see it right now: the 摆棋 working copy while
    // that mode is open, otherwise the live position — or the position after
    // replayIndex plies when a historical view is active. Scrubbing lays the
    // installed stones down first, so a game played out from a hand-set position
    // scrubs to that position's history rather than to the moves alone.
    displayView() {
        if (this.setup.active) {
            if (this.setup.view === null) {
                return { board: this.setup.board, lastMove: this.setupLastMove() };
            }
            const board = emptyMatrix();
            let lastMove = null;
            for (let i = 0; i <= this.setup.view && i < this.setup.record.length; ++i) {
                const m = this.setup.record[i];
                board[m.row][m.col] = m.color;
                lastMove = m;
            }
            return { board, lastMove };
        }
        if (this.replayIndex === null) {
            return { board: this.board, lastMove: this.lastMove };
        }
        const board = emptyMatrix();
        for (const stone of this.baseStones) {
            board[stone.row][stone.col] = stone.color;
        }
        let lastMove = this.lastMove;
        for (let i = 0; i <= this.replayIndex && i < this.moveHistory.length; ++i) {
            const m = this.moveHistory[i];
            board[m.row][m.col] = m.color;
            lastMove = m;
        }
        return { board, lastMove };
    }

    setupLastMove() {
        return this.setup.record.length > 0
            ? this.setup.record[this.setup.record.length - 1]
            : null;
    }

    // A displayed cell -> its point on the canvas, in CSS pixels.
    cellCenter(dr, dc) {
        return { x: this.geom.pad + dc * this.geom.cell,
                 y: this.geom.pad + dr * this.geom.cell };
    }

    // Which logical coordinate an edge names depends on the view: at 90° the top
    // edge runs along rows, not columns. So ask the transform which component is
    // constant along the line and label with that one.
    columnLabel(dc) {
        const a = this.logicalOf(0, dc);
        const b = this.logicalOf(BOARD_SIZE - 1, dc);
        return a[1] === b[1] ? String.fromCharCode(65 + a[1]) : String(BOARD_SIZE - a[0]);
    }

    rowLabel(dr) {
        const a = this.logicalOf(dr, 0);
        const b = this.logicalOf(dr, BOARD_SIZE - 1);
        return a[0] === b[0] ? String(BOARD_SIZE - a[0]) : String.fromCharCode(65 + a[1]);
    }

    drawBoard() {
        const ctx = this.ctx;
        const { size, cell, pad, stone, font } = this.geom;
        const view = this.displayView();
        const far = size - pad;
        
        ctx.clearRect(0, 0, size, size);
        
        ctx.strokeStyle = '#b6ab97';
        ctx.lineWidth = 1;
        for (let i = 0; i < BOARD_SIZE; i++) {
            const pos = pad + i * cell;
            
            ctx.beginPath();
            ctx.moveTo(pad, pos);
            ctx.lineTo(far, pos);
            ctx.stroke();
            
            ctx.beginPath();
            ctx.moveTo(pos, pad);
            ctx.lineTo(pos, far);
            ctx.stroke();
        }
        
        // Star points are logical squares, so they travel with the view.
        ctx.fillStyle = '#8d8375';
        for (const [row, col] of [[3, 3], [3, 11], [11, 3], [11, 11], [7, 7]]) {
            const shown = this.displayOf(row, col);
            const p = this.cellCenter(shown[0], shown[1]);
            ctx.beginPath();
            ctx.arc(p.x, p.y, Math.max(2, cell * 0.09), 0, Math.PI * 2);
            ctx.fill();
        }
        
        // Edge coordinates. Each label names the logical square that currently
        // sits on that line, so rotating or mirroring moves the labels with it
        // and the grid stays readable as the same board seen differently.
        ctx.fillStyle = '#8a8a8a';
        ctx.font = `${font}px ui-monospace, SFMono-Regular, Consolas, monospace`;
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        for (let dc = 0; dc < BOARD_SIZE; dc++) {
            ctx.fillText(this.columnLabel(dc), this.cellCenter(0, dc).x, pad / 2);
        }
        for (let dr = 0; dr < BOARD_SIZE; dr++) {
            ctx.fillText(this.rowLabel(dr), pad / 2, this.cellCenter(dr, 0).y);
        }
        ctx.textAlign = 'start';
        ctx.textBaseline = 'alphabetic';
        
        for (let row = 0; row < BOARD_SIZE; row++) {
            for (let col = 0; col < BOARD_SIZE; col++) {
                if (view.board[row][col] !== EMPTY) {
                    this.drawStone(row, col, view.board[row][col]);
                }
            }
        }
        
        if (view.lastMove) {
            const shown = this.displayOf(view.lastMove.row, view.lastMove.col);
            const p = this.cellCenter(shown[0], shown[1]);
            ctx.beginPath();
            ctx.arc(p.x, p.y, Math.max(2.5, stone * 0.3), 0, Math.PI * 2);
            ctx.fillStyle = view.lastMove.color === BLACK ? '#fff' : '#111';
            ctx.fill();
        }

        if (this.recommendation && !this.setup.active && this.replayIndex === null) {
            // Painted from state so no later repaint (applyView, placeStone) can lose it,
            // and only on the live board: 摆棋's copy and a replay ply are other positions.
            const shown = this.displayOf(this.recommendation.row, this.recommendation.col);
            const p = this.cellCenter(shown[0], shown[1]);
            ctx.beginPath();
            ctx.arc(p.x, p.y, stone + 3, 0, Math.PI * 2);
            ctx.strokeStyle = '#c0392b';
            ctx.lineWidth = 2;
            ctx.stroke();
        }

        if (this.replayIndex !== null) {
            // Say so on the board itself: a past position looks exactly like a
            // live one, and the missing marker is the only other clue.
            ctx.fillStyle = '#8a8a8a';
            ctx.font = `${font}px sans-serif`;
            ctx.fillText(`回放 · 第 ${this.replayIndex + 1} 手`, pad, size - 5);
        }
    }
    
    drawStone(row, col, color) {
        const shown = this.displayOf(row, col);
        const p = this.cellCenter(shown[0], shown[1]);
        const ctx = this.ctx;
        
        ctx.beginPath();
        ctx.arc(p.x, p.y, this.geom.stone, 0, Math.PI * 2);
        ctx.fillStyle = color === BLACK ? '#1b1b1b' : '#ffffff';
        ctx.fill();
        ctx.strokeStyle = color === BLACK ? '#000000' : '#b9b9b9';
        ctx.lineWidth = 1;
        ctx.stroke();
    }
    
    // The status bar is the page's only message surface, so it carries the
    // severity: green for what worked, amber for what needs attention, red for
    // what was refused or broken, grey for narration.
    updateStatus(text, kind = 'idle') {
        const el = document.getElementById('status');
        el.textContent = text;
        el.className = `status ${kind}`;
    }
    
    updateTurnInfo(text) {
        if (text === undefined) {
            if (this.gameStatus === 'playing') {
                const turnColor = this.currentTurn === BLACK ? '黑棋' : '白棋';
                const who = this.currentTurn === this.playerColor ? '你的回合' : 'AI 思考中';
                text = `轮到${turnColor} · ${who}`;
            } else {
                text = '';
            }
        }
        document.getElementById('turn-info').textContent = text;
    }
    
    updateMoveList() {
        const list = document.getElementById('move-list');
        list.innerHTML = '';
        
        this.moveHistory.forEach((move, index) => {
            const div = document.createElement('div');
            const isLive = this.replayIndex === null
                ? index === this.moveHistory.length - 1
                : index === this.replayIndex;
            div.className = `move-item ${move.color === BLACK ? 'black' : 'white'}`
                + (isLive ? ' active' : '');
            const colorName = move.color === BLACK ? '黑' : '白';
            let text = `${index + 1}. ${colorName} ${this.coordName(move.row, move.col)}`;
            const analysis = this.formatAnalysis(this.analysis[index], true);
            if (analysis) text += ` · ${analysis}`;
            div.textContent = text;
            div.addEventListener('click', () => this.showReplayMove(index));
            list.appendChild(div);
        });
        
        list.scrollTop = list.scrollHeight;
    }
}

document.addEventListener('DOMContentLoaded', () => {
    window.game = new GomokuGame();
});
