# MOG compatibility matrix

Reference-toolchain snapshot; regenerate with tools/probe after toolchain changes.
CI verifies generator determinism and floor-feature availability.

## Toolchain

| Item | Value |
|---|---|
| compiler | GNU 15.2.0 |
| stdlib | libstdc++ |
| c++ standard | C++26 |
| build profile | frontier |
| isa sse4.2 | yes |
| isa avx2 | yes |
| isa avx512f | no |

## Language and library features (configure-time probes)

| Feature | Available |
|---|---|
| `MOG_HAS_EXPECTED` | yes |
| `MOG_HAS_SATURATE_ARITHMETIC` | yes |
| `MOG_HAS_STD_SIMD` | no |
| `MOG_HAS_INPLACE_VECTOR` | no |
| `MOG_HAS_FUNCTION_REF` | no |
| `MOG_HAS_ATOMIC_MIN_MAX` | no |
| `MOG_HAS_HIVE` | no |
| `MOG_HAS_PACK_INDEXING` | yes |
| `MOG_HAS_PLACEHOLDER_VARIABLES` | yes |
| `MOG_HAS_DELETED_WITH_REASON` | yes |
| `MOG_HAS_EMBED` | yes |
| `MOG_HAS_REFLECTION` | no |
| `MOG_HAS_EXPANSION_STATEMENTS` | no |
| `MOG_HAS_NATIVE_CONTRACTS` | no |

## Contract enforcement policy

| Profile | Default mode |
|---|---|
| frontier | enforce |

The macro-based layer (`mog/Contracts.hpp`) is authoritative in all profiles.
No native P2900 support in this toolchain; polyfill macros carry the semantics.
