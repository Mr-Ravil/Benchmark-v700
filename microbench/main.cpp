//
// Created by Ravil Galiev on 27.07.2023.
//


#include <fstream>
#include <thread>
#include <limits>
#include <cstring>
#include <ctime>
#include <thread>
#include <atomic>
#include <chrono>
#include <cassert>
#include <parallel/algorithm>
#include <omp.h>
#include <perftools.h>
#include <regex>

#define MAIN_BENCH

#ifdef PRINT_JEMALLOC_STATS
#include <jemalloc/jemalloc.h>
#define DEBUG_PRINT_ARENA_STATS malloc_stats_print(printCallback, NULL, "ag")
void printCallback(void * nothing, const char * data) {
    std::cout<<data;
}
#else
#define DEBUG_PRINT_ARENA_STATS
#endif

/******************************************************************************
 * Configure global statistics tracking & output using GSTATS (common/gstats)
 * Note: it is crucial that this import occurs before any user headers
 * (that might use GSTATS) are included.
 *
 * This is because it define the macro GSTATS_HANDLE_STATS, which is
 * (read: should be) used by all user includes to determine whether to perform
 * any GSTATS_ calls.
 *
 * Thus, including this before all other user headers ENABLES GSTATS in them.
 *****************************************************************************/
#include "define_global_statistics.h"
#include "gstats_global.h" // include the GSTATS code and macros (crucial this happens after GSTATS_HANDLE_STATS is defined)
// Note: any statistics trackers created by headers included below have to be handled separately... we do this below.

#ifdef KEY_DEPTH_TOTAL_STAT
int64_t key_depth_total_sum__;
int64_t key_depth_total_cnt__;
#endif

#ifdef KEY_DEPTH_STAT
int64_t* key_depth_sum__ = nullptr;
int64_t* key_depth_cnt__ = nullptr;
#endif

#ifdef KEY_SEARCH_TOTAL_STAT
int64_t key_search_total_iters_cnt__;
int64_t key_search_total_cnt__;
#endif


// each thread saves its own thread-id (should be used primarily within this file--could be eliminated to improve software engineering)
__thread int tid = 0; //TODO: ???

#include "plaf.h"


#include "globals_extern.h"
#include "json/single_include/nlohmann/json.hpp"
#include "random_xoshiro256p.h"
#include "binding.h"
#include "papi_util_impl.h"
#include "rq_provider.h"


#include "adapter.h" /* data structure adapter header (selected according to the "ds/..." subdirectory in the -I include paths */
#include "tree_stats.h"


#ifdef USE_RCU
#include "eer_prcu_impl.h"
#define __RCU_INIT_THREAD urcu::registerThread(tid);
#define __RCU_DEINIT_THREAD urcu::unregisterThread();
#define __RCU_INIT_ALL urcu::init(TOTAL_THREADS);
#define __RCU_DEINIT_ALL urcu::deinit(TOTAL_THREADS);
#else
#define __RCU_INIT_THREAD
#define __RCU_DEINIT_THREAD
#define __RCU_INIT_ALL
#define __RCU_DEINIT_ALL
#endif

#ifdef USE_RLU
#include "rlu.h"
PAD;
__thread rlu_thread_data_t * rlu_self;
PAD;
rlu_thread_data_t * rlu_tdata = NULL;
#define __RLU_INIT_THREAD rlu_self = &rlu_tdata[tid]; RLU_THREAD_INIT(rlu_self);
#define __RLU_DEINIT_THREAD RLU_THREAD_FINISH(rlu_self);
#define __RLU_INIT_ALL rlu_tdata = new rlu_thread_data_t[MAX_THREADS_POW2]; RLU_INIT(RLU_TYPE_FINE_GRAINED, 1);
#define __RLU_DEINIT_ALL RLU_FINISH(); delete[] rlu_tdata;
#else
#define __RLU_INIT_THREAD
#define __RLU_DEINIT_THREAD
#define __RLU_INIT_ALL
#define __RLU_DEINIT_ALL
#endif

