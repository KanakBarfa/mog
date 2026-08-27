# Summary

What changes and why. Link the issue/RFC.

## Determinism impact

- [ ] Golden digests unchanged (paste ctest evidence if engine-adjacent)
- [ ] If digests intentionally change: justification and updated anchors attached

## Gates

- [ ] `ctest` green on both portable and frontier profiles
- [ ] `pytest tests/python` green (if tools/ or tests/python touched)
- [ ] `pre-commit run --all-files` green
- [ ] Evidence for behavior claims committed under results/ where applicable
