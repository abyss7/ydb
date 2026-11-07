# Two-step PCH selection

`pch_update` selects compatible base/delta PCH chains, measures one build, and
reselects the deltas and their consumers once. The base contents and compiler
classes remain fixed during correction. This is a bounded two-step workflow,
not a convergence guarantee.

```sh
pch_update --baseline /path/to/no-pch-build --disk 16G

# Independently saved optimization profiles:
pch_update --baseline /path/to/no-pch-build --disk 16G --mode=global
pch_update --baseline /path/to/no-pch-build --disk 16G --mode=incremental

# Optional model/resource configuration:
pch_update --baseline /path/to/no-pch-build \
    --workers 32 --ram-gb 96 --disk 16G --q 0.1
```

The normal command runs the complete analysis → compatibility → prediction →
measured verification → correction cycle. It stops on the first failure and
prints the cycle directory and final result path. No manually numbered plan
directories are needed. `run` is an optional explicit command name.

| Option | Default |
| --- | --- |
| `--workers` | Available CPU count |
| `--ram-gb` | 75% of available RAM at startup, capped by the exposed cgroup-v2 budget |
| `--disk` | **required**: resident PCH budget, e.g. `16G`, `4.5G`, `800M`, `400K`; a bare number means gigabytes |
| `--mode` | `incremental`; `global` prioritizes cold builds, `incremental` prioritizes historical rebuilds |
| `--q` | Cold-build weight: `1` for `global`, `0.1` for `incremental`; an explicit value overrides the profile default |
| `--bandwidth-gbps` | 1 GB/s |
| `--action-memory-gb` | 1 GB unmeasured-action RSS prior |
| `--analysis-jobs` | min(24, CPU count) |
| `--state-age` | `7d`: expire the selected profile if no successful cycle renewed it for this long (`12h`, `2w`, bare = days, `0` = never) |
| `--sample` | `100`: share of compile actions that `verify` measures (`10`..`100`); all PCHs and required generator tools are built |
| `--target` | `all` and `tests`; repeat to override |

`--mode=global` and `--mode global` are equivalent. Explicit `--q` takes precedence
regardless of argument order. The profile name chooses the optimization objective
and saved variant; it does not change GN's `pch_mode`. Both profiles select
base/delta PCH chains. Current output is a frozen Ninja plan; exporting these
chains to GN `pch_mode="global"` is a separate, unfinished integration.

All default state is owned by the baseline. Analysis and caches are shared,
while working cycles and saved results are profile-local:

```text
BASELINE/.pch_update/
  cache/                  # header/history/trace jobs and compatibility-v1/
  analysis.path           # latest reusable checkpoint from either profile
  generation              # shared cache identity: schema, source tree, baseline
  modes/
    global/
      runs/cycle-*/initial/
      runs/cycle-*/corrected/
      analysis.path       # this profile's latest checkpoint
      current.path        # this profile's current cycle
      result.path         # this profile's latest successful corrected result
      generation          # profile identity and last successful cycle time
    incremental/          # the same layout, maintained independently
```

Generation identity includes the analysis schema (`pch-analysis-*`), source tree
(`git rev-parse HEAD` plus `git diff`) and baseline configuration (`args.gn`,
`build.ninja`, `build.ninja.d`, `compile_commands.json`). A change invalidates the
shared analysis pointer and compatibility cache. Content-addressed `cache/j`
entries are retained and aged by `jobs.bin`. Changing only `--mode` or `--q`
reuses checked analysis and compatibility work.

Each profile checks its own generation and `--state-age` on a full cycle. Expiry
or cleanup touches that profile's runs and pointers; the other profile's result,
measurements and build remain intact. Age expiry alone does not clear shared
caches. A common cycle lock serializes full cycles using the shared state.

Within each profile the state keeps one working cycle: as soon as a new plan and
its analysis checkpoint exist, older cycles in its `runs/` lose their build products
(`build/`, `actions/`, `snapshot.bin`, `analysis.bin`) but keep their
measurements and reports (`observations/`, `initial-errors.tsv`, `predictions.tsv`,
`summary.tsv`, `calibration.tsv`), so the last result stays comparable for a few
megabytes. Cycles older than the last one are deleted. A profile generation change
does the same within that profile: the last cycle keeps its measurements.
Only a plan given by `--output` keeps the checkpoint it may reuse.

