#ifndef QUANT_PDE_MODULES_HJBQVI_HPP
#define QUANT_PDE_MODULES_HJBQVI_HPP

#include <algorithm>        // std::max
#include <array>            // std::array
#include <chrono>           // std::chrono
#include <cmath>            // std::nan
#include <functional>       // std::function
#include <iostream>         // std::ostream, std::cout
#include <iomanip>          // std::setw
#include <initializer_list> // std::initializer_list
#include <limits>           // std::numeric_limits
#include <memory>           // std::forward, std::unique_ptr
#include <numeric>          // std::accumulate
#include <string>           // std::string
#include <tuple>            // std::make_tuple
#include <vector>           // std::vector

////////////////////////////////////////////////////////////////////////////////

// NOTE: Uncomment the lines below to enable iterated optimal stopping
//#define QUANT_PDE_MODULES_HJBQVI_ITERATED_OPTIMAL_STOPPING
//#define QUANT_PDE_PERMISSIVE

// TODO: Change names to camelCase to match the rest of the Core code
// TODO: Delete assignment operator

namespace QuantPDE {

namespace Modules {

struct HJBQVIControlMethod {
	static constexpr char SEMI_LAGRANGIAN  = 1;
	static constexpr char EXPLICIT_IMPULSE = 1 << 1;
	static constexpr char EXPLICIT_CONTROL = SEMI_LAGRANGIAN
			| EXPLICIT_IMPULSE;
	static constexpr char PENALTY_METHOD = 1 << 2;
	static constexpr char DIRECT_CONTROL = 1 << 3;
	static constexpr char ITERATED_OPTIMAL_STOPPING = DIRECT_CONTROL
			| (1 << 4);
};

struct HJBQVISolver {
	static constexpr char BICGSTAB = 1;
	static constexpr char SPARSE_LU = 1 << 1;
};

template <
	Index Dimension = 1,
	Index StochasticControlDimension = 1,
	Index ImpulseControlDimension = 1
>
class HJBQVI {

////////////////////////////////////////////////////////////////////////////////
// Member structs
////////////////////////////////////////////////////////////////////////////////

public:

struct Result {

	const RectilinearGrid<Dimension> spatial_grid;
	const RectilinearGrid<StochasticControlDimension>
			stochastic_control_grid;
	const RectilinearGrid<ImpulseControlDimension>
			impulse_control_grid;

	const Vector solution_vector;
	/*const*/ Vector stochastic_control_vector[StochasticControlDimension];
	/*const*/ Vector impulse_control_vector[ImpulseControlDimension];

	const int timesteps;
	const Real scaling_factor;
	const Real iteration_tolerance;
	const Real mean_inner_iterations;
	const Real mean_solver_iterations;

	const Real execution_time_seconds;

	Result(
		const RectilinearGrid<Dimension> &spatial_grid,
		const RectilinearGrid<StochasticControlDimension>
				&stochastic_control_grid,
		const RectilinearGrid<ImpulseControlDimension>
				&impulse_control_grid,

		const Vector &solution_vector,
		const Vector (&stochastic_control_vector)[
				StochasticControlDimension],
		const Vector (&impulse_control_vector)[ImpulseControlDimension],

		int timesteps,
		Real scaling_factor,
		Real iteration_tolerance,
		Real mean_inner_iterations,
		Real mean_solver_iterations,

		Real execution_time_seconds
	) noexcept :
		spatial_grid(spatial_grid),
		stochastic_control_grid(stochastic_control_grid),
		impulse_control_grid(impulse_control_grid),

		solution_vector(solution_vector),
		/*stochastic_control_vector(stochastic_control_vector),
		impulse_control_vector(impulse_control_vector),*/

		timesteps(timesteps),
		scaling_factor(scaling_factor),
		iteration_tolerance(iteration_tolerance),
		mean_inner_iterations(mean_inner_iterations),
		mean_solver_iterations(mean_solver_iterations),

