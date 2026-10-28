#include <gtest/gtest.h>

#include <kgraph/random.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

using kgraph::random::Distribution;
using kgraph::random::Parameters;

namespace
{
    constexpr int draws = 200000;

    std::vector<double> Drawn(const Distribution distribution, const Parameters& with, const bool whole = false, const std::uint64_t seed = 7)
    {
        std::mt19937_64 engine(seed);
        std::vector<double> numbers(draws);
        for (double& n : numbers) n = kgraph::random::Draw(distribution, with, whole, engine);
        return numbers;
    }

    /** The draws' mean is the distribution's, within five standard errors; their variance within @p spread of its. */
    void ExpectMoments(const std::vector<double>& numbers, const double mean, const double variance, const double spread = 0.03)
    {
        double sum = 0.0;
        for (const double n : numbers) sum += n;
        const double measured = sum / static_cast<double>(numbers.size());
        double squares = 0.0;
        for (const double n : numbers) squares += (n - measured) * (n - measured);
        const double measuredVariance = squares / static_cast<double>(numbers.size() - 1);
        EXPECT_NEAR(measured, mean, 5.0 * std::sqrt(variance / static_cast<double>(numbers.size()))) << "the mean";
        EXPECT_NEAR(measuredVariance, variance, spread * variance) << "the variance";
    }
}

// Each distribution's numbers have the mean and the variance it is known to have.
TEST(Random, EachDistributionHasItsMeanAndVariance) {
    using enum Distribution;
    ExpectMoments(Drawn(eUniform, { .min = 2, .max = 5 }), 3.5, 0.75);
    ExpectMoments(Drawn(eNormal, { .mean = 1, .stddev = 2 }), 1.0, 4.0);
    ExpectMoments(Drawn(eLognormal, { .mean = 0, .stddev = 0.5 }), std::exp(0.125), (std::exp(0.25) - 1.0) * std::exp(0.25), 0.05);
    ExpectMoments(Drawn(eExponential, { .lambda = 2 }), 0.5, 0.25);
    ExpectMoments(Drawn(eGamma, { .shape = 3, .scale = 2 }), 6.0, 12.0);
    ExpectMoments(Drawn(eGamma, { .shape = 0.5, .scale = 2 }), 1.0, 2.0, 0.05);       // below a shape of 1, the other way
    ExpectMoments(Drawn(eWeibull, { .shape = 2, .scale = 1 }), std::sqrt(std::numbers::pi) / 2.0, 1.0 - std::numbers::pi / 4.0);
    ExpectMoments(Drawn(eExtremeValue, { .scale = 1, .location = 0 }), std::numbers::egamma, std::numbers::pi * std::numbers::pi / 6.0);
    ExpectMoments(Drawn(eChiSquared, { .degrees = 3 }), 3.0, 6.0);
    ExpectMoments(Drawn(eStudentT, { .degrees = 5 }), 0.0, 5.0 / 3.0, 0.1);
    ExpectMoments(Drawn(eFisherF, { .degrees = 5, .denominatorDegrees = 10 }), 1.25, 2600.0 / 1920.0, 0.15);
    ExpectMoments(Drawn(eBernoulli, { .p = 0.3 }), 0.3, 0.21);
    ExpectMoments(Drawn(eBinomial, { .p = 0.3, .trials = 10 }), 3.0, 2.1);
    ExpectMoments(Drawn(eBinomial, { .p = 0.4, .trials = 1000 }), 400.0, 240.0);      // halved by beta numbers first
    ExpectMoments(Drawn(eNegativeBinomial, { .p = 0.4, .successes = 3 }), 4.5, 11.25);
    ExpectMoments(Drawn(eGeometric, { .p = 0.25 }), 3.0, 12.0);
    ExpectMoments(Drawn(ePoisson, { .lambda = 4 }), 4.0, 4.0);
    ExpectMoments(Drawn(ePoisson, { .lambda = 100 }), 100.0, 100.0);                   // by gamma numbers first
}

// A Cauchy number has no mean: half of them are within a scale of where it is.
TEST(Random, HalfOfACauchysNumbersAreWithinAScale) {
    const auto numbers = Drawn(Distribution::eCauchy, { .scale = 2, .location = 10 });
    const auto within = std::ranges::count_if(numbers, [](const double n) { return std::abs(n - 10.0) <= 2.0; });
    EXPECT_NEAR(static_cast<double>(within) / draws, 0.5, 0.005);
}

