# Two-step PCH selection

`pch_update` selects compatible base/delta PCH chains, measures the initial build,
reselects the deltas and their consumers once, measures the corrected build and
installs the groups in the ordinary GN global-PCH build. The base contents and compiler
classes remain fixed while the correction selects, and what the first measurement
showed is kept at every level: targets are anchored at their observed compile, the
class coefficients come from all measured groups, and every built base and delta
teaches its own header units. This is a bounded two-step workflow,
not a convergence guarantee.

The assignment unit is the **GN target that compiles the object**. A generated
enum `source_set` is a separate target from its consumer. Every C++ compilation of one target
uses the same leaf (or none). Candidate partitions, expected-work comparisons,
control selection, verification sampling, calibration rows and compatibility
fallbacks operate on whole targets. Individual compilation timings and the real
action DAG remain the inputs for scheduling and incremental invalidation.
An owner with mixed exact compiler classes stays entirely without PCH; it is
reported as `mixed-compiler-classes`, never split into source-level assignments.
Targets whose GN templates lack a global PCH hook remain unassigned and are
reported as `no-global-pch-hook`. Eligibility comes from `global_pch_owner`
metadata in the real GN graph; each hook names its own compile target.

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

### Console and persistent diagnostics

The console names the seven workflow phases, reports their elapsed wall time,
and distinguishes first attempts, completed samples and partial resumes.
`--verbose` shows selection widths and full child output. It is presentation-only:
it is not stored among model options and does not invalidate checkpoints.
Ordinary child output goes to persistent logs; warnings and failures remain visible.
Progress is throttled on both terminals and redirected output. Ninja progress
counts executed commands, whereas requested cached results count all inputs.

`BASELINE/pch-report/latest.txt` points to the report directory for the latest run.
Retries append logs to the same directory. Successful cleanup removes scratch
builds, not reports. Earlier run reports are retained. The directory contains:

* `summary.txt`, `summary.json`: latest prediction, explicitly not measured speedup.
* `initial/`, `corrected/`: separate prediction, sample measurements, target/action
  errors, assignments, compatibility, sampling and disk-budget tables.
* `selection-targets.tsv`: reason per target in the chosen pre-compatibility plan.
  Reasons include calibration control, missing hook, mixed flags, no candidate in
  the bounded shortlist, no estimated target benefit, disk eviction, or retaining
  the seed because no candidate improved the global objective.
* `selection-choices.tsv`, `selection-widths.tsv`: considered shortlist costs and
  grouping alternatives. Candidate IDs are local to each width before pruning.
  These are weighted work estimates, not CPU or build wall times.
  `selection-candidates.tsv` maps candidate IDs to root headers and parent chains.
* `compatibility-targets.tsv`: assignment mapping through fallback and final ID
  compaction; rejected compiler probe logs are retained alongside the reports.
* `corrected/assignment-changes.tsv`, `calibration.tsv`: initial/corrected assignment
  changes, parameter identifiability, training coverage, leave-one-group-out error
  and the controls, anchors and per-unit tables of each compiler class.
  `calibration-parameters.tsv` contains the fitted/prior coefficients themselves;
  `calibration-targets.tsv` the anchor and saved-time multiplier of every measured
  target with a PCH; `calibration-units.tsv` the header units whose PCH bytes, build
  seconds or saving a measurement calibrated individually (zero: the class prior).
* `phases.tsv`, `logs/`: phase status/duration and complete child output per attempt.
* `final-result.json`: successful installation and full GN build outcome. Building
  test binaries does not execute tests. `README.txt` explains units and reasons.

Measurement reports distinguish sample wall time, summed action/CPU time, actual
PCH bytes, and largest individual action RSS (not aggregate peak RAM). Target WAPE
is `sum(abs(predicted-actual))/sum(actual)` over complete observed targets; the
calibration MAE is leave-one-group-out cross-validated error of the measured
members' compile work per target with a PCH. Initial and corrected plans
measure the same object panel, but their PCHs and prerequisites can differ;
sample wall times are not a full-build speedup comparison.
`q` weights predicted cold/incremental wall times. The disk budget is for resident
PCH artifacts only. GB is decimal. Historical checkpoints without detailed selection
reasons remain usable; missing reasons are reported as unavailable, never inferred.

