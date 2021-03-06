
#ifndef HORDE_MALLOB_SOLVER_THREAD_H
#define HORDE_MALLOB_SOLVER_THREAD_H

#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <thread>
#include <atomic>

#include "util/params.hpp"
#include "util/sys/threading.hpp"
#include "util/logger.hpp"
#include "app/sat/hordesat/solvers/portfolio_solver_interface.hpp"
#include "app/sat/hordesat/solvers/solving_state.hpp"

// Forward declarations
class HordeLib;

class SolverThread {

private:
    const Parameters& _params;
    std::shared_ptr<PortfolioSolverInterface> _solver_ptr;
    PortfolioSolverInterface& _solver;
    Logger& _logger;
    std::thread _thread;

    size_t _f_size;
    const int* _f_lits;
    size_t _a_size;
    const int* _a_lits;
    
    int _local_id;
    std::string _name;
    int _portfolio_rank;
    int _portfolio_size;

    volatile SolvingStates::SolvingState _state;
    Mutex _state_mutex;
    ConditionVariable _state_cond;

    SatResult _result;
    std::vector<int> _solution;
    std::set<int> _failed_assumptions;

    size_t _imported_lits = 0;
    long _tid = -1;

    std::atomic_bool _initialized = false;
    std::atomic_bool* _finished_flag;


public:
    SolverThread(const Parameters& params, std::shared_ptr<PortfolioSolverInterface> solver, 
                size_t fSize, const int* fLits, size_t aSize, const int* aLits,
                int localId, std::atomic_bool* finished);
    ~SolverThread();

    void init();
    void start();
    void setState(SolvingStates::SolvingState state);
    void tryJoin() {if (_thread.joinable()) _thread.join();}

    bool isInitialized() const {
        return _initialized;
    }
    int getTid() const {
        return _tid;
    }
    SolvingStates::SolvingState getState() const {
        return _state;
    }
    SatResult getSatResult() const {
        return _result;
    }
    const std::vector<int>& getSolution() const {
        return _solution;
    }
    const std::set<int>& getFailedAssumptions() const {
        return _failed_assumptions;
    }

private:
    void* run();
    
    void pin();
    void readFormula();
    void read();

    void diversify();
    void sparseDiversification(int mpi_size, int mpi_rank);
	void randomDiversification();
	void sparseRandomDiversification(int mpi_size);
	void nativeDiversification();
	void binValueDiversification(int mpi_size, int mpi_rank);

    void runOnce();
    void waitWhile(SolvingStates::SolvingState state);
    bool cancelRun();
    bool cancelThread();
    void reportResult(int res);

    const char* toStr();

};

#endif