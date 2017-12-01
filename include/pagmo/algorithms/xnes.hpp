/* Copyright 2017 PaGMO development team

This file is part of the PaGMO library.

The PaGMO library is free software; you can redistribute it and/or modify
it under the terms of either:

  * the GNU Lesser General Public License as published by the Free
    Software Foundation; either version 3 of the License, or (at your
    option) any later version.

or

  * the GNU General Public License as published by the Free Software
    Foundation; either version 3 of the License, or (at your option) any
    later version.

or both in parallel, as here.

The PaGMO library is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received copies of the GNU General Public License and the
GNU Lesser General Public License along with the PaGMO library.  If not,
see https://www.gnu.org/licenses/. */

#ifndef PAGMO_ALGORITHMS_XNES_HPP
#define PAGMO_ALGORITHMS_XNES_HPP

#include <pagmo/config.hpp>

#if defined(PAGMO_WITH_EIGEN3)

#include <Eigen/Dense>
#include <unsupported/Eigen/MatrixFunctions>

#include <iomanip>
#include <random>
#include <string>
#include <tuple>

#include <pagmo/algorithm.hpp>
#include <pagmo/detail/custom_comparisons.hpp>
#include <pagmo/exceptions.hpp>
#include <pagmo/io.hpp>
#include <pagmo/population.hpp>
#include <pagmo/rng.hpp>
#include <pagmo/utils/generic.hpp>

namespace pagmo
{
/// Exponential Natural Evolution Strategies
/**
 * \image html xnes.png width=3cm
 *
 * Exponential Natural Evolution Strategies is an algorithm closely related to pagmo::cmaes and based
 * on the adaptation of a gaussian sampling distribution via the so-called natural gradient.
 * Like pagmo::cmaes it is based on the idea of sampling new trial vectors from a multivariate distribution
 * and using the new sampled points to update the distribution parameters. Naively this could be done following
 * the gradient of the expected fitness as approximated by a finite number of sampled points. While this idea
 * offers a powerful lead on algorithmic construction it has some major drawbacks that are solved in the so-called
 * Natural Evolution Strategies class of algorithms by adopting, instead, the natural gradient.
 *
 * \verbatim embed:rst:leading-asterisk
 * .. warning::
 *
 *    A moved-from pagmo::xnes is destructible and assignable. Any other operation will result
 *    in undefined behaviour.
 *
 * .. note::
 *
 *    This user-defined algorithm is available only if pagmo was compiled with the ``PAGMO_WITH_EIGEN3`` option
 *    enabled (see the :ref:`installation instructions <install>`).
 *
 * .. note::
 *
 *    We introduced two changes to the original algorithm in order to simplify its use for the generic user.
 *    1 - when a decision vector is sampled outside the problem bounds it will be forced back in.
 *    2 - the initial covariance matrix depends on the bounds width so that heterogenously scaled variables
 *    are not a problem: the width along the i-th direction will be w_i = sigma_0 * (ub_i - lb_i)
 *
 * .. note::
 *
 *    Since at each generation all newly generated individuals sampled from the adapted distribution are
 *    reinserted into the population, xNES may not preserve the best individual (not elitist).
 *    As a consequence the plot of the population best fitness may not be perfectly monotonically decreasing.
 *
 * .. seealso::
 *
 *    Glasmachers, T., Schaul, T., Yi, S., Wierstra, D., & Schmidhuber, J. (2010, July). Exponential natural
 *    evolution strategies. In Proceedings of the 12th annual conference on Genetic and evolutionary computation (pp.
 * 393-400). ACM. \endverbatim
 */
class xnes
{
public:
    /// Single entry of the log (gen, fevals, best, dx, df, sigma)
    /// Single data line for the algorithm's log.
    /**
     * A log data line is a tuple consisting of:
     * - the generation number,
     * - the number of function evaluations
     * - the best fitness vector so far,
     * - the population flatness evaluated as the distance between the decisions vector of the best and of the worst
     * individual,
     * - the population flatness evaluated as the distance between the fitness of the best and of the worst individual.
     */
    typedef std::tuple<unsigned int, unsigned long long, double, double, double, double> log_line_type;
    /// Log type.
    /**
     * The algorithm log is a collection of nlopt::log_line_type data lines, stored in chronological order
     * during the optimisation if the verbosity of the algorithm is set to a nonzero value
     * (see xnes::set_verbosity()).
     */
    typedef std::vector<log_line_type> log_type;

