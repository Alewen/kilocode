---
name: kilo-db-query
description: Query Kilo database to view session history, messages, and parts with PowerShell or shell scripts
---

# Skill: kilo-db-query

Provide three PowerShell (Windows) and three shell (Linux/macOS) scripts for querying the Kilo database to view session history, messages, and parts.

## When to Use

This skill should be used when:
- Querying Kilo's session history
- Viewing messages from a specific session
- Examining parts from a specific session, optionally with filtering

## Usage

### Windows (PowerShell)

To query Kilo sessions, execute `scripts/kilosessions.ps1`.
To query Kilo messages, execute `scripts/kilomessage.ps1` with a session ID.
To query Kilo parts, execute `scripts/kilopart.ps1` with a session ID and optional filter.

#### Query Sessions (PowerShell)

List all Kilo sessions:
```powershell
scripts/kilosessions.ps1
```

List latest 10 sessions:
```powershell
scripts/kilosessions.ps1 -Limit 10
```

#### Query Messages (PowerShell)

List all messages from a specific session:
```powershell
scripts/kilomessage.ps1 <session_id>
```

List latest 10 messages:
```powershell
scripts/kilomessage.ps1 <session_id> -Limit 10
```

#### Query Parts (PowerShell)

List all parts from a specific session:
```powershell
scripts/kilopart.ps1 <session_id>
```

List parts matching a filter:
```powershell
scripts/kilopart.ps1 <session_id> <filter>
```

List latest 10 parts:
```powershell
scripts/kilopart.ps1 <session_id> -Limit 10
```

List latest 10 parts matching a filter:
```powershell
scripts/kilopart.ps1 <session_id> <filter> -Limit 10
```

### Linux/macOS (Shell)

To query Kilo sessions, execute `scripts/kilosessions.sh`.
To query Kilo messages, execute `scripts/kilomessage.sh` with a session ID.
To query Kilo parts, execute `scripts/kilopart.sh` with a session ID and optional filter.

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

## Database Path

- PowerShell scripts use database path `C:\Users\shaoke\.local\share\kilo\kilo.db`
- Shell scripts use database path `~/.local/share/kilo/kilo.db`
