# Compiler trace timing

GN's `build_time_report.py`, `build_stats.py`, Python `pch_update.py` and native
`pch_update` use distclang v1 metadata when reading compiler traces in any
supported storage format. Each trace gets a proportional CPU-active estimate:

```
factor = min(1, (cpu_user_us + cpu_system_us) / execution_wall_us)
```

The same factor maps **all** relative timestamps and event endpoints, including
async Source pairs, phases, templates, functions and clang's Total events.
Counts and names are unchanged. Mapping endpoints together preserves interval
nesting and unions. Queue wait is already outside execution wall time and is
not subtracted again. CPU accounting above wall time is capped rather than
amplifying phase durations.

This is a whole-compilation estimate, not a measurement of the CPU time of
each phase. It cannot locate stalls, distinguish scheduler contention from I/O
wait, or compensate for CPU frequency and memory-bandwidth changes.

Missing, malformed, zero-CPU or unsupported-version metadata retains raw wall
timing. Reports identify these cases and normalization coverage, so mixed old
and new traces are visible. Original JSON and compressed archives are never
rewritten. Ninja command wall times and the build timeline retain their
original meaning.

`build_time_report` writes per-object provenance and raw/corrected compiler
times to `trace_timing.csv`. Its loader retains all raw phase totals alongside
the corrected summary. `build_stats` persists this provenance in the
`trace_timing` SQLite table; historical rows without it are `legacy-wall`,
not retroactively normalized. New recordings use the new reader.
Python PCH state and native trace/analysis cache versions are invalidated.
Native summaries retain raw ExecuteCompiler/Frontend and the exact rational
factor; stale-trace checks against Ninja use raw ExecuteCompiler.

`pch_target_header.py` only extracts include topology and order. It has no
duration-based decisions and uses the original clock to avoid rounding ties.
Trace compression/lookup likewise operates on the original data.
