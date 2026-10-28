#pragma once

/**
 * @file random.h
 * @brief Random numbers of a distribution, from a seed: what a Random node draws.
 *
 * The same seed draws the same numbers on every machine and with every standard library. The generator is
 * std::mt19937_64, whose output the standard fixes; the distributions are worked out here, not by std::'s, whose
 * algorithms each library chooses for itself. The distributions are those of <random>, by the names a graph gives them.
 */

#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>

namespace kgraph::random
{
    enum class Distribution : std::uint8_t {
        eUniform,           ///< Every number between min and max as likely: a whole one of them, both included, for a whole kind.
        eNormal,            ///< The bell curve: mean, stddev.
        eLognormal,         ///< e to the power of a normal number: mean and stddev are its logarithm's.
        eExponential,       ///< How long until something that happens lambda times a unit happens.
        eGamma,             ///< A sum of shape exponential waits, each scale long.
        eWeibull,           ///< How long until something wears out: shape, scale.
        eExtremeValue,      ///< The largest of many (Gumbel): location, scale.
        eCauchy,            ///< A bell with tails so heavy it has no mean: location, scale.
        eChiSquared,        ///< A sum of degrees squared normal numbers.
        eStudentT,          ///< A normal number divided by how sure degrees samples make it.
        eFisherF,           ///< The ratio of two chi-squared numbers, each over its degrees.
        eBernoulli,         ///< 1 with chance p, else 0.
        eBinomial,          ///< How many of trials tries succeed, each with chance p.
        eNegativeBinomial,  ///< How many tries fail before successes of them succeed, each with chance p.
        eGeometric,         ///< How many tries fail before one succeeds, each with chance p.
        ePoisson,           ///< How many times something that happens lambda times a unit happens in one.
    };

    /** @brief Every distribution's name, as a graph gives it ("eUniform"), in the order of Distribution. */
    [[nodiscard]] std::span<const std::string_view> DistributionNames();
    /** @brief The distribution so named, or none. */
    [[nodiscard]] std::optional<Distribution> DistributionNamed(std::string_view name);
    /** @brief Whether a distribution's numbers are whole: what is counted. */
    [[nodiscard]] bool IsDiscrete(Distribution distribution);

    struct Parameters;

    /** @brief One of what a distribution is drawn with, as its node takes it. */
    struct Input {
        std::string_view name;      ///< As Parameters names it: "stddev".
        double initial;             ///< What it is when not given.
        std::string_view doc;
    };
    /** @brief The node a distribution is drawn by: "RandomUniform", "RandomNormal". */
    [[nodiscard]] std::string_view NodeOf(Distribution distribution);
    /** @brief The distribution a node of @p type draws, or none for a node that draws none. */
    [[nodiscard]] std::optional<Distribution> DistributionOfNode(std::string_view type);
    /** @brief What its node says it draws, and words someone looking for it might type. */
    [[nodiscard]] std::string_view DocOf(Distribution distribution);
    [[nodiscard]] std::string_view KeywordsOf(Distribution distribution);
    /** @brief What a distribution is drawn with, in the order its node lists them. */
    [[nodiscard]] std::span<const Input> InputsOf(Distribution distribution);
    /** @brief Where @p name is among Parameters' numbers, for one of InputsOf's. Null for a name it has not. */
    [[nodiscard]] double* ParameterNamed(Parameters& parameters, std::string_view name);

    /** @brief What a distribution is drawn with. Each uses its own few; the rest are not looked at. */
    struct Parameters {
        double min = 0.0, max = 1.0;                    ///< eUniform.
        double mean = 0.0, stddev = 1.0;                ///< eNormal, eLognormal.
        double lambda = 1.0;                            ///< eExponential, ePoisson.
        double shape = 1.0;                             ///< eGamma, eWeibull.
        double scale = 1.0;                             ///< eGamma, eWeibull, eExtremeValue, eCauchy.
        double location = 0.0;                          ///< eExtremeValue, eCauchy.
        double degrees = 1.0;                           ///< eChiSquared, eStudentT, eFisherF's numerator.
        double denominatorDegrees = 1.0;                ///< eFisherF.
        double p = 0.5;                                 ///< eBernoulli, eBinomial, eNegativeBinomial, eGeometric.
        double trials = 1.0;                            ///< eBinomial.
        double successes = 1.0;                         ///< eNegativeBinomial.
    };

    /**
     * @brief What is wrong with drawing @p distribution with @p parameters — a deviation below 0, a chance above 1, no
     *        whole number between min and max when @p whole — or none when nothing is.
     */
    [[nodiscard]] std::optional<std::string> Check(Distribution distribution, const Parameters& parameters, bool whole);

    /**
     * @brief One number of @p distribution, drawn from @p engine. @p whole: a whole number is wanted, so a uniform one is
     *        a whole number between min and max, both included; any other kind is what it is, for the caller to round.
     *        The parameters are those Check accepts.
     */
    [[nodiscard]] double Draw(Distribution distribution, const Parameters& parameters, bool whole, std::mt19937_64& engine);
}
