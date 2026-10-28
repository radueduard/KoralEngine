#include "kgraph/random.h"

#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>
#include <vector>

namespace kgraph::random
{
    namespace
    {
        constexpr std::array<std::string_view, 16> names {
            "eUniform", "eNormal", "eLognormal", "eExponential", "eGamma", "eWeibull", "eExtremeValue", "eCauchy",
            "eChiSquared", "eStudentT", "eFisherF", "eBernoulli", "eBinomial", "eNegativeBinomial", "eGeometric", "ePoisson" };

        // The most a whole number of a uniform one or of trials may be: past it, a double skips whole numbers.
        constexpr double exact = 9007199254740992.0;    // 2^53

        /** In [0, 1): the top 53 bits of one draw, as many as a double holds. */
        double Unit(std::mt19937_64& engine) { return static_cast<double>(engine() >> 11) * 0x1.0p-53; }
        /** In (0, 1): never 0, for what takes its logarithm. */
        double Open(std::mt19937_64& engine) { return (static_cast<double>(engine() >> 11) + 0.5) * 0x1.0p-53; }

        /** A standard normal number: Marsaglia's polar method. */
        double Normal(std::mt19937_64& engine)
        {
            for (;;) {
                const double u = 2.0 * Unit(engine) - 1.0, v = 2.0 * Unit(engine) - 1.0, s = u * u + v * v;
                if (s > 0.0 && s < 1.0) return u * std::sqrt(-2.0 * std::log(s) / s);
            }
        }

        /** Of shape @p k and scale 1: Marsaglia and Tsang's, and below a shape of 1, one of k + 1 times U^(1/k). */
        double Gamma(const double k, std::mt19937_64& engine)
        {
            if (k < 1.0) return Gamma(k + 1.0, engine) * std::pow(Open(engine), 1.0 / k);
            const double d = k - 1.0 / 3.0, c = 1.0 / std::sqrt(9.0 * d);
            for (;;) {
                double x, v;
                do {
                    x = Normal(engine);
                    v = 1.0 + c * x;
                } while (v <= 0.0);
                v = v * v * v;
                const double u = Open(engine);
                if (u < 1.0 - 0.0331 * x * x * x * x || std::log(u) < 0.5 * x * x + d * (1.0 - v + std::log(v))) return d * v;
            }
        }

        double Beta(const double a, const double b, std::mt19937_64& engine)
        {
            const double x = Gamma(a, engine);
            return x / (x + Gamma(b, engine));
        }

        double ChiSquared(const double degrees, std::mt19937_64& engine) { return 2.0 * Gamma(degrees / 2.0, engine); }

        /**
         * How many of @p n tries succeed. Knuth's (TAOCP 3.4.1 F): the middle one of n uniform numbers is a beta number,
         * which says on which side of it p is, and so how many succeed on one side for certain, leaving the other side's
         * tries — half as many — to count the same way; the last few are tried one at a time.
         */
        double Binomial(double n, double p, std::mt19937_64& engine)
        {
            double count = 0.0;
            while (n > 16.0) {
                if (p <= 0.0) return count;
                if (p >= 1.0) return count + n;
                const double a = 1.0 + std::floor(n / 2.0), b = n + 1.0 - a;
                const double x = Beta(a, b, engine);
                if (x >= p) {
                    n = a - 1.0;
                    p /= x;
                } else {
                    count += a;
                    n = b - 1.0;
                    p = (p - x) / (1.0 - x);
                }
            }
            for (double i = 0.0; i < n; ++i) count += Unit(engine) < p ? 1.0 : 0.0;
            return count;
        }

        /**
         * How many times, at @p lambda times a unit, in one unit. Knuth's (TAOCP 3.4.1 F): the m-th time is a gamma number
         * of order m; whether it came before the unit was out says that m came, and the rest is a smaller Poisson number,
         * or that fewer did, which of m - 1 is binomial. A small one by multiplying uniform numbers until they are below
         * e^-lambda.
         */
        double Poisson(double lambda, std::mt19937_64& engine)
        {
            double count = 0.0;
            while (lambda > 16.0) {
                const double m = std::floor(lambda * 7.0 / 8.0);
                const double x = Gamma(m, engine);
                if (x >= lambda) return count + Binomial(m - 1.0, lambda / x, engine);
                count += m;
                lambda -= x;
            }
            const double limit = std::exp(-lambda);
            for (double product = Unit(engine); product > limit; product *= Unit(engine)) ++count;
            return count;
        }
    }