#define INIT_ALL \
    __RCU_INIT_ALL; \
    __RLU_INIT_ALL;
#define DEINIT_ALL \
    __RLU_DEINIT_ALL; \
    __RCU_DEINIT_ALL;



/******************************************************************************
 * Define global variables to store the numerical IDs of all GSTATS global
 * statistics trackers that have been defined over all files #included.
 *
 * It is CRUCIAL that this occurs AFTER ALL user #includes (so we catch ALL
 * GSTATS statistics trackers/counters/timers defined by those #includes).
 *
 * This includes the statistics trackers defined in define_global_statistics.h
 * as well any that were setup by a particular data structure / allocator /
 * reclaimer / pool / library that was #included above.
 *
 * This is a manually constructed list that you are free to add to if you
 * create, e.g., your own data structure specific statistics trackers.
 * They will only be included / printed when your data structure is active.
 *
 * If you add something here, you must also add to a few similar code blocks
 * below. Search this file for "GSTATS_" and you'll see where...
 *****************************************************************************/
GSTATS_DECLARE_ALL_STAT_IDS;
#ifdef GSTATS_HANDLE_STATS_BROWN_EXT_IST_LF
GSTATS_HANDLE_STATS_BROWN_EXT_IST_LF(__DECLARE_STAT_ID);
#endif
#ifdef GSTATS_HANDLE_STATS_POOL_NUMA
GSTATS_HANDLE_STATS_POOL_NUMA(__DECLARE_STAT_ID);
#endif
#ifdef GSTATS_HANDLE_STATS_RECLAIMERS_WITH_EPOCHS
GSTATS_HANDLE_STATS_RECLAIMERS_WITH_EPOCHS(__DECLARE_STAT_ID);
#endif
#ifdef GSTATS_HANDLE_STATS_USER
GSTATS_HANDLE_STATS_USER(__DECLARE_STAT_ID);
#endif
// Create storage for the CONTENTS of gstats counters (for MAX_THREADS_POW2 threads)
GSTATS_DECLARE_STATS_OBJECT(MAX_THREADS_POW2);
// Create storage for the IDs of all global counters defined in define_global_statistics.h



#define TIMING_START(s) \
    std::cout<<"timing_start "<<s<<"..."<<std::endl; \
    GSTATS_TIMER_RESET(tid, timer_duration);
#define TIMING_STOP \
    std::cout<<"timing_elapsed "<<(GSTATS_TIMER_SPLIT(tid, timer_duration)/1000000000.)<<"s"<<std::endl;
#ifndef OPS_BETWEEN_TIME_CHECKS
#define OPS_BETWEEN_TIME_CHECKS 100
#endif
#ifndef RQS_BETWEEN_TIME_CHECKS
#define RQS_BETWEEN_TIME_CHECKS 10
#endif

#include "workloads/bench_parameters.h"
#include "workloads/thread_loops/thread_loop_impl.h"

#include "globals_t_impl.h"
#include "statistics.h"
#include "parse_argument.h"

void bindThreads(int nthreads) {
    // setup thread pinning/binding
    binding_configurePolicy(nthreads);

    std::cout << "ACTUAL_THREAD_BINDINGS=";
    for (int i = 0; i < nthreads; ++i) {
        std::cout << (i ? "," : "") << binding_getActualBinding(i);
    }
    std::cout << std::endl;
//    if (!binding_isInjectiveMapping(nthreads)) {
//        std::cout << "ERROR: thread binding maps more than one thread to a single logical processor" << std::endl;
//        exit(-1);
//    }
}

void createDataStructure(globals_t *g) {
    g->dsAdapter = new DS_ADAPTER_T(g->benchParameters->getTotalThreads(), g->KEY_MIN,
                                    g->KEY_MAX, g->NO_VALUE,
                                    g->rngs);
}


