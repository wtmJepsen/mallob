
#include <math.h>
#include <thread>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <initializer_list>
#include <limits>

#include "worker.h"
#include "app/sat_job.h"
#include "util/timer.h"
#include "util/console.h"
#include "util/random.h"
#include "util/memusage.h"
#include "balancing/cutoff_priority_balancer.h"
#include "balancing/event_driven_balancer.h"
#include "data/job_description.h"

void mpiMonitor(Worker* worker) {
    while (!worker->exiting) {
        double callStart = 0;
        std::string opName = MyMpi::currentCall(&callStart);
        if (callStart < 0.00001 || opName == "") {
            Console::log(Console::VVVERB, "MONITOR_MPI Not inside MPI call.");
        } else {
            double elapsed = Timer::elapsedSeconds() - callStart;
            Console::log(Console::VERB, "MONITOR_MPI Inside \"%s\" for %.4fs", opName.c_str(), elapsed);
            if (elapsed > 60.0) {
                // Inside some MPI call for a minute
                Console::fail("MPI call takes too long - aborting");
                exit(1);
            }
        }
        usleep(1000 * 1000); // 1s
    }
}

void Worker::init() {

    // Initialize balancer
    //balancer = std::unique_ptr<Balancer>(new ThermodynamicBalancer(comm, params));
    if (params.getParam("bm") == "ed") {
        // Event-driven balancing
        balancer = std::unique_ptr<Balancer>(new EventDrivenBalancer(comm, params, stats));
    } else {
        // Fixed-period balancing
        balancer = std::unique_ptr<Balancer>(new CutoffPriorityBalancer(comm, params, stats));
    }
    
    // Initialize pseudo-random order of nodes
    if (params.isSet("derandomize")) {
        // Pick fixed number k of bounce destinations
        int numBounceAlternatives = params.getIntParam("ba");
        int numWorkers = MyMpi::size(comm);
        assert(numBounceAlternatives % 2 == 0 || 
            Console::fail("ERROR: Parameter bounceAlternatives must be even (for theoretical reasons)"));
        assert(numBounceAlternatives < numWorkers || 
            Console::fail("ERROR: There must be more worker nodes than there are bounce alternatives per worker"));
        bounceAlternatives = std::vector<int>();

        // Generate global permutation over all worker ranks
        AdjustablePermutation p(numWorkers, /*random seed = */1);
        // Find the position where <worldRank> occurs in the permutation
        int i = 0;
        int x = p.get(i);
        while (x != worldRank) x = p.get(++i);
        Console::log(Console::VVERB, "My pos. in global permutation: %i", i);
        // Add neighbors in the permutation
        // to the node's bounce alternatives
        for (int j = i-(numBounceAlternatives/2); j < i; j++) {
            bounceAlternatives.push_back(p.get((j+numWorkers) % numWorkers));
        }
        for (int j = i+1; j <= i+(numBounceAlternatives/2); j++) {
            bounceAlternatives.push_back(p.get((j+numWorkers) % numWorkers));
        }
        assert(bounceAlternatives.size() == numBounceAlternatives);

        // Output found bounce alternatives
        std::string info = "";
        for (int i = 0; i < bounceAlternatives.size(); i++) {
            info += std::to_string(bounceAlternatives[i]) + " ";
        }
        Console::log(Console::VERB, "My bounce alternatives: %s", info.c_str());
    }

    // Send warm-up messages with your pseudorandom bounce destinations
    if (params.isSet("derandomize") && params.isSet("warmup")) {
        IntVec payload({1, 2, 3, 4, 5, 6, 7, 8});
        int numRuns = 5;
        for (int run = 0; run < numRuns; run++) {
            for (auto rank : bounceAlternatives) {
                MyMpi::isend(MPI_COMM_WORLD, rank, MSG_WARMUP, payload);
                Console::log_send(Console::VVERB, rank, "Warmup msg");
            }
        }
    }

    Console::log(Console::VERB, "Global init barrier ...");
    MPI_Barrier(MPI_COMM_WORLD);
    Console::log(Console::VERB, "Passed global init barrier");

    // Begin listening to an incoming message
    MyMpi::beginListening(WORKER);

    mpiMonitorThread = std::thread(mpiMonitor, this);
}

bool Worker::checkTerminate() {
    if (exiting) return true;
    if (globalTimeout > 0 && Timer::elapsedSeconds() > globalTimeout) {
        Console::log(Console::INFO, "Global timeout: terminating.");
        return true;
    }
    return false;
}