Paths and parameters are stored automatically; no `--output`, `--plan`,
`--analysis` or `--export` arguments exist. Retries inherit settings while the run
is unfinished. After successful application, all temporary state and settings are
removed. The next weekly run starts fresh and needs `--disk` again.

| Option | Default |
| --- | --- |
| `--workers` | The `-j` of the whole run: baseline, MEASURE, compatibility checks and analysis. Never reduced when given. Default: the CPU count at run start, at most what `--ram-gb` admits at `--action-memory-gb`. The PCHs themselves are built at most a quarter of it at a time |
| `--ram-gb` | 75% of available RAM before baseline preparation, capped by the exposed cgroup-v2 budget; shapes the default of `--workers` only |
| `--disk` | **required on first predict**: resident PCH budget, e.g. `16G`, `4.5G`, `800M`, `400K`; a bare number means gigabytes |
| `--mode` | `incremental`; `global` prioritizes cold builds, `incremental` prioritizes historical rebuilds |
| `--q` | Cold-build weight: `1` for `global`, `0.1` for `incremental`; an explicit value overrides the profile default |
| `--action-memory-gb` | 1 GB; the unmeasured-action RSS prior, and with `--ram-gb` the default worker count |
| `--analysis-jobs` | `--workers`; analysis compiler jobs, grouping candidates and prediction scenarios |
| `--sample` | Measurement only (`--correct` or the correction phase of a full run): percent of C++ objects (`10`..`100`, default `100`); all PCHs and required generator tools are built |
| `--target` | `all` and `tests`; repeat to override |

PCH compatibility builds and leaf-link probes run as their parent PCHs become
ready, at most a quarter of `--workers` at a time: building a PCH takes several
times the RSS of an ordinary compile, and every delta of a base is ready the
moment the base is built. The syntax checks take the full count, a file with its
PCH mapped read-only being lighter than the compile it comes from. The
measurement graph gives its PCH rules the same pool. Analysis uses
`--analysis-jobs`: grouping candidates run in parallel, then each candidate's
historical scenarios run in parallel. A loop nested in the worker of another
shares its threads instead of adding its own, so no stage exceeds the count.
Scenario sums and candidate tie-breaking retain their serial order.
`--analysis-jobs 1` serializes these model computations without changing the
modeled build.

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

Worktree and baseline locks serialize the workflow. Source identity is HEAD plus the
working tree's diff against it, hashed as `git diff` streams: a diff of any size is
never held in memory. Source/configuration/tool
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

The baseline is generated/built with `pch_mode="none"` and tracing enabled.
Existing `time_trace="json"`, `"lz4"` or `"zstd"` settings are retained;
otherwise `time_trace="json"` is selected.
Existing build arguments are retained. A baseline bound to another worktree is
rejected. `--target NAME` overrides the default `all tests` workload. With no
history, use `--q 1`. Resource limits remain frozen while a measurement resumes.

Worker, RAM and per-action memory defaults are resolved and saved **before**
preparing the baseline. Retries and both MEASURE stages inherit those values,
including when available RAM has changed since the first invocation. Existing
unfinished plans supply their already frozen resource defaults.

`--workers` is the one limit of a run: the `-j` Ninja gets for the baseline and
for `measured.ninja`. Nothing reduces a count that was given. A build without PCH
is what the count was chosen for, and the measurement compiles the same files
with a PCH mapped read-only. Left unset, the count is every CPU, and no more
than `--ram-gb / --action-memory-gb` compiles of the prior size at once; a
count derived that way follows a RAM budget supplied later, a count that was
given is kept. The
model schedules on the same count, the dependency graph and GN's named pools,
inherited from the baseline and checked against the freshly generated
measurement graph. Nothing else bounds a Ninja build, so RAM and disk bandwidth
bound neither the model nor the measurement. Per-action RSS is still estimated
and reported, and calibrates the consumer memory scale.

These settings govern newly executed baseline commands. An incremental baseline
can retain traces produced earlier under other parallelism or cache conditions;
matching launch settings does not retroactively remeasure those traces or prove
full-build speedup.