Statistic getStatistic(long long elapsedMillis) {
    return Statistic(elapsedMillis / 1000.);
}

void execute(globals_t *g, Parameters *parameters) {

    std::thread **threads = new std::thread *[MAX_THREADS_POW2];
    ThreadLoop **threadLoops = parameters->getWorkload(g, g->rngs);

//    if (!parameters->pin.empty()) {
//        binding_parseCustom(parameters->pin); // e.g., "1.2.3.8-11.4-7.0"
//        std::cout << "parsed custom binding: " << parameters->pin << std::endl;
//    }
    std::cout << "pinning...\n";
    binding_setCustom(parameters->getPin());
    bindThreads(parameters->getNumThreads());

    for (int i = 0; i < parameters->getNumThreads(); ++i) {
        threads[i] = new std::thread(&ThreadLoop::run, threadLoops[i]);
    }

    while (g->running < parameters->getNumThreads()) {
        TRACE COUTATOMIC("main thread: waiting for threads to START running=" << g->running << std::endl);
    } // wait for all threads to be ready
    COUTATOMIC("main thread: starting timer..." << std::endl)

////////////////////////////////////

    SOFTWARE_BARRIER;
    g->startTime = std::chrono::high_resolution_clock::now();
    g->startClockTicks = get_server_clock();
    SOFTWARE_BARRIER;
    printUptimeStampForPERF("START");
#ifdef MEASURE_TIMELINE_STATS
    ___timeline_use = 1;
#endif

    parameters->stopCondition->start(parameters->getNumThreads());
    g->start = true;
    SOFTWARE_BARRIER;

    for (size_t i = 0; i < parameters->getNumThreads(); ++i) {
        threads[i]->join();
    }

    SOFTWARE_BARRIER;
    g->done = true;
    __sync_synchronize();
    g->endTime = std::chrono::high_resolution_clock::now();
    __sync_synchronize();
    printUptimeStampForPERF("END")



    //TODO TIME_IS_UP
#if defined TIME_IS_UP
    DEBUG_PRINT_ARENA_STATS;
    COUTATOMIC(std::endl);
    COUTATOMIC("###############################################################################" << std::endl);
    COUTATOMIC("################################## TIME IS UP #################################" << std::endl);
    COUTATOMIC("###############################################################################" << std::endl);
    COUTATOMIC(std::endl);

    const long MAX_NAPPING_MILLIS = (parameters->MAXKEY > 5e7 ? 120000 : 30000);
    g->elapsedMillis = duration_cast<milliseconds>(g->endTime - g->startTime).count();
    g->elapsedMillisNapping = 0;
    while (g->running > 0 && g->elapsedMillisNapping < MAX_NAPPING_MILLIS) {
        nanosleep(&tsNap, NULL);
        g->elapsedMillisNapping =
                duration_cast<milliseconds>(high_resolution_clock::now() - g->startTime).count() - g->elapsedMillis;
    }

    if (g->running > 0) {
        COUTATOMIC(std::endl);
        COUTATOMIC("Validation FAILURE: " << g->running
                                          << " non-terminating thread(s) [did we exhaust physical memory and experience excessive slowdown due to swap mem?]"
                                          << std::endl);
        COUTATOMIC(std::endl);
        COUTATOMIC("elapsedMillis=" << g->elapsedMillis << " elapsedMillisNapping=" << g->elapsedMillisNapping
                                    << std::endl);

        if (g->dsAdapter->validateStructure()) {
            std::cout << "Structural validation OK" << std::endl;
        } else {
            std::cout << "Structural validation FAILURE." << std::endl;
        }

#if defined USE_GSTATS && defined OVERRIDE_PRINT_STATS_ON_ERROR
        GSTATS_PRINT;
        std::cout<<std::endl;
#endif

        g->dsAdapter->printSummary();
#ifdef RQ_DEBUGGING_H
        DEBUG_VALIDATE_RQ(TOTAL_THREADS);
#endif
        exit(-1);
    }
#endif

    g->elapsedMillis = std::chrono::duration_cast<std::chrono::milliseconds>(g->endTime - g->startTime).count();

    parameters->stopCondition->clean();
    delete[] threads;
    delete[] threadLoops;
    binding_deinit();

    g->start = false;
    g->done = false;
}

