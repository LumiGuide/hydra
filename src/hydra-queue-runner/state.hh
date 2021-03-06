#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <queue>

#include "db.hh"
#include "counter.hh"
#include "pathlocks.hh"
#include "pool.hh"
#include "sync.hh"

#include "store-api.hh"
#include "derivations.hh"


typedef unsigned int BuildID;

typedef std::chrono::time_point<std::chrono::system_clock> system_time;


typedef enum {
    bsSuccess = 0,
    bsFailed = 1,
    bsDepFailed = 2,
    bsAborted = 3,
    bsFailedWithOutput = 6,
    bsTimedOut = 7,
    bsUnsupported = 9,
    bsLogLimitExceeded = 10,
} BuildStatus;


typedef enum {
    bssSuccess = 0,
    bssFailed = 1,
    bssAborted = 4,
    bssTimedOut = 7,
    bssCachedFailure = 8,
    bssUnsupported = 9,
    bssLogLimitExceeded = 10,
    bssBusy = 100, // not stored
} BuildStepStatus;


struct RemoteResult : nix::BuildResult
{
    time_t startTime = 0, stopTime = 0;
    nix::Path logFile;

    bool canRetry()
    {
        return status == TransientFailure || status == MiscFailure;
    }
};


struct Step;
struct BuildOutput;


class Jobset
{
public:

    typedef std::shared_ptr<Jobset> ptr;
    typedef std::weak_ptr<Jobset> wptr;

    static const time_t schedulingWindow = 24 * 60 * 60;

private:

    std::atomic<time_t> seconds{0};
    std::atomic<unsigned int> shares{1};

    /* The start time and duration of the most recent build steps. */
    Sync<std::map<time_t, time_t>> steps;

public:

    double shareUsed()
    {
        return (double) seconds / shares;
    }

    void setShares(int shares_)
    {
        assert(shares_ > 0);
        shares = shares_;
    }

    time_t getSeconds() { return seconds; }

    void addStep(time_t startTime, time_t duration);

    void pruneSteps();
};


struct Build
{
    typedef std::shared_ptr<Build> ptr;
    typedef std::weak_ptr<Build> wptr;

    BuildID id;
    nix::Path drvPath;
    std::map<std::string, nix::Path> outputs;
    std::string projectName, jobsetName, jobName;
    time_t timestamp;
    unsigned int maxSilentTime, buildTimeout;
    int localPriority, globalPriority;

    std::shared_ptr<Step> toplevel;

    Jobset::ptr jobset;

    std::atomic_bool finishedInDB{false};

    std::string fullJobName()
    {
        return projectName + ":" + jobsetName + ":" + jobName;
    }

    void propagatePriorities();
};


struct Step
{
    typedef std::shared_ptr<Step> ptr;
    typedef std::weak_ptr<Step> wptr;

    nix::Path drvPath;
    nix::Derivation drv;
    std::set<std::string> requiredSystemFeatures;
    bool preferLocalBuild;
    std::string systemType; // concatenation of drv.platform and requiredSystemFeatures

    struct State
    {
        /* Whether the step has finished initialisation. */
        bool created = false;

        /* The build steps on which this step depends. */
        std::set<Step::ptr> deps;

        /* The build steps that depend on this step. */
        std::vector<Step::wptr> rdeps;

        /* Builds that have this step as the top-level derivation. */
        std::vector<Build::wptr> builds;

        /* Jobsets to which this step belongs. Used for determining
           scheduling priority. */
        std::set<Jobset::ptr> jobsets;

        /* Number of times we've tried this step. */
        unsigned int tries = 0;

        /* Point in time after which the step can be retried. */
        system_time after;

        /* The highest global priority of any build depending on this
           step. */
        int highestGlobalPriority{0};

        /* The lowest share used of any jobset depending on this
           step. */
        double lowestShareUsed;

        /* The highest local priority of any build depending on this
           step. */
        int highestLocalPriority{0};

        /* The lowest ID of any build depending on this step. */
        BuildID lowestBuildID{std::numeric_limits<BuildID>::max()};

        /* The time at which this step became runnable. */
        system_time runnableSince;
    };

    std::atomic_bool finished{false}; // debugging

    Sync<State> state;