void Worker::mainProgram() {

    int iteration = 0;
    float lastMemCheckTime = Timer::elapsedSeconds();
    float lastJobCheckTime = Timer::elapsedSeconds();
    float sleepMicrosecs = 0;

    float memCheckPeriod = 1.0;
    float jobCheckPeriod = 0.05;

    bool doSleep = params.isSet("sleep");
    bool doYield = params.isSet("yield");

    while (!checkTerminate()) {

        if (Timer::elapsedSeconds() - lastMemCheckTime > memCheckPeriod) {

            // Print stats

            // For the this process
            double vm_usage, resident_set; int cpu;
            process_mem_usage(cpu, vm_usage, resident_set);
            vm_usage *= 0.001 * 0.001;
            resident_set *= 0.001 * 0.001;
            Console::log(Console::VERB, "mem cpu=%i vm=%.4fGB rss=%.4fGB", cpu, vm_usage, resident_set);
            checkMemoryBounds(resident_set);

            // For this "management" thread
            double cpu_time;
            long v_ctxswitches, inv_ctxswitches;
            bool success = thread_rusage(cpu_time, v_ctxswitches, inv_ctxswitches);
            if (success) {
                cpu_time = (0.001 * 0.001 * cpu_time);
                double perc_cpu = 100 * cpu_time / Timer::elapsedSeconds();
                double perc_voluntary = 100 * (double)v_ctxswitches / (v_ctxswitches+inv_ctxswitches);
                Console::log(Console::VERB, "meta_thread cpu=%.2f%% voluntary_switches=%.2f%%", 
                        perc_cpu, perc_voluntary);
            }

            // For the current job
            if (currentJob != NULL) currentJob->appl_dumpStats();

            lastMemCheckTime = Timer::elapsedSeconds();
        }

        // If it is time to do balancing (and it is not being done right now)
        if (!balancer->isBalancing() && isTimeForRebalancing()) {
            // Rebalancing
            rebalance();

        } else if (balancer->isBalancing()) {

            // Advance balancing if possible (e.g. an iallreduce finished)
            if (balancer->canContinueBalancing()) {
                bool done = balancer->continueBalancing();
                if (done) finishBalancing();
            }
        }

        // Job communication (e.g. clause sharing)
        if (currentJob != NULL && currentJob->wantsToCommunicate()) {
            currentJob->communicate();
        }

        // Solve loop for active HordeLib instance
        float jobTime = 0;
        if (currentJob != NULL && Timer::elapsedSeconds()-lastJobCheckTime >= jobCheckPeriod) {
            jobTime = Timer::elapsedSeconds();
            lastJobCheckTime = jobTime;

            Job &job = *currentJob;

            int id = job.getId();
            bool abort = false;
            if (job.isRoot()) abort = checkComputationLimits(id);
            if (abort) {
                timeoutJob(id);

            } else {

                bool initializing = job.isInitializing();
                int result = job.appl_solveLoop();

                if (result >= 0) {
                    // Solver done!
                    int jobRootRank = job.getRootNodeRank();
                    IntVec payload({job.getId(), job.getRevision(), result});
                    job.appl_dumpStats();

                    // Signal termination to root -- may be a self message
                    Console::log_send(Console::VERB, jobRootRank, "%s : sending finished info", job.toStr());
                    MyMpi::isend(MPI_COMM_WORLD, jobRootRank, MSG_WORKER_FOUND_RESULT, payload);
                    //stats.increment("sentMessages");

                } else if (initializing && !job.isInitializing()) {
                    if (job.isRoot()) {
                        // Root worker finished initialization: begin growing if applicable
                        if (balancer->hasVolume(id)) updateVolume(id, balancer->getVolume(id));
                    } else {
                        // Non-root worker finished initialization
                        IntVec payload({job.getId()});
                        MyMpi::isend(MPI_COMM_WORLD, job.getParentNodeRank(), MSG_QUERY_VOLUME, payload);
                    }
                }
            }

            jobTime = Timer::elapsedSeconds() - jobTime;
        }

        // Poll a message, if present
        MessageHandlePtr handle;
        float pollTime = Timer::elapsedSeconds();
        while ((handle = MyMpi::poll(ListenerMode::WORKER)) != NULL) {
            pollTime = Timer::elapsedSeconds() - pollTime;

            Console::log(Console::VVVERB, "loop cycle %i", iteration);
            if (jobTime > 0) Console::log(Console::VVVERB, "job time: %.6f s", jobTime);
            Console::log(Console::VVVERB, "poll time: %.6f s", pollTime);

            // Process message
            Console::log_recv(Console::VVVERB, handle->source, "Process msg id=%i, tag %i", handle->id, handle->tag);
            float time = Timer::elapsedSeconds();

            if (handle->tag == MSG_FIND_NODE) {
                handleFindNode(handle);

            } else if (handle->tag == MSG_QUERY_VOLUME) {
                handleQueryVolume(handle);
            
            } else if (handle->tag == MSG_REQUEST_BECOME_CHILD)
                handleRequestBecomeChild(handle);

            else if (handle->tag == MSG_REJECT_BECOME_CHILD)
                handleRejectBecomeChild(handle);

            else if (handle->tag == MSG_ACCEPT_BECOME_CHILD)
                handleAcceptBecomeChild(handle);

            else if (handle->tag == MSG_ACK_ACCEPT_BECOME_CHILD)
                handleAckAcceptBecomeChild(handle);

            else if (handle->tag == MSG_SEND_JOB_DESCRIPTION)
                handleSendJob(handle);

            else if (handle->tag == MSG_UPDATE_VOLUME)
                handleUpdateVolume(handle);

            else if (handle->tag == MSG_JOB_COMMUNICATION)
                handleJobCommunication(handle);

            else if (handle->tag == MSG_WORKER_FOUND_RESULT)
                handleWorkerFoundResult(handle);

            else if (handle->tag == MSG_FORWARD_CLIENT_RANK)
                handleForwardClientRank(handle);
            
            else if (handle->tag == MSG_QUERY_JOB_RESULT)
                handleQueryJobResult(handle);

            else if (handle->tag == MSG_TERMINATE)
                handleTerminate(handle);

            else if (handle->tag == MSG_ABORT)
                handleAbort(handle);
                
            else if (handle->tag == MSG_WORKER_DEFECTING)
                handleWorkerDefecting(handle);

            else if (handle->tag == MSG_NOTIFY_JOB_REVISION)
                handleNotifyJobRevision(handle);
            
            else if (handle->tag == MSG_QUERY_JOB_REVISION_DETAILS)
                handleQueryJobRevisionDetails(handle);
            
            else if (handle->tag == MSG_SEND_JOB_REVISION_DETAILS)
                handleSendJobRevisionDetails(handle);
            
            else if (handle->tag == MSG_ACK_JOB_REVISION_DETAILS)
                handleAckJobRevisionDetails(handle);
            
            else if (handle->tag == MSG_SEND_JOB_REVISION_DATA)
                handleSendJobRevisionData(handle);

            else if (handle->tag == MSG_EXIT) 
                handleExit(handle);
            
            else if (handle->tag == MSG_COLLECTIVES || 
                    handle->tag == MSG_ANYTIME_REDUCTION || 
                    handle->tag == MSG_ANYTIME_BROADCAST) {
                // "Collectives" messages are currently handled only in balancer
                bool done = balancer->continueBalancing(handle);
                if (done) finishBalancing();

            } else if (handle->tag == MSG_WARMUP) 
                Console::log_recv(Console::VVVERB, handle->source, "Warmup msg");
            else
                Console::log_recv(Console::WARN, handle->source, "Unknown message tag %i", handle->tag);

            time = Timer::elapsedSeconds() - time;
            Console::log(Console::VVVERB, "Processing msg, tag %i took %.4f s", handle->tag, time);
            sleepMicrosecs = 0;
            pollTime = Timer::elapsedSeconds();
        }
        
        MyMpi::testSentHandles();

        if (handle == NULL) {
            // Did not process any incoming message: sleep and/or yield
            if (doSleep) {
                // Increase sleep duration, do sleep
                sleepMicrosecs += 100;
                if ((int)sleepMicrosecs > 0)
                    usleep(std::min(10*1000, (int)sleepMicrosecs)); // in microsecs
            }
            if (doYield) {
                // Yield thread, e.g. for some SAT thread
                std::this_thread::yield();
            }
        }

        iteration++;
    }

    Console::flush();
    fflush(stdout);
}

