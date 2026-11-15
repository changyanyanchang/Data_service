# Explicit bounded numeric table projections

`numeric_table.v1` adds a numeric view over an existing `source_tables` ID. It does
not replace `DataTable`, infer types from display strings, or modify `source_cells`.
There is no new general table/catalog service and no new geological relation kind.
Public DTOs in `numeric_table.h` use standard C++ types; SQL/Qt stay inside the SDK.

## Declaration and revision

An operator/host explicitly calls `upgradeNumericTableProjectionSchema` on a
writable session, then `confirmNumericTableProjection`. Ordinary and read-only
opens do not install this capability. Legacy tables without a declaration return
`NumericTableProjectionUnavailable`; missing schema/index/maintenance triggers
return `NumericTableSchemaUnavailable`. Unknown schema versions are rejected.

Confirmation requires the exact current `ModelChangeHead` and previous projection
revision. An empty previous revision creates only. The declaration fixes source
ID/SHA-256, physical row range, column positions/order, stable ASCII IDs, labels,
units, dtype and null rules. It never accepts a path or SQL expression. The source
must be active and its stored strong SHA must match. Original files need not be
online; model values and actual consumed cut remain authoritative.

At most 15 value columns plus the implicit dimensionless `source_row` axis are
supported. Rows keep their one-based physical source positions; the declared
range includes exactly the caller-selected rows, with no header detection,
sorting, sparse-row compaction, resampling or unit conversion. The first version
does not add physical-axis interpretation. Each window contains 0–8192 rows.

Column types are `Float64` and `Boolean`; there is no implicit integer/identifier
type. Float64 uses locale-independent decimal conversion to finite binary64.
Booleans accept only ASCII `0`/`1`, with explicitly dimensionless unit `1`.
Unknown/empty units are rejected. Exact declared null texts take precedence;
otherwise only ASCII surrounding whitespace is removed before conversion.
Missing sparse cells are null only when explicitly allowed. Null masks use 1 for
null and 0 for valid; null payloads are zero. NaN/Inf and invalid numeric tokens
are rejected unless the exact source text was explicitly declared as null.

Confirmation is transactional and CAS-protected. An unchanged current declaration
returns the same revision without advancing the model head. A changed or stale
declaration gets a fresh UUID revision; the returned revision is the committed
identity, not a read cut. Call describe to obtain a current cut.

Source cell value/position, table identity/shape, or declaration-column changes
invalidate the projection in the same transaction. Reconfirmation is explicit.
Source relocation and table display renaming do not change its semantic revision.
Tracking repair marks existing projections stale rather than blessing a gap in
history. No numeric payloads are added to the model change journal.

## Consistent bounded reads

`describeNumericTableProjection` returns the projection and observed
`NumericTableReadCut`: model store/generation/sequence, table/source IDs, source
SHA and projection revision. `readNumericTableWindow` accepts that exact cut,
explicit column IDs and a window relative to the declared range. It reads model
head, metadata and cells in one SQLite savepoint. Changes before the snapshot are
rejected; concurrent commits after it starts cannot mix new cells into the old cut.
The result reports the cut actually consumed. This is not a public lease or a
promise that old numeric values can be reconstructed after the snapshot ends.
The application freezes returned values in its existing operation/input artifacts.

Reads use `(table_id,col_num,row_num)` seeks. They do not load the entire display
table or fetch unselected cells. The index and maintenance capability must exist;
there is no scan fallback. Direct-column SQLite `octet_length`/lazy CASE guards
enforce text field budgets before materialization. A bounded per-column length
preflight in the same snapshot enforces aggregate raw-text budget before that
column is handed to C++.

Defaults: 8 MiB total returned metadata/cell text, 256 bytes per source cell,
2 MiB typed output, 4 MiB conservative working reservation. Hard maxima are
8 MiB text/working, 2 MiB output, 4096 bytes per cell, 8192 bytes per metadata field
and 64 KiB total metadata. Declaration metadata is separately limited to 16 KiB.
That declaration reservation includes the actual persisted JSON encoding of null
texts (including escapes), the fixed identity text, and conservative column
overhead. Confirmation compares an existing declaration under the internal hard
metadata limits; caller-selected describe/window budgets remain unchanged.
Limits compose: a maximum row/column count does not guarantee every representation
fits all byte limits. Output and working reservations precede payload reads.

Stats distinguish returned metadata bytes, cell text bytes, cells consumed, typed
output bytes and conservative reservation. They are neither physical disk I/O nor
process RSS. Failed reads return no partial arrays but preserve actual work stats.
Cancellation is checked on the calling thread throughout metadata and payload
processing, and before releasing the snapshot. No background executor is added.

## Validation

`numeric_table_contract` covers values/nulls/bools, exact same-name table identity,
CAS/no-op/rollback/reopen, read-only/legacy behavior, source edits, old cuts,
concurrent snapshot reads, cancellation, explicit bounds, oversized selected and
unselected cells, and the actual compound-index query plan. Existing metadata,
model-change, display-page, read-only and snapshot suites remain regressions.