void run(globals_t *g) {
    int TOTAL_THREADS = g->benchParameters->getTotalThreads();

    using namespace std::chrono;
    papi_init_program(TOTAL_THREADS);

#ifdef KEY_DEPTH_TOTAL_STAT
    key_depth_total_sum__ = 0;
    key_depth_total_cnt__ = 0;
#endif

#ifdef KEY_DEPTH_STAT
    key_depth_sum__ = new int64_t[g->KEY_MAX + 1];
    key_depth_cnt__ = new int64_t[g->KEY_MAX + 1];
#endif

#ifdef KEY_SEARCH_TOTAL_STAT
    key_search_total_iters_cnt__ = 0;
    key_search_total_cnt__ = 0;
#endif

    // create the actual data structure
    createDataStructure(g);

    INIT_ALL;

#ifdef CALL_DEBUG_GC
    g->dsAdapter->debugGCSingleThreaded();
#endif

    DEBUG_PRINT_ARENA_STATS;
    COUTATOMIC(std::endl);
    COUTATOMIC("###############################################################################" << std::endl)
    COUTATOMIC("################################ BEGIN RUNNING ################################" << std::endl)
    COUTATOMIC("###############################################################################" << std::endl)
    COUTATOMIC(std::endl);

    /**
     * PREFILL STAGE
     */
    if (g->benchParameters->prefill->getNumThreads() != 0) {

        std::cout << toStringStage("Prefill stage");

        // prefill data structure to mimic its structure in the steady state
        g->prefillStartTime = std::chrono::high_resolution_clock::now();

        execute(g, g->benchParameters->prefill);

        {
            // print prefilling status information
            using namespace std::chrono;
            const long totalUpdates =
                    GSTATS_OBJECT_NAME.get_sum<long long>(num_inserts) +
                    GSTATS_OBJECT_NAME.get_sum<long long>(num_deletes);
            g->prefillKeySum = GSTATS_OBJECT_NAME.get_sum<long long>(key_checksum);
            g->prefillSize = GSTATS_OBJECT_NAME.get_sum<long long>(prefill_size);
            auto now = high_resolution_clock::now();
            auto elapsedMillis = duration_cast<milliseconds>(now - g->startTime).count();
            COUTATOMIC(
                    "finished prefilling to size " << g->prefillSize << " for expected size "// << expectedSize
                                                   << " keysum="
                                                   << g->prefillKeySum << ", performing " << totalUpdates
                                                   << " updates; total_prefilling_elapsed_ms=" << elapsedMillis
                                                   << " ms)"
                                                   << std::endl);
            std::cout << "pref_size=" << g->prefillSize << std::endl;
            std::cout << "pref_millis=" << elapsedMillis << std::endl;
            GSTATS_CLEAR_ALL;

            // print total prefilling time
            std::cout << "prefill_elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::high_resolution_clock::now() - g->prefillStartTime).count() << std::endl;
            g->dsAdapter->printSummary(); ///////// debug
        }

    } else {
        std::cout << toStringStage("Without Prefill stage");
    }

    /**
     * WARM UP STAGE
     */
    if (g->benchParameters->warmUp->getNumThreads() != 0) {
        std::cout << toStringStage("WarmUp stage");

        // prefill data structure to mimic its structure in the steady state
//    g->prefillStartTime = std::chrono::high_resolution_clock::now();

        execute(g, g->benchParameters->warmUp);
    } else {
        std::cout << toStringStage("Without WarmUp stage");
    }

    /**
     * TEST STAGE
     */

    GSTATS_OBJECT_NAME.clear_all();

    std::cout << toStringStage("Test stage");

    // prefill data structure to mimic its structure in the steady state
//    g->prefillStartTime = std::chrono::high_resolution_clock::now();



    execute(g, g->benchParameters->test);

    COUTATOMIC(std::endl);
    COUTATOMIC("###############################################################################" << std::endl);
    COUTATOMIC("################################# END RUNNING #################################" << std::endl);
    COUTATOMIC("###############################################################################" << std::endl);
    COUTATOMIC(std::endl);

    COUTATOMIC(((g->elapsedMillis + g->elapsedMillisNapping) / 1000.) << "s" << std::endl);

    papi_deinit_program();DEINIT_ALL;
}