    namespace
    {
        struct About {
            std::string_view node, doc, keywords;
            std::vector<Input> inputs;
        };
        const std::array<About, 16>& Abouts()
        {
            static const Input p { "p", 0.5, "The chance of a success, 0 to 1." };
            static const Input scale { "scale", 1, "How spread it is: above 0." };
            static const Input location { "location", 0, "Where it is most likely." };
            static const std::array<About, 16> abouts { {
                { "RandomUniform", "Every number from min to max as likely: a float never max itself, a whole number each from min to max, both included.",
                  "uniform range between flat even dice", { { "min", 0, "The least it gives." }, { "max", 1, "The most it gives." } } },
                { "RandomNormal", "The bell curve: most near the mean, fewer the further from it, a deviation away two in three.", "normal gaussian bell gauss",
                  { { "mean", 0, "Where the bell is." }, { "stddev", 1, "How wide the bell is: 0 or more." } } },
                { "RandomLognormal", "e to the power of a normal number: never below 0, with a long tail above. A size, a price.", "lognormal log normal skewed size",
                  { { "mean", 0, "Where its logarithm's bell is." }, { "stddev", 1, "How wide its logarithm's bell is: 0 or more." } } },
                { "RandomExponential", "How long until something that happens lambda times a unit happens: a wait.", "exponential wait time decay arrival",
                  { { "lambda", 1, "How many times it happens a unit: above 0. The wait is 1 / lambda on average." } } },
                { "RandomGamma", "A sum of shape waits, each scale long on average.", "gamma waits shape scale",
                  { { "shape", 1, "How many waits it sums: above 0, and not only whole." }, scale } },
                { "RandomWeibull", "How long until something wears out or fails.", "weibull failure lifetime wear",
                  { { "shape", 1, "Above 0: below 1 what fails early, above it what wears out." }, scale } },
                { "RandomExtremeValue", "The largest of many numbers (Gumbel): a flood's height, a year's hottest day.", "extreme value gumbel largest maximum",
                  { location, scale } },
                { "RandomCauchy", "A bell with tails so heavy it has no mean: half within a scale of where it is, and some very far.", "cauchy lorentz heavy tail",
                  { location, scale } },
                { "RandomChiSquared", "A sum of degrees squared normal numbers.", "chi squared chi2 sum squares",
                  { { "degrees", 1, "How many it sums: above 0." } } },
                { "RandomStudentT", "A normal number divided by how sure degrees samples make it: a bell with heavier tails.", "student t heavy",
                  { { "degrees", 1, "Degrees of freedom, above 0: the more, the nearer a normal bell." } } },
                { "RandomFisherF", "The ratio of two chi-squared numbers, each over its degrees.", "fisher f snedecor ratio",
                  { { "degrees", 1, "The numerator's degrees of freedom, above 0." }, { "denominatorDegrees", 1, "The denominator's degrees of freedom, above 0." } } },
                { "RandomBernoulli", "1 with chance p, else 0: a coin.", "bernoulli coin flip chance yes no", { p } },
                { "RandomBinomial", "How many of trials tries succeed, each with chance p.", "binomial successes trials count",
                  { { "trials", 1, "How many tries: a whole number, 0 or more." }, p } },
                { "RandomNegativeBinomial", "How many tries fail before successes of them succeed, each with chance p.", "negative binomial pascal failures",
                  { { "successes", 1, "How many successes it waits for: above 0." }, { "p", 0.5, "The chance of a success: above 0, to 1." } } },
                { "RandomGeometric", "How many tries fail before one succeeds, each with chance p.", "geometric failures first success",
                  { { "p", 0.5, "The chance of a success: above 0, to 1." } } },
                { "RandomPoisson", "How many times something that happens lambda times a unit happens in one: raindrops on a tile, calls in an hour.",
                  "poisson count events occurrences", { { "lambda", 1, "How many times it happens a unit, on average: 0 or more." } } },
            } };
            return abouts;
        }
        const About& AboutOf(const Distribution distribution) { return Abouts()[static_cast<std::size_t>(distribution)]; }
    }

