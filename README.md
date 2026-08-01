![version](https://img.shields.io/badge/version-20%2B-E23089)
![platform](https://img.shields.io/static/v1?label=platform&message=mac-intel%20|%20mac-arm%20|%20win-64&color=blue)
[![license](https://img.shields.io/github/license/miyako/4d-plugin-simple-postgres-client)](LICENSE)
![downloads](https://img.shields.io/github/downloads/miyako/4d-plugin-simple-postgres-client/total)

# 4d-plugin-simple-postgres-client

Simple PostgreSQL Client is a 4D plugin that connects to a PostgreSQL server and runs one SQL statement per call, using [libpq](https://www.postgresql.org/docs/current/libpq-fe.html) internally. There is no separate "connect" step and no persistent connection object: each call opens a connection, runs the statement, collects the result, and closes the connection, returning everything — connection info, status codes, and (for `SELECT`-style queries) result rows — as a single `Object`.

| Command | Returns | Purpose |
|---|---|---|
| [PQ EXECUTE](#pq-execute) | Object | Connect to PostgreSQL and execute one SQL statement |

**Platforms:** Windows (x64) · macOS (arm64, x86_64)

---

## Requirements & platform notes

- `connection` and `SQL` are **mandatory** — both are read unconditionally from parameters 1 and 2 in the plugin source, with no fallback if omitted.
- `params` and `format` are the two trailing, optional parameters. As with any 4D command, you can only omit parameters from the end: `PQ EXECUTE(connect;SQL)`, `PQ EXECUTE(connect;SQL;params)`, and the full 4-parameter form are all valid; you cannot supply `format` without also supplying `params`.
- **No connection pooling.** Every call opens a brand-new PostgreSQL connection and closes it before returning. If you're calling this frequently, consider a server-side pooler (e.g. PgBouncer) rather than relying on the plugin to reuse connections.
- **No enforced connect or query timeout.** Both connecting and executing are synchronous, blocking calls with no built-in timeout. An unreachable host or a slow query blocks the calling 4D process until it resolves. Add `connect_timeout=` to your connection string to bound the connection phase; there is no equivalent built-in control for query execution.
- **Parameters are never concatenated into the SQL text** — they're sent via libpq's `PQexecParams`, the same mechanism behind `$1`/`$2` placeholders. This makes `PQ EXECUTE` safe from SQL injection as long as user-supplied values are passed through `params`, not built into the `SQL` string.
- **No behavioral divergence between platforms.** The command's implementation has no `#if VERSIONMAC` / `#if VERSIONWIN` branches — Windows and macOS run the identical code path. (The plugin header does declare a couple of extra externs and an iconv include under `#if VERSIONMAC`, but these are build-time linkage requirements for that platform's build, not something that changes the command's runtime behavior.)
- **The single biggest gotcha:** if the PostgreSQL *connection itself* fails, the returned object comes back effectively empty — not even `errorMessage` is set. See [Error handling & troubleshooting](#error-handling--troubleshooting) before you write failure-handling code around this command.

---

## PQ EXECUTE

### Syntax

```
PQ EXECUTE ( connection : Text ; SQL : Text ; params : Collection ; format : Integer ) -> Object
```

| Parameter | Type | Description |
|---|---|---|
| `connection` | Text | A libpq connection string (keyword/value or URI form). Mandatory. |
| `SQL` | Text | The SQL statement to run. Use PostgreSQL's native `$1`, `$2`, … placeholders for parameters. Mandatory. |
| `params` | Collection | Positional values substituted for `$1`, `$2`, … Optional — the source explicitly guards for a missing/empty collection, so it can be omitted (or passed empty) for statements with no placeholders. |
| `format` | Integer | Desired *result* value format: `0` = text, `1` = binary. The plugin's own test method calls `PQ EXECUTE` with only 3 arguments, omitting this entirely, so it's treated here as optional defaulting to `0` — though the source reads it the same unconditional way it reads `connection`/`SQL`, so the omission is handled by 4D's/the plugin wrapper's parameter defaulting rather than anything visible in the provided `.cpp`. If in doubt, pass `0` explicitly. |
| Result | Object | Connection info, status codes, and (when applicable) result rows. See below. |

### Description

`connection` accepts any standard libpq connection string, in either form:

```4d
$connect:="host=localhost port=5432 dbname=postgres user=miyako password=secret"
```

```4d
$connect:="postgresql://miyako:secret@localhost:5432/postgres"
```

Any libpq-recognized keyword (`host`, `port`, `dbname`, `user`, `password`, `connect_timeout`, `sslmode`, `application_name`, etc.) is valid.

**`params` value mapping.** Each collection element is substituted, in order, for `$1`, `$2`, … The plugin recognizes six 4D value kinds:

| 4D value type | Sent to PostgreSQL as |
|---|---|
| Text | The text value, UTF-8 encoded |
| Real (number) | A decimal string formatted to 99 digits after the decimal point — see the note below |
| Time | An integer string (seconds) |
| Longint (integer) | An integer string |
| Boolean | `"true"` / `"false"` |
| Null | SQL `NULL` |

Two behavioral quirks are worth knowing about, both traced directly in the plugin's source:

- **Real numbers carry excess, meaningless precision.** The formatting code requests 99 digits after the decimal point (`"%.*f"` with `precision = 99`). A `double` only actually holds about 17 significant digits, so values sent to PostgreSQL will have a long tail of numerically meaningless trailing digits. This doesn't break anything, but don't be surprised by what shows up server-side or in logs.
- **Unsupported collection element types can desynchronize the query.** Any element whose 4D value kind isn't one of the six above (e.g. a `Picture`, `Duration`, `Object`, or nested `Collection`) is silently skipped when the plugin builds the parameter arrays — but the parameter *count* passed to libpq is still the original collection length, not the number of elements actually written. Mixing in an unsupported type therefore mismatches the parameter count against the parameter arrays, which can produce an incorrect query or a crash. Stick to the six supported kinds above for every element in `params`.

`format` controls how *result* values come back, not how `params` values are sent. Leave it at `0` unless you specifically need raw binary for a column (e.g. `bytea`) that you intend to decode yourself; see the binary example below.

**The returned object.** Its shape depends on whether the connection succeeded:

- If `PQstatus` on the new connection is **not** `CONNECTION_OK` (bad host, refused auth, etc.), the plugin returns essentially an empty object — none of the fields below are set, including `errorMessage`. See [Error handling](#error-handling--troubleshooting).
- If the connection succeeds, every field below is set, regardless of whether the *query itself* then succeeds or fails. This means a bad SQL statement (syntax error, constraint violation, etc.) on a good connection *does* give you a populated `errorMessage` — only a failed connection is silent.

| Property | Type | Description |
|---|---|---|
| `errorMessage` | Text | Last error from libpq for this connection (empty string if none) |
| `db` | Text | Database name connected to |
| `user` | Text | User name used for the connection |
| `pass` | Text | Password used for the connection — sensitive; avoid logging or displaying this field |
| `host` | Text | Host connected to |
| `port` | Text | Port connected to |
| `tty` | Text | Legacy debug TTY setting (rarely meaningful) |
| `options` | Text | Command-line options sent to the backend |
| `status` | Integer | Connection status code — see table below |
| `transactionStatus` | Integer | Transaction state — see table below |
| `protocolVersion` | Integer | Negotiated protocol version (typically `3`) |
| `socket` | Integer | Underlying socket file descriptor (diagnostic use only) |
| `backendPID` | Integer | Process ID of the PostgreSQL backend handling this connection |
| `clientEncoding` | Integer | Client encoding ID in use |
| `connectionNeedsPassword` | Boolean | `True` if the server rejected the connection for lack of a password |
| `connectionUsedPassword` | Boolean | `True` if a password was sent during authentication |
| `sslInUse` | Boolean | `True` if the connection is encrypted |
| `values` | Collection | Present **only** for `SELECT`-style results (see below). Absent for `INSERT`/`UPDATE`/`DELETE` and for failed/errored queries. |

`status` and `transactionStatus` are the plugin's plain pass-through of libpq's own `ConnStatusType` and `PGTransactionStatusType` enums (documented in PostgreSQL's [libpq status reference](https://www.postgresql.org/docs/current/libpq-status.html)); they aren't something the plugin defines itself:

| `status` | Meaning |
|---|---|
| `0` | `CONNECTION_OK` |
| `1` | `CONNECTION_BAD` |
| `2`–`13` | Intermediate/async connection states — not normally observed here, since `PQ EXECUTE` connects synchronously |

| `transactionStatus` | Meaning |
|---|---|
| `0` | `PQTRANS_IDLE` — no active transaction |
| `1` | `PQTRANS_ACTIVE` — a command is currently in progress |
| `2` | `PQTRANS_INTRANS` — inside a transaction block |
| `3` | `PQTRANS_INERROR` — inside a failed transaction block |
| `4` | `PQTRANS_UNKNOWN` — connection is bad |

When present, `values` is a collection with one `Object` per row. Each row object's property names are the query's column names; each property's value is:

- the column value directly, when the field is non-null and `format` is `0` (text), or
- `Null`, when the database value is `NULL`, or
- an object `{data: "<base64 text>"}`, when `format` is `1` (binary).

**`INSERT`/`UPDATE`/`DELETE` don't expose a rows-affected count.** A successful non-`SELECT` statement comes back as libpq's `PGRES_COMMAND_OK`, which the plugin doesn't attach any row-count field to — there's no equivalent of `PQcmdTuples` surfaced here. If you need to know how many rows were affected, add a `RETURNING` clause to the statement so it comes back through `values` instead.

### Example

From the plugin's own test method (`TEST.4dm`):

```4d
//%attributes = {}
  //most easy
$connect:="user=miyako dbname=postgres"
$SQL:="SELECT * FROM users"

$params:=New collection:C1472("ccc")
$status:=PQ EXECUTE($connect;$SQL;$params)
```

A parameterized query, checking for both failure modes described above:

```4d
$connect:="host=localhost port=5432 dbname=postgres user=miyako password=secret"
$SQL:="SELECT id, username, email FROM users WHERE username=$1"
$params:=New collection:C1472("ccc")

$status:=PQ EXECUTE($connect;$SQL;$params)

If($status.status=0)  //CONNECTION_OK
	If($status.errorMessage="")
		If($status.values.length>0)
			$user:=$status.values[0]
			ALERT("Found user #"+String($user.id)+": "+$user.email)
		End if
	Else
		ALERT("Query failed: "+$status.errorMessage)
	End if
Else
	ALERT("Could not connect (no further detail available)")
End if
```

Reading a binary column by requesting `format=1`:

```4d
$connect:="user=miyako dbname=postgres"
$SQL:="SELECT id, avatar FROM users WHERE id=$1"
$params:=New collection:C1472(42)

$status:=PQ EXECUTE($connect;$SQL;$params;1)  //1 = binary result format

If(($status.status=0) & ($status.errorMessage=""))
	$row:=$status.values[0]
	If(Value type($row.avatar)=Is object)
		$imageBlob:=Decode base64($row.avatar.data)
	End if
End if
```

---

## Error handling & troubleshooting

- **Connection failures return an almost-empty object, not a 4D error.** If the connection itself fails, none of the fields in the return-object table above are set — including `errorMessage`. Check `$status.status=0` (or the practical absence of expected fields) before assuming `errorMessage` will tell you anything; on a hard connection failure it won't be there to check.
- **A bad SQL statement on a good connection *does* populate `errorMessage`.** Only the connection phase is silent. Once `status=0`, `errorMessage` reliably reflects a failed query (syntax error, constraint violation, etc.).
- **Unsupported `params` element types can desynchronize the query.** Stick to Text, Real, Time, Longint, Boolean, and Null in the `params` collection — anything else is dropped while the parameter count sent to libpq stays unchanged, which can corrupt the call.
- **No rows-affected count for `INSERT`/`UPDATE`/`DELETE`.** Add a `RETURNING` clause if you need to know what changed.
- **Unreachable hosts and slow queries block your 4D process.** There's no built-in timeout for either phase. Add `connect_timeout=` to the connection string to at least bound the connection attempt.
- **`pass` is echoed back in the result object.** Avoid logging, displaying, or persisting the full return value somewhere this could leak credentials.
- **Real number parameters carry ~80 digits of meaningless trailing precision.** Cosmetic only, but expect it if you inspect values server-side.
- **Failed queries aren't fully released internally.** After an errored result (`PGRES_FATAL_ERROR` and similar), the plugin doesn't release the underlying native result object. This doesn't affect the correctness of any individual call, but very high volumes of failing queries in a long-running session could add up in memory use over time.

---

## Quick reference

```4d
$connect:="host=localhost port=5432 dbname=postgres user=me password=secret"
$SQL:="SELECT * FROM users WHERE id=$1"
$params:=New collection:C1472(42)

$status:=PQ EXECUTE($connect;$SQL;$params)

If($status.status=0)
	If($status.errorMessage="")
		For each ($row;$status.values)
			// use $row
		End for each
	End if
Else
	// hard connection failure — $status has no useful fields
End if
```
