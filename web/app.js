const BOARD_SIZE = 15;

const EMPTY = 0;
const BLACK = 1;
const WHITE = 2;

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
        this.view = { rot: 0, flipH: false };
        this.dpr = window.devicePixelRatio || 1;
        
        this.board = Array(BOARD_SIZE).fill(null).map(() => Array(BOARD_SIZE).fill(EMPTY));
        this.moveHistory = [];
        this.playerColor = null;
        this.currentTurn = BLACK;
        this.gameStatus = 'waiting';
        this.aiThinking = false;
        this.lastMove = null;
        // Engine numbers for the current game, index-aligned with moveHistory.
        // Filled by the server's "replay" reply (4.4), not by the live messages.
        this.analysis = [];
        // True when the loaded record already covers the game's ending.
        this.analysisEnded = false;
        // Null while viewing the live position; a move index while replaying.
        this.replayIndex = null;
        this.replayTimer = null;
        this.gameOverText = '';
        // Position editor (摆棋). preview holds the last successful
        // check_position reply; the loaded flag keeps the 载入 button honest
        // about whether what is on screen still matches what was recognised.
        this.positionPreview = null;
        this.editMode = false;
        // True between clicking 载入 and its reply: the reply re-arms the side
        // buttons and clears the chosen colour, so a side picked in between
        // would be silently undone.
        this.positionSetInFlight = false;
        this.sideLock = [false, false];
        
        this.initWebSocket();
        this.initUI();
        this.layout();
        window.addEventListener('resize', () => this.layout());
    }
    
    initWebSocket() {
        const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        const wsUrl = `${protocol}//${window.location.host}`;
        
        this.ws = new WebSocket(wsUrl);
        
        this.ws.onopen = () => {
            console.log('Connected to server');
            this.updateStatus('已连接，请选择执子颜色');
        };
        
        this.ws.onclose = () => {
            console.log('Disconnected from server');
            this.updateStatus('连接断开，请刷新页面重连', 'error');
        };
        
        this.ws.onerror = (error) => {
            console.error('WebSocket error:', error);
            this.updateStatus('连接错误', 'error');
        };
        
        this.ws.onmessage = (event) => {
            const msg = JSON.parse(event.data);
            this.handleServerMessage(msg);
        };
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
        // whenever they change.
        for (const id of ['timeout', 'depth']) {
            document.getElementById(id).addEventListener('change', () => this.sendConfig());
        }

        document.getElementById('btn-check-position').addEventListener('click', () => this.checkPosition());
        document.getElementById('btn-set-position').addEventListener('click', () => this.setPosition());
        document.getElementById('btn-edit-mode').addEventListener('click', () => this.toggleEditMode());
        document.getElementById('btn-clear-position').addEventListener('click', () => this.clearPosition());
        document.getElementById('btn-export-position').addEventListener('click', () => this.exportPosition());
        document.getElementById('position-confirm').addEventListener('change', () => this.refreshPositionControls());
        document.getElementById('position-text').addEventListener('input', () => {
            // Anything typed or pasted replaces the file as the payload, and the
            // old recognition no longer describes it.
            this.positionPreview = null;
            this.refreshPositionControls();
        });
        document.getElementById('position-file').addEventListener('change', (e) => {
            const file = e.target.files[0];
            if (!file) return;
            const reader = new FileReader();
            reader.onload = () => {
                document.getElementById('position-text').value = String(reader.result);
                this.positionPreview = null;
                this.updatePositionStatus(`已读入 ${file.name}，点“识别”检查局面。`, false);
                this.refreshPositionControls();
            };
            reader.onerror = () => this.updatePositionStatus('文件读取失败。', true);
            reader.readAsText(file);
        });

        document.getElementById('btn-position-toggle').addEventListener('click', () => {
            const panel = document.getElementById('position-panel');
            panel.hidden = !panel.hidden;
            document.getElementById('btn-position-toggle').classList.toggle('toggled', !panel.hidden);
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

        // The 摆棋 drawer is a layer rather than a fifth region: it covers the
        // bottom band below the button row (which stays reachable) and scrolls
        // inside itself, so opening it never resizes the board or moves the
        // dividing lines.
        const bar = bottom.querySelector('.bar');
        const panelTop = Math.round(bar.getBoundingClientRect().bottom) + 6;
        const panel = document.getElementById('position-panel');
        panel.style.left = `${x0}px`;
        panel.style.width = `${size}px`;
        panel.style.top = `${panelTop}px`;
        panel.style.maxHeight = `${Math.max(0, height - panelTop - 6)}px`;

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
        this.sendMessage({ type: 'set_config', timeout_ms: timeout, max_depth: depth });
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
        this.board = Array(BOARD_SIZE).fill(null).map(() => Array(BOARD_SIZE).fill(EMPTY));
        for (const m of this.moveHistory) {
            this.board[m.row][m.col] = m.color;
        }
        // The snapshot may have been taken before an in-flight AI stone was
        // committed, so re-derive the turn and marker from the rebuilt history
        // rather than trusting the local flip count.
        this.currentTurn = (this.moveHistory.length % 2 === 0) ? BLACK : WHITE;
        this.lastMove = this.moveHistory.length > 0
            ? this.moveHistory[this.moveHistory.length - 1]
            : null;
        if (msg.game_over) {
            this.gameStatus = 'game_over';
        }
        this.stopReplayAuto();
        this.replayIndex = this.moveHistory.length > 0 ? 0 : null;
        this.analysisEnded = !!msg.game_over;
        this.refreshReplayControls();
        this.updateMoveList();
        this.drawBoard();
        this.updateReplayInfo();
        this.updateTurnInfo();
        this.drawEvalGraph();
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

    showReplayMove(index) {
        if (this.moveHistory.length === 0) return;
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
                this.stopReplayAuto();
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
            // While the AI is thinking, the live board is the thing to watch.
            el.disabled = !hasMoves || this.aiThinking;
        }
        document.getElementById('btn-replay-exit').disabled = this.replayIndex === null;
        // Re-enable when the record is behind the game: a move played after the
        // last load has no analysis, and an ending that arrived since then is
        // not in the loaded summary either.
        const stale = this.analysis.length < this.moveHistory.length ||
            (this.gameStatus === 'game_over' && !this.analysisEnded);
        document.getElementById('btn-replay').disabled =
            !hasMoves || this.aiThinking || !stale;
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
        if (!compact && a.elapsed_ms) parts.push(`${a.elapsed_ms}ms`);
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
            points.push({ x, y, saturated: Math.abs(blackScore) > limit });
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

        // Points pushed to the edge are a proven win/loss, not a vague advantage.
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

        // The click lands on the displayed grid, so it is mapped back to the
        // logical cell before anything else touches it.
        const shown = this.logicalOf(Math.round((y - this.geom.pad) / this.geom.cell),
                                    Math.round((x - this.geom.pad) / this.geom.cell));
        const row = shown[0];
        const col = shown[1];

        // Painting a position is a local edit, so it answers before any of the
        // live-game guards: a loaded position is not yet a game in progress.
        if (this.editMode) {
            if (row >= 0 && row < BOARD_SIZE && col >= 0 && col < BOARD_SIZE) {
                this.paintCell(row, col);
            }
            return;
        }

        if (this.gameStatus !== 'playing' || this.aiThinking) return;
        // A replayed position is a read-only view of the past; clicking there
        // must not send a move for the live turn.
        if (this.replayIndex !== null) return;
        if (this.currentTurn !== this.playerColor) return;
        
        if (row >= 0 && row < BOARD_SIZE && col >= 0 && col < BOARD_SIZE) {
            if (this.board[row][col] === EMPTY) {
                this.sendMove(row, col);
            }
        }
    }
    
    chooseSide(color) {
        this.playerColor = color === 'black' ? BLACK : WHITE;
        // If the AI opens the game (human picks WHITE) its first search starts
        // before any later control change could reach the server.
        this.sendConfig();
        this.sendMessage({
            type: 'choose_side',
            color: color
        });
        
        document.getElementById('btn-black').disabled = true;
        document.getElementById('btn-white').disabled = true;
        document.getElementById('btn-undo').disabled = false;
        document.getElementById('btn-hint').disabled = false;
        document.getElementById('btn-resign').disabled = false;
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
        const timeout = parseInt(document.getElementById('timeout').value);
        const depth = parseInt(document.getElementById('depth').value);
        this.sendMessage({
            type: 'request_hint',
            timeout_ms: timeout,
            max_depth: depth
        });
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
    }
    
    // ---- 摆棋 (hand-set position) -------------------------------------

    // Text from the paste box wins over the painted board: a file or a pasted
    // grid is the thing the player just supplied, and sending it verbatim is
    // what lets the server's auto-recognition run at all.
    positionPayload() {
        const text = document.getElementById('position-text').value.trim();
        if (text) return text;
        return { version: 1, type: 'gomoku_position',
                 encoding: { black: 1, white: 2, empty: 0 },
                 board: this.board.map(row => row.slice()) };
    }

    checkPosition() {
        const payload = this.positionPayload();
        const counts = this.localCounts();
        if (typeof payload !== 'string' && counts.black + counts.white === 0) {
            this.updatePositionStatus('棋盘是空的。', true);
            return;
        }
        this.positionPreview = null;
        this.refreshPositionControls();
        this.sendMessage({ type: 'check_position', position: payload });
    }

    setPosition() {
        const preview = this.positionPreview;
        if (!preview || !preview.ok || this.positionSetInFlight) return;
        const msg = { type: 'set_position', position: this.positionPayload() };
        // Carry the mapping the player just confirmed. A painted board already
        // states 1/2/0 in its own document, so only a pasted or uploaded grid
        // needs it echoed back.
        if (typeof msg.position === 'string' && preview.encoding) {
            msg.encoding = preview.encoding;
        }
        // Hold the side buttons until the reply lands: it re-arms them and
        // clears the chosen colour, so choosing in between would be undone.
        this.positionSetInFlight = true;
        this.sideLock = [document.getElementById('btn-black').disabled,
                         document.getElementById('btn-white').disabled];
        this.setSideButtons(true, true);
        this.sendMessage(msg);
    }

    setSideButtons(blackDisabled, whiteDisabled) {
        document.getElementById('btn-black').disabled = blackDisabled;
        document.getElementById('btn-white').disabled = whiteDisabled;
    }

    handlePositionPreview(msg) {
        this.positionPreview = msg.ok ? msg : null;
        const confirm = document.getElementById('position-confirm');
        if (!msg.ok) {
            confirm.checked = false;
            this.updatePositionStatus(msg.error || '无法识别此局面。', true);
            this.refreshPositionControls();
            return;
        }
        // A preview is a dry run, so it only repaints the board when nothing is
        // in progress; overwriting a live game's display would make the client
        // show stones the server does not have.
        const live = this.gameStatus === 'playing' || this.gameStatus === 'game_over';
        if (!live) this.applyBoardMatrix(msg.board);
        const fromText = typeof this.positionPayload() === 'string';
        // Only a grid has symbols to confirm; a pasted bitboard or move list
        // arrives without a mapping.
        const needsConfirm = fromText && !!msg.encoding;
        const mapping = needsConfirm
            ? `黑=${msg.encoding.black} 白=${msg.encoding.white} 空=${msg.encoding.empty}` : '';
        const summary = `黑 ${msg.black_count} 子、白 ${msg.white_count} 子，轮到${msg.side_to_move === 'black' ? '黑' : '白'}方`;
        const holds = live ? '（本局进行中，棋盘要到“载入”才会替换）' : '';
        document.getElementById('position-confirm-row').hidden = !needsConfirm;
        if (needsConfirm) {
            confirm.checked = false;
            document.getElementById('position-mapping').textContent = mapping;
            this.updatePositionStatus(`识别成功：${summary}（${mapping}）。勾选确认后载入。${holds}`, false);
        } else {
            document.getElementById('position-mapping').textContent = '';
            this.updatePositionStatus(`局面合法：${summary}。${holds}`, false);
        }
        this.refreshPositionControls();
    }

    handlePositionSet(msg) {
        this.positionPreview = null;
        this.positionSetInFlight = false;
        this.editMode = false;
        document.getElementById('btn-edit-mode').textContent = '手动摆棋';
        document.getElementById('position-confirm-row').hidden = true;
        document.getElementById('position-confirm').checked = false;
        this.applyBoardMatrix(msg.board);
        // The loaded stones are not this session's search history: the replay
        // view has nothing to analyse, and the move list would show plies no
        // one played here.
        this.stopReplayAuto();
        this.moveHistory = [];
        this.analysis = [];
        this.analysisEnded = false;
        this.replayIndex = null;
        this.lastMove = null;
        this.currentTurn = msg.current_turn === 'black' ? BLACK : WHITE;
        this.gameStatus = 'waiting';
        this.playerColor = null;
        this.updateMoveList();
        this.updateReplayInfo();
        this.drawEvalGraph();
        this.refreshReplayControls();
        document.getElementById('btn-black').disabled = false;
        document.getElementById('btn-white').disabled = false;
        document.getElementById('btn-undo').disabled = true;
        document.getElementById('btn-hint').disabled = true;
        document.getElementById('btn-resign').disabled = true;
        document.getElementById('btn-restart').disabled = false;
        this.drawBoard();
        this.updateStatus(`局面已摆好：轮到${this.currentTurn === BLACK ? '黑' : '白'}方，请选择执子颜色`, 'ok');
        this.updateTurnInfo('');
        this.updatePositionStatus(
            `局面已载入：黑 ${msg.black_count} 子、白 ${msg.white_count} 子。选边后开始，AI 不会自动落子。`,
            false);
    }

    toggleEditMode() {
        // Painting is only safe while the client and server agree that no game
        // is running; mid-game the board belongs to the session.
        if (!this.editMode && (this.gameStatus === 'playing' || this.aiThinking)) {
            this.updatePositionStatus('本局进行中，请先“重新开始”再手动摆棋。', true);
            return;
        }
        this.editMode = !this.editMode;
        document.getElementById('btn-edit-mode').textContent = this.editMode ? '退出摆棋' : '手动摆棋';
        const box = document.getElementById('position-text');
        // Painting and pasting are two ways to describe one board, and the paste
        // box wins when it has content, so manual mode starts by emptying it.
        const hadText = this.editMode && box.value.trim().length > 0;
        if (this.editMode) box.value = '';
        this.positionPreview = null;
        this.updatePositionStatus(this.editMode
            ? `点击棋盘格循环：空 → 黑 → 白 → 空。摆好后点“识别”。${hadText ? '（粘贴框内容已清除）' : ''}`
            : '已退出手动摆棋。', false);
        this.refreshPositionControls();
    }

    clearPosition() {
        this.applyBoardMatrix(Array(BOARD_SIZE).fill(null).map(() => Array(BOARD_SIZE).fill(EMPTY)));
        document.getElementById('position-text').value = '';
        this.positionPreview = null;
        this.refreshPositionControls();
        this.updatePositionStatus('棋盘已清空。', false);
    }

    paintCell(row, col) {
        const next = (this.board[row][col] + 1) % 3;
        this.board[row][col] = next;
        this.positionPreview = null;
        this.drawBoard();
        this.refreshPositionControls();
        const counts = this.localCounts();
        this.updatePositionStatus(
            `本地：黑 ${counts.black} 子、白 ${counts.white} 子。点“识别”检查禁手与五连。`, false);
    }

    localCounts() {
        let black = 0;
        let white = 0;
        for (const row of this.board) {
            for (const cell of row) {
                if (cell === BLACK) ++black;
                else if (cell === WHITE) ++white;
            }
        }
        return { black, white };
    }

    exportPosition() {
        const doc = {
            version: 1,
            type: 'gomoku_position',
            encoding: { black: 1, white: 2, empty: 0 },
            board: this.board.map(row => row.slice())
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

    refreshPositionControls() {
        const preview = this.positionPreview;
        const confirmRow = document.getElementById('position-confirm-row');
        const confirm = document.getElementById('position-confirm');
        const fromText = typeof this.positionPayload() === 'string';
        // A pasted or uploaded grid is the one case that owes the player the
        // second look, so 载入 stays dead until the box is ticked. A bitboard or
        // move list carries no symbols, so the server sends no mapping for it
        // and there is nothing to confirm.
        const needsConfirm = !!(preview && fromText && preview.encoding);
        confirmRow.hidden = !needsConfirm;
        if (!preview) confirm.checked = false;
        const ready = !!preview && preview.ok && (!needsConfirm || confirm.checked);
        document.getElementById('btn-set-position').disabled = !ready;
        document.getElementById('btn-check-position').disabled = this.aiThinking;
    }

    updatePositionStatus(text, isError) {
        const el = document.getElementById('position-status');
        el.textContent = text;
        el.classList.toggle('error', !!isError);
    }

    restart() {
        this.sendMessage({ type: 'restart' });
        this.resetGame();
    }
    
    resetGame() {
        this.board = Array(BOARD_SIZE).fill(null).map(() => Array(BOARD_SIZE).fill(EMPTY));
        this.moveHistory = [];
        this.currentTurn = BLACK;
        this.gameStatus = 'waiting';
        this.aiThinking = false;
        this.lastMove = null;
        // A new game has no history to replay, and stale analysis would attach
        // the old game's numbers to the new moves.
        this.stopReplayAuto();
        this.analysis = [];
        this.analysisEnded = false;
        this.replayIndex = null;
        this.gameOverText = '';
        this.editMode = false;
        this.positionPreview = null;
        this.positionSetInFlight = false;
        document.getElementById('btn-edit-mode').textContent = '手动摆棋';
        this.refreshPositionControls();
        this.updatePositionStatus('');
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
        
        this.updateMoveList();
        this.drawBoard();
        this.updateStatus('等待开始');
        this.updateTurnInfo('');
    }
    
    sendMessage(msg) {
        if (this.ws && this.ws.readyState === WebSocket.OPEN) {
            this.ws.send(JSON.stringify(msg));
        }
    }
    
    handleServerMessage(msg) {
        switch (msg.type) {
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
                // A refused load must not leave the side buttons held.
                if (this.positionSetInFlight) {
                    this.positionSetInFlight = false;
                    this.setSideButtons(this.sideLock[0], this.sideLock[1]);
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
        this.positionPreview = null;
        if (Array.isArray(msg.board)) this.applyBoardMatrix(msg.board);
        this.lastMove = null;
        this.currentTurn = msg.current_turn === 'black' ? BLACK : WHITE;
        this.gameStatus = 'playing';
        this.updateTurnInfo();
        this.updateStatus('对局开始', 'ok');
    }
    
    handleMoveMade(msg) {
        this.placeStone(msg.row, msg.col, msg.color === 'black' ? BLACK : WHITE);
        this.currentTurn = this.currentTurn === BLACK ? WHITE : BLACK;
        this.updateTurnInfo();
        this.refreshReplayControls();
    }
    
    handleAIMove(msg) {
        this.placeStone(msg.row, msg.col, msg.color === 'black' ? BLACK : WHITE);
        this.currentTurn = this.currentTurn === BLACK ? WHITE : BLACK;
        this.aiThinking = false;
        document.getElementById('btn-stop').disabled = true;
        // ai_move is the only live message that carries the engine's verdict for
        // the stone it just placed, so surface it in the status line.
        this.updateStatus(`AI ${this.coordName(msg.row, msg.col)} · 分值 ${msg.score} · ${msg.thinking_time_ms}ms`,
                          'idle');
        this.updateTurnInfo();
        this.refreshReplayControls();
    }
    
    handleUndoSuccess(msg) {
        // The server may remove 1 or 2 stones (its own reply plus the human's,
        // so a single undo reverts a full round). Reconcile to the authoritative
        // server move_count instead of assuming one pop, and take the turn from
        // parity (BLACK always opens) rather than a local flip.
        const target = Number.isInteger(msg.move_count)
            ? msg.move_count
            : Math.max(0, this.moveHistory.length - 1);
        while (this.moveHistory.length > target) {
            const m = this.moveHistory.pop();
            this.board[m.row][m.col] = EMPTY;
        }
        this.currentTurn = (target % 2 === 0) ? BLACK : WHITE;
        this.lastMove = this.moveHistory.length > 0
            ? this.moveHistory[this.moveHistory.length - 1]
            : null;
        // The popped plies shift every analysis index after them, and a replay
        // position that no longer exists is worse than no replay at all, so the
        // loaded record is dropped and has to be requested again.
        if (this.analysis.length > target) {
            this.stopReplayAuto();
            this.analysis = [];
            this.analysisEnded = false;
            this.replayIndex = null;
            this.updateReplayInfo();
            this.drawEvalGraph();
        }
        if (msg.game_status === 'waiting') {
            this.gameStatus = 'waiting';
        } else if (msg.game_status === 'game_over') {
            this.gameStatus = 'game_over';
        } else {
            this.gameStatus = 'playing';
        }
        this.updateMoveList();
        this.updateTurnInfo();
        this.refreshReplayControls();
        this.drawBoard();
    }
    
    handleHintResult(msg) {
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
            this.refreshReplayControls();
        }
    }
    
    handleGameOver(msg) {
        this.gameStatus = 'game_over';
        this.aiThinking = false;
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
        this.updateStatus(statusText, kind);
        
        document.getElementById('btn-undo').disabled = true;
        document.getElementById('btn-hint').disabled = true;
        document.getElementById('btn-resign').disabled = true;
        document.getElementById('btn-stop').disabled = true;
        this.refreshReplayControls();
    }
    
    placeStone(row, col, color) {
        this.board[row][col] = color;
        this.lastMove = { row, col, color };
        this.moveHistory.push({ row, col, color });
        this.updateMoveList();
        this.drawBoard();
    }
    
    highlightMove(row, col) {
        this.drawBoard();
        const shown = this.displayOf(row, col);
        const p = this.cellCenter(shown[0], shown[1]);
        
        this.ctx.beginPath();
        this.ctx.arc(p.x, p.y, this.geom.stone + 3, 0, Math.PI * 2);
        this.ctx.strokeStyle = '#c0392b';
        this.ctx.lineWidth = 2;
        this.ctx.stroke();
    }
    
    // The board as the player should see it right now: the live position, or the
    // position after replayIndex plies when a historical view is active.
    displayView() {
        if (this.replayIndex === null) {
            return { board: this.board, lastMove: this.lastMove };
        }
        const board = Array(BOARD_SIZE).fill(null).map(() => Array(BOARD_SIZE).fill(EMPTY));
        let lastMove = null;
        for (let i = 0; i <= this.replayIndex && i < this.moveHistory.length; ++i) {
            const m = this.moveHistory[i];
            board[m.row][m.col] = m.color;
            lastMove = m;
        }
        return { board, lastMove };
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
