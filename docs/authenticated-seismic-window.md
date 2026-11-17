# Exact seismic windows from authenticated host bytes

The new `describeSeismicWindow` / `readAuthenticatedSeismicWindow` API complements
the existing seismic readers. It does not change their signatures or behavior,
open the source pathname, silently migrate a database, or load a volume array.
All public types use standard C++.

## Metadata and source authority

`describeSeismicWindow(volumeId, ...)` returns only bounded metadata: the actual
model store/generation/head, volume/source IDs, a relevant-metadata content
revision, declared amplitude unit and common sampling conventions. The revision
uses prefix `seismic-window-descriptor.v1:` and SHA-256 over the relevant stored
metadata. It is distinct from `DataObjectDescriptor.revision`. SQL direct-column
byte guards reject excessive text before materialization; invalid UTF-8 and
undeclared conventions fail. No amplitudes, names, paths or coordinate arrays
are read by this descriptor.

The read query must name that exact cut, complete source SHA, physical file trace
ordinal and sample window. Parameter, scope/revision and memory/read-budget
admission happen before source reads. A savepoint gives one actual metadata cut;
it is not a historical-file lease. A concurrent writer may commit after that
cut; an embedding application must recheck its document/model/Catalog authority
before accepting a prepared result. Successful values report the consumed cut.

`SeismicAuthenticatedSource` is a **trusted C++ embedding interface**. Its range
callback must authenticate every returned byte against the supplied complete
file SHA, including binary and trace headers. It is not a tool-facing callback,
a pathname escape hatch or an authentication implementation supplied by this
SDK. A caller that returns unverified bytes violates this interface. The host
must bind the source to its approved document/version before constructing it.
File handles, mtime, size and SEG-Y quick fingerprints alone are insufficient.

The SDK's registered source fingerprint is retained as metadata. When it is a
complete SHA, it must equal the authenticated source SHA; a legacy `quick:` value
cannot substitute that independent complete anchor. No quick fingerprint is
silently promoted or written back. The host may use an already established
full-SHA/chunk index, account separately for its first complete verification and
later actual range I/O, and invalidate it when document/source authority ends.

## Initial decoding scope

This first explicit contract accepts fixed-length SEG-Y revision 1 with the
fixed-length flag, big-endian IEEE32 samples, 0–32 declared extended text
headers, and declared **ms / TWT or OWT / time reference**. The amplitude unit
must be explicit. Other encodings, endian layouts, variable/unknown trace
lengths, unknown extended-header count, depth axes and inferred conventions are
unavailable through this API. It does not guess a grid ordinal or nearest trace.
The old general-purpose readers retain their wider compatibility behavior.

The binary header, selected trace header and requested amplitude bytes are all
read through the same authenticated callback. The decoder checks source length,
trace position, actual trace sample count/interval/delay against the declared
metadata, and computes exactly the selected axis. It preserves the actual trace
number (including zero) and IL/XL from the chosen header, with no coordinate or
axis conversion, resampling or interpolation. Negative and zero amplitudes stay
valid; source nonfinite values become explicit null-mask entries with a zero
storage placeholder, never an interpolated/valid zero.

At most 8192 samples are returned. Arrays use float64 axis/amplitude values and
one byte per null flag. Budgets cover metadata, requested source ranges, output
and conservative working buffers before allocation; source size is bounded to
64 GiB. `requestedSourceBytes` and `sourceReadCalls` measure callback requests,
not disk reads, kernel cache activity or the host's initial full verification.
The host must report those independently. Failed/short authentication, callback
exceptions, cancellation, stale cuts, unsupported layouts and invalid metadata
return no partial headers or arrays. Existing source files remain read-only.

The synthetic SDK tests use an immutable, fully hashed in-memory source to
exercise this embedding boundary. Real filesystem authentication, process
isolation, publication and end-to-end Agent availability belong to the main
repository's I2 integration; they are not inferred from these SDK tests.