void Worker::handleQueryVolume(MessageHandlePtr& handle) {

    IntVec payload(*handle->recvData);
    int jobId = payload[0];

    assert(hasJob(jobId));
    
    // No volume of this job (yet?) -- ignore.
    if (!balancer->hasVolume(jobId)) return;

    int volume = balancer->getVolume(jobId);
    IntVec response({jobId, volume});

    Console::log_send(Console::VERB, handle->source, "Answering #%i volume query with v=%i", jobId, volume);
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_UPDATE_VOLUME, response);
}

void Worker::handleFindNode(MessageHandlePtr& handle) {

    JobRequest req; req.deserialize(*handle->recvData);

    // Discard request if it has become obsolete
    if (isRequestObsolete(req)) {
        Console::log_recv(Console::VERB, handle->source, "Discarding req. %s from time %.2f", 
                    jobStr(req.jobId, req.requestedNodeIndex).c_str(), req.timeOfBirth);
        return;
    }

    // Discard request if this job already finished to this node's knowledge
    if (hasJob(req.jobId) && jobs[req.jobId]->getState() == JobState::PAST) {
        Console::log(Console::VERB, "Discarding req. %s : already finished", 
                jobStr(req.jobId, req.requestedNodeIndex).c_str());
        return;
    }

    // Decide whether job should be adopted or bounced to another node
    bool adopts = false;
    int maxHops = maxJobHops(req.requestedNodeIndex == 0);
    if (isIdle() && !hasJobCommitments()) {
        // Node is idle and not committed to another job: OK
        adopts = true;

    } else if (req.numHops > maxHops && req.requestedNodeIndex > 0) {
        // Discard job request
        Console::log(Console::VERB, "Discarding req. %s : > %i hops", 
                        jobStr(req.jobId, req.requestedNodeIndex).c_str(), maxHops);
        return;
    
    } else if (req.numHops > maxHops && req.requestedNodeIndex == 0 && !hasJobCommitments()) {
        // Request for a root node exceeded max #hops: Possibly adopt the job while dismissing the active job

        // If this node already computes on _this_ job, don't adopt it
        if (!hasJob(req.jobId) || getJob(req.jobId).isNotInState({ACTIVE, INITIALIZING_TO_ACTIVE})) {

            // Look for an active job that can be suspended
            if (currentJob != NULL) {
                Job& job = *currentJob;
                // Job must be active and a non-root leaf node
                if (job.isInState({ACTIVE, INITIALIZING_TO_ACTIVE}) 
                    && !job.isRoot() && !job.hasLeftChild() && !job.hasRightChild()) {
                    
                    // Inform parent node of the original job  
                    Console::log(Console::VERB, "Suspending %s ...", job.toStr());
                    Console::log(Console::VERB, "... in order to adopt starving %s", 
                                    jobStr(req.jobId, req.requestedNodeIndex).c_str());  
                    IntPair pair(job.getId(), job.getIndex());
                    MyMpi::isend(MPI_COMM_WORLD, job.getParentNodeRank(), MSG_WORKER_DEFECTING, pair);
                    //stats.increment("sentMessages");

                    // Suspend this job
                    job.suspend();
                    setLoad(0, job.getId());

                    adopts = true;
                }
            }
        }
    } 
    
    if (adopts) {
        // Adoption takes place
        std::string jobstr = jobStr(req.jobId, req.requestedNodeIndex);
        Console::log_recv(Console::INFO, handle->source, "Adopting %s after %i hops", jobstr.c_str(), req.numHops);
        assert(isIdle() || Console::fail("Adopting a job, but not idle!"));
        //stats.push_back("hops", req.numHops);

        // Commit on the job, send a request to the parent
        bool fullTransfer = false;
        if (!hasJob(req.jobId)) {
            // Job is not known yet: create instance, request full transfer
            jobs[req.jobId] = new SatJob(params, MyMpi::size(comm), worldRank, req.jobId, epochCounter);
            fullTransfer = true;
        } else if (!getJob(req.jobId).hasJobDescription() && !initializerThreads.count(req.jobId)) {
            // Job is known, but never received full description, and no initializer thread is preparing it:
            // request full transfer
            fullTransfer = true;
        }
        req.fullTransfer = fullTransfer ? 1 : 0;
        jobs[req.jobId]->commit(req);
        jobCommitments[req.jobId] = req;
        MyMpi::isend(MPI_COMM_WORLD, req.requestingNodeRank, MSG_REQUEST_BECOME_CHILD, jobCommitments[req.jobId]);
        //stats.increment("sentMessages");

    } else {
        // Continue job finding procedure somewhere else
        bounceJobRequest(req, handle->source);
    }
}