Analysis is saved before selection/emission. Completed selection is saved as
`selection.bin` immediately, before compatibility probes or graph emission, for
both the initial and corrected plan. A retry reuses it when the tool, model,
calibration and frozen-base inputs match. Resolved resource defaults stay frozen
when resuming an incomplete plan. Interrupted selection or correction also
reuses successful compatibility probes. Retry `--correct` after a failure or
interruption. Within each frozen measurement phase, successful per-action
records and outputs are checked against the command, plan and attempt before
reuse. Missing or damaged measurements are rebuilt; Ninja schedules their
dependents and unfinished work. Each retry gets its own invocation token.
The calibration report counts reused/new observations. A resumed phase has
no continuous sample wall time (`wall_seconds: null` in JSON); each attempt
still has an elapsed time in the log.
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

Selection compares grouping widths at the requested `--disk` budget using
estimated artifact bytes. Owners of one compiler class are clustered by their
residual header-parse profile (bucketed signatures, deterministic farthest-point
seeding) instead of label-alphabet partitions, then refined by two
expectation/maximization rounds: deltas are re-picked strictly from the files
actually assigned to them — never those of an owner the measurement saw save
nothing with any PCH — and each owner takes the cheapest of NONE, the base, the
delta built for its own cluster and up to eight deltas of its compiler class
whose serialized units overlap its profile, ranked by that overlap less the load
of the delta's own content. They include deltas whose cluster its label never
touched, and deltas rooted at an umbrella header, even one with no parse time of
its own, that pulls the owner's headers in transitively. A width therefore only bounds the cluster
count; cluster sizes vary freely. For widths whose layout exceeds the budget
the selector also builds disk-priced variants: a small lambda grid whose prices
cut the candidate where the estimated bytes of its roots, of every class of it
and densest first, reach
60%, 100% and 125% of `--disk`, so every PCH keeps a dense prefix that fills
the budget instead of being evicted wholesale later.
Roots enter a PCH in descending benefit-per-byte order, so these prefixes are
trimmable tails. Selection can evict a delta to its best open fallback — the
base or a surviving sibling delta of the same compiler class sharing its heavy
headers — or in `initial`
drop an entire base and its remaining children to NONE. Each eviction competes
on marginal work lost per freed byte; a shared base is charged only once.
Only the winning width gets a probe reserve: the same selection is fitted to
**2 × `--disk`** in estimated bytes before compatibility (fixed factor).
`selection-widths.tsv` reports the width/lambda candidates at 1× and the
separate winner's probe count and bytes at 2×. A compiler class has at most one
safe stable base; without a base it cannot produce deltas. *Safe*: compiled
alone, the header needs nothing beyond the C++ runtime (operator new and
delete, the ABI, libc++, the unwinder), which every final link supplies; what
counts is the entity a symbol names, not its signature, so a project function
taking a `std::string` is a symbol of the project. *Stable*: not written by
the build (generated output never is, whatever its history says), and either
unchanged in the history or changed cheaply enough for the objective — never
counted under a cold-only objective, at most 2% of commits under a history of
twenty or more; the rebuilds are priced either way.
Each compiling target
uses NONE, the base, or the base plus one child.

