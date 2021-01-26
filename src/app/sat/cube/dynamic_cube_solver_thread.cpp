#include "dynamic_cube_solver_thread.hpp"

#include <cassert>

DynamicCubeSolverThread::DynamicCubeSolverThread(DynamicCubeSolverThreadManagerInterface &manager, DynamicCubeSetup &setup)
    : _manager(manager), _formula(setup.formula), _logger(setup.logger), _result(setup.result) {
    // Initialize solver
    SolverSetup solver_setup;
    solver_setup.logger = &_logger;

    // TODO Fill with valid values
    solver_setup.globalId = 0;
    solver_setup.localId = 0;
    solver_setup.jobname = "cube";
    solver_setup.diversificationIndex = 0;

    _solver = std::make_unique<Cadical>(solver_setup);

    // Initialization is done in a seperate thread thus hard work is allowed
    // Also this allows a universal start
    // Read formula
    for (int lit : *_formula.get()) _solver->addLiteral(lit);
}

DynamicCubeSolverThread::~DynamicCubeSolverThread() {
    if (_thread.joinable()) _thread.join();
}

void DynamicCubeSolverThread::start() {
    // Reset
    _solver->uninterrupt();
    _isInterrupted.store(false);

    assert(!_thread.joinable());

    _thread = std::thread(&DynamicCubeSolverThread::run, this);
}

void DynamicCubeSolverThread::interrupt() {
    _isInterrupted.store(true);

    _solver->interrupt();
}

void DynamicCubeSolverThread::join() {
    // This is also called with the job control thread therefore it cannot be called simultaneously to start
    assert(_thread.joinable());

    _thread.join();
}

void DynamicCubeSolverThread::run() {
    while (!_isInterrupted) {
        // Reset cube
        _cube.reset();

        // Send failed and request new cube
        _manager.shareCubes(_failed, _cube);

        // Failed assumptions were sent
        _failed.reset();

        // TODO Change to possibly learn and definitely add
        {
            const std::lock_guard<Mutex> lock(_new_failed_cubes_lock);

            _logger.log(0, "DynamicCubeSolverThread: Adding new failed clauses from buffer with size %zu", _new_failed_cubes.size());

            // Add received failed cubes to formula
            for (int lit : _new_failed_cubes) _solver->addLiteral(lit);

            // Reset buffer for received failed cubes
            _new_failed_cubes.clear();
        }

        // Start work
        solve();

        // Exit loop if formula was solved
        if (_result != UNKNOWN) return;
    }
    _logger.log(0, "DynamicCubeSolverThread: Leaving the main loop");
}

void DynamicCubeSolverThread::solve() {
    if (_cube.has_value()) {
        _logger.log(0, "DynamicCubeSolverThread: Started solving a cube");

        // Assume and solve
        auto path = _cube.value().getPath();
        auto result = _solver->solve(path);

        // Check result
        if (result == SAT) {
            _logger.log(1, "DynamicCubeSolverThread: Found a solution: SAT");
            _result = SAT;

        } else if (result == UNKNOWN) {
            _logger.log(1, "DynamicCubeSolverThread: Solving interrupted");

        } else if (result == UNSAT) {
            _logger.log(1, "DynamicCubeSolverThread: Cube failed");

            auto failed_assumptions = _solver->getFailedAssumptions();

            if (failed_assumptions.size() > 0) {
                _logger.log(1, "DynamicCubeSolverThread: Found failed assumptions");

                // At least one assumption failed -> Set failed
                _failed.emplace(failed_assumptions.begin(), failed_assumptions.end());

            } else {
                _logger.log(1, "DynamicCubeSolverThread: Found a solution: UNSAT");

                // Intersection of assumptions and core is empty -> Formula is unsatisfiable
                _result = UNSAT;
            }
        }
    } else {
        _logger.log(0, "DynamicCubeSolverThread: Skipped solving, because no cube is available");
    }
}

void DynamicCubeSolverThread::handleFailed(const std::vector<int> &failed) {
    const std::lock_guard<Mutex> lock(_new_failed_cubes_lock);

    _logger.log(0, "DynamicCubeSolverThread: Adding new failed assumption with buffer size %zu", failed.size());

    // Insert failed cubes at the end of new failed cubes
    _new_failed_cubes.insert(_new_failed_cubes.end(), failed.begin(), failed.end());

    // TODO Add a new function in PortfolioSolver that allows clauses to be added for the next call to solve and may be learned asynchronously
    // Learn failed clauses
    // _solver->addLearnedClause(failed.data(), failed.size());
}