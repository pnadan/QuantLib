/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 Copyright (C) 2015 Johannes Goettker-Schnetmann
 Copyright (C) 2015, 2016 Klaus Spanderen

 This file is part of QuantLib, a free-software/open-source library
 for financial quantitative analysts and developers - http://quantlib.org/

 QuantLib is free software: you can redistribute it and/or modify it
 under the terms of the QuantLib license.  You should have received a
 copy of the license along with this program; if not, please email
 <quantlib-dev@lists.sf.net>. The license is also available online at
 <http://quantlib.org/license.shtml>.

 This program is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 FOR A PARTICULAR PURPOSE.  See the license for more details.
*/

#include "riskneutraldensitycalculator.hpp"
#include "utilities.hpp"

#include <ql/timegrid.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <ql/math/functional.hpp>
#include <ql/math/distributions/normaldistribution.hpp>
#include <ql/math/integrals/gausslobattointegral.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/processes/hestonprocess.hpp>
#include <ql/processes/blackscholesprocess.hpp>
#include <ql/instruments/vanillaoption.hpp>
#include <ql/pricingengines/blackcalculator.hpp>
#include <ql/pricingengines/vanilla/fdblackscholesvanillaengine.hpp>
#include <ql/termstructures/volatility/equityfx/localconstantvol.hpp>
#include <ql/termstructures/volatility/equityfx/hestonblackvolsurface.hpp>
#include <ql/termstructures/volatility/equityfx/noexceptlocalvolsurface.hpp>
#include <ql/experimental/finitedifferences/bsmrndcalculator.hpp>
#include <ql/experimental/finitedifferences/gbsmrndcalculator.hpp>
#include <ql/experimental/finitedifferences/hestonrndcalculator.hpp>
#include <ql/experimental/finitedifferences/localvolrndcalculator.hpp>
#include <ql/experimental/finitedifferences/squarerootprocessrndcalculator.hpp>
#include <ql/models/equity/hestonmodel.hpp>
#include <ql/types.hpp>

#if defined(__GNUC__) && (((__GNUC__ == 4) && (__GNUC_MINOR__ >= 8)) || (__GNUC__ > 4))
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-local-typedefs"
#endif
#include <boost/bind.hpp>
#if defined(__GNUC__) && (((__GNUC__ == 4) && (__GNUC_MINOR__ >= 8)) || (__GNUC__ > 4))
#pragma GCC diagnostic pop
#endif

#include <boost/make_shared.hpp>

using namespace QuantLib;
using namespace boost::unit_test_framework;

void RiskNeutralDensityCalculatorTest::testDensityAgainstOptionPrices() {
    BOOST_TEST_MESSAGE("Testing density against option prices...");

    SavedSettings backup;

    const DayCounter dayCounter = Actual365Fixed();
    const Date todaysDate = Settings::instance().evaluationDate();

    const Real s0 = 100;
    const Handle<Quote> spot(
        boost::shared_ptr<SimpleQuote>(new SimpleQuote(s0)));

    const Rate r = 0.075;
    const Rate q = 0.04;
    const Volatility v = 0.27;

    const Handle<YieldTermStructure> rTS(flatRate(todaysDate, r, dayCounter));

    const Handle<YieldTermStructure> qTS(flatRate(todaysDate, q, dayCounter));

    const boost::shared_ptr<BlackScholesMertonProcess> bsmProcess(
        new BlackScholesMertonProcess(
            spot, qTS, rTS,
            Handle<BlackVolTermStructure>(flatVol(v, dayCounter))));

    const BSMRNDCalculator bsm(bsmProcess);
    const Time times[] = { 0.5, 1.0, 2.0 };
    const Real strikes[] = { 75.0, 100.0, 150.0 };

    for (Size i=0; i < LENGTH(times); ++i) {
        const Time t = times[i];
        const Volatility stdDev = v*std::sqrt(t);
        const DiscountFactor df = rTS->discount(t);
        const Real fwd = s0*qTS->discount(t)/df;

        for (Size j=0; j < LENGTH(strikes); ++j) {
            const Real strike = strikes[j];
            const Real xs = std::log(strike);
            const BlackCalculator blackCalc(
                Option::Put, strike, fwd, stdDev, df);

            const Real tol = std::sqrt(QL_EPSILON);
            const Real calculatedCDF = bsm.cdf(xs, t);
            const Real expectedCDF
                = blackCalc.strikeSensitivity()/df;

            if (std::fabs(calculatedCDF - expectedCDF) > tol) {
                BOOST_FAIL("failed to reproduce Black-Scholes-Merton cdf"
                        << "\n   calculated: " << calculatedCDF
                        << "\n   expected:   " << expectedCDF
                        << "\n   diff:       " << calculatedCDF - expectedCDF
                        << "\n   tol:        " << tol);
            }

            const Real deltaStrike = strike*std::sqrt(QL_EPSILON);

            const Real calculatedPDF = bsm.pdf(xs, t);
            const Real expectedPDF = strike/df*
                (  BlackCalculator(Option::Put, strike+deltaStrike,
                       fwd, stdDev, df).strikeSensitivity()
                 - BlackCalculator(Option::Put, strike - deltaStrike,
                         fwd, stdDev, df).strikeSensitivity())/(2*deltaStrike);

            if (std::fabs(calculatedPDF - expectedPDF) > tol) {
                BOOST_FAIL("failed to reproduce Black-Scholes-Merton pdf"
                        << "\n   calculated: " << calculatedPDF
                        << "\n   expected:   " << expectedPDF
                        << "\n   diff:       " << calculatedPDF - expectedPDF
                        << "\n   tol:        " << tol);
            }
        }
    }
}

