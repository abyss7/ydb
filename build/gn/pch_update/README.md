# Two-step PCH selection

`pch_update` selects compatible base/delta PCH chains, measures the initial build,
reselects the deltas and their consumers once, measures the corrected build and
installs the groups in the ordinary GN global-PCH build. The base contents and compiler
classes remain fixed during correction. This is a bounded two-step workflow,
not a convergence guarantee.

The assignment unit is a **GN PCH-owner target**, including generated enum
helpers that inherit its configuration. Every C++ compilation of that owner
uses the same leaf (or none). Candidate partitions, expected-work comparisons,
control selection, verification sampling, calibration rows and compatibility
fallbacks operate on whole targets. Individual compilation timings and the real
action DAG remain the inputs for scheduling and incremental invalidation.
An owner with mixed exact compiler classes stays entirely without PCH; it is
reported as `mixed-compiler-classes`, never split into source-level assignments.
Targets whose GN templates lack a global PCH hook remain unassigned and are
reported as `no-global-pch-hook`. Eligibility comes from `global_pch_owner`
metadata in the real GN graph, including enum helpers inheriting an owner's PCH.

```sh
pch_update --baseline /path/to/no-pch-build --disk 16G

# Choose the objective for this run:
pch_update --baseline /path/to/no-pch-build --disk 16G --mode=global
pch_update --baseline /path/to/no-pch-build --disk 16G --mode=incremental

# Optional model/resource configuration:
pch_update --baseline /path/to/no-pch-build \
    --workers 32 --ram-gb 96 --disk 16G --q 0.1
```

Run from the source worktree (or a subdirectory). The normal command runs
baseline preparation → analysis/selection → initial measurement → correction →
corrected measurement → GN export/replacement/validation. It stops on failure;
repeat the same invocation to continue. There is exactly one correction.

| Stage | Command | Work |
| --- | --- | --- |
| Predict | `pch_update --baseline B --disk 6G --predict` | Prepare/build no-PCH baseline with traces; analyze, select, check compatibility |
| Correct | `pch_update --baseline B --correct` | Measure initial groups, calibrate/correct, measure corrected groups |
| Apply | `pch_update --baseline B --apply` | Export GN groups, replace the old tree, check flags/assignments and build normal global targets |
| All | `pch_update --baseline B --disk 6G` | All three stages in order |

Only one stage flag may be supplied. Stage prerequisites are explicit: a standalone
`--correct` needs a completed prediction; `--apply` needs a verified correction.
Paths and parameters are stored automatically; no `--output`, `--plan`,
`--analysis` or `--export` arguments exist. Retries inherit settings while the run
is unfinished. After successful application, all temporary state and settings are
removed. The next weekly run starts fresh and needs `--disk` again.

| Option | Default |
| --- | --- |
| `--workers` | CPU count at run start; shared baseline/MEASURE worker ceiling |
| `--ram-gb` | 75% of available RAM before baseline preparation, capped by the exposed cgroup-v2 budget; shared baseline/MEASURE budget |
| `--disk` | **required on first predict**: resident PCH budget, e.g. `16G`, `4.5G`, `800M`, `400K`; a bare number means gigabytes |
| `--mode` | `incremental`; `global` prioritizes cold builds, `incremental` prioritizes historical rebuilds |
| `--q` | Cold-build weight: `1` for `global`, `0.1` for `incremental`; an explicit value overrides the profile default |
| `--bandwidth-gbps` | 1 GB/s |
| `--action-memory-gb` | 1 GB unmeasured-action RSS prior |
| `--analysis-jobs` | min(24, CPU count); analysis compiler jobs and parallel prediction scenarios |
| `--sample` | `100`: share of whole PCH-owner targets that `verify` measures (`10`..`100`); all their C++ members, all PCHs and required generator tools are built |
| `--target` | `all` and `tests`; repeat to override |

`--mode=global` and `--mode global` are equivalent. Explicit `--q` takes precedence
regardless of argument order. The profile name chooses the optimization objective
only; it does not change GN's `pch_mode`. Both objectives select
base/delta PCH chains. Both install into GN `pch_mode="global"`. When omitted,
`--mode` resumes the current run's objective (initially `incremental`). There is
only one unfinished run, even when changing the objective.

All scratch belongs to this one run under the baseline:

```text
BASELINE/.pch_update/
  state.bin               # worktree binding and current parameters
  cache/                  # resumable analysis jobs and compatibility probes
  analysis.path           # current reusable analysis checkpoint
  current/
    initial/              # prediction, measured graph and telemetry
    corrected/            # corrected plan and its measured graph
    export/               # generated global PCH tree
    original-global/      # rollback backup of the previous groups
    global/               # ordinary GN validation build
```