    std::string_view NodeOf(const Distribution distribution) { return AboutOf(distribution).node; }
    std::string_view DocOf(const Distribution distribution) { return AboutOf(distribution).doc; }
    std::string_view KeywordsOf(const Distribution distribution) { return AboutOf(distribution).keywords; }
    std::span<const Input> InputsOf(const Distribution distribution) { return AboutOf(distribution).inputs; }

    std::optional<Distribution> DistributionOfNode(const std::string_view type)
    {
        for (std::size_t i = 0; i < Abouts().size(); ++i)
            if (Abouts()[i].node == type) return static_cast<Distribution>(i);
        return std::nullopt;
    }

    double* ParameterNamed(Parameters& with, const std::string_view name)
    {
        const std::pair<std::string_view, double*> each[] {
            { "min", &with.min }, { "max", &with.max }, { "mean", &with.mean }, { "stddev", &with.stddev }, { "lambda", &with.lambda },
            { "shape", &with.shape }, { "scale", &with.scale }, { "location", &with.location }, { "degrees", &with.degrees },
            { "denominatorDegrees", &with.denominatorDegrees }, { "p", &with.p }, { "trials", &with.trials }, { "successes", &with.successes } };
        for (const auto& [named, at] : each)
            if (named == name) return at;
        return nullptr;
    }

    std::span<const std::string_view> DistributionNames() { return names; }

    std::optional<Distribution> DistributionNamed(const std::string_view name)
    {
        for (std::size_t i = 0; i < names.size(); ++i)
            if (names[i] == name) return static_cast<Distribution>(i);
        return std::nullopt;
    }

    bool IsDiscrete(const Distribution distribution)
    {
        using enum Distribution;
        return distribution == eBernoulli || distribution == eBinomial || distribution == eNegativeBinomial || distribution == eGeometric
            || distribution == ePoisson;
    }

    std::optional<std::string> Check(const Distribution distribution, const Parameters& with, const bool whole)
    {
        const auto finite = [](std::initializer_list<std::pair<const char*, double>> each) -> std::optional<std::string> {
            for (const auto& [name, value] : each)
                if (!std::isfinite(value)) return std::format("'{}' is a number, not {}", name, value);
            return std::nullopt;
        };
        const auto chance = [&](const bool zero) -> std::optional<std::string> {
            if (auto wrong = finite({ { "p", with.p } })) return wrong;
            if (with.p < 0.0 || with.p > 1.0 || (!zero && with.p == 0.0))
                return std::format("'p' is a chance, {} 0 to 1, not {}", zero ? "from" : "above", with.p);
            return std::nullopt;
        };
        const auto positive = [&](const char* name, const double value) -> std::optional<std::string> {
            if (auto wrong = finite({ { name, value } })) return wrong;
            if (value <= 0.0) return std::format("'{}' is above 0, not {}", name, value);
            return std::nullopt;
        };
        using enum Distribution;
        switch (distribution) {
            case eUniform:
                if (auto wrong = finite({ { "min", with.min }, { "max", with.max } })) return wrong;
                if (with.min > with.max) return std::format("'min' ({}) is above 'max' ({})", with.min, with.max);
                if (whole) {
                    if (std::abs(with.min) > exact || std::abs(with.max) > exact) return "'min' and 'max' are within 2^53 of 0, for whole numbers";
                    if (std::ceil(with.min) > std::floor(with.max)) return std::format("there is no whole number from {} to {}", with.min, with.max);
                }
                return std::nullopt;
            case eNormal:
            case eLognormal:
                if (auto wrong = finite({ { "mean", with.mean }, { "stddev", with.stddev } })) return wrong;
                if (with.stddev < 0.0) return std::format("'stddev' is 0 or more, not {}", with.stddev);
                return std::nullopt;
            case eExponential: return positive("lambda", with.lambda);
            case ePoisson:
                if (auto wrong = finite({ { "lambda", with.lambda } })) return wrong;
                if (with.lambda < 0.0) return std::format("'lambda' is 0 or more, not {}", with.lambda);
                return std::nullopt;
            case eGamma:
            case eWeibull:
                if (auto wrong = positive("shape", with.shape)) return wrong;
                return positive("scale", with.scale);
            case eExtremeValue:
            case eCauchy:
                if (auto wrong = finite({ { "location", with.location } })) return wrong;
                return positive("scale", with.scale);
            case eChiSquared:
            case eStudentT: return positive("degrees", with.degrees);
            case eFisherF:
                if (auto wrong = positive("degrees", with.degrees)) return wrong;
                return positive("denominatorDegrees", with.denominatorDegrees);
            case eBernoulli: return chance(true);
            case eBinomial:
                if (auto wrong = chance(true)) return wrong;
                if (auto wrong = finite({ { "trials", with.trials } })) return wrong;
                if (with.trials < 0.0 || with.trials != std::floor(with.trials) || with.trials > exact)
                    return std::format("'trials' is a whole number, 0 or more, not {}", with.trials);
                return std::nullopt;
            case eNegativeBinomial:
                if (auto wrong = chance(false)) return wrong;
                return positive("successes", with.successes);
            case eGeometric: return chance(false);
        }
        return "it is no distribution there is";
    }