void RiskNeutralDensityCalculatorTest::testBSMagainstHestonRND() {
    BOOST_TEST_MESSAGE("Testing Black-Scholes-Merton and Heston densities...");

    SavedSettings backup;

    const DayCounter dayCounter = Actual365Fixed();
    const Date todaysDate = Settings::instance().evaluationDate();

    const Real s0 = 10;
    const Handle<Quote> spot(
        boost::shared_ptr<SimpleQuote>(new SimpleQuote(s0)));

    const Rate r = 0.155;
    const Rate q = 0.0721;
    const Volatility v = 0.27;

    const Real kappa = 1.0;
    const Real theta = v*v;
    const Real rho = -0.75;
    const Real v0 = v*v;
    const Real sigma = 0.0001;

    const Handle<YieldTermStructure> rTS(flatRate(todaysDate, r, dayCounter));

    const Handle<YieldTermStructure> qTS(flatRate(todaysDate, q, dayCounter));

    const boost::shared_ptr<BlackScholesMertonProcess> bsmProcess(
        new BlackScholesMertonProcess(
            spot, qTS, rTS,
            Handle<BlackVolTermStructure>(flatVol(v, dayCounter))));

    const BSMRNDCalculator bsm(bsmProcess);
    const HestonRNDCalculator heston(
        boost::make_shared<HestonProcess>(
            rTS, qTS, spot,
            v0, kappa, theta, sigma, rho), 1e-8);

    const Time times[] = { 0.5, 1.0, 2.0 };
    const Real strikes[] = { 7.5, 10, 15 };
    const Real probs[] = { 1e-6, 0.01, 0.5, 0.99, 1.0-1e-6 };

    for (Size i=0; i < LENGTH(times); ++i) {
        const Time t = times[i];

        for (Size j=0; j < LENGTH(strikes); ++j) {
            const Real strike = strikes[j];
            const Real xs = std::log(strike);

            const Real expectedPDF = bsm.pdf(xs, t);
            const Real calculatedPDF = heston.pdf(xs, t);

            const Real tol = 1e-4;
            if (std::fabs(expectedPDF - calculatedPDF) > tol) {
                BOOST_FAIL("failed to reproduce Black-Scholes-Merton pdf "
                           "with the Heston model"
                        << "\n   calculated: " << calculatedPDF
                        << "\n   expected:   " << expectedPDF
                        << "\n   diff:       " << calculatedPDF - expectedPDF
                        << "\n   tol:        " << tol);
            }

            const Real expectedCDF = bsm.cdf(xs, t);
            const Real calculatedCDF = heston.cdf(xs, t);

            if (std::fabs(expectedCDF - calculatedCDF) > tol) {
                BOOST_FAIL("failed to reproduce Black-Scholes-Merton cdf "
                           "with the Heston model"
                        << "\n   calculated: " << calculatedCDF
                        << "\n   expected:   " << expectedCDF
                        << "\n   diff:       " << calculatedCDF - expectedCDF
                        << "\n   tol:        " << tol);
            }
        }

        for (Size j=0; j < LENGTH(probs); ++j) {
            const Real prob = probs[j];
            const Real expectedInvCDF = bsm.invcdf(prob, t);
            const Real calculatedInvCDF = heston.invcdf(prob, t);

            const Real tol = 1e-3;
            if (std::fabs(expectedInvCDF - calculatedInvCDF) > tol) {
                BOOST_FAIL("failed to reproduce Black-Scholes-Merton "
                        "inverse cdf with the Heston model"
                        << "\n   calculated: " << calculatedInvCDF
                        << "\n   expected:   " << expectedInvCDF
                        << "\n   diff:       " << calculatedInvCDF - expectedInvCDF
                        << "\n   tol:        " << tol);
            }
        }
    }
}