		execution_time_seconds(execution_time_seconds)
	{
		for(Index i = 0; i < StochasticControlDimension; ++i) {
			this->stochastic_control_vector[i] =
					stochastic_control_vector[i];
		}

		for(Index i = 0; i < ImpulseControlDimension; ++i) {
			this->impulse_control_vector[i] =
					impulse_control_vector[i];
		}
	}

};

private:

struct ControlledOperator final : public RawControlledLinearSystem<
	Dimension,
	StochasticControlDimension
> {

	const HJBQVI hjbqvi;
	RectilinearGrid<Dimension> refined_spatial_grid;

	int offsets[Dimension];

	template <typename H, typename R>
	ControlledOperator(
		H &hjbqvi,
		R &refined_spatial_grid
	) noexcept :
		hjbqvi(hjbqvi),
		refined_spatial_grid(refined_spatial_grid)
	{
		// Space between ticks
		offsets[0] = 1;
		for(int d = 1; d < Dimension; ++d) {
			offsets[d] = offsets[d-1]
					* refined_spatial_grid[d-1].size();
		}
	}

	virtual Matrix A(Real time) {
		Matrix A = refined_spatial_grid.matrix();

		// Reserve nonzero entries per row
		// Each dimension requires at most 2 cells (i.e. i-1 and i+1)
		A.reserve(
			IntegerVector::Constant(
				refined_spatial_grid.size(),
				1 + 2 * Dimension
			)
		);

		// Control as a vector
		Vector q[StochasticControlDimension];
		for(int d = 0; d < StochasticControlDimension; ++d) {
			q[d] = hjbqvi.semi_lagrangian() ?
					refined_spatial_grid.zero()
					: this->control(d);
		}

		// Iterate through points on grid
		int i[Dimension];
		Real args[1+Dimension+StochasticControlDimension];
		for(int row = 0; row < refined_spatial_grid.size(); ++row) {
			Real total = 0.;

			// Get coordinates of point
			args[0] = time; // Time
			for(int d = 0; d < Dimension; ++d) {
				i[d] = (row / offsets[d]) %
						refined_spatial_grid[d].size();
				args[1+d] = refined_spatial_grid[d][i[d]];
			}
			for(int d = 0; d < StochasticControlDimension; ++d) {
				args[1+Dimension+d] = q[d](row); // Control
			}

			for(int d = 0; d < Dimension; ++d) {
				// Left boundary
				if(i[d] == 0) {
					if(hjbqvi.lboundary[d] != nullptr) {
						total += hjbqvi.lboundary[d](
							hjbqvi,
							refined_spatial_grid,
							d, // index
							args, i, offsets,
							A, row
						);
					}
					continue;
				}

				// Right boundary
				if(i[d] == refined_spatial_grid[d].size() - 1) {
					if(hjbqvi.rboundary[d] != nullptr) {
						total += hjbqvi.rboundary[d](
							hjbqvi,
							refined_spatial_grid,
							d, // index
							args, i, offsets,
							A, row
						);
					}
					continue;
				}

				const Axis &x = refined_spatial_grid[d];

				const Real
					dxb = x[ i[d]     ] - x[ i[d] - 1 ],
					dxc = x[ i[d] + 1 ] - x[ i[d] - 1 ],
					dxf = x[ i[d] + 1 ] - x[ i[d]     ]
				;

				// Get volatility
				const Real v = packAndCall<1+Dimension>(
					hjbqvi.volatility[d],
					args
				);

				// Get drifts
				/*Real mu = packAndCall<1+Dimension>(
					hjbqvi.uncontrolled_drift[d],
					args
				);*/
				Real mu = 0.;
				if(!hjbqvi.semi_lagrangian()) {
					mu += packAndCall<
						1
						+Dimension
						+StochasticControlDimension
					>(
						hjbqvi.controlled_drift[d],
						args
					);
				}

				const Real vv = v * v;

				const Real alpha_common = vv / dxb / dxc;
				const Real  beta_common = vv / dxf / dxc;

				// Central
				Real alpha = alpha_common - mu / dxc;
				Real  beta =  beta_common + mu / dxc;
				if(alpha < 0.) {
					alpha = alpha_common;
					 beta =  beta_common + mu / dxf;
				} else if(beta < 0.) {
					alpha = alpha_common - mu / dxb;
					 beta =  beta_common;
				}

				A.insert(row, row - offsets[d]) = -alpha;
				A.insert(row, row + offsets[d]) = - beta;

				total += alpha + beta;
			}

			const Real rho = packAndCall<1+Dimension>(
				hjbqvi.discount,
				args
			);

			A.insert(row, row) = total + rho;
		}

		A.makeCompressed();
		return A;
	}

	virtual Vector b(Real time) {
		Vector b = refined_spatial_grid.vector();

		// Control as a vector
		Vector q[StochasticControlDimension];
		for(int d = 0; d < StochasticControlDimension; ++d) {
			q[d] = hjbqvi.semi_lagrangian() ?
					refined_spatial_grid.zero()
					: this->control(d);
		}

		// Iterate through points on grid
		int i[Dimension];
		Real args[1+Dimension+StochasticControlDimension];
		for(int row = 0; row < refined_spatial_grid.size(); ++row) {

			// Get coordinates of point
			args[0] = time; // Time
			for(int d = 0; d < Dimension; ++d) {
				i[d] = (row / offsets[d]) %
						refined_spatial_grid[d].size();
				args[1+d] = refined_spatial_grid[d][i[d]];
			}
			for(int d = 0; d < StochasticControlDimension; ++d) {
				args[1+Dimension+d] = q[d](row); // Control
			}

			// Get flows
			Real controlled = packAndCall<
				1
				+Dimension
				+StochasticControlDimension
			>(
				hjbqvi.controlled_continuous_flow,
				args
			);
			/*Real uncontrolled = packAndCall<1+Dimension>(
				hjbqvi.uncontrolled_continuous_flow,
				args
			);*/

			b(row) = (hjbqvi.semi_lagrangian() ? 0 : controlled);
					/*+ uncontrolled;*/
		}

		return b;
	}

	virtual bool isATheSame() const {
		// TODO: Do this automagically
		return hjbqvi.semi_lagrangian()
				&& hjbqvi.time_independent_coefficients;
	}

};

class ExplicitEvent : public EventBase {

