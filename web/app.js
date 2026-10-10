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
    }
    
    handleCanvasClick(e) {
        if (this.gameStatus !== 'playing' || this.aiThinking) return;
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
    }
    
    handleAIMove(msg) {
        this.placeStone(msg.row, msg.col, msg.color === 'black' ? BLACK : WHITE);
        this.currentTurn = this.currentTurn === BLACK ? WHITE : BLACK;
        this.aiThinking = false;
        document.getElementById('btn-stop').disabled = true;
        this.updateThinkingInfo('');
        this.updateTurnInfo();
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
        if (msg.game_status === 'waiting') {
            this.gameStatus = 'waiting';
        } else if (msg.game_status === 'game_over') {
            this.gameStatus = 'game_over';
        } else {
            this.gameStatus = 'playing';
        }
        this.updateMoveList();
        this.updateTurnInfo();
        this.drawBoard();
    }
    
    handleHintResult(msg) {
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
        this.updateStatus(statusText);
        
        document.getElementById('btn-undo').disabled = true;
        document.getElementById('btn-hint').disabled = true;
        document.getElementById('btn-resign').disabled = true;
        document.getElementById('btn-stop').disabled = true;
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
    
    drawBoard() {
        const ctx = this.ctx;
        const width = this.canvas.width;
        const height = this.canvas.height;
        
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
                if (this.board[row][col] !== EMPTY) {
                    this.drawStone(row, col, this.board[row][col]);
                }
            }
        }
        
        if (this.lastMove) {
            const x = PADDING + this.lastMove.col * CELL_SIZE;
            const y = PADDING + this.lastMove.row * CELL_SIZE;
            ctx.beginPath();
            ctx.arc(x, y, 5, 0, Math.PI * 2);
            ctx.fillStyle = this.lastMove.color === BLACK ? '#fff' : '#000';
            ctx.fill();
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
            div.className = `move-item ${move.color === BLACK ? 'black' : 'white'}`;
            const colorName = move.color === BLACK ? '黑' : '白';
            const colName = String.fromCharCode(65 + move.col);
            div.textContent = `${index + 1}. ${colorName} ${colName}${BOARD_SIZE - move.row}`;
            list.appendChild(div);
        });
        
        list.scrollTop = list.scrollHeight;
    }
}

document.addEventListener('DOMContentLoaded', () => {
    window.game = new GomokuGame();
});