namespace {
    // see Svetlana Borovkova, Ferry J. Permana
    // Implied volatility in oil markets
    // http://www.researchgate.net/publication/46493859_Implied_volatility_in_oil_markets
    class DumasParametricVolSurface : public BlackVolatilityTermStructure {
      public:
        DumasParametricVolSurface(
            Real b1, Real b2, Real b3, Real b4, Real b5,
            const boost::shared_ptr<Quote>& spot,
            const boost::shared_ptr<YieldTermStructure>& rTS,
            const boost::shared_ptr<YieldTermStructure>& qTS)
        : BlackVolatilityTermStructure(
              0, NullCalendar(), Following, rTS->dayCounter()),
          b1_(b1), b2_(b2), b3_(b3), b4_(b4), b5_(b5),
          spot_(spot), rTS_(rTS), qTS_(qTS) {}

        Date maxDate() const { return Date::maxDate(); }
        Rate minStrike() const { return 0.0; }
        Rate maxStrike() const { return QL_MAX_REAL; }

      protected:
        Volatility blackVolImpl(Time t, Real strike) const {
            QL_REQUIRE(t >= 0.0, "t must be >= 0");

            if (t < QL_EPSILON)
                return b1_;

            const Real fwd = spot_->value()*qTS_->discount(t)/rTS_->discount(t);
            const Real mn = std::log(fwd/strike)/std::sqrt(t);

            return b1_ + b2_*mn + b3_*mn*mn + b4_*t + b5_*mn*t;
        }

      private:
        const Real b1_, b2_, b3_, b4_, b5_;
        const boost::shared_ptr<Quote> spot_;
        const boost::shared_ptr<YieldTermStructure> rTS_;
        const boost::shared_ptr<YieldTermStructure> qTS_;
    };

    class ProbWeightedPayoff : public std::unary_function<Real, Real> {
      public:
        ProbWeightedPayoff(
            Time t,
            const boost::shared_ptr<Payoff>& payoff,
            const boost::shared_ptr<RiskNeutralDensityCalculator>& calc)
      : t_(t), payoff_(payoff), calc_(calc) {}

        Real operator()(Real x) const {
            return calc_->pdf(x, t_) * (*payoff_)(std::exp(x));
        }

      private:
        const Real t_;
        const boost::shared_ptr<Payoff> payoff_;
        const boost::shared_ptr<RiskNeutralDensityCalculator> calc_;
    };

    Disposable<std::vector<Time> > adaptiveTimeGrid(
        Size maxStepsPerYear, Size minStepsPerYear, Real decay, Time endTime) {
        const Time maxDt = 1.0/maxStepsPerYear;
        const Time minDt = 1.0/minStepsPerYear;

        Time t=0.0;
        std::vector<Time> times(1, t);
        while (t < endTime) {
            const Time dt = maxDt*std::exp(-decay*t)
                          + minDt*(1.0-std::exp(-decay*t));
            t+=dt;
            times.push_back(std::min(endTime, t));
        }

        return times;
    }
}

