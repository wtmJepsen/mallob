
#include <utility>
#include <assert.h>

#include "cutoff_priority_balancer.h"
#include "util/random.h"
#include "util/console.h"


bool CutoffPriorityBalancer::beginBalancing(std::map<int, Job*>& jobs) {

    // Initialize
    _assignments.clear();
    _priorities.clear();
    _demands.clear();
    _resources_info = ResourcesInfo();
    _stage = INITIAL_DEMAND;
    _balancing = true;

    // Identify jobs to balance
    bool isWorkerBusy = false;
    int numActiveJobs = 0;
    _jobs_being_balanced = std::map<int, Job*>();
    assert(_local_jobs == NULL || Console::fail("Found localJobs instance of size %i", _local_jobs->size()));
    _local_jobs = new std::set<int, PriorityComparator>(PriorityComparator(jobs));
    for (auto it : jobs) {
        bool isActiveRoot = it.second->isRoot() && it.second->isNotInState({INITIALIZING_TO_PAST}) 
                            && (it.second->isInState({ACTIVE, STANDBY}) || it.second->isInitializing());
        // Node must be root node to participate
        bool participates = it.second->isRoot();
        // Job must be active, or must be initializing and already having the description
        participates &= it.second->isInState({JobState::ACTIVE, JobState::STANDBY})
                        || (it.second->isInState({JobState::INITIALIZING_TO_ACTIVE}) 
                            && it.second->hasJobDescription());
        if (participates) {
            // Job participates
            _jobs_being_balanced[it.first] = it.second;
            _local_jobs->insert(it.first);
            numActiveJobs++;
        } else if (isActiveRoot) {
            // Root process cannot participate at balancing yet
            // => automatically assign an implicit demand of one, but
            // do not let the job be an actual participant of the procedure
            Console::log(Console::VERB, "Job #%i : demand 1, final assignment 1 (implicit)", it.first);
            numActiveJobs++;
        }
        // Set "busy" flag of this worker node to true, if applicable
        if (it.second->isInState({JobState::ACTIVE, JobState::INITIALIZING_TO_ACTIVE})) {
            isWorkerBusy = true;
        }
    }

    // Find global aggregation of demands and amount of busy nodes
    float aggregatedDemand = 0;
    for (auto it : *_local_jobs) {
        int jobId = it;
        _demands[jobId] = getDemand(*_jobs_being_balanced[jobId]);
        _priorities[jobId] = _jobs_being_balanced[jobId]->getDescription().getPriority();
        aggregatedDemand += (_demands[jobId]-1) * _priorities[jobId];
        
        Console::log(Console::VERB, "Job #%i : demand %i", jobId, _demands[jobId]);
    }

    Console::log(Console::VERB, "Local aggregated demand: %.3f", aggregatedDemand);
    _demand_and_busy_nodes_contrib[0] = aggregatedDemand;
    _demand_and_busy_nodes_contrib[1] = (isWorkerBusy ? 1 : 0);
    _demand_and_busy_nodes_contrib[2] = numActiveJobs;
    _demand_and_busy_nodes_result[0] = 0;
    _demand_and_busy_nodes_result[1] = 0;
    _demand_and_busy_nodes_result[2] = 0;
    _reduce_request = MyMpi::iallreduce(_comm, _demand_and_busy_nodes_contrib, _demand_and_busy_nodes_result, 3);

    return false; // not finished yet: wait for end of iallreduce
}

bool CutoffPriorityBalancer::canContinueBalancing() {
    if (_stage == INITIAL_DEMAND || _stage == GLOBAL_ROUNDING) {
        // Check if reduction is done
        MPI_Status status;
        return MyMpi::test(_reduce_request, status);
    }
    return false;
}

