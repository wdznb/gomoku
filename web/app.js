const BOARD_SIZE = 15;
const CELL_SIZE = 38;
const PADDING = 20;
const STONE_RADIUS = 16;

const EMPTY = 0;
const BLACK = 1;
const WHITE = 2;

class GomokuGame {
    constructor() {
        this.canvas = document.getElementById('board');
        this.ctx = this.canvas.getContext('2d');
        this.ws = null;
        
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
        
        this.initWebSocket();
        this.initUI();
        this.drawBoard();
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
            this.updateStatus('连接断开');
        };
        
        this.ws.onerror = (error) => {
            console.error('WebSocket error:', error);
            this.updateStatus('连接错误');
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
        document.getElementById('btn-replay-play').textContent = '⏸ 暂停';
    }

    stopReplayAuto() {
        if (this.replayTimer) {
            clearInterval(this.replayTimer);
            this.replayTimer = null;
        }
        document.getElementById('btn-replay-play').textContent = '▶ 自动';
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
        const coord = `${String.fromCharCode(65 + m.col)}${BOARD_SIZE - m.row}`;
        let text = `第 ${this.replayIndex + 1}/${this.moveHistory.length} 手 · ${colorName} ${coord}`;
        if (a) {
            text += ` · ${this.formatAnalysis(a, false)}`;
        } else {
            text += ' · 尚未载入分析';
        }
        el.textContent = text;
    }