    /// Constructor.
    /**
     * Constructs xnes
     *
     * @param gen number of generations.
     * @param eta_mu learning rate for mean update (if -1 will be automatically selected to be 1)
     * @param eta_sigma learning rate for step-size update (if -1 will be automatically selected)
     * @param eta_b  learning rate for the covariance matrix update (if -1 will be automatically selected)
     * @param sigma0 the initial search width will be sigma0 * (ub - lb)
     * @param ftol stopping criteria on the x tolerance (default is 1e-6)
     * @param xtol stopping criteria on the f tolerance (default is 1e-6)
     * @param memory when true the distribution parameters are not reset between successive calls to the evolve method
     * @param seed seed used by the internal random number generator (default is random)

     * @throws std::invalid_argument if eta_mu, eta_sigma, eta_b and sigma0 are not in [0, 1] or -1
     */
    xnes(unsigned int gen = 1, double eta_mu = -1, double eta_sigma = -1, double eta_b = -1, double sigma0 = -1,
         double ftol = 1e-6, double xtol = 1e-6, bool memory = false, unsigned int seed = pagmo::random_device::next())
        : m_gen(gen), m_eta_mu(eta_mu), m_eta_sigma(eta_sigma), m_eta_b(eta_b), m_sigma0(sigma0), m_ftol(ftol),
          m_xtol(xtol), m_memory(memory), m_e(seed), m_seed(seed), m_verbosity(0u), m_log()
    {
        if (((eta_mu <= 0.) || (eta_mu > 1.)) && !(eta_mu == -1)) {
            pagmo_throw(std::invalid_argument,
                        "eta_mu must be in ]0,1] or -1 if its value has to be initialized automatically, a value of "
                            + std::to_string(eta_mu) + " was detected");
        }
        if (((eta_sigma <= 0.) || (eta_sigma > 1.)) && !(eta_sigma == -1)) {
            pagmo_throw(
                std::invalid_argument,
                "eta_sigma needs to be in ]0,1] or -1 if its value has to be initialized automatically, a value of "
                    + std::to_string(eta_sigma) + " was detected");
        }
        if (((eta_b <= 0.) || (eta_b > 1.)) && !(eta_b == -1)) {
            pagmo_throw(std::invalid_argument,
                        "eta_b needs to be in ]0,1] or -1 if its value has to be initialized automatically, a value of "
                            + std::to_string(eta_b) + " was detected");
        }
        if (((sigma0 <= 0.) || (sigma0 > 1.)) && !(sigma0 == -1)) {
            pagmo_throw(
                std::invalid_argument,
                "sigma0 needs to be in ]0,1] or -1 if its value has to be initialized automatically, a value of "
                    + std::to_string(sigma0) + " was detected");
        }
        // Initialize explicitly the algorithm memory
        sigma = m_sigma0;
        mean = Eigen::VectorXd::Zero(1);
        A = Eigen::MatrixXd::Identity(1, 1);
    }