void RiskNeutralDensityCalculatorTest::testLocalVolatilityRND() {
    BOOST_TEST_MESSAGE("Testing Fokker-Planck forward equation "
                       "for local volatility process to calculate "
                       "risk neutral densities...");

    SavedSettings backup;

    const Calendar nullCalendar = NullCalendar();
    const DayCounter dayCounter = Actual365Fixed();
    const Date todaysDate = Date(28, Dec, 2012);
    Settings::instance().evaluationDate() = todaysDate;

    const Rate r       = 0.015;
    const Rate q       = 0.025;
    const Real s0      = 100;
    const Volatility v = 0.25;

    const boost::shared_ptr<Quote> spot(
        boost::make_shared<SimpleQuote>(s0));
    const boost::shared_ptr<YieldTermStructure> rTS(
        flatRate(todaysDate, r, dayCounter));
    const boost::shared_ptr<YieldTermStructure> qTS(
        flatRate(todaysDate, q, dayCounter));

    const boost::shared_ptr<TimeGrid> timeGrid(new TimeGrid(1.0, 101));

    const boost::shared_ptr<LocalVolRNDCalculator> constVolCalc(
        new LocalVolRNDCalculator(
            spot, rTS, qTS,
            boost::make_shared<LocalConstantVol>(todaysDate, v, dayCounter),
            timeGrid, 201));

    const Real rTol = 0.01, atol = 0.005;
    for (Time t=0.1; t < 0.99; t+=0.015) {
        const Volatility stdDev = v * std::sqrt(t);
        const Real xm = - 0.5 * stdDev * stdDev +
            std::log(s0 * qTS->discount(t)/rTS->discount(t));

        const GaussianDistribution gaussianPDF(xm, stdDev);
        const CumulativeNormalDistribution gaussianCDF(xm, stdDev);
        const InverseCumulativeNormal gaussianInvCDF(xm, stdDev);

        for (Real x = xm - 3*stdDev; x < xm + 3*stdDev; x+=0.05) {
            const Real expectedPDF = gaussianPDF(x);
            const Real calculatedPDF = constVolCalc->pdf(x, t);
            const Real absDiffPDF = std::fabs(expectedPDF - calculatedPDF);

            if (absDiffPDF > atol || absDiffPDF/expectedPDF > rTol) {
                BOOST_FAIL("failed to reproduce forward probability density"
                        << "\n   time:       " << t
                        << "\n   spot        " << std::exp(x)
                        << "\n   calculated: " << calculatedPDF
                        << "\n   expected:   " << expectedPDF
                        << "\n   abs diff:   " << absDiffPDF
                        << "\n   rel diff:   " << absDiffPDF/expectedPDF
                        << "\n   abs tol:    " << atol
                        << "\n   rel tol:    " << rTol);
            }

            const Real expectedCDF =  gaussianCDF(x);
            const Real calculatedCDF = constVolCalc->cdf(x, t);
            const Real absDiffCDF = std::fabs(expectedCDF - calculatedCDF);

            if (absDiffCDF > atol) {
                BOOST_FAIL("failed to reproduce forward "
                        "cumulative probability density"
                        << "\n   time:       " << t
                        << "\n   spot        " << std::exp(x)
                        << "\n   calculated: " << calculatedCDF
                        << "\n   expected:   " << expectedCDF
                        << "\n   abs diff:   " << absDiffCDF
                        << "\n   abs tol:    " << atol);
            }

            const Real expectedX = x;
            const Real calculatedX = constVolCalc->invcdf(expectedCDF, t);
            const Real absDiffX = std::fabs(expectedX - calculatedX);

            if (absDiffX > atol || absDiffX/expectedX > rTol) {
                BOOST_FAIL("failed to reproduce "
                        "inverse cumulative probability density"
                        << "\n   time:       " << t
                        << "\n   spot        " << std::exp(x)
                        << "\n   calculated: " << calculatedX
                        << "\n   expected:   " << expectedX
                        << "\n   abs diff:   " << absDiffX
                        << "\n   abs tol:    " << atol);
            }
        }
    }

    const Time tl = timeGrid->at(timeGrid->size()-5);
    const Real xl = constVolCalc->mesher(tl)->locations().front();
    if (!(   constVolCalc->pdf(xl+0.0001, tl) > 0.0
          && constVolCalc->pdf(xl-0.0001, tl) == 0.0)) {
        BOOST_FAIL("probability outside interpolation range is not zero");
    }

    const Real b1 = 0.25;
    const Real b2 = 0.03;
    const Real b3 = 0.005;
    const Real b4 = -0.02;
    const Real b5 = -0.005;

    const boost::shared_ptr<DumasParametricVolSurface> dumasVolSurface(
        new DumasParametricVolSurface(b1, b2, b3, b4, b5, spot, rTS, qTS));

    const boost::shared_ptr<BlackScholesMertonProcess> bsmProcess(
        new BlackScholesMertonProcess(
            Handle<Quote>(spot),
            Handle<YieldTermStructure>(qTS),
            Handle<YieldTermStructure>(rTS),
            Handle<BlackVolTermStructure>(dumasVolSurface)));

    const boost::shared_ptr<LocalVolTermStructure> localVolSurface
        = boost::make_shared<NoExceptLocalVolSurface>(
              Handle<BlackVolTermStructure>(dumasVolSurface),
              Handle<YieldTermStructure>(rTS),
              Handle<YieldTermStructure>(qTS),
              Handle<Quote>(spot), b1);

    const std::vector<Time> adaptiveGrid
        = adaptiveTimeGrid(400, 50, 5.0, 3.0);

    const boost::shared_ptr<TimeGrid> dumasTimeGrid(
        new TimeGrid(adaptiveGrid.begin(), adaptiveGrid.end()));

    const boost::shared_ptr<LocalVolRNDCalculator> dumasVolCalc(
        new LocalVolRNDCalculator(
            spot, rTS, qTS, localVolSurface, dumasTimeGrid, 401, 0.1, 1e-8));

    const Real strikes[] = { 25, 50, 95, 100, 105, 150, 200, 400 };
    const Date maturityDates[] = {
        todaysDate + Period(1, Weeks),   todaysDate + Period(1, Months),
        todaysDate + Period(3, Months),  todaysDate + Period(6, Months),
        todaysDate + Period(12, Months), todaysDate + Period(18, Months),
        todaysDate + Period(2, Years),   todaysDate + Period(3, Years) };
    const std::vector<Date> maturities(
        maturityDates, maturityDates + LENGTH(maturityDates));


    for (Size i=0; i < maturities.size(); ++i) {
        const Time expiry
            = rTS->dayCounter().yearFraction(todaysDate, maturities[i]);

        const boost::shared_ptr<PricingEngine> engine(
            new FdBlackScholesVanillaEngine(
                bsmProcess, std::max(Size(51), Size(expiry*101)),
                201, 0, FdmSchemeDesc::Douglas(), true, b1));

        const boost::shared_ptr<Exercise> exercise(
            new EuropeanExercise(maturities[i]));

        for (Size k=0; k < LENGTH(strikes); ++k) {
            const Real strike = strikes[k];
            const boost::shared_ptr<StrikedTypePayoff> payoff(
                new PlainVanillaPayoff(
                    (strike > spot->value()) ? Option::Call : Option::Put
                    , strike));

            VanillaOption option(payoff, exercise);
            option.setPricingEngine(engine);
            const Real expected = option.NPV();

            const Time tx = std::max(dumasTimeGrid->at(1),
                                     dumasTimeGrid->closestTime(expiry));
            const std::vector<Real> x = dumasVolCalc->mesher(tx)->locations();

            const ProbWeightedPayoff probWeightedPayoff(
                expiry, payoff, dumasVolCalc);

            const DiscountFactor df = rTS->discount(expiry);
            const Real calculated =    GaussLobattoIntegral(10000, 1e-10)(
                probWeightedPayoff, x.front(), x.back()) * df;

            const Real absDiff = std::fabs(expected - calculated);

            if (absDiff > 0.5*atol) {
                BOOST_ERROR("failed to reproduce option prices for"
                        << "\n   expiry:     " << expiry
                        << "\n   strike:     " << strike
                        << "\n   expected:   " << expected
                        << "\n   calculated: " << calculated
                        << "\n   diff:       " << absDiff
                        << "\n   abs tol:    " << atol);
            }
        }
    }
}

