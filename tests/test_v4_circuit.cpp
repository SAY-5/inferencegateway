// v4 tests: circuit breaker state transitions.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <thread>

#include "ig/circuit.hpp"

using namespace ig::circuit;

#define EXPECT(cond) do {                                              \
    if (!(cond)) {                                                     \
        std::cerr << "FAIL " << __FILE__ << ":" << __LINE__            \
                  << ": " << #cond << "\n";                            \
        std::exit(1);                                                  \
    }                                                                  \
} while (0)

static void testClosedAllowsTraffic() {
    Breaker b(3);
    EXPECT(b.allow_request());
    EXPECT(b.state() == State::Closed);
}

static void testFailuresUnderThresholdKeepClosed() {
    Breaker b(3);
    b.on_failure();
    b.on_failure();
    EXPECT(b.state() == State::Closed);
    EXPECT(b.allow_request());
}

static void testThresholdOpensBreaker() {
    Breaker b(3);
    b.on_failure();
    b.on_failure();
    b.on_failure();
    EXPECT(b.state() == State::Open);
    EXPECT(!b.allow_request());
}

static void testSuccessResetsCounter() {
    Breaker b(3);
    b.on_failure();
    b.on_failure();
    b.on_success();
    EXPECT(b.consecutive_failures() == 0);
    EXPECT(b.state() == State::Closed);
}

static void testHalfOpenAfterCooldown() {
    Breaker b(2, std::chrono::milliseconds(50));
    b.on_failure();
    b.on_failure();
    EXPECT(b.state() == State::Open);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT(b.allow_request());
    EXPECT(b.state() == State::HalfOpen);
    EXPECT(!b.allow_request());
}

static void testHalfOpenFailureReopens() {
    Breaker b(2, std::chrono::milliseconds(50));
    b.on_failure();
    b.on_failure();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    (void)b.allow_request();
    b.on_failure();
    EXPECT(b.state() == State::Open);
}

int main() {
    testClosedAllowsTraffic();
    testFailuresUnderThresholdKeepClosed();
    testThresholdOpensBreaker();
    testSuccessResetsCounter();
    testHalfOpenAfterCooldown();
    testHalfOpenFailureReopens();
    std::printf("circuit: 6/6 passed\n");
    return 0;
}