void Worker::handleRequestBecomeChild(MessageHandlePtr& handle) {

    JobRequest req; req.deserialize(*handle->recvData);
    Console::log_recv(Console::VERB, handle->source, "Request to join job tree as %s", 
                    jobStr(req.jobId, req.requestedNodeIndex).c_str());

    // Retrieve concerned job
    assert(hasJob(req.jobId));
    Job &job = getJob(req.jobId);

    // Check if node should be adopted or rejected
    bool reject = false;
    if (isRequestObsolete(req)) {
        // Wrong (old) epoch
        Console::log_recv(Console::VERB, handle->source, "Reject req. %s : obsolete, from time %.2f", 
                            job.toStr(), req.timeOfBirth);
        reject = true;

    } else if (job.isNotInState({ACTIVE, INITIALIZING_TO_ACTIVE})) {
        // Job is not active
        Console::log_recv(Console::VERB, handle->source, "Reject req. %s : not active", job.toStr());
        Console::log(Console::VERB, "Actual job state: %s", job.jobStateToStr());
        reject = true;
    
    } else if (req.requestedNodeIndex == job.getLeftChildIndex() && job.hasLeftChild()) {
        // Job already has a left child
        Console::log_recv(Console::VERB, handle->source, "Reject req. %s : already has left child", job.toStr());
        reject = true;

    } else if (req.requestedNodeIndex == job.getRightChildIndex() && job.hasRightChild()) {
        // Job already has a right child
        Console::log_recv(Console::VERB, handle->source, "Reject req. %s : already has right child", job.toStr());
        reject = true;

    } else {
        // Adopt the job
        // TODO Check if this node already has the full description!
        const JobDescription& desc = job.getDescription();

        // Send job signature
        JobSignature sig(req.jobId, req.rootRank, req.revision, desc.getTransferSize(true));
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_ACCEPT_BECOME_CHILD, sig);
        //stats.increment("sentMessages");

        // If req.fullTransfer, then wait for the child to acknowledge having received the signature
        if (req.fullTransfer == 1) {
            Console::log_send(Console::INFO, handle->source, "Will send desc. of %s", jobStr(req.jobId, req.requestedNodeIndex).c_str());
        } else {
            Console::log_send(Console::INFO, handle->source, "Resuming child %s", jobStr(req.jobId, req.requestedNodeIndex).c_str());
        }
        // Child *will* start / resume its job solvers
        // Mark new node as one of the node's children
        if (req.requestedNodeIndex == jobs[req.jobId]->getLeftChildIndex()) {
            jobs[req.jobId]->setLeftChild(handle->source);
        } else if (req.requestedNodeIndex == jobs[req.jobId]->getRightChildIndex()) {
            jobs[req.jobId]->setRightChild(handle->source);
        } else assert(req.requestedNodeIndex == 0);
    }

    // If rejected: Send message to rejected child node
    if (reject) {
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_REJECT_BECOME_CHILD, req);
        //stats.increment("sentMessages");
    }
}

void Worker::handleRejectBecomeChild(MessageHandlePtr& handle) {

    // Retrieve committed job
    JobRequest req; req.deserialize(*handle->recvData);
    assert(hasJob(req.jobId));
    Job &job = getJob(req.jobId);
    if (job.isNotInState({COMMITTED, INITIALIZING_TO_COMMITTED}))
        return; // Commitment was already erased

    // Erase commitment
    Console::log_recv(Console::VERB, handle->source, "Rejected to become %s : uncommitting", job.toStr());
    jobCommitments.erase(req.jobId);
    job.uncommit();
}

void Worker::handleAcceptBecomeChild(MessageHandlePtr& handle) {

    // Retrieve according job commitment
    JobSignature sig; sig.deserialize(*handle->recvData);
    assert(jobCommitments.count(sig.jobId));
    JobRequest& req = jobCommitments[sig.jobId];

    if (req.fullTransfer == 1) {
        // Full transfer of job description is required:
        // Send ACK to parent and receive full job description
        Console::log(Console::VERB, "Will receive desc. of #%i, size %i", req.jobId, sig.getTransferSize());
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_ACK_ACCEPT_BECOME_CHILD, req);
        //stats.increment("sentMessages");
        MyMpi::irecv(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_DESCRIPTION, sig.getTransferSize()); // to be received later
        //stats.increment("receivedMessages");

    } else {
        // Already has job description: Directly resume job (if not terminated yet)
        assert(hasJob(req.jobId));
        Job& job = getJob(req.jobId);
        if (job.getState() != JobState::PAST) {
            Console::log_recv(Console::INFO, handle->source, "Starting or resuming %s (state: %s)", 
                        jobStr(req.jobId, req.requestedNodeIndex).c_str(), job.jobStateToStr());
            setLoad(1, req.jobId);
            job.reinitialize(req.requestedNodeIndex, req.rootRank, req.requestingNodeRank);
        }
        // Erase job commitment
        jobCommitments.erase(sig.jobId);
    }
}

void Worker::handleAckAcceptBecomeChild(MessageHandlePtr& handle) {
    JobRequest req; req.deserialize(*handle->recvData);

    // Retrieve and send concerned job description
    assert(hasJob(req.jobId));
    Job& job = getJob(req.jobId);
    // If job already terminated, the description contains the job id ONLY
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_DESCRIPTION, job.getSerializedDescription());
    //stats.increment("sentMessages");
    Console::log_send(Console::VERB, handle->source, "Sent job desc. of %s", job.toStr());

    if (job.getState() == JobState::PAST) {
        // Job already terminated -- send termination signal
        IntPair pair(req.jobId, req.requestedNodeIndex);
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_TERMINATE, pair);
        //stats.increment("sentMessages");
    } else {

        // Mark new node as one of the node's children
        if (req.requestedNodeIndex == job.getLeftChildIndex()) {
            job.setLeftChild(handle->source);
        } else if (req.requestedNodeIndex == job.getRightChildIndex()) {
            job.setRightChild(handle->source);
        } else assert(req.requestedNodeIndex == 0);

        // Send current volume / initial demand update
        if (job.isInState({ACTIVE, INITIALIZING_TO_ACTIVE})) {
            int volume = balancer->getVolume(req.jobId);
            assert(volume >= 1);
            Console::log_send(Console::VERB, handle->source, "Propagating v=%i to new child", volume);
            IntPair jobIdAndVolume(req.jobId, volume);
            MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_UPDATE_VOLUME, jobIdAndVolume);
            //stats.increment("sentMessages");
        }
    }
}

void Worker::handleSendJob(MessageHandlePtr& handle) {
    auto data = handle->recvData;
    Console::log_recv(Console::VVVERB, handle->source, "Receiving some desc. of size %i", data->size());
    int jobId; memcpy(&jobId, data->data(), sizeof(int));
    assert(hasJob(jobId) || Console::fail("I don't know job #%i !", jobId));

    // Erase job commitment
    if (jobCommitments.count(jobId))
        jobCommitments.erase(jobId);

    if (handle->recvData->size() == sizeof(int)) {
        // Empty job description!
        Console::log(Console::VERB, "Received empty desc. of #%i - uncommitting", jobId);
        if (getJob(jobId).isInState({COMMITTED, INITIALIZING_TO_COMMITTED})) {
            getJob(jobId).uncommit();
        }
        return;
    }

    // Initialize job inside a separate thread
    setLoad(1, jobId);
    Job& job = getJob(jobId);
    job.beginInitialization();
    Console::log(Console::VERB, "Received desc. of #%i - initializing", jobId);

    assert(!initializerThreads.count(jobId));
    initializerThreads[jobId] = std::thread(&Worker::initJob, this, handle);
}

