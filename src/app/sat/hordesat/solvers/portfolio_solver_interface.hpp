/*
 * portfolioSolverInterface.h
 *
 *  Created on: Oct 10, 2014
 *      Author: balyo
 * 
 * portfolio_solver_interface.hpp
 * 
 *  Modified by Dominik Schreiber, 2019-*
 */

#ifndef PORTFOLIOSOLVERINTERFACE_H_
#define PORTFOLIOSOLVERINTERFACE_H_

#include <vector>
#include <set>
#include <stdexcept>
#include <functional>

#include "util/logger.hpp"

enum SatResult {
	SAT = 10,
	UNSAT = 20,
	UNKNOWN = 0
};

struct SolvingStatistics {
	unsigned long propagations = 0;
	unsigned long decisions = 0;
	unsigned long conflicts = 0;
	unsigned long restarts = 0;
	unsigned long receivedClauses = 0;
	unsigned long digestedClauses = 0;
	unsigned long discardedClauses = 0;
	double memPeak = 0;
};

struct SolverSetup {

	// General important fields
	Logger* logger;
	int globalId;
	int localId; 
	std::string jobname; 
	int diversificationIndex;

	// SAT Solving settings

	// In any case, these bounds MUST be fulfilled for a clause to be exported
	unsigned int hardMaxClauseLength;
	unsigned int hardInitialMaxLbd;
	unsigned int hardFinalMaxLbd;
	// These bounds may not be fulfilled in case the solver deems the clause very good
	// due to other observations
	unsigned int softMaxClauseLength;
	unsigned int softInitialMaxLbd;
	unsigned int softFinalMaxLbd;
	// For lingeling ("use old diversification")
	bool useAdditionalDiversification;

	size_t anticipatedLitsToImportPerCycle;
};

void updateTimer(std::string jobName);

typedef std::function<void(std::vector<int>& cls, int solverId)> LearnedClauseCallback;

/**
 * Interface for solvers that can be used in the portfolio.
 */
class PortfolioSolverInterface {

protected:
	Logger _logger;
	SolverSetup _setup;


// ************** INTERFACE TO IMPLEMENT **************

public:

	// constructor
	PortfolioSolverInterface(const SolverSetup& setup);

    // destructor
	virtual ~PortfolioSolverInterface() {}

	// Get the number of variables of the formula
	virtual int getVariablesCount() = 0;

	// Get a variable suitable for search splitting
	virtual int getSplittingVariable() = 0;

	// Set initial phase for a given variable
	// Used only for diversification of the portfolio
	virtual void setPhase(const int var, const bool phase) = 0;

	// Solve the formula with a given set of assumptions
	virtual SatResult solve(size_t numAssumptions, const int* assumptions) = 0;

	// Get a solution vector containing lit or -lit for each lit in the model
	virtual std::vector<int> getSolution() = 0;

	// Get a set of failed assumptions
	virtual std::set<int> getFailedAssumptions() = 0;

	// Add a permanent literal to the formula (zero for clause separator)
	virtual void addLiteral(int lit) = 0;

	// Add a learned clause to the formula
	// The learned clauses might be added later or possibly never
	virtual void addLearnedClause(const int* begin, int size) = 0;

	// Set a function that should be called for each learned clause
	virtual void setLearnedClauseCallback(const LearnedClauseCallback& callback) = 0;

	// Request the solver to produce more clauses
	virtual void increaseClauseProduction() = 0;

	// Get solver statistics
	virtual SolvingStatistics getStatistics() = 0;

	// Diversify your parameters (seeds, heuristics, etc.) according to the seed
	// and the individual diversification index given by getDiversificationIndex().
	virtual void diversify(int seed) = 0;

	// How many "true" different diversifications do you have?
	// May be used to decide when to apply additional diversifications.
	virtual int getNumOriginalDiversifications() = 0;

protected:
	// Interrupt the SAT solving, solving cannot continue until interrupt is unset.
	virtual void setSolverInterrupt() = 0;

	// Resume SAT solving after it was interrupted.
	virtual void unsetSolverInterrupt() = 0;

    // Suspend the SAT solver DURING its execution (ASYNCHRONOUSLY), 
	// temporarily freeing up CPU for other threads
    virtual void setSolverSuspend() = 0;

	// Resume SAT solving after it was suspended.
    virtual void unsetSolverSuspend() = 0;

// ************** END OF INTERFACE TO IMPLEMENT **************


// Other methods

public:
	/**
	 * The solver's ID which is globally unique for the particular job
	 * that is being computed on.
	 * Equal to <rank> * <solvers_per_node> + <local_id>.
	 */
	int getGlobalId() {return _global_id;}
	/**
	 * The solver's local ID on this node and job. 
	 */
	int getLocalId() {return _local_id;}
	/**
	 * This number n denotes that this solver is the n-th solver of this type
	 * being employed to compute on this job.
	 * Equal to the global ID minus the number of solvers of a different type.
	 */
	int getDiversificationIndex() {return _diversification_index;}

	Logger& getLogger() {return _logger;}
	
	void interrupt();
	void uninterrupt();
	void suspend();
	void resume();

private:
	std::string _global_name;
	std::string _job_name;
	int _global_id;
	int _local_id;
	int _diversification_index;
};

// Returns the elapsed time (seconds) since the currently registered solver's start time.
double getTime();

#endif /* PORTFOLIOSOLVERINTERFACE_H_ */