bool CutoffPriorityBalancer::continueBalancing() {
    if (_stage == INITIAL_DEMAND) {

        // Finish up initial reduction
        float aggregatedDemand = _demand_and_busy_nodes_result[0];
        int busyNodes = _demand_and_busy_nodes_result[1];
        int numJobs = _demand_and_busy_nodes_result[2];
        bool rankZero = MyMpi::rank(MPI_COMM_WORLD) == 0;
        Console::log(rankZero ? Console::VVERB : Console::VVVVERB, 
            "%i/%i nodes (%.2f%%) are busy", (int)busyNodes, MyMpi::size(_comm), 
            ((float)100*busyNodes)/MyMpi::size(_comm));
        Console::log(rankZero ? Console::VVERB : Console::VVVVERB, 
            "Aggregation of demands: %.3f", aggregatedDemand);
        Console::log(rankZero ? Console::VVERB : Console::VVVVERB, 
            "%i jobs being balanced", numJobs);
        
        // The total available volume where the "atomic" demand of each job is already subtracted
        _total_avail_volume = MyMpi::size(_comm) * _load_factor - numJobs;

        // Calculate local initial assignments
        for (auto it : *_local_jobs) {
            int jobId = it;
            float initialMetRatio = _total_avail_volume * _priorities[jobId] / aggregatedDemand;
            // job demand minus "atomic" demand that is met by default
            int remainingDemand = _demands[jobId] - 1;
            // assignment: atomic node plus fair share of reduced aggregation
            _assignments[jobId] = 1 + std::min(1.0f, initialMetRatio) * remainingDemand;
            Console::log(Console::VVERB, "Job #%i : initial assignment %.3f", jobId, _assignments[jobId]);
        }

        // Create ResourceInfo instance with local data
        for (auto it : *_local_jobs) {
            int jobId = it;
            _resources_info.assignedResources += _assignments[jobId]-1;
            // Note: jobs are sorted descendingly by priority!
            _resources_info.priorities.push_back(_priorities[jobId]);
            _resources_info.demandedResources.push_back( _demands[jobId] - _assignments[jobId] );
        }

        // Continue
        return continueBalancing(NULL);
    }

    if (_stage == GLOBAL_ROUNDING) {
        return continueRoundingFromReduction();
    }
    return false;
}

bool CutoffPriorityBalancer::continueBalancing(MessageHandlePtr handle) {
    bool done;
    BalancingStage stage = _stage;
    if (_stage == INITIAL_DEMAND) {
        _stage = REDUCE_RESOURCES;
    }
    if (_stage == REDUCE_RESOURCES) {
        if (handle == NULL || stage != REDUCE_RESOURCES) {
            done = _resources_info.startReduction(_comm);
        } else {
            done = _resources_info.advanceReduction(handle);
        }
        if (done) _stage = BROADCAST_RESOURCES;
    }
    if (_stage == BROADCAST_RESOURCES) {
        if (handle == NULL || stage != BROADCAST_RESOURCES) {
            done = _resources_info.startBroadcast(_comm, _resources_info.getExcludedRanks());
        } else {
            done = _resources_info.advanceBroadcast(handle);
        }
        if (done) {
            return finishResourcesReduction();
        }
    }

    if (_stage == REDUCE_REMAINDERS) {
        if (handle == NULL || stage != REDUCE_REMAINDERS) {
            done = _remainders.startReduction(_comm, _resources_info.getExcludedRanks());
        } else {
            done = _remainders.advanceReduction(handle);
        }
        if (done) _stage = BROADCAST_REMAINDERS;
    }
    if (_stage == BROADCAST_REMAINDERS) {
        if (handle == NULL || stage != BROADCAST_REMAINDERS) {
            done = _remainders.startBroadcast(_comm, _remainders.getExcludedRanks());
        } else {
            done = _remainders.advanceBroadcast(handle);
        }
        if (done) {
            done = finishRemaindersReduction();
            _stage = GLOBAL_ROUNDING;
            return done;
        }
    }
    return false;
}