void Worker::initJob(MessageHandlePtr handle) {
    
    // Deserialize job description
    assert(handle->recvData->size() >= sizeof(int));
    int jobId; memcpy(&jobId, handle->recvData->data(), sizeof(int));
    Console::log_recv(Console::VERB, handle->source, "Deserializing job #%i, desc. of size %i", jobId, handle->recvData->size());
    Job& job = getJob(jobId);
    job.setDescription(handle->recvData);

    // Remember arrival and initialize used CPU time (if root node)
    jobArrivals[jobId] = Timer::elapsedSeconds();
    
    if (job.isInState({INITIALIZING_TO_PAST})) {
        // Job was already aborted
        job.terminate();
    } else {
        // Initialize job
        job.initialize();
    }
}

void Worker::handleUpdateVolume(MessageHandlePtr& handle) {
    IntPair recv(*handle->recvData);
    int jobId = recv.first;
    int volume = recv.second;
    if (!hasJob(jobId)) {
        Console::log(Console::WARN, "Got volume update for unknown #%i", jobId);
        return;
    }

    // Update volume assignment inside balancer and in actual job instance
    // (and its children)
    balancer->updateVolume(jobId, volume);
    updateVolume(jobId, volume);
}

void Worker::handleJobCommunication(MessageHandlePtr& handle) {

    // Deserialize job-specific message
    JobMessage msg; msg.deserialize(*handle->recvData);
    int jobId = msg.jobId;
    if (!hasJob(jobId)) {
        Console::log(Console::WARN, "Job message from unknown job #%i", jobId);
        return;
    }
    // Give message to corresponding job
    Job& job = getJob(jobId);
    job.appl_communicate(handle->source, msg);
}

void Worker::handleWorkerFoundResult(MessageHandlePtr& handle) {

    // Retrieve job
    IntVec res(*handle->recvData);
    int jobId = res[0];
    int revision = res[1];
    assert(hasJob(jobId) && getJob(jobId).isRoot());
    Console::log_recv(Console::VERB, handle->source, "Result found for job #%i", jobId);
    if (getJob(jobId).isInState({PAST, INITIALIZING_TO_PAST})) {
        Console::log_recv(Console::VERB, handle->source, "Discard obsolete result for job #%i", jobId);
        return;
    }
    if (getJob(jobId).getRevision() > revision) {
        Console::log_recv(Console::VERB, handle->source, "Discarding obsolete result for job #%i rev. %i", jobId, revision);
        return;
    }

    // Redirect termination signal
    IntPair payload(jobId, getJob(jobId).getParentNodeRank());
    if (handle->source == worldRank) {
        // Self-message of root node: Directly send termination message to client
        informClient(payload.first, payload.second);
    } else {
        // Send rank of client node to the finished worker,
        // such that the worker can inform the client of the result
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_FORWARD_CLIENT_RANK, payload);
        //stats.increment("sentMessages");
        Console::log_send(Console::VERB, handle->source, "Forwarding rank of client (%i)", payload.second); 
    }

    // Terminate job and propagate termination message
    if (getJob(jobId).getDescription().isIncremental()) {
        handleInterrupt(handle);
    } else {
        handleTerminate(handle);
    }
}

void Worker::handleForwardClientRank(MessageHandlePtr& handle) {

    // Receive rank of the job's client
    IntPair recv(*handle->recvData);
    int jobId = recv.first;
    int clientRank = recv.second;
    assert(hasJob(jobId));

    // Inform client of the found job result
    informClient(jobId, clientRank);
}

void Worker::handleQueryJobResult(MessageHandlePtr& handle) {

    // Receive acknowledgement that the client received the advertised result size
    // and wishes to receive the full job result
    int jobId; memcpy(&jobId, handle->recvData->data(), sizeof(int));
    assert(hasJob(jobId));
    const JobResult& result = getJob(jobId).getResult();
    Console::log_send(Console::VERB, handle->source, "Sending full result to client");
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_RESULT, result);
    //stats.increment("sentMessages");
}

void Worker::handleTerminate(MessageHandlePtr& handle) {

    int jobId; memcpy(&jobId, handle->recvData->data(), sizeof(int));
    interruptJob(handle, jobId, /*terminate=*/true, /*reckless=*/false);
}

void Worker::handleInterrupt(MessageHandlePtr& handle) {

    int jobId; memcpy(&jobId, handle->recvData->data(), sizeof(int));
    interruptJob(handle, jobId, /*terminate=*/false, /*reckless=*/false);
}

void Worker::handleAbort(MessageHandlePtr& handle) {

    int jobId; memcpy(&jobId, handle->recvData->data(), sizeof(int));

    if (getJob(jobId).isRoot()) {
        // Forward information on aborted job to client
        MyMpi::isend(MPI_COMM_WORLD, getJob(jobId).getParentNodeRank(), MSG_ABORT, handle->recvData);
    }

    interruptJob(handle, jobId, /*terminate=*/true, /*reckless=*/true);
}

void Worker::handleWorkerDefecting(MessageHandlePtr& handle) {

    // Retrieve job
    IntPair recv(*handle->recvData);
    int jobId = recv.first;
    int index = recv.second;
    assert(hasJob(jobId));
    Job& job = getJob(jobId);

    // Which child is defecting?
    if (job.getLeftChildIndex() == index) {
        // Prune left child
        job.unsetLeftChild();
    } else if (job.getRightChildIndex() == index) {
        // Prune right child
        job.unsetRightChild();
    } else {
        Console::fail("%s : unknown child %s is defecting to another node", job.toStr(), jobStr(jobId, index).c_str());
    }
    
    int nextNodeRank;
    if (params.isSet("derandomize")) {
        nextNodeRank = Random::choice(bounceAlternatives);
    } else {
        nextNodeRank = getRandomWorkerNode();
    }

    // Initiate search for a replacement for the defected child
    Console::log(Console::VERB, "%s : trying to find new child replacing defected node %s", 
                    job.toStr(), jobStr(jobId, index).c_str());
    JobRequest req(jobId, job.getRootNodeRank(), worldRank, index, Timer::elapsedSeconds(), 0);
    MyMpi::isend(MPI_COMM_WORLD, nextNodeRank, MSG_FIND_NODE, req);
    //stats.increment("sentMessages");
}