The initial plan keeps a three-point control in every compiler class of four owners
or more so that the correction can be measured: `clamp((⌊√n⌋+1)/2, 1, 6)` owners
stay without any PCH (`calibration-control`: the compiler's speed on this class
against the traces, and how much it varies between owners) and, in classes of eight
owners or more, as many owners that would take a delta stay on the base
(`calibration-base-control`: what the delta adds over it for an owner that would
take one). Both kinds are taken from the lighter half of the class, by the parse
seconds a PCH can save an owner, so the heaviest owners always remain to pay for
the shared base. A class of fewer than four owners has no control: a bare control
pays nothing towards its base, a lone owner would be left without any PCH, and a
base that leaves the initial plan cannot return in the correction. Within that half
both sets are spread evenly over the label order, which is independent of an
owner's headers, and are measured first (see sampling). The counts grow with the
square root of the class, so a large class gives up a few percent of its coverage
instead of a tenth of it. Correction has no controls: every owner takes its best
measured option. Before scoring each width and
before invoking Clang, reprice candidates on their **assigned GN targets**.
A delta must save more weighted consumer work over its best open alternative
than its own build costs. Otherwise its owners take that alternative.
A family must then pay for its base and surviving children versus NONE. A frozen
base is frozen in content only: in the correction its measured build is charged
like any other, and a family that does not cover it leaves the plan. A base
without a single assigned target or surviving delta leaves a
candidate as soon as the candidate is scored, frozen or not: no candidate is
charged with building it or counted its bytes, it is not exported, and a family
emptied by an eviction releases its base's bytes at once. Cycle-induced reassignments repeat the test; where no single move opens a cycle, its
owners step to the parent of their leaf one at a time, not to NONE.
`selection-payback.tsv` records modeled parse savings, load, extra compiles,
build and net seconds for both decisions (delta and family rows overlap; do not
add them). After building and checking the
winning candidates, preflight fits `--disk` using actual PCH file sizes. An
initial base measured larger than the room the plan leaves for it — the budget
less what else the plan must keep, every other base (everything else, while
the whole plan fits by estimate) — is first trimmed to that room: the thin
tail of its roots, in the order `Pick` accepted them, is dropped instead of
losing its whole family. A kept root that only compiles with a dropped one
cancels the trim: the base stays whole and the measured fit decides. This
measured pass can likewise evict entire initial families or individual children
(accounting for saved build cost). On delta eviction each owner moves atomically
to its cheapest compatible open fallback or NONE. This is a greedy work
estimate, not an optimum for build wall time.
Corrected bases are frozen; if mandatory bases alone exceed the cap, planning
fails. Temporary probe storage can exceed the final cap. Scheduling of the
retained plan uses probe artifact bytes; `--apply` rechecks the MEASURE bytes.
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

1. Compile each selected PCH chain with its exact compiler class and the
   `pch_only_cflags` of `build/gn/pch/global/pch_flags.gni` that change the
   artifact (`-fpch-instantiate-templates`): the plan measures the sizes, build
   times and consumer times of the PCH GN deploys. The PCH
   header includes the roots in layer order (sysroot/libc++, contrib,
   util/library, project sources, generated), not selector mass order. A
   chain that fails to compile is bisected by its roots — the smallest
   breaking prefix of that order is dropped and the remainder retried — so
   one incompatible header no longer voids a whole delta or, with it, the
   base family and every sibling delta. Frozen corrected bases are never
   reduced; their failure aborts correction.
2. Check every C++ member of each assigned target with `-fsyntax-only` and its PCH.
3. If any member fails, move the **whole target** to the parent PCH and recheck
   every member, then try all members without PCH. Only a universally successful
   fallback is accepted. A no-PCH failure or compiler crash aborts planning.
4. Remove unused chains and recompute the prediction from the checked layout.
   A frozen base is kept as it is as long as an assigned target or a surviving
   delta needs it, and failure to compile one aborts correction; one that the
   compatibility fallbacks or the measured disk fit leave without either goes
   with the unused chains.

This catches cases where a PCH compiles alone but introduces declarations that
make a consumer ambiguous (for example `llvm::Module` in `Minidump.cpp`).
`compatibility.tsv` records target-wide fallback decisions and diagnostic log paths; its
group IDs refer to the candidate before unused groups are compacted.

Successful probes and PCH artifacts are cached under
`BASELINE/.pch_update/cache/compatibility-v2`; `--work-dir DIR` changes the cache
location for all three planning commands. Before preflight builds new PCHs it
removes older-format compatibility directories under the cache lock, including
the mixed `compatibility-v1` cache. Each entry is scoped to its compiler,
flags, include order, working paths and relevant environment. Reuse validates
the recorded depfile inputs and any explicitly consumed PCH artifact. A change
to an unrelated translation unit does not invalidate these probes. Plan and
baseline validation still cover the full analysis. Missing artifacts and corrupt records are
recomputed. Results are written atomically after successful completion, so a
later failure can be retried with `replan` into a new output directory. Failed
and interrupted probes are not cached as successes.