	const HJBQVI &hjbqvi;
	const RectilinearGrid<Dimension> &refined_spatial_grid;
	const RectilinearGrid<StochasticControlDimension>
			&refined_stochastic_control_grid;
	const RectilinearGrid<ImpulseControlDimension>
			&refined_impulse_control_grid;
	Vector (&stochastic_control_vector)[StochasticControlDimension];
	Vector (&impulse_control_vector)[ImpulseControlDimension];
	Real time, dt;
	std::vector<bool> &mask;

	int offsets[Dimension];

	template <typename V>
	Vector _doEvent(V &&vector) const {

		mask.clear();

		Vector best = refined_spatial_grid.vector();

		PiecewiseLinear<Dimension> u(refined_spatial_grid, vector);

		Real args[
			1
			+Dimension
			+(
				(StochasticControlDimension
						> ImpulseControlDimension)
				? StochasticControlDimension
				: ImpulseControlDimension
			)
		];
		int i[Dimension];
		for(int row = 0; row < refined_spatial_grid.size(); ++row) {

		////////////////////////////////////////////////////////////////
		// begin row loop
		////////////////////////////////////////////////////////////////

		// Get coordinates of point
		args[0] = time; // Time
		for(int d = 0; d < Dimension; ++d) {
			i[d] = (row / offsets[d]) %
					refined_spatial_grid[d].size();
			args[1+d] = refined_spatial_grid[d][i[d]];
		}

		Real a = -std::numeric_limits<Real>::infinity();
		if(hjbqvi.semi_lagrangian()) {

			// Find optimal control
			for(auto node : refined_stochastic_control_grid) {

				for(
					int d = 0;
					d < StochasticControlDimension;
					++d
				) {
					args[1+Dimension+d] = node[d];
				}

				bool skip = false;
				std::array<Real, Dimension> new_state;
				for(int d = 0; d < Dimension; ++d) {
					const Real m = packAndCall<
						1
						+Dimension
						+StochasticControlDimension
					>(
						hjbqvi.controlled_drift[d],
						args
					);
					new_state[d] = args[1+d] + m * dt;

					const Axis &axis =
							refined_spatial_grid[d];
					if( hjbqvi.drop_semi_lagrangian_off_grid
							&& ( new_state[d]
							< axis[0] ||
							new_state[d] > axis[
							axis.size()-1] ) ) {
						skip = true;
						break;
					}
				}
				if(skip) { continue; }

				const Real flow = packAndCall<
					1
					+Dimension
					+StochasticControlDimension
				>(
					hjbqvi.controlled_continuous_flow,
					args
				);

				const Real new_value =
					u.interpolate(new_state)
					+ flow * dt
				;

				if(new_value > a) {
					a = new_value;
					for(
						int d = 0;
						d < StochasticControlDimension;
						++d
					) {
						stochastic_control_vector[d](
								row) = args[
								1+Dimension+d];
					}
				}
			}

		} else {
			a = vector(row);
		}


		Real b = -std::numeric_limits<Real>::infinity();
		if(hjbqvi.explicit_impulse()) {

			// Find optimal control
			for(auto node : refined_impulse_control_grid) {

				for(
					int d = 0;
					d < ImpulseControlDimension;
					++d
				) {
					args[1+Dimension+d] = node[d];
				}

				std::array<Real, Dimension> new_state;
				for(int d = 0; d < Dimension; ++d) {
					new_state[d] = packAndCall<
						1
						+Dimension
						+ImpulseControlDimension
					>(
						hjbqvi.transition[d],
						args
					);
				}

				const Real flow = packAndCall<
						1
						+Dimension
						+ImpulseControlDimension
				>(
					hjbqvi.impulse_flow,
					args
				);

				const Real new_value =
					u.interpolate(new_state)
					+ flow
				;

				if(new_value > b) {
					b = new_value;
					for(
						int d = 0;
						d < ImpulseControlDimension;
						++d
					) {
						impulse_control_vector[d](row) =
							args[1+Dimension+d]
						;
					}
				}
			}

		}

		if(a >= b) {
			mask.push_back(false);
			best(row) = a;
		} else {
			mask.push_back(true);
			best(row) = b;
		}

		////////////////////////////////////////////////////////////////
		// end row loop
		////////////////////////////////////////////////////////////////

		}

		return best;

	}

	virtual Vector doEvent(const Vector &vector) const {
		return _doEvent(vector);
	}

	virtual Vector doEvent(Vector &&vector) const {
		return _doEvent(std::move(vector));
	}

public:

	template <typename H, typename R, typename S, typename I>
	ExplicitEvent(
		H &hjbqvi,
		R &refined_spatial_grid,
		S &refined_stochastic_control_grid,
		I &refined_impulse_control_grid,
		Vector (&stochastic_control_vector)[StochasticControlDimension],
		Vector (&impulse_control_vector)[ImpulseControlDimension],
		Real time,
		Real dt,
		std::vector<bool> &mask
	) noexcept :
		hjbqvi(hjbqvi),
		refined_spatial_grid(refined_spatial_grid),
		refined_stochastic_control_grid(
				refined_stochastic_control_grid),
		refined_impulse_control_grid(refined_impulse_control_grid),
		stochastic_control_vector(stochastic_control_vector),
		impulse_control_vector(impulse_control_vector),
		time(time),
		dt(dt),
		mask(mask)
	{
		// Space between ticks
		offsets[0] = 1;
		for(int d = 1; d < Dimension; ++d) {
			offsets[d] = offsets[d-1]
					* refined_spatial_grid[d-1].size();
		}
	}

};

////////////////////////////////////////////////////////////////////////////////
// Methods
////////////////////////////////////////////////////////////////////////////////

public:

Result solve(int refinement = 0) const {

	const bool finite_horizon =
			this->expiry < std::numeric_limits<Real>::infinity();

	const bool variable_timesteps = target_timestep_relative_error > 0.;

	// Refine grids
	auto refined_spatial_grid = this->spatial_grid.refined(refinement);

	auto refined_stochastic_control_grid =
			this->stochastic_control_grid.refined(
			refine_stochastic_control_grid ? refinement : 0);

	auto refined_impulse_control_grid = this->impulse_control_grid.refined(
			refine_impulse_control_grid ? refinement : 0);

	Vector stochastic_control_vector[StochasticControlDimension];
	Vector impulse_control_vector[ImpulseControlDimension];

	for(int d = 0; d < StochasticControlDimension; ++d) {
		stochastic_control_vector[d] = refined_spatial_grid.vector();
	}

	for(int d = 0; d < ImpulseControlDimension; ++d) {
		impulse_control_vector[d] = refined_spatial_grid.vector();
	}

	std::vector<bool> mask;
	mask.reserve(refined_spatial_grid.size());

	// Refine parameters
	int timesteps = this->timesteps;
	Real scaling_factor = this->scaling_factor;
	Real iteration_tolerance = this->iteration_tolerance;
	Real target = this->target_timestep_relative_error;
	for(int i = 0; i < refinement; ++i) {
		timesteps *= 2;
		target /= 2;
	}

	const Real dt = this->expiry / timesteps;
	Real scaling_factor_dt = scaling_factor * dt;

	ToleranceIteration tolerance_iteration(iteration_tolerance);

	ControlledOperator controlled_operator(
		*this,
		refined_spatial_grid
	);

	MinPolicyIteration<
		Dimension,
		StochasticControlDimension
	> stochastic_policy(
		refined_spatial_grid,
		refined_stochastic_control_grid,
		controlled_operator
	);

	Impulse<
		Dimension,
		ImpulseControlDimension
	> impulse(
		refined_spatial_grid,
		this->impulse_flow,
		this->transition
	);

	MinPolicyIteration<
		Dimension,
		ImpulseControlDimension
	> impulse_policy(
		refined_spatial_grid,
		refined_impulse_control_grid,
		impulse
	);

	stochastic_policy.setIteration(tolerance_iteration);
	impulse_policy.setIteration(tolerance_iteration);

	std::unique_ptr<ReverseTimeIteration> stepper;
	if(finite_horizon) {
		if(variable_timesteps) {
			stepper = std::unique_ptr<ReverseTimeIteration>(
				(ReverseTimeIteration*)
				new ReverseVariableStepper(
					0.,
					this->expiry,
					this->expiry / timesteps,
					target
				)
			);
		} else {
			stepper = std::unique_ptr<ReverseTimeIteration>(
				(ReverseTimeIteration*)
				new ReverseConstantStepper(
					0.,
					this->expiry,
					this->expiry / timesteps
				)
			);
		}

		if(!this->explicit_control()) {
			stepper->setInnerIteration(tolerance_iteration);
		}
	}

	LinearSystem *discretize;
	if(this->semi_lagrangian()) {
		// Semi-Lagrangian
		discretize = &controlled_operator;
	} else {
		discretize = &stochastic_policy;
	}

	typedef ReverseBDFOne Discretization;

	Discretization discretization(
		refined_spatial_grid,
		*discretize
	);

	// Finite or infinite horizon
	Iteration *iteration;
	IterationNode *penalized;
	if(finite_horizon) {
		discretization.setIteration(*stepper);

		iteration = stepper.get();
		penalized = &discretization;
	} else {
		iteration = &tolerance_iteration;
		penalized = &stochastic_policy;
	}

	const bool direct = this->iterated_optimal_stopping()
			|| this->direct_control();

	// Penalty method
	MinPenaltyMethod penalty(
		refined_spatial_grid,
		*penalized,
		impulse_policy,
		scaling_factor_dt,
		direct ? true : false // Direct
		#ifdef QUANT_PDE_MODULES_HJBQVI_ITERATED_OPTIMAL_STOPPING
		, this->iterated_optimal_stopping() ? true : false  // Obstacle
		#endif
	);
	penalty.setIteration(tolerance_iteration);

	// Pick root
	IterationNode *root;
	if(this->explicit_impulse()) {
		// Explicit impulse
		root = penalized;
	} else {
		// Implicit impulse
		root = &penalty;
	}

	// Add events
	if(!this->fully_implicit()) {
		for(int e = 0; e < timesteps; ++e) {
			const Real time = e * dt;

			stepper->add(
				time,
				std::unique_ptr<EventBase>(
					new ExplicitEvent(
						*this,
						refined_spatial_grid,
						refined_stochastic_control_grid,
						refined_impulse_control_grid,
						stochastic_control_vector,
						impulse_control_vector,
						time,
						dt,
						mask
					)
				)
			);
		}
	}

	// Linear system solver
	LinearSolver *solver;
	if(this->bicgstab()) {
		solver = new BiCGSTABSolver;
	} else if(this->sparse_lu()) {
		solver = new SparseLUSolver;
	}

	// Bind exit function to expiry time to get payoff
	auto cauchy_data = curry<Dimension+1>(
		this->exit_function,
		this->expiry
	);

	// Solution
	Vector solution_vector;

	// Timing
	Real seconds;
	auto start = std::chrono::steady_clock::now();

	// Solve
	#ifdef QUANT_PDE_MODULES_HJBQVI_ITERATED_OPTIMAL_STOPPING
	if(!this->iterated_optimal_stopping()) {
	#endif

		auto u = iteration->solve(
			refined_spatial_grid,
			cauchy_data,
			*root,
			*solver
		);
		solution_vector = refined_spatial_grid.image(u);

	#ifdef QUANT_PDE_MODULES_HJBQVI_ITERATED_OPTIMAL_STOPPING
	} else {

		// Iterated optimal stopping

		Vector *u_this = new Vector[timesteps+1];
		Vector *u_last = new Vector[timesteps+1];

		tolerance_iteration.history = new Iteration::CB(1);
		stepper           ->history = new Iteration::CB(1);

		// k loop
		bool converged, first = true;
		tolerance_iteration.its.push_back(0);
		do {
			// Solution at the expiry
			u_this[0] = refined_spatial_grid.image(cauchy_data);

			// Timestep (n) loop
			converged = true;
			for(int n = 1; n <= timesteps; ++n) {
				// Explicit and implicit times
				const Real t_explicit = expiry * (1.
						- (double) (n-1) / timesteps);
				const Real t_implicit = expiry * (1.
						- (double) n / timesteps);

				// What to use as the previous iterand?
				Vector *previous =
					first ?
						  &u_this[n-1]
						: &u_last[n]
				;

				// Previous iterand
				tolerance_iteration.history->clear();
				tolerance_iteration.history->push(
					std::make_tuple(
						t_implicit,
						*previous
					)
				);
				stepper->history->clear();
				stepper->history->push(
					std::make_tuple(
						t_explicit,
						u_this[n-1]
					)
				);

				tolerance_iteration.implicitTime = t_implicit;
				stepper           ->implicitTime = t_implicit;

				// Start nodes
				tolerance_iteration.startNodes();
				stepper           ->startNodes();

				// If we are on the first iteration, we solve
				// the simpler nonvariational problem
				IterationNode *tmp =
					first ?
						  &discretization
						: root
				;

				// Solve Ax=b
				solver->initialize(tmp->A(t_implicit));
				u_this[n] = solver->solve(
					tmp->b(t_implicit),
					*previous
				);

				// End nodes
				stepper           ->endNodes();
				tolerance_iteration.endNodes();

				// Compare
				if(!first && converged) {
					Vector &a = u_this[n];
					Vector &b = u_last[n];

					const Real tmp = relativeError(a, b);

					if(tmp > iteration_tolerance) {
						converged = false;
					}
				}

			}

			// For debugging
			/*
			PiecewiseLinear<Dimension> V(
				refined_spatial_grid,
				u_this[timesteps]
			);
			std::cout << V(0.) << std::endl;
			*/

			// No longer on the first outer iteration
			if(first) {
				converged = false;
				first = false;
			}

			// Swap solutions
			Vector *tmp = u_this;
			u_this = u_last;
			u_last = tmp;

			// Increment number of iterations
			++(tolerance_iteration.its.back());
		} while(!converged);

		// Save solution_vector
		solution_vector = u_last[timesteps];

		// Housekeeping
		delete [] u_this;
		u_this = nullptr;
		delete [] u_last;
		u_last = nullptr;

		// The histories will be deleted after tolerance_iteration and
		// stepper go out of scope

	}
	#endif

	// Timing
	auto end = std::chrono::steady_clock::now();
	auto diff = end - start;
	seconds = std::chrono::duration<Real>(diff).count();

	// Implicit stochastic control
	if(!this->semi_lagrangian()) {
		for(int d = 0; d < StochasticControlDimension; ++d) {
			stochastic_control_vector[d] =
					controlled_operator.control(d);
		}
	}

	// Implicit impulse control
	if(!this->explicit_impulse()) {
		for(int d = 0; d < ImpulseControlDimension; ++d) {
			impulse_control_vector[d] =
					impulse.control(d);
		}
		mask = penalty.constraintMask();
	}

	// Mean iterations
	Real mean_inner_iterations = std::nan("");
	if(!this->explicit_control()) {
		auto its = tolerance_iteration.iterations();
		mean_inner_iterations = std::accumulate(its.begin(), its.end(),
				0.) / its.size();
	}

	auto its = solver->iterations();
	Real mean_solver_iterations = std::accumulate(its.begin(), its.end(),
			0.) / its.size();

	// Apply mask
	for(int i = 0; i < refined_spatial_grid.size(); ++i) {
		if(mask[i]) {
			// Impulse control IS active here
			for(int d = 0; d < StochasticControlDimension; ++d) {
				stochastic_control_vector[d](i) = std::nan("");
			}
		} else {
			// Impulse control IS NOT active here
			for(int d = 0; d < ImpulseControlDimension; ++d) {
				impulse_control_vector[d](i) = std::nan("");
			}
		}
	}

	if(this->explicit_control()) {
		scaling_factor_dt = std::nan("");
		iteration_tolerance = std::nan("");
	}

	delete solver;

	// Return
	return Result(
		refined_spatial_grid,
		refined_stochastic_control_grid,
		refined_impulse_control_grid,

		solution_vector,
		stochastic_control_vector,
		impulse_control_vector,

		finite_horizon ? timesteps : 0,
		scaling_factor_dt,
		iteration_tolerance,
		mean_inner_iterations,
		mean_solver_iterations,

		seconds
	);

}

////////////////////////////////////////////////////////////////////////////////
// Members
////////////////////////////////////////////////////////////////////////////////