void Worker::handleNotifyJobRevision(MessageHandlePtr& handle) {
    IntVec payload(*handle->recvData);
    int jobId = payload[0];
    int revision = payload[1];

    assert(hasJob(jobId));
    assert(getJob(jobId).isInState({STANDBY}) || Console::fail("%s : state %s", 
            getJob(jobId).toStr(), jobStateStrings[getJob(jobId).getState()]));

    // No other job running on this node
    assert(load == 1 && currentJob->getId() == jobId);

    // Request receiving information on revision size
    Job& job = getJob(jobId);
    int lastKnownRevision = job.getRevision();
    if (revision > lastKnownRevision) {
        Console::log(Console::VERB, "Received revision update #%i rev. %i (I am at rev. %i)", jobId, revision, lastKnownRevision);
        IntVec request({jobId, lastKnownRevision+1, revision});
        MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_QUERY_JOB_REVISION_DETAILS, request);
    } else {
        // TODO ???
        Console::log(Console::WARN, "Useless revision update #%i rev. %i (I am already at rev. %i)", jobId, revision, lastKnownRevision);
    }
}

void Worker::handleQueryJobRevisionDetails(MessageHandlePtr& handle) {
    IntVec request(*handle->recvData);
    int jobId = request[0];
    int firstRevision = request[1];
    int lastRevision = request[2];

    assert(hasJob(jobId));

    JobDescription& desc = getJob(jobId).getDescription();
    IntVec response({jobId, firstRevision, lastRevision, desc.getTransferSize(firstRevision, lastRevision)});
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_REVISION_DETAILS, response);
}

void Worker::handleSendJobRevisionDetails(MessageHandlePtr& handle) {
    IntVec response(*handle->recvData);
    //int jobId = response[0];
    int transferSize = response[3];
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_ACK_JOB_REVISION_DETAILS, handle->recvData);
    MyMpi::irecv(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_REVISION_DATA, transferSize);
}

void Worker::handleAckJobRevisionDetails(MessageHandlePtr& handle) {
    IntVec response(*handle->recvData);
    int jobId = response[0];
    int firstRevision = response[1];
    int lastRevision = response[2];
    //int transferSize = response[3];
    MyMpi::isend(MPI_COMM_WORLD, handle->source, MSG_SEND_JOB_REVISION_DATA, 
                getJob(jobId).getDescription().serialize(firstRevision, lastRevision));
}

void Worker::handleSendJobRevisionData(MessageHandlePtr& handle) {
    int jobId; memcpy(&jobId, handle->recvData->data(), sizeof(int));
    assert(hasJob(jobId) && getJob(jobId).isInState({STANDBY}));

    // TODO in separate thread
    Job& job = getJob(jobId);
    job.addAmendment(handle->recvData);
    int revision = job.getDescription().getRevision();
    Console::log(Console::INFO, "%s : computing on #%i rev. %i", job.toStr(), jobId, revision);
    
    // Propagate to children
    if (job.hasLeftChild()) {
        IntVec payload({jobId, revision});
        MyMpi::isend(MPI_COMM_WORLD, job.getLeftChildNodeRank(), MSG_NOTIFY_JOB_REVISION, payload);
    }
    if (job.hasRightChild()) {
        IntVec payload({jobId, revision});
        MyMpi::isend(MPI_COMM_WORLD, job.getRightChildNodeRank(), MSG_NOTIFY_JOB_REVISION, payload);
    }
}

void Worker::handleIncrementalJobFinished(MessageHandlePtr& handle) {
    int jobId; memcpy(&jobId, handle->recvData->data(), sizeof(int));
    interruptJob(handle, jobId, /*terminate=*/true, /*reckless=*/false);
}

void Worker::handleExit(MessageHandlePtr& handle) {
    Console::log_recv(Console::VERB, handle->source, "Received exit signal");
    exiting = true;
}

void Worker::interruptJob(MessageHandlePtr& handle, int jobId, bool terminate, bool reckless) {

    assert(hasJob(jobId));
    Job& job = getJob(jobId);

    // Do not terminate yet if the job is still in a committed state, because the job description should still arrive
    // (except if in reckless mode, where the description probably will never arrive from the parent)
    if (!reckless && job.isInState({COMMITTED, INITIALIZING_TO_COMMITTED})) {
        Console::log(Console::INFO, "%s : deferring interruption/termination : desc. did not arrive yet", job.toStr());
        MyMpi::deferHandle(handle); // do not consume this message while job state is "COMMITTED"
    }

    bool acceptMessage = job.isNotInState({NONE, STORED, PAST});

    // Propagate message down the job tree
    int msgTag;
    if (terminate && reckless) msgTag = MSG_ABORT;
    else if (terminate) msgTag = MSG_TERMINATE;
    else msgTag = MSG_INTERRUPT;
    if (acceptMessage && job.hasLeftChild()) {
        MyMpi::isend(MPI_COMM_WORLD, job.getLeftChildNodeRank(), msgTag, handle->recvData);
        Console::log_send(Console::VERB, job.getLeftChildNodeRank(), "Propagating interruption of %s ...", job.toStr());
        //stats.increment("sentMessages");
    }
    if (acceptMessage && job.hasRightChild()) {
        MyMpi::isend(MPI_COMM_WORLD, job.getRightChildNodeRank(), msgTag, handle->recvData);
        Console::log_send(Console::VERB, job.getRightChildNodeRank(), "Propagating interruption of %s ...", job.toStr());
        //stats.increment("sentMessages");
    }
    for (auto childRank : job.getPastChildren()) {
        MyMpi::isend(MPI_COMM_WORLD, childRank, msgTag, handle->recvData);
        Console::log_send(Console::VERB, childRank, "Propagating interruption of %s (past child) ...", job.toStr());
    }
    job.getPastChildren().clear();

    if (acceptMessage) {
        // Stop / terminate
        if (job.isInitializing() || job.isInState({ACTIVE, STANDBY, SUSPENDED})) {
            Console::log(Console::INFO, "%s : interrupting (state: %s)", job.toStr(), job.jobStateToStr());
            job.stop(); // Solvers are interrupted, not suspended!
            Console::log(Console::VERB, "%s : interrupted", job.toStr());
            if (terminate) {
                if (getLoad() && currentJob->getId() == job.getId()) setLoad(0, job.getId());
                job.terminate();
                Console::log(Console::INFO, "%s : terminated", job.toStr());
                balancer->updateVolume(jobId, 0);
            } 
        }
    }
}