void printExecutionTime(globals_t *g) {
    auto programExecutionElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - g->programExecutionStartTime).count();
    std::cout << "total_execution_walltime=" << (programExecutionElapsed / 1000.) << "s" << std::endl;
}

void printOutput(globals_t *g) {
    std::cout << "PRODUCING OUTPUT" << std::endl;

#ifdef KEY_DEPTH_TOTAL_STAT
    std::cout << "\nKEY_DEPTH_TOTAL_STAT START" << std::endl;
    if (key_depth_total_cnt__ > 0) {
        std::cout << "TOTAL_ACCESSES=" << key_depth_total_cnt__ << '\n';
        std::cout << "TOTAL_AVG_DEPTH=" << (key_depth_total_sum__ / static_cast<double>(key_depth_total_cnt__)) << '\n';
    }
    std::cout << "KEY_DEPTH_TOTAL_STAT END" << std::endl;
#endif

#ifdef KEY_DEPTH_STAT
    std::cout << "\nKEY_DEPTH_STAT START" << std::endl;
    for (int key = g->KEY_MIN; key <= g->KEY_MAX; ++key) {
        if (key_depth_cnt__[key] == 0) {
            continue;
        }
        std::cout << "KEY=" << key << "; ";
        std::cout << "ACCESSES=" << key_depth_cnt__[key] << "; ";
        std::cout << "AVG_DEPTH=" << (key_depth_sum__[key] / static_cast<double>(key_depth_cnt__[key])) << ";\n";
    }
    std::cout << "KEY_DEPTH_STAT END" << std::endl;
#endif

#ifdef KEY_SEARCH_TOTAL_STAT
    std::cout << "\nKEY_SEARCH_TOTAL_STAT START" << std::endl;
    if (key_search_total_cnt__ > 0) {
        std::cout << "TOTAL_SEARCH_ITERS=" << key_search_total_iters_cnt__ << '\n';
        std::cout << "TOTAL_SEARCH_CNT=" << key_search_total_cnt__ << '\n';
        std::cout << "AVG_SEARCH_ITERS=" << (key_search_total_iters_cnt__ / static_cast<double>(key_search_total_cnt__)) << '\n';
    }
    std::cout << "KEY_SEARCH_TOTAL_STAT END" << std::endl;
#endif

#ifdef USE_TREE_STATS
    auto timeBeforeTreeStats = std::chrono::high_resolution_clock::now();
    auto treeStats = g->dsAdapter->createTreeStats(g->KEY_MIN, g->KEY_MAX);
    auto timeAfterTreeStats = std::chrono::high_resolution_clock::now();
    auto elapsedTreeStats = std::chrono::duration_cast<std::chrono::milliseconds>(timeAfterTreeStats-timeBeforeTreeStats).count();
    std::cout<<std::endl;
    std::cout<<"tree_stats_computeWalltime="<<(elapsedTreeStats/1000.)<<"s"<<std::endl;
    std::cout<<std::endl;
    //std::cout<<"size_nodes="<<
    if (treeStats) std::cout<<treeStats->toString()<<std::endl;
#endif
    g->dsAdapter->printSummary(); // can put this before GSTATS_PRINT to help some hacky debug code in reclaimer_ebr_token route some information to GSTATS_ to be printed. not a big deal, though.

#ifdef USE_GSTATS
    GSTATS_PRINT;
    std::cout << std::endl;
#endif

    long long threadsKeySum = 0;
    long long threadsSize = 0;

#ifdef USE_GSTATS
    {
        threadsKeySum = GSTATS_GET_STAT_METRICS(key_checksum, TOTAL)[0].sum + g->prefillKeySum;
        // threadsSize = GSTATS_GET_STAT_METRICS(size_checksum, TOTAL)[0].sum + g->prefillSize;
#ifdef USE_TREE_STATS
        long long dsKeySum = (treeStats) ? treeStats->getSumOfKeys() : threadsKeySum;
        long long dsSize = (treeStats) ? treeStats->getKeys() : -1; // threadsSize;
#endif
        std::cout << "threads_final_keysum=" << threadsKeySum << std::endl;
        // std::cout<<"threads_final_size="<<threadsSize<<std::endl;
#ifdef USE_TREE_STATS
        std::cout<<"final_keysum="<<dsKeySum<<std::endl;
        std::cout<<"final_size="<<dsSize<<std::endl;
        if (threadsKeySum == dsKeySum) { // && threadsSize == dsSize) {
            std::cout<<"validate_result=success"<<std::endl;
            std::cout<<"Validation OK."<<std::endl;
            if (treeStats == NULL) std::cout<<"**** WARNING: VALIDATION WAS ACTUALLY _SKIPPED_ AS THIS DS DOES NOT SUPPORT IT!"<<std::endl;
        } else {
            std::cout<<"validate_result=fail"<<std::endl;
            std::cout<<"Validation FAILURE: threadsKeySum="<<threadsKeySum<<" dsKeySum="<<dsKeySum<</*" threadsSize="<<threadsSize<<*/" dsSize="<<dsSize<<std::endl;
            // std::cout<<"Validation comment: data structure is "<<(dsSize > threadsSize ? "LARGER" : "SMALLER")<<" than it should be according to the operation return values"<<std::endl;
            printExecutionTime(g);
            exit(-1);
        }
#endif
    }
#endif

#if !defined SKIP_VALIDATION
    if (g->dsAdapter->validateStructure()) {
        std::cout << "Structural validation OK." << std::endl;
    } else {
        std::cout << "Structural validation FAILURE." << std::endl;
        printExecutionTime(g);
        exit(-1);
    }
#endif

    long long totalAll = 0;

#ifdef USE_GSTATS
    {
        Statistic statistic = getStatistic(g->elapsedMillis);
        statistic.printTotalStatistic();
        totalAll = statistic.totalAll;
    }
#endif


    COUTATOMIC(indented_title_with_data("elapsed milliseconds", g->elapsedMillis));
    COUTATOMIC(indented_title_with_data("napping milliseconds overtime", g->elapsedMillisNapping));
    COUTATOMIC(std::endl);

//    g->dsAdapter->printSummary();

    // free ds
#if !defined NO_CLEANUP_AFTER_WORKLOAD
    std::cout << "begin delete ds..." << std::endl;
    if (g->benchParameters->range > 10000000) {
        std::cout << "    SKIPPING deletion of data structure to save time! (because key range is so large)"
                  << std::endl;
    } else {
        delete g->dsAdapter;
    }
    std::cout << "end delete ds." << std::endl;
#endif
#ifdef KEY_DEPTH_STAT
    delete[] key_depth_sum__;
    delete[] key_depth_cnt__;
#endif
    papi_print_counters(totalAll);
#ifdef USE_TREE_STATS
    if (treeStats) delete treeStats;
#endif

#if !defined NDEBUG
    std::cout << "WARNING: NDEBUG is not defined, so experiment results may be affected by assertions and debug code."
              << std::endl;
#endif
#if defined MEASURE_REBUILDING_TIME || defined MEASURE_TIMELINE_STATS || defined RAPID_RECLAMATION
    std::cout<<"WARNING: one or more of MEASURE_REBUILDING_TIME | MEASURE_TIMELINE_STATS | RAPID_RECLAMATION are defined, which *may* affect experiments results."<<std::endl;
#endif
}