Worktree and baseline locks serialize the workflow. Source/configuration/tool
fingerprints protect frozen measurements. Changing inputs restarts prediction
when running the full cycle or `--predict`; later stages request `--predict` first.
Analysis checkpoints and content-addressed compiler probes are reused when their
inputs still match. Stale results are replaced in the same working directory,
with a journal for interrupted resets; no previous iterations are archived.
An existing legacy active run is adopted without relocating its embedded paths.
Its old scratch is also removed when application succeeds.

Successful probes discard their redundant depfiles after saving dependency
fingerprints. Once the corrected plan is durable, compatibility scratch and
initial `.o`/`.pch`/`.dwo` artifacts that are not snapshot inputs are discarded.
The corrected model and its measured build remain available until application
finishes. There are no age limits, LRU policies or persistent cross-run caches.

The baseline is generated/built with `pch_mode="none"`, `time_trace=true`.
Existing build arguments are retained. A baseline bound to another worktree is
rejected. `--target NAME` overrides the default `all tests` workload. With no
history, use `--q 1`. Resource limits remain frozen while a measurement resumes.

Worker, RAM, bandwidth and per-action memory defaults are resolved and saved
**before** preparing the baseline. Retries and both MEASURE stages inherit those
values, including when available RAM has changed since the first invocation.
Existing unfinished plans supply their already frozen resource defaults.

The no-PCH model reserves `--action-memory-gb` for each non-phony action. Baseline
Ninja therefore uses explicit
`-j min(workers, floor(ram-gb / action-memory-gb))`. MEASURE uses `-j workers`
and admission against the same RAM budget, with additional PCH memory and later
calibrated per-action reservations. These are estimated reservations, not OS hard
RSS limits. GN's named pools are inherited from the baseline and checked against
the freshly generated measurement graph. Bandwidth admission applies to modeled
PCH reads/writes; no-PCH actions have no such cost.

These settings govern newly executed baseline commands. An incremental baseline
can retain traces produced earlier under other parallelism or cache conditions;
matching launch settings does not retroactively remeasure those traces or prove
full-build speedup.

Analysis is saved before selection/emission. Completed selection is saved as
`selection.bin` immediately, before compatibility probes or graph emission, for
both the initial and corrected plan. A retry reuses it when the tool, model,
calibration and frozen-base inputs match. Resolved resource defaults stay frozen
when resuming an incomplete plan. Interrupted selection or correction also
reuses successful compatibility probes. A failed or interrupted MEASURE restarts
the entire affected phase: all measured outputs and telemetry are discarded and
a new measurement identity is created. The selected plan and Ninja graph are
retained. Missing or invalid telemetry also restarts the whole phase. Completed
phases with intact outputs and telemetry are reused while the run is unfinished.
Elapsed wall time and calibration observations belong to one successful attempt.
Source/tool changes that invalidate a frozen measurement require recomputation;
resuming does not bypass input fingerprints.

Ninja stays in the terminal's foreground process group, so Ctrl+C interrupts the
controller and the build together. MEASURE runs direct compiler commands without
compiler-cache daemons. Run measurements on an otherwise idle host; external
CPU load is not monitored or corrected automatically.

`--apply` emits exact owner labels and ordered compiler classes, resolves generated
headers through public codegen targets and represents parent PCH dependencies in
GN. Before building, it checks every assigned consumer and every PCH producer
against the actual global graph. The normal GN workload is then built, including
bootstrap-tool work. A journal and backup restore the old global tree on failure
or recover an interrupted replacement on the next invocation. User edits made
during a suspended replacement are detected rather than overwritten.
After the successful build and installation commit, the entire `.pch_update`
directory is renamed to `.pch_update.cleanup` and deleted. A retry finishes an
interrupted cleanup without rebuilding or reinstalling anything. The baseline
itself and the installed source-tree PCH groups remain. A later invocation with
`--disk` starts a new run rather than recalling the previous one.

Selection removes unused artifacts and prunes low-benefit leaf groups to fit
the PCH disk budget before scoring the complete graph. Frozen bases survive
correction, including when they lose all consumers. Artifact size is predicted
from the header source bytes a PCH serializes (one prior, refitted from the
first verification), not from parse time: a PCH stores declarations once,
whatever the number of files parsing them.
Absolute-path includes are not injected as PCH roots: directly including a
sysroot header can lose Clang's system-header classification and break consumer
compilation. Such headers remain eligible as transitive dependencies of roots
with ordinary include-search spelling.
Textual `.inc` and `.def` fragments are also excluded as roots: compiling one
alone does not prove that injecting it is safe (for example, Protobuf's
`port_def.inc` requires a matching `port_undef.inc`). Their transitive costs
remain part of the model.