bool CutoffPriorityBalancer::finishResourcesReduction() {

    _stats.increment("reductions"); _stats.increment("broadcasts");

    // "resourcesInfo" now contains global data from all concerned jobs
    if (_resources_info.getExcludedRanks().count(MyMpi::rank(_comm)) 
                && _params.getParam("r") == ROUNDING_PROBABILISTIC) {
        Console::log(Console::VVERB, "Ended all-reduction. Balancing finished.");
        _balancing = false;
        delete _local_jobs;
        _local_jobs = NULL;
        _assignments = std::map<int, float>();
        return true;
    } else {
        Console::log(Console::VVERB, "Ended all-reduction. Calculating final job demands");
    }

    // Assign correct (final) floating-point resources
    int rank = MyMpi::rank(MPI_COMM_WORLD);
    Console::log(!rank ? Console::VVERB : Console::VVVVERB, "Initially assigned resources: %.3f", 
        _resources_info.assignedResources);
    
    // Atomic job assignments are already subtracted from _total_avail_volume
    // and are not part of the all-reduced assignedResources either
    float remainingResources = _total_avail_volume - _resources_info.assignedResources;
    if (remainingResources < 0.1) remainingResources = 0; // too low a remainder to make a difference

    Console::log(!rank ? Console::VVERB : Console::VVVVERB, "Remaining resources: %.3f", remainingResources);

    for (auto it : _jobs_being_balanced) {
        int jobId = it.first;
        if (_demands[jobId] == 1) continue;

        float demand = _demands[jobId];
        float priority = _priorities[jobId];
        std::vector<float>& priorities = _resources_info.priorities;
        std::vector<float>& demandedResources = _resources_info.demandedResources;
        std::vector<float>::iterator itPrio = std::find(priorities.begin(), priorities.end(), priority);
        assert(itPrio != priorities.end() || Console::fail("Priority %.3f not found in histogram!", priority));
        int prioIndex = std::distance(priorities.begin(), itPrio);

        if (_assignments[jobId] == demand
            || priorities[prioIndex] <= remainingResources) {
            // Case 1: Assign full demand
            _assignments[jobId] = demand;
        } else {
            if (prioIndex == 0 || demandedResources[prioIndex-1] >= remainingResources) {
                // Case 2: No additional resources assigned
            } else {
                // Case 3: Evenly distribute ratio of remaining resources
                assert(remainingResources >= 0);
                float ratio = (remainingResources - demandedResources[prioIndex-1])
                            / (demandedResources[prioIndex] - demandedResources[prioIndex-1]);
                assert(ratio > 0);
                assert(ratio <= 1);
                _assignments[jobId] += ratio * (demand - _assignments[jobId]);
            }
        }
        
        Console::log(Console::VVERB, "Job #%i : adjusted assignment %.3f", jobId, _assignments[jobId]);
    }

    if (_params.getParam("r") == ROUNDING_BISECTION)  {
        // Build contribution to all-reduction of non-zero remainders
        _remainders = SortedDoubleSequence();
        for (auto it : _jobs_being_balanced) {
            int jobId = it.first;
            double remainder = _assignments[jobId] - (int)_assignments[jobId];
            if (remainder > 0 && remainder < 1) _remainders.add(remainder);
        }
        _last_utilization = 0;
        _best_remainder_idx = -1;

        _stage = REDUCE_REMAINDERS;
        return continueBalancing(NULL);

    } else return true;    
}

bool CutoffPriorityBalancer::finishRemaindersReduction() {
    if (!_remainders.isEmpty()) {
        Console::getLock();
        Console::appendUnsafe(Console::VVVERB, "ROUNDING remainders: ");
        for (int i = 0; i < _remainders.size(); i++) Console::appendUnsafe(Console::VVVERB, "%.3f ", _remainders[i]);
        Console::logUnsafe(Console::VVVERB, "");
        Console::releaseLock();
    }
    return continueRoundingUntilReduction(0, _remainders.size());
}

std::map<int, int> CutoffPriorityBalancer::getRoundedAssignments(int remainderIdx, int& sum) {

    double remainder = remainderIdx < _remainders.size() ? _remainders[remainderIdx] : 1.0;
    //int occurrences =  remainderIdx < _remainders.size() ? _remainders.getOccurrences(remainderIdx) : 0;

    std::map<int, int> roundedAssignments;
    for (auto it : _assignments) {

        double r = it.second - (int)it.second;
        
        if (r < remainder) 
            roundedAssignments[it.first] = std::floor(it.second);
        else 
            roundedAssignments[it.first] = std::ceil(it.second);
        
        sum += roundedAssignments[it.first];
    }
    return roundedAssignments;
}

bool CutoffPriorityBalancer::continueRoundingUntilReduction(int lower, int upper) {

    _lower_remainder_idx = lower;
    _upper_remainder_idx = upper;

    int idx = (_lower_remainder_idx+_upper_remainder_idx)/2;
    
    int localSum = 0;
    if (idx <= _remainders.size()) {
        // Remainder is either one of the remainders from the reduced sequence
        // or the right-hand limit 1.0
        double remainder = (idx < _remainders.size() ? _remainders[idx] : 1.0);
        // Round your local assignments and calculate utilization sum
        _rounded_assignments = getRoundedAssignments(idx, localSum);
    }

    iAllReduce(localSum);
    return false;
}