    ~Step()
    {
        //printMsg(lvlError, format("destroying step %1%") % drvPath);
    }
};


void getDependents(Step::ptr step, std::set<Build::ptr> & builds, std::set<Step::ptr> & steps);

/* Call ‘visitor’ for a step and all its dependencies. */
void visitDependencies(std::function<void(Step::ptr)> visitor, Step::ptr step);


struct Machine
{
    typedef std::shared_ptr<Machine> ptr;

    bool enabled{true};

    std::string sshName, sshKey;
    std::set<std::string> systemTypes, supportedFeatures, mandatoryFeatures;
    unsigned int maxJobs = 1;
    float speedFactor = 1.0;
    std::string sshPublicHostKey;

    struct State {
        typedef std::shared_ptr<State> ptr;
        counter currentJobs{0};
        counter nrStepsDone{0};
        counter totalStepTime{0}; // total time for steps, including closure copying
        counter totalStepBuildTime{0}; // total build time for steps
        std::atomic<time_t> idleSince{0};

        struct ConnectInfo
        {
            system_time lastFailure, disabledUntil;
            unsigned int consecutiveFailures;
        };
        Sync<ConnectInfo> connectInfo;

        /* Mutex to prevent multiple threads from sending data to the
           same machine (which would be inefficient). */
        std::mutex sendLock;
    };

    State::ptr state;

    bool supportsStep(Step::ptr step)
    {
        if (systemTypes.find(step->drv.platform) == systemTypes.end()) return false;
        for (auto & f : mandatoryFeatures)
            if (step->requiredSystemFeatures.find(f) == step->requiredSystemFeatures.end()
                && !(step->preferLocalBuild && f == "local"))
                return false;
        for (auto & f : step->requiredSystemFeatures)
            if (supportedFeatures.find(f) == supportedFeatures.end()) return false;
        return true;
    }
};


class State
{
private:

    // FIXME: Make configurable.
    const unsigned int maxTries = 5;
    const unsigned int retryInterval = 60; // seconds
    const float retryBackoff = 3.0;
    const unsigned int maxParallelCopyClosure = 4;

    nix::Path hydraData, logDir;

    /* The queued builds. */
    typedef std::map<BuildID, Build::ptr> Builds;
    Sync<Builds> builds;

    /* The jobsets. */
    typedef std::map<std::pair<std::string, std::string>, Jobset::ptr> Jobsets;
    Sync<Jobsets> jobsets;

    /* All active or pending build steps (i.e. dependencies of the
       queued builds). Note that these are weak pointers. Steps are
       kept alive by being reachable from Builds or by being in
       progress. */
    typedef std::map<nix::Path, Step::wptr> Steps;
    Sync<Steps> steps;

    /* Build steps that have no unbuilt dependencies. */
    typedef std::list<Step::wptr> Runnable;
    Sync<Runnable> runnable;

    /* CV for waking up the dispatcher. */
    Sync<bool> dispatcherWakeup;
    std::condition_variable_any dispatcherWakeupCV;

    /* PostgreSQL connection pool. */
    Pool<Connection> dbPool;

    /* The build machines. */
    typedef std::map<std::string, Machine::ptr> Machines;
    Sync<Machines> machines; // FIXME: use atomic_shared_ptr

    /* Various stats. */
    time_t startedAt;
    counter nrBuildsRead{0};
    counter nrBuildsDone{0};
    counter nrStepsDone{0};
    counter nrActiveSteps{0};
    counter nrStepsBuilding{0};
    counter nrStepsCopyingTo{0};
    counter nrStepsCopyingFrom{0};
    counter nrStepsWaiting{0};
    counter nrRetries{0};
    counter maxNrRetries{0};
    counter totalStepTime{0}; // total time for steps, including closure copying
    counter totalStepBuildTime{0}; // total build time for steps
    counter nrQueueWakeups{0};
    counter nrDispatcherWakeups{0};
    counter bytesSent{0};
    counter bytesReceived{0};

    /* Log compressor work queue. */
    Sync<std::queue<nix::Path>> logCompressorQueue;
    std::condition_variable_any logCompressorWakeup;