void parseJsonFile(const std::string &fileName, BenchParameters &benchParameters) {
    std::ifstream fin;
    fin.open(fileName);

    nlohmann::json j = nlohmann::json::parse(fin);
    benchParameters = j;

    fin.close();
}

template<typename T>
void writeJsonFile(const std::string &fileName, T &t) {
    std::ofstream fout;
    fout.open(fileName);

    nlohmann::json j = t;

    fout << j.dump(4);

    fout.close();
}

int main(int argc, char **argv) {
    printUptimeStampForPERF("MAIN_START");

    std::cout << "binary=" << argv[0] << std::endl;

    BenchParameters benchParameters;

    ParseArgument args = ParseArgument(argc, argv).next();
    bool resultStatisticToFile = false;
    std::string resultStatisticFileName;

    while (args.hasNext()) {
        if (strcmp(args.getCurrent(), "-json-file") == 0) {
            parseJsonFile(args.getNext(), benchParameters);
        } else if (strcmp(args.getCurrent(), "-result-file") == 0) {
            resultStatisticToFile = true;
            resultStatisticFileName = args.getNext();
        } else {
            std::cout << "Unexpected option: " << args.getCurrent() << "\nindex: " << args.pointer << std::endl;
        }
        args.next();
    }


    // print used args
    PRINTS(DS_TYPENAME)
    PRINTS(FIND_FUNC)
    PRINTS(INSERT_FUNC)
    PRINTS(ERASE_FUNC)
    PRINTS(RQ_FUNC)
    PRINTS(RECLAIM)
    PRINTS(ALLOC)
    PRINTS(POOL)
    PRINTS(MAX_THREADS_POW2)
    PRINTS(CPU_FREQ_GHZ)

    benchParameters.init();

    std::cout << std::endl;

    std::cout << benchParameters.toString();

    std::cout << std::endl;

    globals_t *g = new globals_t(&benchParameters);

    g->programExecutionStartTime = std::chrono::high_resolution_clock::now();

    // print object sizes, to help debugging/sanity checking memory layouts
    g->dsAdapter->printObjectSizes();

//TODO maybe move it to bindThreads()?
    /******************************************************************************
        * Perform the actual creation of all GSTATS global statistics trackers that
        * have been defined over all files #included.
        *
        * This includes the statistics trackers defined in define_global_statistics.h
        * as well any that were setup by a particular data structure / allocator /
        * reclaimer / pool / library that was #included above.
        *
        * This is a manually constructed list that you are free to add to if you
        * create, e.g., your own data structure specific statistics trackers.
        * They will only be included / printed when your data structure is active.
        *****************************************************************************/
    std::cout << std::endl;

#ifdef GSTATS_HANDLE_STATS_BROWN_EXT_IST_LF
    GSTATS_HANDLE_STATS_BROWN_EXT_IST_LF(__CREATE_STAT);
#endif
#ifdef GSTATS_HANDLE_STATS_POOL_NUMA
    GSTATS_HANDLE_STATS_POOL_NUMA(__CREATE_STAT);
#endif
#ifdef GSTATS_HANDLE_STATS_RECLAIMERS_WITH_EPOCHS
    GSTATS_HANDLE_STATS_RECLAIMERS_WITH_EPOCHS(__CREATE_STAT);
#endif
#ifdef GSTATS_HANDLE_STATS_USER
    GSTATS_HANDLE_STATS_USER(__CREATE_STAT);
#endif
    GSTATS_CREATE_ALL;
    std::cout << std::endl;

    run(g);
    printOutput(g);

    if (resultStatisticToFile) {
        Statistic stats = getStatistic(g->elapsedMillis);
        writeJsonFile(resultStatisticFileName, stats);
    }

    printUptimeStampForPERF("MAIN_END");
}