	template <unsigned N, unsigned M>
	using ArrayFunction = std::array< Function<N>, M >;

	const int timesteps;
	const RectilinearGrid<Dimension> spatial_grid;

	const RectilinearGrid<StochasticControlDimension>
			stochastic_control_grid;
	const RectilinearGrid<ImpulseControlDimension> impulse_control_grid;

	const Real expiry;

	const Function<1+Dimension> discount;
	const ArrayFunction<1+Dimension, Dimension> volatility;
	const ArrayFunction<1+Dimension+StochasticControlDimension, Dimension>
			controlled_drift;
	/*const ArrayFunction<1+Dimension, Dimension> uncontrolled_drift;*/
	const Function<1+Dimension+StochasticControlDimension>
			controlled_continuous_flow;
	/*const Function<1+Dimension> uncontrolled_continuous_flow;*/
	const ArrayFunction<1+Dimension+ImpulseControlDimension, Dimension>
			transition;
	const Function<1+Dimension+ImpulseControlDimension> impulse_flow;
	const Function<1+Dimension> exit_function;

	/*const*/ char handling;
	char solver;

	/*const bool bounded_domain;*/

	/*const*/ bool refine_stochastic_control_grid;
	/*const*/ bool refine_impulse_control_grid;

	/*const*/ bool time_independent_coefficients;

