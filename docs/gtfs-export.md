# GTFS export

Export an imported timetable as one GTFS feed:

```sh
nigiri-export --tt_path data/tt.bin --gtfs_export_dir combined.zip
nigiri-export --tt_path data/tt.bin --shapes data --gtfs_export_dir combined.zip
```

Use a compatible binary. The timetable and shape files must come from the same
import. Shape files are opened read-only. Missing files or a different trip count
cause failure before output is created. These checks do not verify that the files
come from the same import.

## Parallel export

Use `--threads 8 --chunk-size 64` to enable parallel export. The default is one
worker and 64 transports per chunk. The valid ranges are 1–256 workers and
1–65536 transports per chunk.

A shared worker pool processes transport chunks. Up to four additional threads
write separate tables. Each table keeps at most `--threads` chunks in progress.
With `--threads 1`, tables are written in sequence.

Chunks are written in transport order. Shapes are written once, in first-use
order. Directory files and ZIP members have the same content as a single-thread
export. ZIP compression starts after all tables are complete and is sequential.

The queue limit applies to chunk count, not bytes. Memory use depends on stop
counts and shape lengths. Reduce `--chunk-size` or `--threads` to reduce buffer
memory. If a worker or writer fails, pending work is cancelled. The exporter waits
for active work to finish before it releases shared data.

## Output files

Use an empty directory instead of a `.zip` path to write separate files. A failed
directory export can leave incomplete files. Remove them before you try again.

ZIP output uses `<output>.tmp_export` as a temporary directory. The exporter
closes the archive, then renames it to the destination. Until the rename succeeds,
an existing archive is unchanged. An existing temporary directory causes failure.
After a crash, remove it only when no export uses it. ZIP output needs space for
both the uncompressed tables and the archive.

## Timetable semantics

- Output represents the imported timetable, not the original feeds. Unknown
  fields and alternative identities of merged trips are not preserved.
- Agencies use `Etc/UTC`. Frequency services become individual scheduled trips.
- A transport is split when its selected internal trip or an accessibility flag
  changes. Sections share a boundary stop and `block_id`, but use their own route,
  agency, headsign, and flags. Export selects the first internal trip on a merged
  section.
- Nigiri does not retain the difference between an unknown accessibility value
  and an explicit prohibition. Export uses unknown for unset flags.
- IDs can change between imports. Source metadata is not validated. Check data
  quality and redistribution rights before publication.
- `original_stop_id` in `stops.txt` and `cars_allowed` in `trips.txt` are extension
  columns.

## Source attribution

`attributions.txt` records source filenames with the following convention:

- `attribution_id` is `nigiri:<source>:<route>:<file-index>`. Each row has a unique
  ID. The source index applies only to the current import.
- `route_id` links the row to an exported route.
- `organization_name` contains the recorded input filename or path.
- `is_producer` is `1`.

This is a project convention, not a standard GTFS method to record source files.
Other consumers can display these filenames as producer credits. They are not
verified organization names.

Each used route has one row for each recorded file of its source. The file index
follows sorted path order. Group by the source part of the ID, not by filename or
the complete attribution ID. Missing paths use `unknown-source-<source>`. For
merged services, the source is that of the selected internal trip. Original input
attributions are not preserved. Paths can include local directories. Check them
before publication.

When shape storage is supplied, a separate row with ID `osm` credits
`OpenStreetMap contributors` and links to
<https://www.openstreetmap.org/copyright>. It applies to the complete dataset.
It does not mean that every shape comes from OSM. Publisher shapes can also use
OSM data. The export cannot determine all geometry licenses. Applications must
provide the required visible credits. The file alone does not ensure license
compliance.

## Shapes

With `--shapes`, export writes `shapes.txt`, `trips.txt.shape_id`, and stop
distances in metres. It clips each selected internal trip's stored geometry to
the exported stop range. It does not generate new geometry or recover shapes
from a routing cache.

Uses of the same stored slice share a shape ID. Publisher and routed indices are
separate. Consecutive zero-distance points are omitted. If stop distances do not
strictly increase, the shape is retained but all stop distances for that trip are
omitted.

Missing shapes, ambiguous trip ranges, invalid offsets, invalid coordinates, and
zero-length geometry leave trips without shapes. Export reports coverage and
omission counts.