void RiskNeutralDensityCalculatorTest::testSquareRootProcessRND() {
    BOOST_TEST_MESSAGE("Testing probability density for a square root process...");

    struct SquareRootProcessParams {
        const Real v0, kappa, theta, sigma;
    };

    const SquareRootProcessParams params[]
        = { { 0.17, 1.0, 0.09, 0.5 },
            { 1.0, 0.6, 0.1, 0.75 },
            { 0.005, 0.6, 0.1, 0.05 } };

    for (Size i=0; i < LENGTH(params); ++i) {
        const SquareRootProcessRNDCalculator rndCalculator(
            params[i].v0, params[i].kappa, params[i].theta, params[i].sigma);

        const Time t = 0.75;
        const Time tInfty = 60.0/params[i].kappa;

        const Real tol = 1e-10;
        for (Real v = 1e-5; v < 1.0; v+=(v < params[i].theta) ? 0.005 : 0.1) {

            const Real cdfCalculated = rndCalculator.cdf(v, t);
            const Real cdfExpected = GaussLobattoIntegral(10000, 0.01*tol)(
                boost::bind(&SquareRootProcessRNDCalculator::pdf,
                    &rndCalculator, _1, t), 0, v);

            if (std::fabs(cdfCalculated - cdfExpected) > tol) {
                BOOST_FAIL("failed to calculate cdf"
                        << "\n   t:          " << t
                        << "\n   v:          " << v
                        << "\n   calculated: " << cdfCalculated
                        << "\n   expected:   " << cdfExpected
                        << "\n   diff:       " << cdfCalculated - cdfExpected
                        << "\n   tolerance:  " << tol);
            }

            if (cdfExpected < (1-1e-6) && cdfExpected > 1e-6) {
                const Real vCalculated = rndCalculator.invcdf(cdfCalculated, t);

                if (std::fabs(v - vCalculated) > tol) {
                    BOOST_FAIL("failed to calculate round trip cdf <-> invcdf"
                            << "\n   t:          " << t
                            << "\n   v:          " << v
                            << "\n   cdf:        " << cdfExpected
                            << "\n   calculated: " << vCalculated
                            << "\n   diff:       " << v - vCalculated
                            << "\n   tolerance:  " << tol);
                }
            }

            const Real statPdfCalculated = rndCalculator.pdf(v, tInfty);
            const Real statPdfExpected = rndCalculator.stationary_pdf(v);

            if (std::fabs(statPdfCalculated - statPdfExpected) > tol) {
                BOOST_FAIL("failed to calculate stationary pdf"
                        << "\n   v:          " << v
                        << "\n   calculated: " << statPdfCalculated
                        << "\n   expected:   " << statPdfExpected
                        << "\n   diff:       " << statPdfCalculated - statPdfExpected
                        << "\n   tolerance:  " << tol);
            }

            const Real statCdfCalculated = rndCalculator.cdf(v, tInfty);
            const Real statCdfExpected = rndCalculator.stationary_cdf(v);

            if (std::fabs(statCdfCalculated - statCdfExpected) > tol) {
                BOOST_FAIL("failed to calculate stationary cdf"
                        << "\n   v:          " << v
                        << "\n   calculated: " << statCdfCalculated
                        << "\n   expected:   " << statCdfExpected
                        << "\n   diff:       " << statCdfCalculated - statCdfExpected
                        << "\n   tolerance:  " << tol);
            }
        }

        for (Real q = 1e-5; q < 1.0; q+=0.001) {
            const Real statInvCdfCalculated = rndCalculator.invcdf(q, tInfty);
            const Real statInvCdfExpected = rndCalculator.stationary_invcdf(q);

            if (std::fabs(statInvCdfCalculated - statInvCdfExpected) > tol) {
                BOOST_FAIL("failed to calculate stationary inverse of cdf"
                        << "\n   q:          " << q
                        << "\n   calculated: " << statInvCdfCalculated
                        << "\n   expected:   " << statInvCdfExpected
                        << "\n   diff:       " << statInvCdfCalculated - statInvCdfExpected
                        << "\n   tolerance:  " << tol);
            }
        }
    }
}