void Worker::informClient(int jobId, int clientRank) {
    const JobResult& result = getJob(jobId).getResult();

    // Send "Job done!" with advertised result size to client
    Console::log_send(Console::VERB, clientRank, "%s : sending JOB_DONE to client", getJob(jobId).toStr());
    IntPair payload(jobId, result.getTransferSize());
    MyMpi::isend(MPI_COMM_WORLD, clientRank, MSG_JOB_DONE, payload);
    //stats.increment("sentMessages");
}

void Worker::setLoad(int load, int whichJobId) {
    assert(load + this->load == 1); // (load WAS 1) XOR (load BECOMES 1)
    this->load = load;

    // Measure time for which worker was {idle,busy}
    float now = Timer::elapsedSeconds();
    //stats.add((load == 0 ? "busyTime" : "idleTime"), now - lastLoadChange);
    lastLoadChange = now;
    assert(hasJob(whichJobId));
    if (load == 1) {
        assert(currentJob == NULL);
        Console::log(Console::VERB, "LOAD 1 (+%s)", getJob(whichJobId).toStr());
        currentJob = &getJob(whichJobId);
    }
    if (load == 0) {
        assert(currentJob != NULL);
        Console::log(Console::VERB, "LOAD 0 (-%s)", getJob(whichJobId).toStr());
        currentJob = NULL;
    }
}

int Worker::getRandomWorkerNode() {

    // All clients are excluded from drawing
    std::set<int> excludedNodes = std::set<int>(this->clientNodes);
    // THIS node is also excluded from drawing
    excludedNodes.insert(worldRank);
    // Draw a node from the remaining nodes
    int randomOtherNodeRank = MyMpi::random_other_node(MPI_COMM_WORLD, excludedNodes);
    return randomOtherNodeRank;
}

void Worker::bounceJobRequest(JobRequest& request, int senderRank) {

    // Increment #hops
    request.numHops++;
    //stats.increment("bouncedJobs");
    int num = request.numHops;

    // Show warning if #hops is a large power of two
    if ((num >= 512) && ((num & (num - 1)) == 0)) {
        Console::log(Console::WARN, "Hop no. %i for %s", num, jobStr(request.jobId, request.requestedNodeIndex).c_str());
    }

    int nextRank;
    if (params.isSet("derandomize")) {
        // Get random choice from bounce alternatives
        nextRank = Random::choice(bounceAlternatives);
        // while skipping the requesting node and the sender
        while (nextRank == request.requestingNodeRank || nextRank == senderRank) {
            nextRank = Random::choice(bounceAlternatives);
        }
    } else {
        // Generate pseudorandom permutation of this request
        int n = MyMpi::size(comm);
        AdjustablePermutation perm(n, 3 * request.jobId + 7 * request.requestedNodeIndex + 11 * request.requestingNodeRank);
        // Fetch next index of permutation based on number of hops
        int permIdx = request.numHops % n;
        nextRank = perm.get(permIdx);
        // (while skipping yourself, the requesting node, and the sender)
        while (nextRank == worldRank || nextRank == request.requestingNodeRank || nextRank == senderRank) {
            permIdx = (permIdx+1) % n;
            nextRank = perm.get(permIdx);
        }
    }

    // Send request to "next" worker node
    Console::log_send(Console::VVERB, nextRank, "Hop %s", jobStr(request.jobId, request.requestedNodeIndex).c_str());
    MyMpi::isend(MPI_COMM_WORLD, nextRank, MSG_FIND_NODE, request);
    //stats.increment("sentMessages");
}

void Worker::rebalance() {

    // Initiate balancing procedure
    bool done = balancer->beginBalancing(jobs);
    // If nothing to do, finish up balancing
    if (done) finishBalancing();
}

void Worker::finishBalancing() {
    // All collective operations are done; reset synchronized timer
    epochCounter.resetLastSync();

    // Retrieve balancing results
    Console::log(Console::VVVERB, "Finishing balancing ...");
    jobVolumes = balancer->getBalancingResult();
    Console::log(MyMpi::rank(comm) == 0 ? Console::VERB : Console::VVERB, "Balancing completed.");

    // Add last slice of idle/busy time 
    //stats.add((load == 1 ? "busyTime" : "idleTime"), Timer::elapsedSeconds() - lastLoadChange);
    lastLoadChange = Timer::elapsedSeconds();

    // Advance to next epoch
    epochCounter.increment();
    Console::log(Console::VVERB, "Advancing to epoch %i", epochCounter.getEpoch());
    
    // Update volumes found during balancing, and trigger job expansions / shrinkings
    for (auto it = jobVolumes.begin(); it != jobVolumes.end(); ++it) {
        int jobId = it->first;
        int volume = it->second;
        if (hasJob(jobId) && getJob(jobId).getLastVolume() != volume) {
            Console::log(Console::INFO, "#%i : update v=%i", it->first, it->second);
        }
        updateVolume(it->first, it->second);
    }
}

void Worker::timeoutJob(int jobId) {
    // "Virtual self message" aborting the job
    IntVec payload({jobId, getJob(jobId).getRevision()});
    MessageHandlePtr handle(new MessageHandle(MyMpi::nextHandleId()));
    handle->source = worldRank;
    handle->recvData = payload.serialize();
    handle->tag = MSG_ABORT;
    handle->finished = true;
    handleAbort(handle);
}