    /* Notification sender work queue. FIXME: if hydra-queue-runner is
       killed before it has finished sending notifications about a
       build, then the notifications may be lost. It would be better
       to mark builds with pending notification in the database. */
    typedef std::pair<BuildID, std::vector<BuildID>> NotificationItem;
    Sync<std::queue<NotificationItem>> notificationSenderQueue;
    std::condition_variable_any notificationSenderWakeup;

    /* Specific build to do for --build-one (testing only). */
    BuildID buildOne;

    /* Statistics per machine type for the Hydra auto-scaler. */
    struct MachineType
    {
        unsigned int runnable{0}, running{0};
        system_time lastActive;
        std::chrono::seconds waitTime; // time runnable steps have been waiting
    };

    Sync<std::map<std::string, MachineType>> machineTypes;

    struct MachineReservation
    {
        typedef std::shared_ptr<MachineReservation> ptr;
        State & state;
        Step::ptr step;
        Machine::ptr machine;
        MachineReservation(State & state, Step::ptr step, Machine::ptr machine);
        ~MachineReservation();
    };

    std::atomic<time_t> lastDispatcherCheck{0};

public:
    State();

private:

    void clearBusy(Connection & conn, time_t stopTime);

    void parseMachines(const std::string & contents);

    /* Thread to reload /etc/nix/machines periodically. */
    void monitorMachinesFile();

    int allocBuildStep(pqxx::work & txn, Build::ptr build);

    int createBuildStep(pqxx::work & txn, time_t startTime, Build::ptr build, Step::ptr step,
        const std::string & machine, BuildStepStatus status, const std::string & errorMsg = "",
        BuildID propagatedFrom = 0);

    void finishBuildStep(pqxx::work & txn, time_t startTime, time_t stopTime, BuildID buildId, int stepNr,
        const std::string & machine, BuildStepStatus status, const std::string & errorMsg = "",
        BuildID propagatedFrom = 0);

    int createSubstitutionStep(pqxx::work & txn, time_t startTime, time_t stopTime,
        Build::ptr build, const nix::Path & drvPath, const std::string & outputName, const nix::Path & storePath);

    void updateBuild(pqxx::work & txn, Build::ptr build, BuildStatus status);

    void queueMonitor();

    void queueMonitorLoop();

    /* Check the queue for new builds. */
    bool getQueuedBuilds(Connection & conn, std::shared_ptr<nix::StoreAPI> store, unsigned int & lastBuildId);

    /* Handle cancellation, deletion and priority bumps. */
    void processQueueChange(Connection & conn);

    Step::ptr createStep(std::shared_ptr<nix::StoreAPI> store,
        Connection & conn, Build::ptr build, const nix::Path & drvPath,
        Build::ptr referringBuild, Step::ptr referringStep, std::set<nix::Path> & finishedDrvs,
        std::set<Step::ptr> & newSteps, std::set<Step::ptr> & newRunnable);

    Jobset::ptr createJobset(pqxx::work & txn,
        const std::string & projectName, const std::string & jobsetName);

    void processJobsetSharesChange(Connection & conn);

    void makeRunnable(Step::ptr step);

    /* The thread that selects and starts runnable builds. */
    void dispatcher();

    system_time doDispatch();

    void wakeDispatcher();

    void builder(MachineReservation::ptr reservation);

    /* Perform the given build step. Return true if the step is to be
       retried. */
    bool doBuildStep(std::shared_ptr<nix::StoreAPI> store, Step::ptr step,
        Machine::ptr machine);

    void buildRemote(std::shared_ptr<nix::StoreAPI> store,
        Machine::ptr machine, Step::ptr step,
        unsigned int maxSilentTime, unsigned int buildTimeout,
        RemoteResult & result);

    void markSucceededBuild(pqxx::work & txn, Build::ptr build,
        const BuildOutput & res, bool isCachedBuild, time_t startTime, time_t stopTime);

    bool checkCachedFailure(Step::ptr step, Connection & conn);

    /* Thread that asynchronously bzips logs of finished steps. */
    void logCompressor();

    /* Thread that asynchronously invokes hydra-notify to send build
       notifications. */
    void notificationSender();

    /* Acquire the global queue runner lock, or null if somebody else
       has it. */
    std::shared_ptr<nix::PathLocks> acquireGlobalLock();

    void dumpStatus(Connection & conn, bool log);

public:

    void showStatus();

    void unlock();

    void run(BuildID buildOne = 0);
};