// A whole uniform number is each of those from min to max, both included, as often as any other; a real one is never max.
TEST(Random, AWholeUniformNumberIsEachFromMinToMaxAsOften) {
    const auto dice = Drawn(Distribution::eUniform, { .min = 1, .max = 6 }, true);
    for (int face = 1; face <= 6; ++face)
        EXPECT_NEAR(static_cast<double>(std::ranges::count(dice, face)) / draws, 1.0 / 6.0, 0.005) << face;
    EXPECT_TRUE(std::ranges::all_of(dice, [](const double n) { return n >= 1.0 && n <= 6.0; }));
    ExpectMoments(dice, 3.5, 35.0 / 12.0);
    const auto between = Drawn(Distribution::eUniform, { .min = -0.5, .max = 2.5 }, true);
    EXPECT_TRUE(std::ranges::all_of(between, [](const double n) { return n >= 0.0 && n <= 2.0; })) << "the whole numbers between them";
    const auto real = Drawn(Distribution::eUniform, { .min = 0, .max = 1 });
    EXPECT_TRUE(std::ranges::all_of(real, [](const double n) { return n >= 0.0 && n < 1.0; }));
}

// The same seed draws the same numbers, and these: the generator and every algorithm are the engine's own, so a graph
// saved with a seed gives the same numbers with any standard library. A change here changes every saved graph.
TEST(Random, TheSameSeedDrawsTheSameNumbersEverywhere) {
    using enum Distribution;
    EXPECT_EQ(Drawn(eNormal, {}, false, 42), Drawn(eNormal, {}, false, 42));
    EXPECT_NE(Drawn(eNormal, {}, false, 42), Drawn(eNormal, {}, false, 43));
    const auto first = [](const Distribution distribution, const Parameters& with, const bool whole = false) {
        std::mt19937_64 engine(42);
        return kgraph::random::Draw(distribution, with, whole, engine);
    };
    EXPECT_DOUBLE_EQ(first(eUniform, {}), 0.75515553295453897);
    EXPECT_DOUBLE_EQ(first(eUniform, { .min = 1, .max = 6 }, true), 1.0);
    EXPECT_DOUBLE_EQ(first(eNormal, {}), 1.2938204232729367);
    EXPECT_DOUBLE_EQ(first(eGamma, { .shape = 2.5 }), 4.6836048874623302);
    EXPECT_DOUBLE_EQ(first(ePoisson, { .lambda = 50 }), 42.0);
    EXPECT_DOUBLE_EQ(first(eBinomial, { .p = 0.3, .trials = 100 }), 30.0);
}

// What a distribution cannot be drawn with is said, by the input that is wrong.
TEST(Random, WhatCannotBeDrawnIsSaid) {
    using enum Distribution;
    using kgraph::random::Check;
    EXPECT_FALSE(Check(eUniform, {}, false));
    EXPECT_NE(Check(eUniform, { .min = 2, .max = 1 }, false).value_or(""), "");
    EXPECT_NE(Check(eUniform, { .min = 0.2, .max = 0.8 }, true).value_or("").find("no whole number"), std::string::npos);
    EXPECT_FALSE(Check(eUniform, { .min = 0.2, .max = 0.8 }, false));
    EXPECT_NE(Check(eNormal, { .stddev = -1 }, false).value_or("").find("stddev"), std::string::npos);
    EXPECT_NE(Check(eExponential, { .lambda = 0 }, false).value_or("").find("lambda"), std::string::npos);
    EXPECT_FALSE(Check(ePoisson, { .lambda = 0 }, false));
    EXPECT_NE(Check(eBernoulli, { .p = 1.5 }, false).value_or("").find("'p'"), std::string::npos);
    EXPECT_NE(Check(eGeometric, { .p = 0 }, false).value_or("").find("'p'"), std::string::npos);
    EXPECT_NE(Check(eBinomial, { .trials = 2.5 }, false).value_or("").find("trials"), std::string::npos);
    EXPECT_NE(Check(eGamma, { .shape = 0 }, false).value_or("").find("shape"), std::string::npos);
    EXPECT_NE(Check(eNormal, { .mean = std::nan("") }, false).value_or("").find("mean"), std::string::npos);
    EXPECT_EQ(kgraph::random::DistributionNamed("ePoisson"), ePoisson);
    EXPECT_FALSE(kgraph::random::DistributionNamed("eDice"));
}