Legacy `BASELINE/.pch_update/runs/` and root-level result pointers are left in
place: moving them would break frozen absolute paths. Old analysis checkpoints
and compatibility records remain readable; snapshots without a profile field
default to `incremental` while retaining their recorded `q`. New full cycles use
the profile directories above, including when `--mode` is omitted.

Repeating an unchanged invocation within a profile resumes an unstarted verification or reuses a
completed successful verification/result, after identity checks. Resource
defaults are frozen in that plan when resuming. Failed/interrupted measured
builds require a fresh cycle; valid analysis and compatibility results are still
reused automatically. Changing the tool invalidates frozen measured plans but
allows checked analysis reuse. Explicit extraction options (`--target`,
`--main-ref`, `--root`, `--gn`, `--ninja`) conservatively rerun analysis using the
lower-level caches. `--output` overrides the cycle directory; `--work-dir`
overrides the cache location.

The baseline must be complete and current, with `pch_mode = "none"` and
`time_trace = true`. New output directories must not exist and must be outside
the source tree; inside the baseline only `.pch_update/` descendants are allowed.
Use `--target NAME` repeatedly to change the default
`all tests` workload. `--q` is the cold-build fraction; with no history, use
`--q 1`. Run `pch_update --help` for the remaining options.

`predict` reuses cached trace, history and standalone-header analysis. It runs
GN and analysis compiler probes, then emits a separate full Ninja graph with
per-file PCH assignments, real generators and linking. The baseline and the
source-tree PCH configuration are preserved; tool state is stored in the
baseline's reserved `.pch_update/` subtree.

For diagnosis or explicit stage control, the low-level commands remain:

```sh
pch_update predict --baseline /path/to/no-pch-build --output /path/to/initial-plan --disk 16G
pch_update verify --plan /path/to/initial-plan
pch_update correct --plan /path/to/initial-plan --output /path/to/corrected-plan
```

After analysis, `analysis.bin` is saved **before** selection and build emission.
If selection or emission needs a tool fix, reuse this boundary in a new directory:

```sh
pch_update replan --plan /path/to/initial-plan --output /path/to/replanned --disk 16G
```

This skips trace parsing, history extraction and standalone-header analysis. It retains
the original model inputs and resource settings, but runs the current selector
and emitter. `--disk` supplies the new PCH budget. `replan` inherits the saved
profile and `q` unless overridden: `--mode=global` selects the global profile
and its default weight; adding `--q 0.5` overrides that weight. `verify` and
`correct` always use the frozen profile and objective.
Source/configuration fingerprints, Git history and baseline logs
are checked; object/trace sizes and modification times are checked without
rereading their contents. Selector/tool edits are allowed unless those files
are themselves measured build inputs. Extraction changes require incrementing
the analysis schema and a fresh `predict`. Complete older v2 plans can also be
replanned, with a stricter unchanged-tree check. This starts a new cycle with
new predictions; it does not revise a measured cycle or reuse its observations.

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
2. Check every assigned translation unit with `-fsyntax-only` and its PCH.
3. On a compiler diagnostic, try the parent PCH, then a no-PCH compile. Only a
   successful fallback is accepted. A no-PCH failure or compiler crash aborts
   planning instead of silently discarding assignments.
4. Remove unused chains and recompute the prediction from the checked layout.
   Correction preserves every frozen base; failure to compile one aborts it.

This catches cases where a PCH compiles alone but introduces declarations that
make a consumer ambiguous (for example `llvm::Module` in `Minidump.cpp`).
`compatibility.tsv` records fallback decisions and diagnostic log paths; its
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

`verify` runs the generated `build/measured.ninja` once, using direct local
compiler commands and explicit per-action observations. Worker/pool limits are
enforced by Ninja; the runner reserves predicted RAM and PCH I/O bandwidth
across processes. `--disk` limits resident PCH artifacts, rather than total
build-directory size. Memory reservations are estimates, not OS memory limits.
Each fresh plan permits one verification invocation; failed or interrupted
runs cannot provide calibration data. Use a new plan directory to retry.

