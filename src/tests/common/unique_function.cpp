// SPDX-License-Identifier: GPL-2.0-or-later

#include <functional>
#include <string>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "common/unique_function.h"

namespace {
struct Noisy {
    Noisy() : state{"Default constructed"} {}
    Noisy(Noisy&& rhs) noexcept : state{"Move constructed"} {
        rhs.state = "Moved away";
    }
    Noisy& operator=(Noisy&& rhs) noexcept {
        state = "Move assigned";
        rhs.state = "Moved away";
        return *this;
    }
    Noisy(const Noisy&) : state{"Copied constructed"} {}
    Noisy& operator=(const Noisy&) {
        state = "Copied assigned";
        return *this;
    }

    std::string state;
};

int Add(int a, int b) {
    return a + b;
}
} // Anonymous namespace

TEST_CASE("UniqueFunction", "[common]") {
    SECTION("Capture reference") {
        int value = 0;
        Common::UniqueFunction<void> func = [&value] { value = 5; };
        func();
        REQUIRE(value == 5);
    }
    SECTION("Capture pointer") {
        int value = 0;
        int* pointer = &value;
        Common::UniqueFunction<void> func = [pointer] { *pointer = 5; };
        func();
        REQUIRE(value == 5);
    }
    SECTION("Move object") {
        Noisy noisy;
        REQUIRE(noisy.state == "Default constructed");

        Common::UniqueFunction<void> func = [noisy_inner = std::move(noisy)] {
            REQUIRE(noisy_inner.state == "Move constructed");
        };
        REQUIRE(noisy.state == "Moved away");
        func();
    }
    SECTION("Move construct function") {
        int value = 0;
        Common::UniqueFunction<void> func = [&value] { value = 5; };
        Common::UniqueFunction<void> new_func = std::move(func);
        new_func();
        REQUIRE(value == 5);
    }
    SECTION("Move assign function") {
        int value = 0;
        Common::UniqueFunction<void> func = [&value] { value = 5; };
        Common::UniqueFunction<void> new_func;
        new_func = std::move(func);
        new_func();
        REQUIRE(value == 5);
    }
    SECTION("Default construct then assign function") {
        int value = 0;
        Common::UniqueFunction<void> func;
        func = [&value] { value = 5; };
        func();
        REQUIRE(value == 5);
    }
    SECTION("Pass arguments") {
        int result = 0;
        Common::UniqueFunction<void, int, int> func = [&result](int a, int b) { result = a + b; };
        func(5, 4);
        REQUIRE(result == 9);
    }
    SECTION("Pass arguments and return value") {
        Common::UniqueFunction<int, int, int> func = [](int a, int b) { return a + b; };
        REQUIRE(func(5, 4) == 9);
    }
    SECTION("Destructor") {
        int num_destroyed = 0;
        struct Foo {
            Foo(int* num_) : num{num_} {}
            Foo(Foo&& rhs) : num{std::exchange(rhs.num, nullptr)} {}
            Foo(const Foo&) = delete;

            ~Foo() {
                if (num) {
                    ++*num;
                }
            }

            int* num = nullptr;
        };
        Foo object{&num_destroyed};
        {
            Common::UniqueFunction<void> func = [object_inner = std::move(object)] {};
            REQUIRE(num_destroyed == 0);
        }
        REQUIRE(num_destroyed == 1);
    }
    SECTION("Owns a callable given as a const lvalue") {
        // A worker thread was handed a const lambda by name and ran it after the function that
        // held it had returned: the wrapper kept a reference, and the lambda's captures were gone.
        struct Setter {
            void operator()() const {
                *out = value;
            }
            mutable int value;
            int* out;
        };
        int result = 0;
        const Setter setter{5, &result};
        Common::UniqueFunction<void> func{setter};
        // The wrapper holds its own copy: what happens to the original no longer reaches it.
        setter.value = 6;
        func();
        REQUIRE(result == 5);
    }
    SECTION("Copies a callable given as a non-const lvalue") {
        struct Setter {
            void operator()() const {
                *out = value;
            }
            int value;
            int* out;
        };
        int result = 0;
        Setter setter{5, &result};
        Common::UniqueFunction<void> func{setter};
        setter.value = 6;
        func();
        REQUIRE(result == 5);
        setter();
        REQUIRE(result == 6);
    }
    SECTION("Outlives the const lvalue it was made from") {
        const auto make = [] {
            const auto named = [text = std::string(64, 'x')] { return text; };
            return Common::UniqueFunction<std::string>{named};
        };
        const Common::UniqueFunction<std::string> func = make();
        REQUIRE(func() == std::string(64, 'x'));
    }
    SECTION("Shares a callable given through std::ref") {
        struct Counter {
            void operator()() {
                ++calls;
            }
            int calls = 0;
        };
        Counter counter;
        Common::UniqueFunction<void> func{std::ref(counter)};
        func();
        func();
        REQUIRE(counter.calls == 2);
    }
    SECTION("Drops the result of a callable when it returns nothing") {
        int calls = 0;
        Common::UniqueFunction<void> func{[&calls] { return ++calls; }};
        func();
        REQUIRE(calls == 1);
    }
    SECTION("Calls a member function through its pointer") {
        struct Receiver {
            int Add(int amount) {
                return total += amount;
            }
            int total = 0;
        };
        Receiver receiver;
        Common::UniqueFunction<int, Receiver&, int> func{&Receiver::Add};
        REQUIRE(func(receiver, 4) == 4);
        Common::UniqueFunction<void, Receiver&, int> dropped{&Receiver::Add};
        dropped(receiver, 5);
        REQUIRE(receiver.total == 9);
    }
    SECTION("Is not constructible from what it cannot own or call") {
        struct MoveOnly {
            MoveOnly() = default;
            MoveOnly(MoveOnly&&) = default;
            MoveOnly(const MoveOnly&) = delete;
            void operator()() const {}
        };
        STATIC_REQUIRE(std::is_constructible_v<Common::UniqueFunction<void>, MoveOnly>);
        STATIC_REQUIRE_FALSE(std::is_constructible_v<Common::UniqueFunction<void>, const MoveOnly&>);
        STATIC_REQUIRE_FALSE(std::is_constructible_v<Common::UniqueFunction<void>, int>);
        STATIC_REQUIRE_FALSE(
            std::is_constructible_v<Common::UniqueFunction<void>, const Common::UniqueFunction<void>&>);
    }
    SECTION("Takes a function by name") {
        Common::UniqueFunction<int, int, int> func{Add};
        REQUIRE(func(5, 4) == 9);
    }
}