void RiskNeutralDensityCalculatorTest::testBlackScholesWithSkew() {
    BOOST_TEST_MESSAGE(
        "Testing probability density for a BSM process "
        "with strike dependent volatility vs local volatility...");

    SavedSettings backup;

    const Date todaysDate = Date(3, Oct, 2016);
    Settings::instance().evaluationDate() = todaysDate;

    const DayCounter dc = Actual365Fixed();
    const Date maturityDate = todaysDate + Period(3, Months);
    const Time maturity = dc.yearFraction(todaysDate, maturityDate);

    // use Heston model to create volatility surface with skew
    const Real r     =  0.08;
    const Real q     =  0.03;
    const Real s0    =  100;
    const Real v0    =  0.06;
    const Real kappa =  1.0;
    const Real theta =  0.06;
    const Real sigma =  0.4;
    const Real rho   = -0.75;

    const Handle<YieldTermStructure> rTS(flatRate(todaysDate, r, dc));
    const Handle<YieldTermStructure> qTS(flatRate(todaysDate, q, dc));
    const Handle<Quote> spot(boost::make_shared<SimpleQuote>(s0));

    const boost::shared_ptr<HestonProcess> hestonProcess(
        boost::make_shared<HestonProcess>(
            rTS, qTS, spot, v0, kappa, theta, sigma, rho));

    const Handle<BlackVolTermStructure> hestonSurface(
        boost::make_shared<HestonBlackVolSurface>(
            Handle<HestonModel>(boost::make_shared<HestonModel>(hestonProcess)),
            AnalyticHestonEngine::AndersenPiterbarg,
            AnalyticHestonEngine::Integration::discreteTrapezoid(32)));

    const boost::shared_ptr<TimeGrid> timeGrid(new TimeGrid(maturity, 51));

    const boost::shared_ptr<LocalVolTermStructure> localVol(
        boost::make_shared<NoExceptLocalVolSurface>(
            hestonSurface, rTS, qTS, spot, std::sqrt(theta)));

    const LocalVolRNDCalculator localVolCalc(
        spot.currentLink(), rTS.currentLink(), qTS.currentLink(), localVol,
        timeGrid, 151, 0.25);

    const HestonRNDCalculator hestonCalc(hestonProcess);

    const GBSMRNDCalculator gbsmCalc(
        boost::make_shared<BlackScholesMertonProcess>(
            spot, qTS, rTS, hestonSurface));

    const Real strikes[] = { 85, 75, 90, 110, 125, 150 };

    for (Size i=0; i < LENGTH(strikes); ++i) {
        const Real strike = strikes[i];
        const Real logStrike = std::log(strike);

        const Real expected = hestonCalc.cdf(logStrike, maturity);
        const Real calculatedGBSM = gbsmCalc.cdf(strike, maturity);

        const Real gbsmTol = 1e-5;
        if (std::fabs(expected - calculatedGBSM) > gbsmTol) {
            BOOST_FAIL("failed to match Heston and GBSM cdf"
                    << "\n   t:          " << maturity
                    << "\n   k:          " << strike
                    << "\n   calculated: " << calculatedGBSM
                    << "\n   expected:   " << expected
                    << "\n   diff:       " <<
                        std::fabs(calculatedGBSM - expected)
                    << "\n   tolerance:  " << gbsmTol);
        }

        const Real calculatedLocalVol = localVolCalc.cdf(logStrike, maturity);
        const Real localVolTol = 1e-3;
        if (std::fabs(expected - calculatedLocalVol) > localVolTol) {
            BOOST_FAIL("failed to match Heston and local Volatility cdf"
                    << "\n   t:          " << maturity
                    << "\n   k:          " << strike
                    << "\n   calculated: " << calculatedLocalVol
                    << "\n   expected:   " << expected
                    << "\n   diff:       " <<
                        std::fabs(calculatedLocalVol - expected)
                    << "\n   tolerance:  " << localVolTol);
        }
    }

    for (Size i=0; i < LENGTH(strikes); ++i) {
        const Real strike = strikes[i];
        const Real logStrike = std::log(strike);

        const Real expected = hestonCalc.pdf(logStrike, maturity)/strike;
        const Real calculatedGBSM = gbsmCalc.pdf(strike, maturity);

        const Real gbsmTol = 1e-5;
        if (std::fabs(expected - calculatedGBSM) > gbsmTol) {
            BOOST_FAIL("failed to match Heston and GBSM pdf"
                    << "\n   t:          " << maturity
                    << "\n   k:          " << strike
                    << "\n   calculated: " << calculatedGBSM
                    << "\n   expected:   " << expected
                    << "\n   diff:       " <<
                        std::fabs(calculatedGBSM - expected)
                    << "\n   tolerance:  " << gbsmTol);
        }

        const Real calculatedLocalVol
            = localVolCalc.pdf(logStrike, maturity)/strike;
        const Real localVolTol = 1e-4;
        if (std::fabs(expected - calculatedLocalVol) > localVolTol) {
            BOOST_FAIL("failed to match Heston and local Volatility pdf"
                    << "\n   t:          " << maturity
                    << "\n   k:          " << strike
                    << "\n   calculated: " << calculatedLocalVol
                    << "\n   expected:   " << expected
                    << "\n   diff:       " <<
                        std::fabs(calculatedLocalVol - expected)
                    << "\n   tolerance:  " << localVolTol);
        }
    }

    const Real quantiles[] = { 0.05, 0.25, 0.5, 0.75, 0.95 };
    for (Size i=0; i < LENGTH(quantiles); ++i) {
        const Real quantile = quantiles[i];

        const Real expected = std::exp(hestonCalc.invcdf(quantile, maturity));
        const Real calculatedGBSM = gbsmCalc.invcdf(quantile, maturity);

        const Real gbsmTol = 1e-3;
        if (std::fabs(expected - calculatedGBSM) > gbsmTol) {
            BOOST_FAIL("failed to match Heston and GBSM invcdf"
                    << "\n   t:          " << maturity
                    << "\n   quantile:   " << quantile
                    << "\n   calculated: " << calculatedGBSM
                    << "\n   expected:   " << expected
                    << "\n   diff:       " <<
                        std::fabs(calculatedGBSM - expected)
                    << "\n   tolerance:  " << gbsmTol);
        }

        const Real calculatedLocalVol
            = std::exp(localVolCalc.invcdf(quantile, maturity));
        const Real localVolTol = 0.1;
        if (std::fabs(expected - calculatedLocalVol) > localVolTol) {
            BOOST_FAIL("failed to match Heston and local Volatility invcdf"
                    << "\n   t:          " << maturity
                    << "\n   k:          " << quantile
                    << "\n   calculated: " << calculatedLocalVol
                    << "\n   expected:   " << expected
                    << "\n   diff:       " <<
                        std::fabs(calculatedLocalVol - expected)
                    << "\n   tolerance:  " << localVolTol);
        }
    }
}

test_suite* RiskNeutralDensityCalculatorTest::experimental(SpeedLevel speed) {
    test_suite* suite = BOOST_TEST_SUITE("Risk neutral density calculator tests");

    suite->add(QUANTLIB_TEST_CASE(RiskNeutralDensityCalculatorTest::testDensityAgainstOptionPrices));
    suite->add(QUANTLIB_TEST_CASE(RiskNeutralDensityCalculatorTest::testBSMagainstHestonRND));
    suite->add(QUANTLIB_TEST_CASE(RiskNeutralDensityCalculatorTest::testLocalVolatilityRND));
    suite->add(QUANTLIB_TEST_CASE(RiskNeutralDensityCalculatorTest::testSquareRootProcessRND));

    if (speed <= Fast) {
        suite->add(QUANTLIB_TEST_CASE(RiskNeutralDensityCalculatorTest::testBlackScholesWithSkew));
    }

    return suite;
}