Preflight builds at most a quarter of `--workers` PCHs at a time and checks the
consumers on the full count. The cache occupies additional
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
cache and its current versioned compatibility subdirectory need this marker. Old paths then
contain only links, while all artifacts live in the baseline. Keep those aliases
while the migrated entries are in use. Newly created caches need no aliases.

The measurement steps run `build/measured.ninja`, using direct local
compiler commands and explicit per-action observations. Ninja alone enforces the
limits, as in any build of the project: the worker count, GN's named pools and a
pool of a quarter of the workers for the PCH builds. `--disk` limits resident
PCH artifacts, rather than total build-directory size.
Each measurement attempt has its own identity. A retry validates earlier
observations from the same frozen plan; calibration is allowed only after every
required measurement has completed successfully and passed output validation.

After `--predict`, run `pch_update --baseline B --correct --sample 20` to
measure 20% of the modeled C++ objects (rounded up). The first correction
freezes the percentage and the object panel; omit `--sample` on resume.
Prediction does not emit the measurement graph. Give each compiling GN target
one slot, the calibration controls first (they inform their whole compiler class,
not only their own anchor). If not all fit, take the remaining targets in descending
order of summed baseline compile seconds. A control without PCH then takes one extra
member (the class baseline is read from it); the remaining slots go to the largest
baseline compile seconds per slot, a control kept on the base taking its turn in
that queue like any other owner (`work / (slots + 1)` first, up to all files; ties
by GN label): a target's stratum
estimate varies with the work it holds, not with the number of its files, so fifty
generated stubs of 0.2 s no longer draw as many objects as fifty core files of 25 s.
Within each target, sort objects by baseline compile seconds, split them into
equal-count strata (within one object), and select one per stratum with
probability proportional to its baseline compile seconds. The draw is keyed
by the initial plan nonce and object output. Corrected measurement reads the
exact panel back from the initial snapshot, regardless of PCH reassignment.
`sampling.tsv` marks these compiles as `target-stratum`. Every PCH is built.
Sampling changes prerequisites, not the selected compiler
commands or PCH assignments. For sampled compilation, GN's target-wide order-only
dependencies are replaced by generated inputs recorded in the frozen baseline's
`.ninja_deps`, supplemented by inputs from successful compatibility probes with
the assigned PCH. A missing compiler dependency record is an error. Explicit and
implicit inputs, validations, and PCH dependencies remain required.

Generators still compile and link their tools and libraries as needed; only
unnecessary target-wide ordering is removed from compilation. Therefore 20%
does not promise a fivefold speedup: all PCHs and their required tools still
have to be built. `sampling.tsv` and the `measurement sample` log line report
selected objects, their targets and the actual prerequisite closure, including links.
At 100%, the full GN ordering is preserved.

Calibration uses the selected observations, keeping mandatory extra tool builds
out of the sample, and keeps what the measurement showed at every level:

* **Per target.** The sampled objects of a target are a draw with probability
  proportional to baseline time from equal-count strata, so each stratum's trace
  time times the winner's observed-to-trace ratio estimates the stratum without
  bias (Horvitz–Thompson); unsampled members take their stratum's ratio. Every
  measured target, controls included, is *anchored* at that estimate: the second
  selection prices another leaf for it by the headers the leaf adds or removes
  (`saved`, `load`, the class coefficients), never from its trace, so a stale
  `distclang` estimate or another machine's load cancels. A no-PCH control is
  anchored at its measured compile without PCH.
* **Per class.** The controls give the baseline ratio and, where their trace times
  spread enough to tell it apart, the per-compile seconds outside the traces. A
  class without a measured control (fewer than four owners, or none in the sample)
  keeps the prior baseline ratio of 1.
  `saved` and `load` are a weighted least-squares fit on the measured targets with
  a PCH over the data centered by both (no count column to collide with a load
  shared by every group); each target's equation is normalized by the root of its
  members' squared trace times, and Huber reweighting keeps a few exceptional
  targets from dragging the class. A third regressor, the parse saving times
  `(Frontend - Outer) / Outer` of the member's trace, captures what
  `-fpch-instantiate-templates` removes outside the includes (`instantiate`); it
  is accepted only when identified next to the others. Leave-one-group-out error is
  a diagnostic only: the working coefficients are fitted on every measured group.
