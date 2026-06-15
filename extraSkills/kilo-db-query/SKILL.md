---
name: kilo-db-query
description: This skill should be used when querying Kilo's session history, messages, and parts using PowerShell or shell scripts
---

# Skill: kilo-db-query

Provide scripts for querying the Kilo database to view session history, messages, and parts.

## When to Use

Use this skill when:
- Querying Kilo's session history
- Viewing messages from a specific session
- Examining parts from a specific session, optionally with filtering
- Querying messages or parts within a specific time range

## Usage

### Windows (PowerShell)

> **Important:** Use `pwsh` (PowerShell Core) instead of `powershell` (Windows PowerShell 5.1). Otherwise, time parsing may fail.

Query Kilo sessions with `pwsh -File scripts/kilosessions.ps1`.
Query Kilo messages with `pwsh -File scripts/kilomessage.ps1 <session_id>`.
Query Kilo parts with `pwsh -File scripts/kilopart.ps1 <session_id> [<filter>]`.
Query messages by time range with `pwsh -File scripts/kilomessage_time.ps1 <session_id> <start_time> <end_time>`.
Query parts by time range with `pwsh -File scripts/kilopart_time.ps1 <session_id> <start_time> <end_time>`.

#### Query Sessions (PowerShell)

List all Kilo sessions:
```powershell
pwsh -File scripts/kilosessions.ps1
```

List latest 10 sessions:
```powershell
pwsh -File scripts/kilosessions.ps1 -Limit 10
```

#### Query Messages (PowerShell)

List all messages from a specific session:
```powershell
pwsh -File scripts/kilomessage.ps1 <session_id>
```

List latest 10 messages:
```powershell
pwsh -File scripts/kilomessage.ps1 <session_id> -Limit 10
```

#### Query Messages by Time Range (PowerShell)

```powershell
pwsh -File scripts/kilomessage_time.ps1 <session_id> <start_time> <end_time>
```

Time format: `"2026-06-05 12:00:01"` / `"06-05 12:00:01"` / `"05 12:00:01"` / `"12:00:01"`

Examples:
```powershell
pwsh -File scripts/kilomessage_time.ps1 ses_xxx "2026-06-09 11:50" "2026-06-09 11:54"
pwsh -File scripts/kilomessage_time.ps1 ses_xxx "06-09 11:50" "11:54"
pwsh -File scripts/kilomessage_time.ps1 ses_xxx "11:50" "11:54"
```

#### Query Parts (PowerShell)

List all parts from a specific session:
```powershell
pwsh -File scripts/kilopart.ps1 <session_id>
```

List parts matching a filter:
```powershell
pwsh -File scripts/kilopart.ps1 <session_id> <filter>
```

List latest 10 parts:
```powershell
pwsh -File scripts/kilopart.ps1 <session_id> -Limit 10
```

List latest 10 parts matching a filter:
```powershell
pwsh -File scripts/kilopart.ps1 <session_id> <filter> -Limit 10
```

#### Query Parts by Time Range (PowerShell)

```powershell
pwsh -File scripts/kilopart_time.ps1 <session_id> <start_time> <end_time>
```

Time format: same as `kilomessage_time.ps1`

Examples:
```powershell
pwsh -File scripts/kilopart_time.ps1 ses_xxx "2026-06-09 11:50" "2026-06-09 11:54"
pwsh -File scripts/kilopart_time.ps1 ses_xxx "06-09 11:50" "11:54"
pwsh -File scripts/kilopart_time.ps1 ses_xxx "11:50" "11:54"
```

### Linux/macOS (Shell)

Query Kilo sessions with `scripts/kilosessions.sh`.
Query Kilo messages with `scripts/kilomessage.sh <session_id>`.
Query Kilo parts with `scripts/kilopart.sh <session_id> [<filter>]`.
Query messages by time range with `scripts/kilomessage_time.sh <session_id> <start_time> <end_time>`.
Query parts by time range with `scripts/kilopart_time.sh <session_id> <start_time> <end_time>`.

#### Query Sessions (Shell)

List all Kilo sessions:
```bash
scripts/kilosessions.sh
```

List latest 10 sessions:
```bash
scripts/kilosessions.sh -L 10
```

#### Query Messages (Shell)

List all messages from a specific session:
```bash
scripts/kilomessage.sh <session_id>
```

List latest 10 messages:
```bash
scripts/kilomessage.sh <session_id> -L 10
```

#### Query Messages by Time Range (Shell)

```bash
scripts/kilomessage_time.sh <session_id> <start_time> <end_time>
```

Time format: `"2026-06-05 12:00:01"` / `"06-05 12:00:01"` / `"05 12:00:01"` / `"12:00:01"`

#### Query Parts (Shell)

List all parts from a specific session:
```bash
scripts/kilopart.sh <session_id>
```

List parts matching a filter:
```bash
scripts/kilopart.sh <session_id> <filter>
```

List latest 10 parts:
```bash
scripts/kilopart.sh <session_id> -L 10
```

List latest 10 parts matching a filter:
```bash
scripts/kilopart.sh <session_id> <filter> -L 10
```

#### Query Parts by Time Range (Shell)

```bash
scripts/kilopart_time.sh <session_id> <start_time> <end_time>
```

Time format: same as `kilomessage_time.sh`

## Database Path

- PowerShell scripts: `$HOME/.local/share/kilo/kilo.db`
- Shell scripts: `~/.local/share/kilo/kilo.db`
