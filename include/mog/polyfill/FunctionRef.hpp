// Non-owning callable view per P0792; std alias when available.
#pragma once

#include <mog/BuildFeatures.hpp>

#if MOG_HAS_FUNCTION_REF
#include <functional>

namespace mog {
using std::function_ref;
}

#else

#include <concepts>
#include <type_traits>
#include <utility>

namespace mog {

template <class Sig>
class function_ref;

template <class R, class... A>
class function_ref<R(A...)> {
public:
    template <class F>
        requires(!std::same_as<std::remove_cvref_t<F>, function_ref> &&
                 std::is_invocable_r_v<R, F&, A...>)
    function_ref(F&& f) noexcept
        : obj_(static_cast<void*>(std::addressof(f))), call_(&invoke<std::remove_cvref_t<F>>) {}

    function_ref(const function_ref&) noexcept = default;
    function_ref& operator=(const function_ref&) noexcept = default;

    R operator()(A... args) const noexcept { return call_(obj_, static_cast<A&&>(args)...); }

private:
    template <class F>
    static R invoke(void* obj, A... args) noexcept {
        return (*static_cast<F*>(obj))(static_cast<A&&>(args)...);
    }

    void* obj_;
    R (*call_)(void*, A...) noexcept;
};

} // namespace mog

#endif