* **Per unit.** Every base and delta S0 built is one observation over its own
  units. A weak ridge decomposition assigns each unit its PCH bytes per source
  byte and its build seconds per parse second (a lone group's units move by the
  same share; units shared by groups split the evidence), so `Pick` sees which
  headers inflated a delta and the second selection fits `--disk` before
  compiling. The strata are cut exactly as the draw cut them. An artifact with
  exactly the units, roots and parent chain S0 built reuses its measured size,
  build time and RSS outright, and the consumer memory scale is read from the
  measured artifacts. A leaf that is unusually heavy to deserialize shows in its
  consumers' residuals below; the load feature itself stays in parse seconds.
* **Per target again (empirical Bayes).** Each measured target is compared with
  the compile time the selector computes for it, backend floor included, relative
  to the parse seconds the PCH saved it. The mean
  deviation of a leaf over its measured owners, shrunk by their noise, is
  projected on the leaf's own units; the remainder of each target is shrunk by
  the controls' spread and the target's sampling noise into a saved-time
  multiplier (`target_gain`, never below zero). A target measured no faster with
  its PCH than the controls are without one, beyond that noise, keeps a
  multiplier of zero: the PCH saved it nothing, so it loses its PCH at the
  payback test, and its no-PCH time is what S0 measured less the load. A PCH is
  never credited with slowing a compile beyond that load. One measured far
  faster than its own parse time predicts keeps a multiplier above one and, with
  it, its delta. The same holds per unit for the leaves: one whose owners S0 saw
  far slower gets a unit multiplier of zero and saves nothing for the owners S0
  never measured. Only a negative multiplier means "not measured".

Compilations whose trace shows the frontend are floored at the backend
(`ExecuteCompiler - Frontend`), which no PCH removes; others keep a 2% prior.
Unmeasured actions keep the model estimate; `correct` reports coverage in
`calibration.tsv` (`# measured_actions`, `# measured_targets`, `anchored_targets`).
With a sampled run `cold_seconds` is a prediction rather than a measured full
build; `summary.tsv` says so.

`correct` checks source, tool, configuration, action and output identity before
using any measurements. It compares the frozen initial ledger with observations,
reports unidentifiable parameters and cross-validated error, and
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
from the root `group("all")`. Build and run tools in this worktree's ordinary
`out/gn` directory. From the source root:

```sh
ninja -C out/gn build/gn/pch_update:pch_update
./out/gn/pch_update --baseline /path/to/no-pch-build --disk 16G
```

The executable is `out/gn/pch_update`, alongside its shared libraries. Use this
copy for commands, self-tests and integration tests. The no-PCH baseline is a
separate input and is not modified by the tool build.

## Artifacts

- `snapshot.bin`: checksummed analysis, configuration, initial predictions and
  action identities; never rewritten by verification or correction.
- `selection.bin`: completed layout keyed by selector inputs, reusable even
  when compatibility checking or build emission has not finished yet; also
  retains disk-budget diagnostics so a retry can restore a missing report.
- `disk-budget.tsv`: pending until preflight, then the measured candidate,
  base, budget and retained bytes and the actual eviction order. Each
  row reports the eviction kind (`delta` or `family`), pre-compaction group ID,
  freed bytes, estimated marginal work loss,
  and counts of targets reassigned to their best open fallback
  (base or a surviving sibling delta) or NONE.
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

`pch_update self-test` checks scheduling, invalidation, frozen-base refinement and
release of emptied families, bare controls of small and lopsided classes,
parasites and crowded clusters, disk prices by bytes across classes,
calibration rank, cross-validation, anchors and
empirical-Bayes multipliers (a harmful leaf at zero), per-unit decomposition, sampling by work with control
priority, cycle repair to the parent, base material (generated output never) and
roots without parse time, the C++ runtime symbols a header may need alone,
streamed tree identity, snapshot corruption, measurement restart,
missing telemetry and interrupted replacement recovery.

Shared bases require headers that alone need nothing beyond the C++ runtime.
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
