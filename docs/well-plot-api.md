# Stored well plot input

`listSourceWells(sourceId, ids, error)` resolves the well IDs of stored log,
interval, sample and well-object records. It does not infer associations from
folder names. A coordinate-only source without a business-object relation is
not assigned a well by this query. Multiwell sources require explicit selection.

`readWellPlotData(wellId, options, result, error, cancelled)` reads all stored
curve groups, interval tracks, markers and core-photo references for one stable
well ID in a deferred SQLite read transaction. It works with a read-only
session and never reads or verifies the original numeric source files.
Groups retain their data/source IDs, depth conventions and source sheets;
the API does not merge overlapping interpretations or interpolate curves.
Photo paths reference the external images; images are decoded by the consumer.

`options.processed` selects edited/cleaned or raw log values. `maxValues`
preflights the total curve sample, interval, marker and photo counts (default
8 million, maximum 64 million). It is a logical count budget, not a process
memory limit. Cancellation is checked between object reads. Output is empty
on failure, budget rejection or cancellation.

`readProjectCoordinates(result, error)` returns the two stored project CRS fields
in one query, distinguishing missing metadata from an inaccessible database.
`WellPlotData.well.coordinateSystem/coordinateUnit` always describe the well's
actual source coordinates. `projectCoordinates` describes the project target.
Missing well CRS stays empty even when project CRS is present; this module does
not transform coordinates or infer EPSG codes. Zero coordinates remain valid.

The host-confirmed comprehensive SpreadsheetML option also records MD/metre
depth for interval tables with the exact leading headers 井号/顶深/底深. Generic
tables retain their original unknown conventions. Existing cached sources are
not silently reparsed: correcting an earlier import uses explicit reimport.
