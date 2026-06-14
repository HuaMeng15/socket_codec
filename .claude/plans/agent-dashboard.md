# Claude Code Subagent Dashboard

## Overview
A lightweight local web dashboard to view, monitor, and launch Claude Code subagents. Single HTML file + a small Python server that reads Claude Code's on-disk state.

## Architecture

```
[Python FastAPI server]  ←→  [~/.claude/ filesystem]
        ↓
[Single-page HTML/JS dashboard served at localhost:8420]
```

## Data Sources (from ~/.claude/)

- `~/.claude/sessions/*.json` — active sessions (pid, sessionId, cwd, status, startedAt)
- `~/.claude/projects/<project-slug>/<session-id>/subagents/*.meta.json` — agent metadata (type, description)
- `~/.claude/projects/<project-slug>/<session-id>/subagents/*.jsonl` — agent conversation transcripts

## Features

### 1. View History & Status
- List all sessions grouped by project
- For each session, show its subagents with: type, description, status (running/done)
- Click an agent to see its transcript (tool calls, results, final output)

### 2. Real-time Monitoring
- Poll server every 2s for active session updates
- Show live agent status (running spinner, completed checkmark)
- Display latest tool call / output as it appears in the JSONL

### 3. Launch New Agents (stretch goal)
- Input field to describe a task
- Select agent type (Explore, general-purpose, Plan, claude-code-guide)
- Triggers via Claude Code CLI subprocess

## Files to Create

1. **`/Users/menghua/Research/socket_codec/agent-dashboard/server.py`**
   - Python (stdlib + uvicorn/fastapi or just http.server)
   - Endpoints:
     - `GET /api/sessions` — list all sessions with their subagents
     - `GET /api/agents/<session-id>/<agent-id>` — get agent transcript
     - `GET /api/status` — current active session status
   - Serves the static HTML

2. **`/Users/menghua/Research/socket_codec/agent-dashboard/index.html`**
   - Single-file dashboard (HTML + CSS + JS inlined)
   - Dark theme, clean layout
   - Auto-refresh via polling
   - Sections: Active Agents | History | (optional) Launch

## Tech Choices
- **Server**: Python with standard library `http.server` (no dependencies needed beyond Python 3.10+)
- **Frontend**: Vanilla HTML/CSS/JS, no build step
- **Polling**: fetch() every 2 seconds for live updates
- **No auth**: localhost only

## How to Run
```bash
cd agent-dashboard
python server.py
# Opens at http://localhost:8420
```