Before freezing any initial or corrected prediction, `predict`, `replan` and
`correct` run a **compatibility preflight** against the completed baseline:

1. Compile each selected PCH chain with its exact compiler class.
2. Check every C++ member of each assigned target with `-fsyntax-only` and its PCH.
3. If any member fails, move the **whole target** to the parent PCH and recheck
   every member, then try all members without PCH. Only a universally successful
   fallback is accepted. A no-PCH failure or compiler crash aborts planning.
4. Remove unused chains and recompute the prediction from the checked layout.
   Correction preserves every frozen base; failure to compile one aborts it.

This catches cases where a PCH compiles alone but introduces declarations that
make a consumer ambiguous (for example `llvm::Module` in `Minidump.cpp`).
`compatibility.tsv` records target-wide fallback decisions and diagnostic log paths; its
group IDs refer to the candidate before unused groups are compacted.

Successful probes and PCH artifacts are cached under
`BASELINE/.pch_update/cache/compatibility-v1`; `--work-dir DIR` changes the cache
location for all three planning commands. Entries are scoped to the validated
analysis inputs, compiler, flags, include order, parent artifact and relevant
environment. Depfile fingerprints additionally cover newly exposed headers.
Any analysis-input change invalidates that analysis's probe keys; selector-only
changes reuse unchanged probes. Missing artifacts and corrupt records are
recomputed. Results are written atomically after successful completion, so a
later failure can be retried with `replan` into a new output directory. Failed
and interrupted probes are not cached as successes.

Preflight compiles PCHs serially and bounds consumer parallelism by the worker
limit and the largest predicted compiler RSS. The cache occupies additional
disk space outside the resident-PCH budget. Preflight timings are not calibration
data; its artifacts never populate the verification build. Syntax checking does
not replace the full verification build (code generation and linking can still
fail).

### Existing data migration

Frozen plans and Clang PCHs contain absolute paths. Existing directories can be
moved into `.pch_update/` without rewriting their contents if their old paths
are retained as symlink aliases. A migrated cache's `.path-alias` file records
its old absolute path; the tool validates that it resolves to the same physical
directory and preserves it in compiler commands/cache keys. Both the analysis
cache and its `compatibility-v1/` subdirectory need this marker. Old paths then
contain only links, while all artifacts live in the baseline. Keep those aliases
while the migrated entries are in use. Newly created caches need no aliases.

The measurement steps run `build/measured.ninja`, using direct local
compiler commands and explicit per-action observations. Worker/pool limits are
enforced by Ninja; the runner reserves predicted RAM and PCH I/O bandwidth
across processes. `--disk` limits resident PCH artifacts, rather than total
build-directory size. Memory reservations are estimates, not OS memory limits.
Each measurement attempt has its own identity. A failed or interrupted run is
repeated in full under a new identity; calibration is allowed only after every
required measurement has completed successfully and passed output validation.

`--sample PERCENT` (10..100, default 100) makes verification build only a share
of the targets. Selection is deterministic per plan and GN owner label, independent
of target size: every PCH build plus **all C++ compilations** of a pseudo-random
share of targets. Sampling changes prerequisites, not the selected compiler
commands or PCH assignments. For sampled compilation, GN's target-wide order-only
dependencies are replaced by generated inputs recorded in the frozen baseline's
`.ninja_deps`, supplemented by inputs from successful compatibility probes with
the assigned PCH. A missing compiler dependency record is an error. Explicit and
implicit inputs, validations, and PCH dependencies remain required.

Generators still compile and link their tools and libraries as needed; only
unnecessary target-wide ordering is removed from compilation. Therefore 20%
does not promise a fivefold speedup: all PCHs and their required tools still
have to be built. `sampling.tsv` and the `measurement sample` log line report
selected targets, their compilations and the actual prerequisite closure, including links.
At 100%, the full GN ordering is preserved.

Calibration uses the selected observations, keeping mandatory extra tool builds
out of the random sample. Calibration sums features and timings per complete
target; partially observed targets contribute neither controls nor training or
holdout rows. Unmeasured actions keep the model estimate; `correct` reports
coverage in `calibration.tsv` (`# measured_actions`, `# measured_targets`).
With a sampled run `cold_seconds` is a prediction rather than a measured full
build; `summary.tsv` says so.

`correct` checks source, tool, configuration, action and output identity before
using any measurements. It compares the frozen initial ledger with observations,
holds out entire groups during fitting, reports unidentifiable parameters, and
emits a new independent build directory. The corrected configuration remains
a prediction. It can optionally be measured with `verify --plan` too, but cannot
be corrected again within this cycle.