`--sample PERCENT` (10..100, default 100) makes verification build only a share
of the compilations. The selection is deterministic per plan, so a plan always
measures the same actions: every PCH build plus a pseudo-random share of the
compile actions. Sampling changes prerequisites, not the selected compiler
commands or PCH assignments. For sampled compilation, GN's target-wide order-only
dependencies are replaced by generated inputs recorded in the frozen baseline's
`.ninja_deps`, supplemented by inputs from successful compatibility probes with
the assigned PCH. A missing compiler dependency record is an error. Explicit and
implicit inputs, validations, and PCH dependencies remain required.

Generators still compile and link their tools and libraries as needed; only
unnecessary target-wide ordering is removed from compilation. Therefore 20%
does not promise a fivefold speedup: all PCHs and their required tools still
have to be built. `sampling.tsv` and the `measurement sample` log line report
both selected compilations and the actual prerequisite closure, including links.
At 100%, the full GN ordering is preserved.

Calibration uses the selected observations, keeping mandatory extra tool builds
out of the random sample. Unmeasured actions keep the model estimate; `correct`
reports calibration coverage in `calibration.tsv` (`# measured_actions`).
With a sampled run `cold_seconds` is a prediction rather than a measured full
build; `summary.tsv` says so.

`correct` checks source, tool, configuration, action and output identity before
using any measurements. It compares the frozen initial ledger with observations,
holds out entire groups during fitting, reports unidentifiable parameters, and
emits a new independent build directory. The corrected configuration remains
a prediction. It can optionally be measured with `verify --plan` too, but cannot
be corrected again within this cycle.

## Building

The binary is an ordinary GN target, `//build/gn/pch_update:pch_update`. It is
deliberately absent from the root `group("all")`, so a full build does not need
it. Because GN only loads referenced `BUILD.gn` files, a build directory must be
generated with the tool reachable from a root target:

```sh
# 1. add a temporary root group (NOT inside group("all"))
printf '\ngroup("pch_tools") {\n    deps = [ "//build/gn/pch_update" ]\n}\n' >> BUILD.gn

# 2. generate and build in a dedicated directory
gn gen ~/b/gn-tools --script-executable=python3.9 \
    --args='build_type="relwithdebinfo" distclang=true clang_bin_path="'"$HOME"'/.ya/tools/v4/13899442575/bin" pch_mode="none"'
ninja -C ~/b/gn-tools build/gn/pch_update:pch_update

# 3. restore the tracked tree, keeping the previous mtime
cp -p BUILD.gn.pristine BUILD.gn   # or: git checkout -- BUILD.gn && touch -r <old> BUILD.gn
```

The executable is written to the build directory root: `~/b/gn-tools/pch_update`.
Restoring `BUILD.gn` keeps the source-tree identity used by analysis checkpoints
intact. Keep the temporary group only while generating and building; a later
`gn gen` without it removes the target from that graph, but the binary remains.

Restore `BUILD.gn` **with its old modification time** (`cp -p`, `touch -r`). A
plain copy makes it newer than the baseline's generated `build.ninja`, and
`pch_update` then refuses the baseline as stale ("gn files changed since the
build"). The content is what matters: the same bytes need no regeneration, and
`ninja -C BASELINE -n build.ninja` should report "no work to do".
`build/gn/pch_update/BUILD.gn` is not an input of the baseline graph at all
(the target is not referenced there), so its timestamp never matters.

## Artifacts

- `snapshot.bin`: checksummed analysis, configuration, initial predictions and
  action identities; never rewritten by verification or correction.
- `summary.tsv`, `predictions.tsv`, `assignments.tsv`: objective, per-action
  estimates and per-file leaf assignments.
- `build/pch/*.h`: selected root headers; the Ninja manifest records the chains.
- `observations/*.bin`, `run.bin`: action timing, child CPU/RSS, output sizes,
  command identity and verification invocation.
- `initial-errors.tsv`, `calibration.tsv` in the corrected plan: original
  prediction errors, measured wall time, fit coverage and held-out errors.

The model uses exclusive header costs and full cold/historical DAG scheduling.
Order-only edges affect scheduling but not dirty propagation. Validation edges
are additional roots. Changed restat outputs are conservatively assumed to
propagate. Header selection uses a bounded local shortlist, followed by full
graph scoring; it is heuristic. Initial size, RSS and loading costs are explicit
priors. Exact compiler flags, macros and include order define compatibility
classes, so some classes may have too few groups to identify separate costs.

`pch_update self-test` checks scheduling, invalidation, frozen-base refinement,
calibration rank/holdout isolation, snapshot corruption and measurement records.
