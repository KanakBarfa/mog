# RFC NNNN: <title>

- **Status:** draft | accepted | declined | superseded by RFC-MMMM
- **Created:** YYYY-MM-DD
- **Requires:** (none | RFC-MMMM, issue #N)

## Problem

What is wrong or missing, stated before any solution. Include the scenario
that hurts today - a script, a trace digest divergence, an unrepresentable
experiment. If no concrete scenario exists, this is probably an issue, not
an RFC.

## Proposed change

The design: data structures, API signatures, ownership, and which invariants
are new. Enough detail that an implementer other than the author could do
it without inventing semantics.

## Determinism impact

Does any traced value change? Name the anchors affected (golden hash,
scenario digests) and whether they move. "No change" must be justified by
the design, not asserted.

## Compatibility impact

C++23 fallback path, portable-profile contracts, wheel consumers, probe
matrix changes.

## Performance impact

Expected effect on tracked kernels; gate expectations for the implementing
PR (regressions need counter-level attribution per the benchmark
methodology).

## Alternatives considered

At least one honest alternative and why it loses.

## Implementation plan

Commit sequence if accepted; each step independently testable.
