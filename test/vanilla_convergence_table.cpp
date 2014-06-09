#include <QuantPDE/Core>

// TODO: Change these includes; shouldn't include src directory explicitly
#include <QuantPDE/src/Modules/Payoffs.hpp>
#include <QuantPDE/src/Modules/BlackScholesOperator.hpp>

using namespace QuantPDE;
using namespace QuantPDE::Modules;

///////////////////////////////////////////////////////////////////////////////

#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <unistd.h>

using namespace std;

///////////////////////////////////////////////////////////////////////////////

int main(int argc, char **argv) {

	// Default options
	Real expiry         = 1.;
	Real interest       = 0.04;
	Real volatility     = 0.2;
	Real dividends      = 0.;
	Real stock          = 100.;
	Real strike         = 100.;
	unsigned refinement = 5;
	unsigned steps      = 25;
	bool call           = true;
	bool variable       = false;
	bool american       = false;

	// Setting options with getopt
	{ char c;
	while((c = getopt(argc, argv, "Ad:hK:pr:R:s:S:T:v:V")) != -1) {
		switch(c) {
			case 'A':
				american = true;
				break;
			case 'd':
				dividends = atof(optarg);
				break;
			case 'h':
				cerr <<
"vanilla_convergence_table [OPTIONS]" << endl << endl <<
"Outputs the rate of convergence for computing the price of a call or put using" << endl <<
"a discretization of the Black-Scholes partial differential equation." << endl <<
endl <<
"-A" << endl <<
endl <<
"    American option (default is European)" << endl <<
endl <<
"-d REAL" << endl <<
endl <<
"    sets the dividend rate (default is 0.)" << endl <<
endl <<
"-K REAL" << endl <<
endl <<
"    sets the strike price (default is 100.)" << endl <<
endl <<
"-p" << endl <<
endl <<
"    computes the price of a European put (default is call)" << endl <<
endl <<
"-r REAL" << endl <<
endl <<
"    sets interest rate (default is 0.04)" << endl <<
endl <<
"-R POSITIVE_INTEGER" << endl <<
endl <<
"    sets the maximum number of refinement steps in the computation (default is" << endl <<
"    5)" << endl <<
endl <<
"-s POSITIVE_INTEGER" << endl <<
endl <<
"    sets the initial number of steps to take in time (default is 25)" << endl <<
endl <<
"-S REAL" << endl <<
endl <<
"    sets the initial stock price (default is 100.)" << endl <<
endl <<
"-T POSITIVE_REAL" << endl <<
endl <<
"    sets the expiry time (default is 1.)" << endl <<
endl <<
"-v REAL" << endl <<
endl <<
"    sets the volatility" << endl <<
endl <<
"-V" << endl <<
"    uses variable-size timestepping" << endl << endl;
				return 0;
			case 'K':
				strike = atof(optarg);
				break;
			case 'p':
				call = false;
				break;
			case 'r':
				interest = atof(optarg);
				break;
			case 'R':
				refinement = atoi(optarg);
				break;
			case 's':
				steps = atoi(optarg);
				if(steps <= 0) {
					cerr <<
"error: the number of steps must be positive" << endl;
					return 1;
				}
				break;
			case 'S':
				stock = atof(optarg);
				break;
			case 'T':
				if((expiry = atof(optarg)) <= 0.) {
					cerr <<
"error: expiry time must be positive" << endl;
					return 1;
				}
				break;
			case 'v':
				volatility = atof(optarg);
				break;
			case 'V':
				variable = true;
				break;
		}
	} }

	// Setting up the table
	const int td = 10;
	cout
		<< setw(td) << "Nodes"  << "\t"
		<< setw(td) << "Steps"  << "\t"
		<< setw(td) << "Value"  << "\t"
		<< setw(td) << "Change" << "\t"
		<< setw(td) << "Ratio"
		<< endl
	;
	cout.precision(6);
	Real previousValue = nan(""), previousChange = nan("");

	// Initial discretization
	// TODO: Create grid based on initial stock price and strike price
	RectilinearGrid1 grid(
		Axis {
			0., 10., 20., 30., 40., 50., 60., 70.,
			75., 80.,
			84., 88., 92.,
			94., 96., 98., 100., 102., 104., 106., 108., 110.,
			114., 118.,
			123.,
			130., 140., 150.,
			175.,
			225.,
			300.,
			750.,
			2000.,
			10000.
		}
	);

	// Payoff function
	Function1 payoff = bind(
		call ? callPayoff : putPayoff,
		placeholders::_1,
		strike
	);

	// Alternatively, we could have used...
	//auto payoff = QUANT_PDE_MODULES_CALL_PAYOFF_FIXED_STRIKE(strike);
	//auto payoff = QUANT_PDE_MODULES_PUT_PAYOFF_FIXED_STRIKE(strike);

	Real target = expiry / steps * 10.;
	for(unsigned l = 0; l < refinement; l++, steps *= variable ? 4 : 2) {

		///////////////////////////////////////////////////////////////
		// Build spatial grid
		///////////////////////////////////////////////////////////////

		// Refine the grid
		grid.refine( RectilinearGrid1::NewTickBetweenEachPair() );

		///////////////////////////////////////////////////////////////
		// Solve problem
		///////////////////////////////////////////////////////////////

		unsigned realizedSteps;
		Real value;
		{
			// Black-Scholes operator (L in V_t = LV)
			BlackScholesOperator bsOperator(
				grid,
				[interest]   (Real, Real) {return interest;  },
				[volatility] (Real, Real) {return volatility;},
				[dividends]  (Real, Real) {return dividends; }
			);

			// Timestepping method
			unique_ptr<Iteration> stepper;
			if(!variable) {
				stepper = unique_ptr<Iteration>(
					new ReverseConstantStepper(
						0., // Initial time
						expiry,
						steps
					)
				);
			} else {
				stepper = unique_ptr<Iteration>(
					new ReverseVariableStepper(
						0., // Initial time
						expiry,
						expiry / steps,
						target
					)
				);
				target /= 2;
			}

			// Time discretization method
			ReverseLinearBDFTwo bdf(grid, bsOperator);
			bdf.setIteration(*stepper);

			// American-specific components; penalty method or not?
			Linearizer *root;
			unique_ptr<ToleranceIteration> tolerance;
			unique_ptr<SimplePenaltyMethod1> penalty;
			if(american) {
				penalty = unique_ptr<SimplePenaltyMethod1>(
					new SimplePenaltyMethod1(
						grid,
						bdf,
						payoff // Penalty function
					)
				);

				tolerance = unique_ptr<ToleranceIteration>(
						new ToleranceIteration());

				penalty->setIteration(*tolerance);
				stepper->setInnerIteration(*tolerance);
				root = penalty.get();
			} else {
				root = &bdf;
			}

			// Linear system solver
			BiCGSTABSolver solver;

			// Compute solution
			Vector solutionVector = stepper->iterateUntilDone(
				grid.image(payoff),
				*root,
				solver
			);

			// Number of steps taken (outermost iteration)
			realizedSteps = stepper->iterations();

			// Solution at S = 100.
			value = grid.accessor(solutionVector)(stock);
		}

		///////////////////////////////////////////////////////////////
		// Table
		///////////////////////////////////////////////////////////////

		// Change and ratio between successive solutions
		Real
			change = value - previousValue,
			ratio = previousChange / change
		;

		// Print out row of table
		cout
			<< scientific
			<< setw(td) << grid.size()   << "\t"
			<< setw(td) << realizedSteps << "\t"
			<< setw(td) << value         << "\t"
			<< setw(td) << change        << "\t"
			<< setw(td) << ratio
			<< endl
		;

		previousChange = change;
		previousValue = value;

	}

	return 0;
}

