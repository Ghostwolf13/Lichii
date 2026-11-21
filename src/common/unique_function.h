// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace Common {

/// General purpose function wrapper similar to std::function.
/// Unlike std::function, the captured values don't have to be copyable.
/// This class can be moved but not copied.
/// It owns its callable: one given as an lvalue is copied, never referred to, so the function
/// can outlive the scope it was made in. Pass std::ref to share a callable with the caller.
template <typename ResultType, typename... Args>
class UniqueFunction {
    class CallableBase {
    public:
        virtual ~CallableBase() = default;
        virtual ResultType operator()(Args&&...) = 0;
    };

    template <typename Functor>
    class Callable final : public CallableBase {
    public:
        template <typename Arg>
        explicit Callable(Arg&& functor_) : functor(std::forward<Arg>(functor_)) {}
        ~Callable() override = default;

        ResultType operator()(Args&&... args) override {
            // As the constructor's constraint: any callable std::invoke takes, and a result
            // that is dropped when the wrapper returns nothing.
            if constexpr (std::is_void_v<ResultType>) {
                std::invoke(functor, std::forward<Args>(args)...);
            } else {
                return std::invoke(functor, std::forward<Args>(args)...);
            }
        }

    private:
        Functor functor;
    };

public:
    UniqueFunction() = default;

    template <typename Functor>
        requires(!std::same_as<std::remove_cvref_t<Functor>, UniqueFunction> &&
                 std::constructible_from<std::decay_t<Functor>, Functor> &&
                 std::is_invocable_r_v<ResultType, std::decay_t<Functor>&, Args...>)
    UniqueFunction(Functor&& functor)
        : callable{std::make_unique<Callable<std::decay_t<Functor>>>(
              std::forward<Functor>(functor))} {}

    UniqueFunction& operator=(UniqueFunction&& rhs) noexcept = default;
    UniqueFunction(UniqueFunction&& rhs) noexcept = default;

    UniqueFunction& operator=(const UniqueFunction&) = delete;
    UniqueFunction(const UniqueFunction&) = delete;

    ResultType operator()(Args&&... args) const {
        return (*callable)(std::forward<Args>(args)...);
    }

    explicit operator bool() const noexcept {
        return static_cast<bool>(callable);
    }

private:
    std::unique_ptr<CallableBase> callable;
};

} // namespace Common
