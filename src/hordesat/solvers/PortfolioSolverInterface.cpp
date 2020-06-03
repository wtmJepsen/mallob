

#include <map>
#include <chrono>

#include "../utilities/Threading.h"
#include "../utilities/logging_interface.h"

#include "PortfolioSolverInterface.h"

using namespace std::chrono;

Mutex timeCallbackLock;
std::map<std::string, high_resolution_clock::time_point> times;
std::string currentSolverName = "";
high_resolution_clock::time_point lglSolverStartTime;

void updateTimer(std::string jobName) {
	auto lock = timeCallbackLock.getLock();
	if (currentSolverName == jobName) return;
	if (!times.count(jobName)) {
		times[jobName] = high_resolution_clock::now();
	}
	lglSolverStartTime = times[jobName];
	currentSolverName = jobName;
}
double getTime() {
    high_resolution_clock::time_point nowTime = high_resolution_clock::now();
	timeCallbackLock.lock();
    duration<double, std::milli> time_span = nowTime - lglSolverStartTime;    
	timeCallbackLock.unlock();
	return time_span.count() / 1000;
}
void slog(PortfolioSolverInterface* slv, int verbosityLevel, const char* fmt, ...) {
	std::string msg = slv->_global_name + " ";
	msg += fmt;
	va_list vl;
	va_start(vl, fmt);
	slv->_logger.log_va_list(verbosityLevel, msg.c_str(), vl);
	va_end(vl);
}

PortfolioSolverInterface::PortfolioSolverInterface(LoggingInterface& logger, int globalId, int localId, std::string jobname) 
		: _logger(logger), _global_id(globalId), _local_id(localId), _job_name(jobname) {
	updateTimer(jobname);
	_global_name = "<h-" + jobname + "_S" + std::to_string(globalId) + ">";
}

void PortfolioSolverInterface::interrupt() {
	setSolverInterrupt();
}
void PortfolioSolverInterface::uninterrupt() {
	updateTimer(_job_name);
	unsetSolverInterrupt();
}
void PortfolioSolverInterface::suspend() {
	setSolverSuspend();
}
void PortfolioSolverInterface::resume() {
	updateTimer(_job_name);
	unsetSolverSuspend();
}