	/*const*/ bool drop_semi_lagrangian_off_grid;

	const Real target_timestep_relative_error;

	const Real scaling_factor;
	const Real iteration_tolerance;

	typedef std::function< Real (
		const HJBQVI &,
		const RectilinearGrid<Dimension> &,
		Index,
		const Real (&)[1+Dimension+StochasticControlDimension],
		const Index (&)[Dimension],
		const Index (&)[Dimension],
		Matrix &, Index
	) > boundary_routine;

private:

	boundary_routine lboundary[Dimension];
	boundary_routine rboundary[Dimension];

public:

	template <typename R>
	void left_boundary(int index, R &&routine) {
		lboundary[index] = std::forward<R>(routine);
	}

	template <typename R>
	void right_boundary(int index, R &&routine) {
		rboundary[index] = std::forward<R>(routine);
	}

	bool iterated_optimal_stopping() const
	{ return handling == HJBQVIControlMethod::ITERATED_OPTIMAL_STOPPING; }

	bool penalty_method() const
	{ return handling & HJBQVIControlMethod::PENALTY_METHOD; }

	bool direct_control() const
	{ return handling & HJBQVIControlMethod::DIRECT_CONTROL; }

	bool fully_implicit() const
	{ return handling & HJBQVIControlMethod::PENALTY_METHOD || handling
			& HJBQVIControlMethod::DIRECT_CONTROL; }

	bool semi_lagrangian() const
	{ return handling & HJBQVIControlMethod::SEMI_LAGRANGIAN; }

	bool explicit_impulse() const
	{ return handling & HJBQVIControlMethod::EXPLICIT_IMPULSE; }

	bool explicit_control() const
	{ return handling & HJBQVIControlMethod::EXPLICIT_CONTROL; }

	bool bicgstab() const
	{ return solver & HJBQVISolver::BICGSTAB; }

	bool sparse_lu () const
	{ return solver & HJBQVISolver::SPARSE_LU; }

