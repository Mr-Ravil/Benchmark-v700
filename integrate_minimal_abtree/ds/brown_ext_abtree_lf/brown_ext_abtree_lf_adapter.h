/* 
 * File:   bst_adapter.h
 * Author: trbot
 *
 * Created on August 31, 2017, 6:53 PM
 */

#ifndef BST_ADAPTER_H
#define BST_ADAPTER_H

#include <iostream>
#include "brown_ext_abtree_lf_impl.h"
#include "errors.h"
#include "random.h"
using namespace abtree_ns;

#if !defined ABTREE_DEGREE
    #warning "ABTREE_DEGREE was not defined... using default: 16."
    #define ABTREE_DEGREE 16
#endif

#define RECORD_MANAGER_T record_manager<Reclaim, Alloc, Pool, Node<ABTREE_DEGREE, K>>
#define DATA_STRUCTURE_T abtree<ABTREE_DEGREE, K, std::less<K>, RECORD_MANAGER_T>

template <typename K, typename V, class Reclaim = reclaimer_debra<K>, class Alloc = allocator_new<K>, class Pool = pool_none<K>>
class ds_adapter {
private:
    const void * NO_VALUE;
    DATA_STRUCTURE_T * const ds;

public:
    ds_adapter(const K& MIN_KEY /* can be ANY key */, const K& MAX_KEY /* unused */, const V& _NO_VALUE /* unused */, const int numThreads, Random * const rngs /* unused */)
    : ds(new DATA_STRUCTURE_T(numThreads, MIN_KEY))
    {
        if (numThreads > MAX_THREADS_POW2) {
            error("numThreads exceeds MAX_THREADS_POW2");
        }
    }
    ~ds_adapter() {
        delete ds;
    }
    
    void * getNoValue() {
        return ds->NO_VALUE;
    }
    
    void initThread(const int tid) {
        ds->initThread(tid);
    }
    void deinitThread(const int tid) {
        ds->deinitThread(tid);
    }

    bool contains(const int tid, const K& key) {
        return ds->contains(tid, key);
    }
    void * const insert(const int tid, const K& key, void * const val) {
        return ds->insert(tid, key, val);
    }
    void * const insertIfAbsent(const int tid, const K& key, void * const val) {
        return ds->insertIfAbsent(tid, key, val);
    }
    void * const erase(const int tid, const K& key) {
        return ds->erase(tid, key).first;
    }
    void * find(const int tid, const K& key) {
        return ds->find(tid, key).first;
    }
    int rangeQuery(const int tid, const K& lo, const K& hi, K * const resultKeys, void ** const resultValues) {
        return ds->rangeQuery(tid, lo, hi, resultKeys, resultValues);
    }
    /**
     * Sequential operation to get the number of keys in the set
     */
    int getSize() {
        return ds->getSize();
    }
    void printSummary() {
        stringstream ss;
        ss<<ds->getSizeInNodes()<<" nodes in tree";
        cout<<ss.str()<<endl;
        
        auto recmgr = ds->debugGetRecMgr();
        recmgr->printStatus();
    }
    long long getKeyChecksum() {
        return ds->debugKeySum();
    }
    bool validateStructure() {
        return true;
    }
    void printObjectSizes() {
        std::cout<<"sizes: node="
                 <<(sizeof(Node<ABTREE_DEGREE, K>))
                 <<" descriptor="<<(sizeof(SCXRecord<ABTREE_DEGREE, K>))<<" (statically allocated)"
                 <<std::endl;
    }
};

#endif