    double Draw(const Distribution distribution, const Parameters& with, const bool whole, std::mt19937_64& engine)
    {
        using enum Distribution;
        switch (distribution) {
            case eUniform: {
                if (!whole) return with.min + (with.max - with.min) * Unit(engine);
                // A whole number of span as likely as any other: what falls in the last, short run of span is drawn again.
                const double low = std::ceil(with.min);
                const auto span = static_cast<std::uint64_t>(std::floor(with.max) - low) + 1;
                constexpr std::uint64_t most = std::numeric_limits<std::uint64_t>::max();
                const std::uint64_t fair = most - most % span;
                std::uint64_t drawn;
                do drawn = engine();
                while (drawn >= fair);
                return low + static_cast<double>(drawn % span);
            }
            case eNormal: return with.mean + with.stddev * Normal(engine);
            case eLognormal: return std::exp(with.mean + with.stddev * Normal(engine));
            case eExponential: return -std::log(Open(engine)) / with.lambda;
            case eGamma: return with.scale * Gamma(with.shape, engine);
            case eWeibull: return with.scale * std::pow(-std::log(Open(engine)), 1.0 / with.shape);
            case eExtremeValue: return with.location - with.scale * std::log(-std::log(Open(engine)));
            case eCauchy: return with.location + with.scale * std::tan(std::numbers::pi * (Open(engine) - 0.5));
            case eChiSquared: return ChiSquared(with.degrees, engine);
            case eStudentT: return Normal(engine) / std::sqrt(ChiSquared(with.degrees, engine) / with.degrees);
            case eFisherF:
                return (ChiSquared(with.degrees, engine) / with.degrees) / (ChiSquared(with.denominatorDegrees, engine) / with.denominatorDegrees);
            case eBernoulli: return Unit(engine) < with.p ? 1.0 : 0.0;
            case eBinomial: return Binomial(with.trials, with.p, engine);
            case eNegativeBinomial:
                // A Poisson number whose rate is a gamma number: how many fail before so many succeed.
                if (with.p >= 1.0) return 0.0;
                return Poisson(Gamma(with.successes, engine) * (1.0 - with.p) / with.p, engine);
            case eGeometric:
                if (with.p >= 1.0) return 0.0;
                return std::floor(std::log(Open(engine)) / std::log1p(-with.p));
            case ePoisson: return Poisson(with.lambda, engine);
        }
        return 0.0;
    }
}
