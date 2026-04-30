// v4: per-backend circuit breaker.
//
// v3's session-affinity routing improves cache locality when
// backends are healthy. v4 adds a circuit breaker so a failing
// backend doesn't keep getting traffic: after K consecutive errors
// the breaker trips and the backend is excluded from routing for
// `cool_down_ms`. After the timeout, one probe request goes
// through; if it succeeds, the breaker closes; if it fails, the
// timeout extends.
//
// This is the canonical 3-state breaker (Closed / Open / Half-
// Open) pattern from Hystrix / resilience4j, hand-rolled small
// because we don't want a dependency.

#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace ig::circuit {

enum class State : uint8_t { Closed, Open, HalfOpen };

class Breaker {
   public:
    Breaker(uint32_t fail_threshold = 5,
            std::chrono::milliseconds cool_down = std::chrono::milliseconds(30000))
        : fail_threshold_(fail_threshold), cool_down_(cool_down) {}

    // Should the caller send a request? Closed → yes; Open → no
    // (until cool_down expires, then transition to HalfOpen and let
    // exactly one probe through).
    bool allow_request() {
        std::lock_guard g(mu_);
        if (state_ == State::Closed) return true;
        auto now = std::chrono::steady_clock::now();
        if (state_ == State::Open) {
            if (now - opened_at_ < cool_down_) return false;
            state_ = State::HalfOpen;
            half_open_in_flight_ = true;
            return true;
        }
        // HalfOpen: one probe at a time.
        if (!half_open_in_flight_) {
            half_open_in_flight_ = true;
            return true;
        }
        return false;
    }

    void on_success() {
        std::lock_guard g(mu_);
        consecutive_failures_ = 0;
        state_ = State::Closed;
        half_open_in_flight_ = false;
    }

    void on_failure() {
        std::lock_guard g(mu_);
        ++consecutive_failures_;
        half_open_in_flight_ = false;
        if (consecutive_failures_ >= fail_threshold_ || state_ == State::HalfOpen) {
            state_ = State::Open;
            opened_at_ = std::chrono::steady_clock::now();
        }
    }

    State state() const {
        std::lock_guard g(mu_);
        return state_;
    }

    uint32_t consecutive_failures() const {
        std::lock_guard g(mu_);
        return consecutive_failures_;
    }

   private:
    mutable std::mutex mu_;
    State state_{State::Closed};
    uint32_t fail_threshold_;
    std::chrono::milliseconds cool_down_;
    uint32_t consecutive_failures_{0};
    std::chrono::steady_clock::time_point opened_at_;
    bool half_open_in_flight_{false};
};

}  // namespace ig::circuit