	HJBQVI(
		int timesteps,
		const std::array<Axis, Dimension> &spatial_axes,

		const std::array<Axis, StochasticControlDimension>
				&stochastic_control_axes,
		const std::array<Axis, ImpulseControlDimension>
				&impulse_control_axes,

		Real expiry,

		const Function<1+Dimension> &discount,
		const ArrayFunction<1+Dimension, Dimension> &volatility,
		const ArrayFunction<1+Dimension+StochasticControlDimension,
				Dimension> &controlled_drift,
		/*const ArrayFunction<1+Dimension, Dimension> &uncontrolled_drift,*/
		const Function<1+Dimension+StochasticControlDimension>
				&controlled_continuous_flow,
		/*const Function<1+Dimension> &uncontrolled_continuous_flow,*/
		const std::array<Function<1+Dimension+ImpulseControlDimension>,
				Dimension> &transition,
		const Function<1+Dimension+ImpulseControlDimension>
				&impulse_flow,
		const Function<1+Dimension> &exit_function

		/*
		int handling = HJBQVIControlMethod::PENALTY_METHOD,

		bool bounded_domain = false,

		bool refine_stochastic_control_grid = true,
		bool refine_impulse_control_grid = true,

		bool time_independent_coefficients = false,

		bool drop_semi_lagrangian_off_grid = false,

		Real target_timestep_relative_error = -1.,

		Real scaling_factor = 1e-2, // QuantPDE::tolerance,
		Real iteration_tolerance = QuantPDE::tolerance
		*/
	) :
		timesteps(timesteps),
		spatial_grid(spatial_axes),

		stochastic_control_grid(stochastic_control_axes),
		impulse_control_grid(impulse_control_axes),

		expiry(expiry),

		discount(discount),
		volatility(volatility),
		controlled_drift(controlled_drift),
		/*uncontrolled_drift(uncontrolled_drift),*/
		controlled_continuous_flow(controlled_continuous_flow),
		/*uncontrolled_continuous_flow(uncontrolled_continuous_flow),*/
		transition(transition),
		impulse_flow(impulse_flow),
		exit_function(exit_function),

		handling(HJBQVIControlMethod::PENALTY_METHOD),
		solver(HJBQVISolver::BICGSTAB),

		/*bounded_domain(false),*/

		refine_stochastic_control_grid(true),
		refine_impulse_control_grid(true),

		time_independent_coefficients(false),

		drop_semi_lagrangian_off_grid(false),

		target_timestep_relative_error(-1.),

		scaling_factor(1e-2),
		iteration_tolerance(QuantPDE::tolerance)
	{
		// TODO: Proper exceptions

		const bool finite_horizon =
			expiry < std::numeric_limits<Real>::infinity();

		const bool variable_timesteps =
				target_timestep_relative_error > 0.;

		/*
		if(bounded_domain) {
			// TODO: Implement
			throw "error: bounded domain not yet implemented";
		}
		*/

		if(expiry <= 0.) {
			throw "error: expiry must be positive";
		}

		if(!finite_horizon && !fully_implicit()) {
			throw "error: only an implicit method can be used for"
					" infinite-horizon problems";
		}

		if(finite_horizon && timesteps <= 0) {
			throw "error: number of timesteps must be positive";
		}

		if(
			variable_timesteps
			&& (!finite_horizon || !fully_implicit())
		) {
			throw "error: variable timestepping can only be used on"
					" finite horizon problems with"
					" fully implicit discretizations";
		}

		#ifdef QUANT_PDE_MODULES_HJBQVI_ITERATED_OPTIMAL_STOPPING
		if(
			iterated_optimal_stopping()
			&& (variable_timesteps || !finite_horizon)
		) {
			throw "error: iterated optimal stopping does not yet"
					" support variable timesteps or"
					" infinite horizon problems";
		}
		#else
		if(iterated_optimal_stopping()) {
			throw "error: not compiled with iterated optimal"
					" stopping support";
		}
		#endif
	}