    drawEvalGraph() {
        const canvas = document.getElementById('eval-graph');
        const ctx = canvas.getContext('2d');
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        ctx.fillStyle = '#f5f0e6';
        ctx.fillRect(0, 0, canvas.width, canvas.height);

        // Mid line and axis.
        ctx.strokeStyle = '#bbb';
        ctx.beginPath();
        ctx.moveTo(0, canvas.height / 2);
        ctx.lineTo(canvas.width, canvas.height / 2);
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
            const x = canvas.width * (i + 0.5) / this.analysis.length;
            const y = canvas.height / 2 - (clamped / limit) * (canvas.height / 2 - 8);
            points.push({ x, y, saturated: Math.abs(blackScore) > limit });
        });
        if (points.length === 0) return;

        ctx.fillStyle = '#666';
        ctx.font = '10px sans-serif';
        ctx.fillText('评估曲线（黑棋视角）', 4, 10);

        ctx.strokeStyle = '#3a6ea5';
        ctx.lineWidth = 1.5;
        ctx.beginPath();
        points.forEach((p, i) => i === 0 ? ctx.moveTo(p.x, p.y) : ctx.lineTo(p.x, p.y));
        ctx.stroke();

        // Points pushed to the edge are a proven win/loss, not a vague advantage.
        ctx.fillStyle = '#c0392b';
        for (const p of points) {
            if (p.saturated) {
                ctx.beginPath();
                ctx.arc(p.x, p.y, 3, 0, Math.PI * 2);
                ctx.fill();
            }
        }
    }
    
    handleCanvasClick(e) {
        if (this.gameStatus !== 'playing' || this.aiThinking) return;
        // A replayed position is a read-only view of the past; clicking there
        // must not send a move for the live turn.
        if (this.replayIndex !== null) return;
        if (this.currentTurn !== this.playerColor) return;
        
        const rect = this.canvas.getBoundingClientRect();
        const x = e.clientX - rect.left;
        const y = e.clientY - rect.top;
        
        const col = Math.round((x - PADDING) / CELL_SIZE);
        const row = Math.round((y - PADDING) / CELL_SIZE);
        
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
                alert('错误: ' + msg.message);
                break;
        }
    }
    
    handleStateSync(msg) {
        this.gameStatus = 'playing';
        this.updateStatus('游戏开始');
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
        // the stone it just placed, so surface it before the next search clears
        // the thinking line.
        this.updateThinkingInfo(`AI 落子：分值 ${msg.score}，耗时 ${msg.thinking_time_ms}ms`);
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
    }
    
    handleThinkingProgress(msg) {
        this.updateThinkingInfo(`思考中... 深度: ${msg.depth}, 节点: ${msg.nodes}, 耗时: ${msg.elapsed_ms}ms`);
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
        if (msg.winner === 'draw') {
            statusText += '和棋';
        } else {
            const winnerColor = msg.winner === 'black' ? '黑棋' : '白棋';
            if (msg.winner === (this.playerColor === BLACK ? 'black' : 'white')) {
                statusText += `你赢了！(${winnerColor})`;
            } else {
                statusText += `你输了 (${winnerColor}胜)`;
            }
        }
        // The reason the engine reports (five / resign / draw) is part of the
        // result the player is owed, not only a status code.
        statusText += ` · ${this.reasonText(msg.reason)}`;
        this.gameOverText = statusText;
        this.updateStatus(statusText);
        
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
        const x = PADDING + col * CELL_SIZE;
        const y = PADDING + row * CELL_SIZE;
        
        this.ctx.beginPath();
        this.ctx.arc(x, y, STONE_RADIUS + 3, 0, Math.PI * 2);
        this.ctx.strokeStyle = '#ff0000';
        this.ctx.lineWidth = 3;
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

    drawBoard() {
        const ctx = this.ctx;
        const width = this.canvas.width;
        const height = this.canvas.height;
        const view = this.displayView();
        
        ctx.fillStyle = '#DEB887';
        ctx.fillRect(0, 0, width, height);
        
        ctx.strokeStyle = '#000';
        ctx.lineWidth = 1;
        
        for (let i = 0; i < BOARD_SIZE; i++) {
            const pos = PADDING + i * CELL_SIZE;
            
            ctx.beginPath();
            ctx.moveTo(PADDING, pos);
            ctx.lineTo(PADDING + (BOARD_SIZE - 1) * CELL_SIZE, pos);
            ctx.stroke();
            
            ctx.beginPath();
            ctx.moveTo(pos, PADDING);
            ctx.lineTo(pos, PADDING + (BOARD_SIZE - 1) * CELL_SIZE);
            ctx.stroke();
        }
        
        const starPoints = [[3, 3], [3, 11], [11, 3], [11, 11], [7, 7]];
        ctx.fillStyle = '#000';
        for (const [row, col] of starPoints) {
            const x = PADDING + col * CELL_SIZE;
            const y = PADDING + row * CELL_SIZE;
            ctx.beginPath();
            ctx.arc(x, y, 4, 0, Math.PI * 2);
            ctx.fill();
        }
        
        for (let row = 0; row < BOARD_SIZE; row++) {
            for (let col = 0; col < BOARD_SIZE; col++) {
                if (view.board[row][col] !== EMPTY) {
                    this.drawStone(row, col, view.board[row][col]);
                }
            }
        }
        
        if (view.lastMove) {
            const x = PADDING + view.lastMove.col * CELL_SIZE;
            const y = PADDING + view.lastMove.row * CELL_SIZE;
            ctx.beginPath();
            ctx.arc(x, y, 5, 0, Math.PI * 2);
            ctx.fillStyle = view.lastMove.color === BLACK ? '#fff' : '#000';
            ctx.fill();
        }

        if (this.replayIndex !== null) {
            // Say so on the board itself: a past position looks exactly like a
            // live one, and the missing marker is the only other clue.
            ctx.fillStyle = '#8b4513';
            ctx.font = '14px sans-serif';
            ctx.fillText(`回放中 · 第 ${this.replayIndex + 1} 手`, PADDING, height - 4);
        }
    }
    
    drawStone(row, col, color) {
        const ctx = this.ctx;
        const x = PADDING + col * CELL_SIZE;
        const y = PADDING + row * CELL_SIZE;
        
        ctx.beginPath();
        ctx.arc(x, y, STONE_RADIUS, 0, Math.PI * 2);
        
        if (color === BLACK) {
            const gradient = ctx.createRadialGradient(x - 5, y - 5, 2, x, y, STONE_RADIUS);
            gradient.addColorStop(0, '#666');
            gradient.addColorStop(1, '#000');
            ctx.fillStyle = gradient;
        } else {
            const gradient = ctx.createRadialGradient(x - 5, y - 5, 2, x, y, STONE_RADIUS);
            gradient.addColorStop(0, '#fff');
            gradient.addColorStop(1, '#ccc');
            ctx.fillStyle = gradient;
        }
        
        ctx.fill();
        ctx.strokeStyle = color === BLACK ? '#000' : '#999';
        ctx.lineWidth = 1;
        ctx.stroke();
    }
    
    updateStatus(text) {
        document.getElementById('status').textContent = text;
    }
    
    updateTurnInfo(text) {
        if (text === undefined) {
            if (this.gameStatus === 'playing') {
                const turnColor = this.currentTurn === BLACK ? '黑棋' : '白棋';
                const isPlayerTurn = this.currentTurn === this.playerColor;
                text = `当前: ${turnColor} (${isPlayerTurn ? '你的回合' : 'AI思考中'})`;
            } else {
                text = '';
            }
        }
        document.getElementById('turn-info').textContent = text;
    }
    
    updateThinkingInfo(text) {
        const el = document.getElementById('thinking-info');
        if (text) {
            el.textContent = text;
            el.classList.add('active');
        } else {
            el.textContent = '';
            el.classList.remove('active');
        }
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
            const colName = String.fromCharCode(65 + move.col);
            let text = `${index + 1}. ${colorName} ${colName}${BOARD_SIZE - move.row}`;
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
