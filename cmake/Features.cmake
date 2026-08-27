# Configure-time feature probes; emits generated/mog/BuildFeatures.hpp.
# Every frontier feature is probed, never assumed (PLAN.md 4.2).

include(CheckCXXSourceCompiles)

set(MOG_FEATURES_HEADER_IN ${CMAKE_CURRENT_LIST_DIR}/BuildFeatures.hpp.in)

set(CMAKE_REQUIRED_QUIET OFF)
set(CMAKE_REQUIRED_FLAGS "-std=c++${MOG_CXX_STANDARD}")

check_cxx_source_compiles([[
#include <version>
int main() { return __cpp_lib_expected >= 202211L ? 0 : 1; }
]] MOG_PROBE_EXPECTED)
set(MOG_HAS_EXPECTED ${MOG_PROBE_EXPECTED})

check_cxx_source_compiles([[
#include <numeric>
#include <version>
int main() {
  auto x = std::add_sat<long long>(1, 2);
  static_assert(__cpp_lib_saturation_arithmetic >= 202311L);
  return int(x);
}
]] MOG_PROBE_SATURATE)
set(MOG_HAS_SATURATE_ARITHMETIC ${MOG_PROBE_SATURATE})

check_cxx_source_compiles([[
#include <simd>
int main() {
  std::simd<int, 8> a{}, b{};
  auto c = a + b;
  return c[0] == 0 ? 0 : 1;
}
]] MOG_PROBE_SIMD)
set(MOG_HAS_STD_SIMD ${MOG_PROBE_SIMD})

check_cxx_source_compiles([[
#include <inplace_vector>
int main() { std::inplace_vector<int, 4> v; v.push_back(1); return v[0] == 1 ? 0 : 1; }
]] MOG_PROBE_INPLACE_VECTOR)
set(MOG_HAS_INPLACE_VECTOR ${MOG_PROBE_INPLACE_VECTOR})

check_cxx_source_compiles([[
#include <functional>
int main() { std::function_ref<int()> f([] { return 1; }); return f(); }
]] MOG_PROBE_FUNCTION_REF)
set(MOG_HAS_FUNCTION_REF ${MOG_PROBE_FUNCTION_REF})

check_cxx_source_compiles([[
#include <atomic>
int main() { std::atomic<long long> a{1}; a.fetch_max(5); a.fetch_min(2); return a == 2 ? 0 : 1; }
]] MOG_PROBE_ATOMIC_MIN_MAX)
set(MOG_HAS_ATOMIC_MIN_MAX ${MOG_PROBE_ATOMIC_MIN_MAX})

check_cxx_source_compiles([[
#include <hive>
int main() { std::hive<int> h; h.insert(1); return h.size() == 1 ? 0 : 1; }
]] MOG_PROBE_HIVE)
set(MOG_HAS_HIVE ${MOG_PROBE_HIVE})

check_cxx_source_compiles([[
#include <version>
#if !defined(__GLIBCXX__)
#error not libstdc++
#endif
int main() {}
]] MOG_PROBE_LIBSTDCXX)
if(MOG_PROBE_LIBSTDCXX)
  set(MOG_STDLIB_NAME "libstdc++")
else()
  check_cxx_source_compiles([[
#include <version>
#if !defined(_LIBCPP_VERSION)
#error not libc++
#endif
int main() {}
]] MOG_PROBE_LIBCXX)
  if(MOG_PROBE_LIBCXX)
    set(MOG_STDLIB_NAME "libc++")
  else()
    set(MOG_STDLIB_NAME "unknown")
  endif()
endif()

check_cxx_source_compiles([[
template <class... Ts> using First = Ts...[0];
int main() { First<int, double> x = 0; return int(x); }
]] MOG_PROBE_PACK_INDEXING)
set(MOG_HAS_PACK_INDEXING ${MOG_PROBE_PACK_INDEXING})

check_cxx_source_compiles([[
void g(int _) { (void)_; }
int main() {}
]] MOG_PROBE_PLACEHOLDER)
set(MOG_HAS_PLACEHOLDER_VARIABLES ${MOG_PROBE_PLACEHOLDER})

check_cxx_source_compiles([[
struct D { void bad() && = delete("use good()"); };
void good(D) {}
int main() { D d; good(d); }
]] MOG_PROBE_DELETED_REASON)
set(MOG_HAS_DELETED_WITH_REASON ${MOG_PROBE_DELETED_REASON})

file(WRITE "${CMAKE_BINARY_DIR}/embed_probe.bin" "mog")
# Some compilers do not predefine __STDC_EMBED_FOUND_ in C++ mode; probe numerically.
set(MOG_EMBED_SRC "#include <cstdio>
#if __has_embed(\"${CMAKE_BINARY_DIR}/embed_probe.bin\") != 1
#error embed unavailable
#endif
constexpr unsigned char blob[] = {
#embed \"${CMAKE_BINARY_DIR}/embed_probe.bin\"
};
int main() { return blob[0] == 'm' ? 0 : 1; }")
check_cxx_source_compiles("${MOG_EMBED_SRC}" MOG_PROBE_EMBED)
set(MOG_HAS_EMBED ${MOG_PROBE_EMBED})

check_cxx_source_compiles([[
#include <meta>
int main() { constexpr auto info = ^^int; (void)info; }
]] MOG_PROBE_REFLECTION)
set(MOG_HAS_REFLECTION ${MOG_PROBE_REFLECTION})

check_cxx_source_compiles([[
struct S { int a; };
template <class T> constexpr auto member_count() {
  if constexpr (requires { template for (constexpr auto m :
      define_static_array(std::meta::nonstatic_data_members_of(^^T))) { m; }; }) {
    return 1;
  } else {
    return 0;
  }
}
static_assert(member_count<S>() >= 0);
int main() {}
]] MOG_PROBE_EXPANSION)
set(MOG_HAS_EXPANSION_STATEMENTS ${MOG_PROBE_EXPANSION})

check_cxx_source_compiles([[
#include <cstdio>
int f(int x) pre(x > 0) post(r : r > 0) { return x + 1; }
int main() { return f(1) == 2 ? 0 : 1; }
]] MOG_PROBE_CONTRACTS_PLAIN)
if(NOT MOG_PROBE_CONTRACTS_PLAIN)
  set(CMAKE_REQUIRED_FLAGS "-std=c++${MOG_CXX_STANDARD} -fcontracts")
  check_cxx_source_compiles([[
#include <cstdio>
int f(int x) pre(x > 0) post(r : r > 0) { return x + 1; }
int main() { return f(1) == 2 ? 0 : 1; }
]] MOG_PROBE_CONTRACTS_FLAG)
  set(CMAKE_REQUIRED_FLAGS "-std=c++${MOG_CXX_STANDARD}")
  if(MOG_PROBE_CONTRACTS_FLAG)
    set(MOG_NATIVE_CONTRACTS_FLAGS "-fcontracts")
  endif()
else()
  set(MOG_PROBE_CONTRACTS_FLAG FALSE)
endif()
set(MOG_HAS_NATIVE_CONTRACTS ${MOG_PROBE_CONTRACTS_FLAG})

set(CMAKE_REQUIRED_FLAGS "")

configure_file(${MOG_FEATURES_HEADER_IN}
  ${CMAKE_BINARY_DIR}/generated/mog/BuildFeatures.hpp @ONLY)
