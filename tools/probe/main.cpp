// Emits docs/COMPATIBILITY.md; output is byte-deterministic for a given machine+toolchain.

#include <mog/Build.hpp>
#include <mog/Contracts.hpp>

#include <cstdio>

namespace {

void row(const char* name, bool ok) noexcept {
    std::printf("| `%s` | %s |\n", name, ok ? "yes" : "no");
}

} // namespace

int main() {
    std::printf("# MOG compatibility matrix\n\n");
    std::printf("Reference-toolchain snapshot; regenerate with tools/probe after toolchain "
                "changes.\n");
    std::printf("CI verifies generator determinism and floor-feature availability.\n\n");
    std::printf("## Toolchain\n\n");
    std::printf("| Item | Value |\n|---|---|\n");
    std::printf("| compiler | %s %s |\n", ::mog::kCompilerId, ::mog::kCompilerVersion);
    std::printf("| stdlib | %s |\n", ::mog::kStdlibName);
    std::printf("| c++ standard | C++%d |\n", ::mog::kCxxStandard);
    std::printf("| build profile | %s |\n", ::mog::kBuildProfile);
#ifdef __x86_64__
    __builtin_cpu_init();
    std::printf("| isa sse4.2 | %s |\n", __builtin_cpu_supports("sse4.2") ? "yes" : "no");
    std::printf("| isa avx2 | %s |\n", __builtin_cpu_supports("avx2") ? "yes" : "no");
    std::printf("| isa avx512f | %s |\n", __builtin_cpu_supports("avx512f") ? "yes" : "no");
#endif

    std::printf("\n## Language and library features (configure-time probes)\n\n");
    std::printf("| Feature | Available |\n|---|---|\n");
    row("MOG_HAS_EXPECTED", MOG_HAS_EXPECTED);
    row("MOG_HAS_SATURATE_ARITHMETIC", MOG_HAS_SATURATE_ARITHMETIC);
    row("MOG_HAS_STD_SIMD", MOG_HAS_STD_SIMD);
    row("MOG_HAS_INPLACE_VECTOR", MOG_HAS_INPLACE_VECTOR);
    row("MOG_HAS_FUNCTION_REF", MOG_HAS_FUNCTION_REF);
    row("MOG_HAS_ATOMIC_MIN_MAX", MOG_HAS_ATOMIC_MIN_MAX);
    row("MOG_HAS_HIVE", MOG_HAS_HIVE);
    row("MOG_HAS_PACK_INDEXING", MOG_HAS_PACK_INDEXING);
    row("MOG_HAS_PLACEHOLDER_VARIABLES", MOG_HAS_PLACEHOLDER_VARIABLES);
    row("MOG_HAS_DELETED_WITH_REASON", MOG_HAS_DELETED_WITH_REASON);
    row("MOG_HAS_EMBED", MOG_HAS_EMBED);
    row("MOG_HAS_REFLECTION", MOG_HAS_REFLECTION);
    row("MOG_HAS_EXPANSION_STATEMENTS", MOG_HAS_EXPANSION_STATEMENTS);
    row("MOG_HAS_NATIVE_CONTRACTS", MOG_HAS_NATIVE_CONTRACTS);

    std::printf("\n## Contract enforcement policy\n\n");
    std::printf("| Profile | Default mode |\n|---|---|\n");
#if defined(MOG_PROFILE_PORTABLE)
    std::printf("| portable | ignore |\n");
#elif defined(MOG_PROFILE_FRONTIER)
    std::printf("| frontier | enforce |\n");
#else
    std::printf("| lab | enforce |\n");
#endif
    std::printf(
        "\nThe macro-based layer (`mog/Contracts.hpp`) is authoritative in all profiles.\n");
#if MOG_HAS_NATIVE_CONTRACTS
    std::printf("Native P2900 contracts are also compiled in via `-fcontracts`.\n");
#else
    std::printf(
        "No native P2900 support in this toolchain; polyfill macros carry the semantics.\n");
#endif
    return 0;
}