bool Worker::checkComputationLimits(int jobId) {

    if (!getJob(jobId).isRoot()) return false;

    if (!lastLimitCheck.count(jobId) || !jobCpuTimeUsed.count(jobId)) {
        // Job is new
        lastLimitCheck[jobId] = Timer::elapsedSeconds();
        jobCpuTimeUsed[jobId] = 0;
        return false;
    }

    float elapsedTime = Timer::elapsedSeconds() - lastLimitCheck[jobId];
    bool terminate = false;
    assert(elapsedTime >= 0);

    // Calculate CPU seconds: (volume during last epoch) * #threads * (effective time of last epoch) 
    float newCpuTime = (jobVolumes.count(jobId) ? jobVolumes[jobId] : 1) * params.getIntParam("t") * elapsedTime;
    assert(newCpuTime >= 0);
    jobCpuTimeUsed[jobId] += newCpuTime;
    float cpuLimit = params.getFloatParam("cpuh-per-instance")*3600.f;
    bool hasCpuLimit = cpuLimit > 0;
    
    if (hasCpuLimit && jobCpuTimeUsed[jobId] > cpuLimit) {
        // Job exceeded its cpu time limit
        Console::log(Console::INFO, "#%i CPU TIMEOUT: aborting", jobId);
        terminate = true;

    } else {

        // Calculate wall clock time
        float jobAge = currentJob->getAge();
        float timeLimit = params.getFloatParam("time-per-instance");
        if (timeLimit > 0 && jobAge > timeLimit) {
            // Job exceeded its wall clock time limit
            Console::log(Console::INFO, "#%i WALLCLOCK TIMEOUT: aborting", jobId);
            terminate = true;
        }
    }
    
    if (terminate) lastLimitCheck.erase(jobId);
    else lastLimitCheck[jobId] = Timer::elapsedSeconds();
    
    return terminate;
}

void Worker::updateVolume(int jobId, int volume) {

    Job &job = getJob(jobId);
    job.setLastVolume(volume);

    if (job.isNotInState({ACTIVE, INITIALIZING_TO_ACTIVE})) {
        // Job is not active right now
        return;
    }

    // Prepare volume update to propagate down the job tree
    IntPair payload(jobId, volume);

    // Root node update message
    int thisIndex = job.getIndex();
    if (thisIndex == 0) {
        Console::log(Console::VERB, "%s : update volume to %i", job.toStr(), volume);
    }

    // Left child
    int nextIndex = job.getLeftChildIndex();
    if (job.hasLeftChild()) {
        // Propagate left
        MyMpi::isend(MPI_COMM_WORLD, job.getLeftChildNodeRank(), MSG_UPDATE_VOLUME, payload);
        //stats.increment("sentMessages");
        if (nextIndex >= volume) {
            // Prune child
            Console::log_send(Console::VERB, job.getLeftChildNodeRank(), "%s : Pruning left child", job.toStr());
            job.unsetLeftChild();
        }
    } else if (job.hasJobDescription() && nextIndex < volume && !jobCommitments.count(jobId)) {
        // Grow left
        JobRequest req(jobId, job.getRootNodeRank(), worldRank, nextIndex, Timer::elapsedSeconds(), 0);
        int nextNodeRank = job.getLeftChildNodeRank();
        MyMpi::isend(MPI_COMM_WORLD, nextNodeRank, MSG_FIND_NODE, req);
        //stats.increment("sentMessages");
    }

    // Right child
    nextIndex = job.getRightChildIndex();
    if (job.hasRightChild()) {
        // Propagate right
        MyMpi::isend(MPI_COMM_WORLD, job.getRightChildNodeRank(), MSG_UPDATE_VOLUME, payload);
        //stats.increment("sentMessages");
        if (nextIndex >= volume) {
            // Prune child
            Console::log_send(Console::VERB, job.getRightChildNodeRank(), "%s : Pruning right child", job.toStr());
            job.unsetRightChild();
        }
    } else if (job.hasJobDescription() && nextIndex < volume && !jobCommitments.count(jobId)) {
        // Grow right
        JobRequest req(jobId, job.getRootNodeRank(), worldRank, nextIndex, Timer::elapsedSeconds(), 0);
        int nextNodeRank = job.getRightChildNodeRank();
        MyMpi::isend(MPI_COMM_WORLD, nextNodeRank, MSG_FIND_NODE, req);
        //stats.increment("sentMessages");
    }

    // Shrink (and pause solving) if necessary
    if (thisIndex > 0 && thisIndex >= volume) {
        jobs[jobId]->suspend();
        setLoad(0, jobId);
    }
}

bool Worker::isTimeForRebalancing() {
    return epochCounter.getSecondsSinceLastSync() >= params.getFloatParam("p");
}

void Worker::checkMemoryBounds(float rssGb) {
    if (!params.isSet("mem")) return;
    float maxMem = params.getFloatParam("mem");
    if (rssGb > 0.9 * maxMem) {
        int jobId = pickJobToForget();
        if (jobId < 0) return;
    }
}

int Worker::pickJobToForget() {

    // Try to pick an inactive non-root leaf job of maximum description size
    int maxSize = 0;
    int jobId;
    for (auto idJobPair : jobs) {
        int id = idJobPair.first;
        Job& job = *idJobPair.second;

        if (job.isNotInState({SUSPENDED, INITIALIZING_TO_SUSPENDED, PAST, INITIALIZING_TO_PAST}))
            continue;
        if (!job.hasJobDescription()) continue;
        if (job.isRoot()) continue;
        if (job.hasLeftChild() || job.hasRightChild()) continue;

        int size = job.getSerializedDescription()->size();
        if (size > maxSize) {
            maxSize = size;
            jobId = id;
        }
    }
    if (maxSize > 0) {
        return jobId;
    }

    // If there is a current job: pick if non-root leaf
    if (currentJob != NULL) {
        Job& job = *currentJob;
        if (!job.isRoot() && !job.hasLeftChild() && !job.hasRightChild()) {
            return job.getId();
        }
    }

    // No job found that can be forgotten
    return -1;
}

Worker::~Worker() {

    exiting = true;

    for (auto idJobPair : jobs) {

        int id = idJobPair.first;
        Job* job = idJobPair.second;
        Console::log(Console::VVERB, "Cleaning up %s ...", job->toStr());

        // Join and delete initializer thread
        if (initializerThreads.count(id)) {
            Console::log(Console::VVERB, "Cleaning up init thread of %s ...", job->toStr());
            if (initializerThreads[id].joinable()) {
                initializerThreads[id].join();
            }
            initializerThreads.erase(id);
            Console::log(Console::VVERB, "Cleaned up init thread of %s.", job->toStr());
        }
        // Delete job and its solvers
        delete job;
    }

    if (mpiMonitorThread.joinable())
        mpiMonitorThread.join();

    Console::log(Console::VVERB, "Leaving worker destructor");
}