bool CutoffPriorityBalancer::continueRoundingFromReduction() {

    int rank = MyMpi::rank(MPI_COMM_WORLD);
    _rounding_iterations++;

    float utilization = _reduce_result;
    float diffToOptimum = (MyMpi::size(_comm) * _load_factor) - utilization;

    int idx = (_lower_remainder_idx+_upper_remainder_idx)/2;

    // Store result, if it is the best one so far:
    // either the first result, or
    // the first not-oversubscribing result, or
    // another oversubscribing result that is better than previous result, or
    // a not-oversubscribing result with lower absolute diff than previous non-oversubscribing result
    if (_best_remainder_idx == -1
            || (diffToOptimum > -1 && _best_utilization_diff <= -1)
            || (diffToOptimum <= -1 && _best_utilization_diff <= -1 && diffToOptimum > _best_utilization_diff)
            || (diffToOptimum > -1 && std::abs(diffToOptimum) < std::abs(_best_utilization_diff))
        ) {
        _best_utilization_diff = diffToOptimum;
        _best_remainder_idx = idx;
        _best_utilization = utilization;
    }

    // Log iteration
    if (!_remainders.isEmpty() && idx <= _remainders.size()) {
        double remainder = (idx < _remainders.size() ? _remainders[idx] : 1.0);
        Console::log(Console::VVERB, "ROUNDING it=%i [%i,%i]=>%i rmd=%.3f util=%.2f err=%.2f", 
                        _rounding_iterations, _lower_remainder_idx, _upper_remainder_idx, idx,
                        remainder, utilization, diffToOptimum);
    }

    // Termination?
    if (utilization == _last_utilization) { // Utilization unchanged?
        // Finished!
        if (!_remainders.isEmpty() && _best_remainder_idx <= _remainders.size()) {
            // Remainders are known to this node: apply and report
            int sum = 0;
            _rounded_assignments = getRoundedAssignments(_best_remainder_idx, sum);
            for (auto it : _rounded_assignments) {
                _assignments[it.first] = it.second;
            }
            double remainder = (_best_remainder_idx < _remainders.size() ? _remainders[_best_remainder_idx] : 1.0);
            Console::log(Console::VVERB, 
                        "ROUNDING DONE its=%i rmd=%.3f util=%.2f err=%.2f", 
                        _rounding_iterations, remainder, _best_utilization, _best_utilization_diff);
        }
        // reset to original state
        _best_remainder_idx = -1;
        _rounding_iterations = 0; 
        return true; // Balancing completely done
    }

    if (_lower_remainder_idx < _upper_remainder_idx) {
        if (utilization < _load_factor*MyMpi::size(_comm)) {
            // Too few resources utilized
            _upper_remainder_idx = idx-1;
        }
        if (utilization > _load_factor*MyMpi::size(_comm)) {
            // Too many resources utilized
            _lower_remainder_idx = idx+1;
        }
    }
    
    _last_utilization = utilization;
    return continueRoundingUntilReduction(_lower_remainder_idx, _upper_remainder_idx);
}

std::map<int, int> CutoffPriorityBalancer::getBalancingResult() {

    // Convert float assignments into actual integer volumes, store and return them
    std::map<int, int> volumes;
    for (auto it = _assignments.begin(); it != _assignments.end(); ++it) {
        int jobId = it->first;
        float assignment = std::max(1.0f, it->second);
        int intAssignment = Random::roundProbabilistically(assignment);
        volumes[jobId] = intAssignment;
        if (intAssignment != (int)assignment) {
            Console::log(Console::VVERB, " #%i : final assignment %.3f ~> %i", jobId, _assignments[jobId], intAssignment);
        } else {
            Console::log(Console::VVERB, " #%i : final assignment %i", jobId, intAssignment);
        }
    }
    for (auto it = volumes.begin(); it != volumes.end(); ++it) {
        updateVolume(it->first, it->second);
    }

    _balancing = false;
    delete _local_jobs;
    _local_jobs = NULL;
    return volumes;
}
