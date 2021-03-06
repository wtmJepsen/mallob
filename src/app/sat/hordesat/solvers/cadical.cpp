/*
 * Cadical.cpp
 *
 *  Created on: Jun 26, 2020
 *      Author: schick
 */

#include <ctype.h>
#include <stdarg.h>
#include <chrono>

#include "app/sat/hordesat/solvers/cadical.hpp"
#include "app/sat/hordesat/utilities/debug_utils.hpp"

const int CLAUSE_LEARN_INTERRUPT_THRESHOLD = 10000;

Cadical::Cadical(const SolverSetup& setup)
	: PortfolioSolverInterface(setup),
	  solver(new CaDiCaL::Solver), terminator(*setup.logger), learner(*this) {
	
	solver->connect_terminator(&terminator);
}

void Cadical::addLiteral(int lit) {
	solver->add(lit);
}

void Cadical::diversify(int seed) {

	// Options may only be set in the initialization phase, so the seed cannot be re-set
	if (!seedSet) {
		solver->set("seed", seed);
		seedSet = true;
	}
	
	// TODO: More diversification using getDiversificationIndex()
}

void Cadical::setPhase(const int var, const bool phase) {
	solver->phase(phase ? var : -var);
}

// Solve the formula with a given set of assumptions
// return 10 for SAT, 20 for UNSAT, 0 for UNKNOWN
SatResult Cadical::solve(size_t numAssumptions, const int* assumptions) {

	// add the learned clauses
	learnMutex.lock();
	for (auto clauseToAdd : learnedClauses) {
		for (auto litToAdd : clauseToAdd) {
			addLiteral(litToAdd);
		}
		addLiteral(0);
	}
	learnedClauses.clear();
	learnMutex.unlock();

	// set the assumptions
	this->assumptions.clear();
	for (size_t i = 0; i < numAssumptions; i++) {
		int lit = assumptions[i];
		solver->assume(lit);
		this->assumptions.push_back(lit);
	}

	// start solving
	int res = solver->solve();
	switch (res) {
	case 0:
		return UNKNOWN;
	case 10:
		return SAT;
	case 20:
		return UNSAT;
	default:
		return UNKNOWN;
	}
}

void Cadical::setSolverInterrupt() {
	terminator.setInterrupt();
}

void Cadical::unsetSolverInterrupt() {
	terminator.unsetInterrupt();
}

void Cadical::setSolverSuspend() {
    terminator.setSuspend();
}

void Cadical::unsetSolverSuspend() {
    terminator.unsetSuspend();
}

std::vector<int> Cadical::getSolution() {
	std::vector<int> result = {0};

	for (int i = 1; i <= getVariablesCount(); i++)
		result.push_back(solver->val(i));

	return result;
}

std::set<int> Cadical::getFailedAssumptions() {
	std::set<int> result;
	for (auto assumption : assumptions)
		if (solver->failed(assumption))
			result.insert(assumption);

	return result;
}

void Cadical::addLearnedClause(const int* begin, int size) {
	auto lock = learnMutex.getLock();
	if (size == 1) {
		learnedClauses.emplace_back(begin, begin + 1);
	} else {
		// Skip glue in front of array
		learnedClauses.emplace_back(begin + 1, begin + size);
	}
	if (learnedClauses.size() > CLAUSE_LEARN_INTERRUPT_THRESHOLD) {
		setSolverInterrupt();
	}
}

void Cadical::setLearnedClauseCallback(const LearnedClauseCallback& callback) {
	learner.setCallback(callback);
	solver->connect_learner(&learner);
}

void Cadical::increaseClauseProduction() {
	learner.incGlueLimit();
}

int Cadical::getVariablesCount() {
	return solver->vars();
}

int Cadical::getNumOriginalDiversifications() {
	return 0;
}

int Cadical::getSplittingVariable() {
	return solver->lookahead();
}

SolvingStatistics Cadical::getStatistics() {
	SolvingStatistics st;
	// Stats are currently not accessible for the outside
	// The can be directly printed with
	// solver->statistics();
	return st;
}

Cadical::~Cadical() {
	solver.release();
}