    /// Algorithm evolve method (juice implementation of the algorithm)
    /**
     *
     * Evolves the population for a maximum number of generations, until one of
     * tolerances set on the population flatness (x_tol, f_tol) are met.
     *
     * @param pop population to be evolved
     * @return evolved population
     * @throws std::invalid_argument if the problem is multi-objective or constrained
     * @throws std::invalid_argument if the problem is unbounded
     * @throws std::invalid_argument if the population size is not at least 5
     */
    population evolve(population pop) const
    {
        // We store some useful variables
        const auto &prob = pop.get_problem(); // This is a const reference, so using set_seed for example will not be
                                              // allowed.
        auto dim = prob.get_nx();
        const auto bounds = prob.get_bounds();
        const auto &lb = bounds.first;
        const auto &ub = bounds.second;
        auto lam = pop.size();
        auto prob_f_dimension = prob.get_nf();
        auto fevals0 = prob.get_fevals(); // discount for the already made fevals
        auto count = 1u;                  // regulates the screen output

        // PREAMBLE--------------------------------------------------
        // Checks on the problem type
        if (prob.get_nc() != 0u) {
            pagmo_throw(std::invalid_argument, "Non linear constraints detected in " + prob.get_name() + " instance. "
                                                   + get_name() + " cannot deal with them");
        }
        if (prob_f_dimension != 1u) {
            pagmo_throw(std::invalid_argument, "Multiple objectives detected in " + prob.get_name() + " instance. "
                                                   + get_name() + " cannot deal with them");
        }
        if (lam < 4u) {
            pagmo_throw(std::invalid_argument, get_name() + " needs at least 5 individuals in the population, "
                                                   + std::to_string(lam) + " detected");
        }
        // Get out if there is nothing to do.
        if (m_gen == 0u) {
            return pop;
        }
        // -----------------------------------------------------------

        // No throws, all valid: we clear the logs
        m_log.clear();

        // -------------------------------------------------------//
        // HERE WE PREPARE AND DEFINE VARIOUS PARAMETERS          //
        // -------------------------------------------------------//
        // Initializing the random number generators
        std::uniform_real_distribution<double> randomly_distributed_number(0., 1.); // to generate a number in [0, 1)
        std::normal_distribution<double> normally_distributed_number(0., 1.);
        // Initialize default values for the learning rates
        double N = static_cast<double>(dim);
        double eta_mu(m_eta_mu), eta_sigma(m_eta_sigma), eta_b(m_eta_b);
        if (eta_mu = -1) {
            eta_mu = 1.;
        }
        double common_default = 0.6 * (3. + std::log(N)) / (N * std::sqrt(N));
        if (eta_sigma = -1) {
            eta_sigma = common_default;
        }
        if (eta_b = -1) {
            eta_b = common_default;
        }
        // Initialize the utility function u
        std::vector<double> u(lam);
        for (decltype(u.size()) i = 0u; i < u.size(); ++i) {
            u[i] = std::max(0., std::log(lam / 2. + 1.) - std::log(i + 1));
        }
        double sum = 0.;
        for (decltype(u.size()) i = 0u; i < u.size(); ++i) {
            sum += u[i];
        }
        for (decltype(u.size()) i = 0u; i < u.size(); ++i) {
            u[i] = u[i] / sum - 1. / lam; // without the uniform baselines seems to improve (get rid of 1/lam?)
        }
        // If m_memory is false we redefine mutable members erasing the memory of past calls.
        // This is also done if the problem dimension has changed
        if ((mean.size() != dim) || (m_memory == false)) {
            if (m_sigma0 == -1) {
                sigma = 1.;
            } else {
                sigma = m_sigma0;
            }
            A = Eigen::MatrixXd::Identity(_(dim), _(dim));
            // The diagonal of the initial covariance matrix A defines the search width in all directions.
            // By default we set this to be sigma times the witdh of the box bounds or 1e-6 if too small.
            for (decltype(dim) j = 0u; j < dim; ++j) {
                A(_(j), _(j)) = std::max((ub[j] - lb[j]), 1e-6) * sigma;
            }
            mean.resize(_(dim));
            auto idx_b = pop.best_idx();
            for (decltype(dim) i = 0u; i < dim; ++i) {
                mean(_(i)) = pop.get_x()[idx_b][i];
            }
        }
        // This will hold in the eigen data structure the sampled population
        Eigen::VectorXd tmp = Eigen::VectorXd::Zero(_(dim));
        auto z = std::vector<Eigen::VectorXd>(lam, tmp);
        auto x = std::vector<Eigen::VectorXd>(lam, tmp);
        // Temporary container
        vector_double dumb(dim, 0.);

        if (m_verbosity > 0u) {
            std::cout << "xNES 4 PaGMO: " << std::endl;
            print("eta_mu: ", eta_mu, " - eta_sigma: ", eta_sigma, " - eta_b: ", eta_b, " - sigma0: ", sigma, "\n");
            print("utilities: ", u, "\n");
        }

        // ----------------------------------------------//
        // HERE WE START THE JUICE OF THE ALGORITHM      //
        // ----------------------------------------------//
        for (decltype(m_gen) gen = 1u; gen <= m_gen; ++gen) {
            // 0 -If the problem is stochastic change seed first
            if (prob.is_stochastic()) {
                // change the problem seed. This is done via the population_set_seed method as prob.set_seed
                // is forbidden being prob a const ref.
                pop.get_problem().set_seed(std::uniform_int_distribution<unsigned int>()(m_e));
            }
            // 1 - We generate lam new individuals uing the current probability distribution
            for (decltype(lam) i = 0u; i < lam; ++i) {
                // 1a - we create a randomly normal distributed vector
                for (decltype(dim) j = 0u; j < dim; ++j) {
                    z[i](_(j)) = normally_distributed_number(m_e);
                }
                // 1b - and store its transformed value in the new chromosomes
                x[i] = mean + A * z[i];
                // We fix the bounds (only x is changed, not z)
                bool changed = false;
                for (decltype(dim) j = 0u; j < dim; ++j) {
                    if ((x[i][_(j)] < lb[j]) || (x[i][_(j)] > ub[j])) {
                        x[i][_(j)] = lb[j] + randomly_distributed_number(m_e) * (ub[j] - lb[j]);
                        changed = true;
                    }
                }
                for (decltype(dim) j = 0u; j < dim; ++j) {
                    dumb[j] = x[i](_(j));
                }
                pop.set_x(i, dumb);
            }

            // 2 - Check the exit conditions (every 10 generations) and logs
            if (gen % 10u == 0u) {
                // Exit condition on xtol
                if ((A * z[0]).norm() < m_xtol) {
                    if (m_verbosity > 0u) {
                        std::cout << "Exit condition -- xtol < " << m_xtol << std::endl;
                    }
                    return pop;
                }
                // Exit condition on ftol
                auto idx_b = pop.best_idx();
                auto idx_w = pop.worst_idx();
                double delta_f = std::abs(pop.get_f()[idx_b][0] - pop.get_f()[idx_w][0]);
                if (delta_f < m_ftol) {
                    if (m_verbosity) {
                        std::cout << "Exit condition -- ftol < " << m_ftol << std::endl;
                    }
                    return pop;
                }
            }
            // 2bis - Logs and prints (verbosity modes > 1: a line is added every m_verbosity generations)
            if (m_verbosity > 0u) {
                // Every m_verbosity generations print a log line
                if (gen % m_verbosity == 1u || m_verbosity == 1u) {
                    // The population flattness in chromosome
                    auto dx = (A * z[0]).norm();
                    // The population flattness in fitness
                    auto idx_b = pop.best_idx();
                    auto idx_w = pop.worst_idx();
                    auto df = std::abs(pop.get_f()[idx_b][0] - pop.get_f()[idx_w][0]);
                    // Every 50 lines print the column names
                    if (count % 50u == 1u) {
                        print("\n", std::setw(7), "Gen:", std::setw(15), "Fevals:", std::setw(15),
                              "Best:", std::setw(15), "dx:", std::setw(15), "df:", std::setw(15), "sigma:", '\n');
                    }
                    print(std::setw(7), gen, std::setw(15), prob.get_fevals() - fevals0, std::setw(15),
                          pop.get_f()[idx_b][0], std::setw(15), dx, std::setw(15), df, std::setw(15), sigma, '\n');
                    ++count;
                    // Logs
                    m_log.emplace_back(gen, prob.get_fevals() - fevals0, pop.get_f()[idx_b][0], dx, df, sigma);
                }
            }

            // 3 - We sort the population
            std::vector<vector_double::size_type> s_idx(lam);
            std::iota(s_idx.begin(), s_idx.end(), vector_double::size_type(0u));
            std::sort(s_idx.begin(), s_idx.end(), [&pop](vector_double::size_type a, vector_double::size_type b) {
                return pop.get_f()[a][0] < pop.get_f()[b][0];
            });
            // 4 - We update the distribution parameters mu, sigma and B following the xnes rules
            Eigen::MatrixXd I = Eigen::MatrixXd::Identity(dim, dim);
            Eigen::VectorXd d_center = u[0] * z[s_idx[0]];
            for (decltype(u.size()) i = 1u; i < u.size(); ++i) {
                d_center += u[i] * z[s_idx[i]];
            }
            Eigen::MatrixXd cov_grad = u[0] * (z[s_idx[0]] * z[s_idx[0]].transpose() - I);
            for (decltype(u.size()) i = 1u; i < u.size(); ++i) {
                cov_grad += u[i] * (z[s_idx[i]] * z[s_idx[i]].transpose() - I);
            }
            double cov_trace = cov_grad.trace();
            cov_grad = cov_grad - cov_trace / dim * I;
            Eigen::MatrixXd d_A = 0.5 * (eta_sigma * cov_trace / dim * I + eta_b * cov_grad);
            mean = mean + eta_mu * A * d_center;
            A = A * d_A.exp();
            sigma = sigma * std::exp(eta_sigma / 2. * cov_trace / dim); // used only for cmaes comparisons
        }
        if (m_verbosity) {
            std::cout << "Exit condition -- generations = " << m_gen << std::endl;
        }
        return pop;
    }
    /// Sets the seed
    /**
     * @param seed the seed controlling the algorithm stochastic behaviour
     */
    void set_seed(unsigned int seed)
    {
        m_e.seed(seed);
        m_seed = seed;
    };
    /// Gets the seed
    /**
     * @return the seed controlling the algorithm stochastic behaviour
     */
    unsigned int get_seed() const
    {
        return m_seed;
    }
    /// Sets the algorithm verbosity
    /**
     * Sets the verbosity level of the screen output and of the
     * log returned by get_log(). \p level can be:
     * - 0: no verbosity
     * - >0: will print and log one line each \p level generations.
     *
     * Example (verbosity 1):
     * @code{.unparsed}
     * Gen:      Fevals:          Best:            dx:            df:         sigma:
     * 51           1000    1.15409e-06     0.00205151    3.38618e-05       0.138801
     * 52           1020     3.6735e-07     0.00423372    2.91669e-05        0.13002
     * 53           1040     3.7195e-07    0.000655583    1.04182e-05       0.107739
     * 54           1060    6.26405e-08     0.00181163    3.86002e-06      0.0907474
     * 55           1080    4.09783e-09    0.000714699    3.57819e-06      0.0802022
     * 56           1100    1.77896e-08    4.91136e-05    9.14752e-07       0.075623
     * 57           1120    7.63914e-09    0.000355162    1.10134e-06      0.0750457
     * 58           1140    1.35199e-09    0.000356034    2.65614e-07      0.0622128
     * 59           1160    8.24796e-09    0.000695454    1.14508e-07        0.04993
     * @endcode
     * Gen, is the generation number, Fevals the number of function evaluation used, Best is the best fitness
     * function currently in the population, dx is the norm of the distance to the population mean of
     * the mutant vectors, df is the population flatness evaluated as the distance between the fitness
     * of the best and of the worst individual and sigma is the current step-size
     *
     * @param level verbosity level
     */
    void set_verbosity(unsigned int level)
    {
        m_verbosity = level;
    };
    /// Gets the verbosity level
    /**
     * @return the verbosity level
     */
    unsigned int get_verbosity() const
    {
        return m_verbosity;
    }
    /// Gets the generations
    /**
     * @return the number of generations to evolve for
     */
    unsigned int get_gen() const
    {
        return m_gen;
    }
    /// Algorithm name
    /**
     * One of the optional methods of any user-defined algorithm (UDA).
     *
     * @return a string containing the algorithm name
     */
    std::string get_name() const
    {
        return "xNES: Exponential Natural Evolution Strategies";
    }
    /// Extra informations
    /**
     * One of the optional methods of any user-defined algorithm (UDA).
     *
     * @return a string containing extra informations on the algorithm
     */
    std::string get_extra_info() const
    {
        std::ostringstream ss;
        stream(ss, "\tGenerations: ", m_gen);
        stream(ss, "\n\teta_mu: ");
        if (m_eta_mu == -1)
            stream(ss, "auto");
        else
            stream(ss, m_eta_mu);
        stream(ss, "\n\tm_eta_sigma: ");
        if (m_eta_sigma == -1)
            stream(ss, "auto");
        else
            stream(ss, m_eta_sigma);
        stream(ss, "\n\tm_eta_b: ");
        if (m_eta_b == -1)
            stream(ss, "auto");
        else
            stream(ss, m_eta_b);
        stream(ss, "\n\tcmu: ");
        stream(ss, "\n\tsigma0: ", m_sigma0);
        stream(ss, "\n\tStopping xtol: ", m_xtol);
        stream(ss, "\n\tStopping ftol: ", m_ftol);
        stream(ss, "\n\tMemory: ", m_memory);
        stream(ss, "\n\tVerbosity: ", m_verbosity);
        stream(ss, "\n\tSeed: ", m_seed);
        return ss.str();
    }
    /// Get log
    /**
     * A log containing relevant quantities monitoring the last call to evolve. Each element of the returned
     * <tt>std::vector</tt> is a cmaes::log_line_type containing: Gen, Fevals, Best, dx, df, sigma
     * as described in cmaes::set_verbosity
     * @return an <tt>std::vector</tt> of cmaes::log_line_type containing the logged values Gen, Fevals, Best, dx, df,
     * sigma
     */
    const log_type &get_log() const
    {
        return m_log;
    }
    /// Object serialization
    /**
     * This method will save/load \p this into the archive \p ar.
     *
     * @param ar target archive.
     *
     * @throws unspecified any exception thrown by the serialization of the UDP and of primitive types.
     */
    template <typename Archive>
    void serialize(Archive &ar)
    {
        ar(m_gen, m_eta_mu, m_eta_sigma, m_eta_b, m_sigma0, m_ftol, m_xtol, m_memory, sigma, mean, A, m_e, m_seed,
           m_verbosity, m_log);
    }

private:
    // Eigen stores indexes and sizes as signed types, while PaGMO
    // uses STL containers thus sizes and indexes are unsigned. To
    // make the conversion as painless as possible this template is provided
    // allowing, for example, syntax of the type D(_(i),_(j)) to adress an Eigen matrix
    // when i and j are unsigned
    template <typename I>
    static Eigen::DenseIndex _(I n)
    {
        return static_cast<Eigen::DenseIndex>(n);
    }
    // "Real" data members
    unsigned int m_gen;
    double m_eta_mu;
    double m_eta_sigma;
    double m_eta_b;
    double m_sigma0;
    double m_ftol;
    double m_xtol;
    bool m_memory;

    // "Memory" data members (these are adapted during each evolve call and may be remembered if m_memory is true)
    mutable double sigma;
    mutable Eigen::VectorXd mean;
    mutable Eigen::MatrixXd A;

    // "Common" data members
    mutable detail::random_engine_type m_e;
    unsigned int m_seed;
    unsigned int m_verbosity;
    mutable log_type m_log;
};

} // namespace pagmo

PAGMO_REGISTER_ALGORITHM(pagmo::xnes)

#else // PAGMO_WITH_EIGEN3

#error The xnes.hpp header was included, but pagmo was not compiled with eigen3 support

#endif // PAGMO_WITH_EIGEN3

#endif