`assignments.tsv` has one row per owner target, including its compilation count
and predicted aggregate compile work in seconds; `target-members.tsv` maps each
member's source and output back to its owner. `initial-errors.tsv` compares
aggregate compile work per measured target and reports target WAPE. Detailed
per-action measurements remain in `initial-action-errors.tsv`. Aggregate work
is not target wall time: wall predictions still use the complete scheduled DAG.

## Building

The binary is an ordinary GN target, `//build/gn/pch_update:pch_update`, reachable
from the root `group("all")`. To reuse an existing GN build directory after
switching worktrees, regenerate its graph against the new source root before
building the tool:

```sh
gn gen ~/b/gn-tools --root="$PWD" --script-executable=python3.9
ninja -C ~/b/gn-tools build/gn/pch_update:pch_update
```

The executable is written to the build directory root: `~/b/gn-tools/pch_update`.
The existing `args.gn` is preserved. Regenerating a *different* build directory
against this tree requires its own GN invocation; the no-PCH baseline is not
modified by the tool build. Adding the tool to `group("all")` changes the source
tree identity, so existing analysis checkpoints must be regenerated.

## Artifacts

- `snapshot.bin`: checksummed analysis, configuration, initial predictions and
  action identities; never rewritten by verification or correction.
- `selection.bin`: completed layout keyed by selector inputs, reusable even
  when compatibility checking or build emission has not finished yet; also
  retains disk-budget diagnostics so a retry can restore a missing report.
- `disk-budget.tsv`: predicted restoration of evicted PCHs up to **2x** the
  configured disk budget. Each grouping width has its own cumulative sequence;
  missing parents are included once and oversized packages are skipped.
  Columns show added MB, total occupied MB, the required budget increase,
  restored targets and the estimated step/cumulative work savings. MB means
  1,000,000 bytes. Estimates reuse `FitDisk` scores: they are neither measured
  CPU seconds nor predicted build wall time, and precede compatibility checks.
  `selected_at_original_budget` identifies the width chosen at the original
  budget. Sequences of different widths must not be added together.
- `BASELINE/pch-disk-budget.tsv`: latest completed prediction phase's copy of
  the report above; survives successful cleanup of `.pch_update/`. A corrected
  prediction replaces the initial report. No extra compilations are performed
  for these diagnostics.
- `summary.tsv`, `predictions.tsv`, `assignments.tsv`, `target-members.tsv`:
  objective, per-action estimates, target-level leaves and their C++ members.
- `build/pch/*.h`: selected root headers; the Ninja manifest records the chains.
- `observations/*.bin`, `run.bin`: action timing, child CPU/RSS, output sizes,
  command identity and verification invocation.
- `initial-errors.tsv`, `initial-action-errors.tsv`, `calibration.tsv` in the
  corrected plan: aggregate target and detailed action errors, measured wall
  time, fit coverage and held-out errors.

The model uses exclusive header costs and full cold/historical DAG scheduling.
Order-only edges affect scheduling but not dirty propagation. Validation edges
are additional roots. Changed restat outputs are conservatively assumed to
propagate. Header selection greedily considers all eligible roots, followed by
full graph scoring; it is heuristic. Initial size, RSS and loading costs are explicit
priors. Exact compiler flags, macros and include order define compatibility
classes, so some classes may have too few groups to identify separate costs.

`pch_update self-test` checks scheduling, invalidation, frozen-base refinement,
calibration rank/holdout isolation, snapshot corruption, measurement restart,
missing telemetry and interrupted replacement recovery.

Shared bases require headers without standalone external-symbol requirements.
Specialized leaves may include library-dependent headers: compatibility builds
an empty source with the complete PCH and the consumer's actual compiler flags,
then checks its remaining strong symbols against every final link of every
member of the assigned GN owner. Missing providers or incomplete link metadata
move the whole owner to a checked parent PCH, or to no PCH. Library presence is
a necessary check; actual final linking and initialization semantics still need
validation. `compatibility.tsv` records link probes and target-local reasons.

Set `PCH_UPDATE_TEST_CXX=/path/to/clang++` when running `self-test` to also exercise
real PCH chains, provider discovery, helper-owner fallback and cache invalidation
with a small native fixture. Analysis snapshots now include exact link inputs;
older snapshots require fresh prediction.

The public CLI and real GN export/build are exercised by:

```sh
python3 build/gn/pch_update/workflow_test.py /path/to/pch_update /path/to/clang++
```

This uses a temporary sparse shared clone without creating commits, and retains
its fixture/log on failure. It covers separate stages, inherited parameters,
repeat invocations and the full default pipeline after a source change.