	void usePenalizedScheme() { handling = HJBQVIControlMethod::PENALTY_METHOD;   }
	void useDirectControlScheme() { handling = HJBQVIControlMethod::DIRECT_CONTROL;   }
	void useSemiLagrangianScheme() { handling = HJBQVIControlMethod::EXPLICIT_CONTROL; }
	void disableStochasticControlRefinement() { refine_stochastic_control_grid = false; }
	void disableImpulseControlRefinement() { refine_impulse_control_grid = false; }
	void coefficientsAreTimeIndependent() { time_independent_coefficients = true; }
	void ignoreExtrapolatoryControls() { drop_semi_lagrangian_off_grid = true; }
	void useBiCGSTABSolver() { solver = HJBQVISolver::BICGSTAB; }
	void useSparseLUSolver() { solver = HJBQVISolver::SPARSE_LU; }

};

#define QUANT_PDE_MODULES_HJBQVI_BOUNDARY_SIGNATURE \
	const HJBQVI<Dimension, StochasticControlDimension, \
			ImpulseControlDimension> &hjbqvi, \
	const RectilinearGrid<Dimension> &refined_spatial_grid, \
	Index d, \
	const Real (&args)[1+Dimension+StochasticControlDimension], \
	const int (&i)[Dimension], \
	const Index (&offsets)[Dimension], \
	Matrix &A, Index row

template <
	Index Dimension,
	Index StochasticControlDimension,
	Index ImpulseControlDimension
>
Real HJBQVILinearBoundary(QUANT_PDE_MODULES_HJBQVI_BOUNDARY_SIGNATURE) {

	// Get drifts
	/*Real mu = packAndCall<1+Dimension>(
		hjbqvi.uncontrolled_drift[d],
		args
	);*/
	Real mu = 0.;
	if(!hjbqvi.semi_lagrangian()) {
		mu += packAndCall<
			1
			+Dimension
			+StochasticControlDimension
		>(
			hjbqvi.controlled_drift[d],
			args
		);
	}

	return - mu / args[1+d];

}

template <
	Index Dimension,
	Index StochasticControlDimension,
	Index ImpulseControlDimension
>
Real HJBQVIZeroDiffusionRightBoundary(
		QUANT_PDE_MODULES_HJBQVI_BOUNDARY_SIGNATURE) {

	const Axis &x = refined_spatial_grid[d];
	const Real dxb = x[ i[d] ] - x[ i[d] - 1 ];

	// Get drifts
	/*Real mu = packAndCall<1+Dimension>(
		hjbqvi.uncontrolled_drift[d],
		args
	);*/
	Real mu = 0.;
	if(!hjbqvi.semi_lagrangian()) {
		mu += packAndCall<
			1
			+Dimension
			+StochasticControlDimension
		>(
			hjbqvi.controlled_drift[d],
			args
		);
	}

	const Real alpha = - mu / dxb;
	A.insert(row, row - offsets[d]) = -alpha;
	return alpha;

}

#undef QUANT_PDE_HJBQVI_BOUNDARY_SIGNATURE

template <
	Index Dimension,
	Index StochasticControlDimension,
	Index ImpulseControlDimension
>
typename HJBQVI<
	Dimension,
	StochasticControlDimension,
	ImpulseControlDimension
>::Result HJBQVI_main(
	const HJBQVI<
		Dimension,
		StochasticControlDimension,
		ImpulseControlDimension
	> &hjbqvi,
	const std::array<Real, Dimension> &test_point,
	int max_refinement = 0,
	int min_refinement = 0,
	std::ostream &out = std::cout,
	bool verbose = true
) {

	out.precision(12);

	const int spacing = 23;
	auto space = [=] () { return std::setw(spacing); };

	// Headers
	out
		<< space() << "Spatial Nodes"
		<< space() << "Stochastic Ctrl Nodes"
		<< space() << "Impulse Ctrl Nodes"
		<< space() << "Timesteps"
		<< space() << "Scaling Factor"
		<< space() << "Policy Tolerance"
		<< space() << "Mean Policy Iterations"
		<< space() << "Mean Solver Iterations"
		<< space() << "Value"
		<< space() << "Change"
		<< space() << "Ratio"
		<< space() << "Execution Time (sec)"
		<< std::endl
	;

	Real
		previousValue = nan(""),
		previousChange = nan(""),
		value,
		change,
		ratio
	;

	int refinement = min_refinement;
	while(1) {
		auto result = hjbqvi.solve(refinement);

		PiecewiseLinear<Dimension> u(
			result.spatial_grid,
			result.solution_vector
		);

		// Get value of function at the test point
		value = u.interpolate(test_point);
		change = value - previousValue;
		ratio = previousChange / change;
		previousValue = value;
		previousChange = change;

		// Print
		out
			<< space() << result.spatial_grid.size()
			<< space() << result.stochastic_control_grid.size()
			<< space() << result.impulse_control_grid.size()
			<< space() << result.timesteps
			<< space() << result.scaling_factor
			<< space() << result.iteration_tolerance
			<< space() << result.mean_inner_iterations
			<< space() << result.mean_solver_iterations
			<< space() << value
			<< space() << change
			<< space() << ratio
			<< space() << result.execution_time_seconds
			<< std::endl
		;

		if(refinement++ == max_refinement) { if(verbose) {
			// Print header
			out << std::endl; // Extra spacing
			for(int d = 0; d < Dimension; ++d) {
				out << space() << ("x_" + std::to_string(d+1));
			}
			out << space() << "Value u(t=0, x)";
			for(int d = 0; d < StochasticControlDimension; ++d) {
				out << space() << ("Stochastic Control w_"
						+ std::to_string(d+1));
			}
			for(int d = 0; d < ImpulseControlDimension; ++d) {
				out << space() << ("Impulse Control z_"
						+ std::to_string(d+1));
			}
			out << std::endl;

			int k = 0;
			for(auto node : result.spatial_grid) {
				for(int d = 0; d < Dimension; ++d) {
					out << space() << node[d];
				}
				out << space() << result.solution_vector(k);
				for(
					int d = 0;
					d < StochasticControlDimension;
					++d
				) {
					out << space() << result
						.stochastic_control_vector
						[d](k)
					;
				}
				for(
					int d = 0;
					d < ImpulseControlDimension;
					++d
				) {
					out << space() << result
						.impulse_control_vector
						[d](k)
					;
				}
				out << std::endl;
				++k;
			}
		} return result; }
	}
}

} // namespace Modules

} // namespace QuantPDE

#endif

