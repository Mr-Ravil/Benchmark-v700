#ifndef BROWN_EXT_IST_LF_IMPL_H
#define BROWN_EXT_IST_LF_IMPL_H

#include "brown_ext_ist_lf.h"

template <typename K, typename V, class Interpolate, class RecManager>
V istree<K,V,Interpolate,RecManager>::find(const int tid, const K& key) {
    casword_t ptr = prov->readPtr(tid, root->ptrAddr(0));
    assert(ptr);
    while (true) {
        if (unlikely(IS_KVPAIR(ptr))) {
            auto kv = CASWORD_TO_KVPAIR(ptr);
            if (kv->k == key) return kv->v;
            return NO_VALUE;
        } else if (unlikely(IS_REBUILDOP(ptr))) {
            auto rebuild = CASWORD_TO_REBUILDOP(ptr);
            ptr = NODE_TO_CASWORD(rebuild->candidate);
        } else {
            assert(IS_NODE(ptr));
            assert(ptr);
            // ptr is an internal node
            auto node = CASWORD_TO_NODE(ptr);
            const int ix = interpolationSearch(tid, key, node);
            ptr = prov->readPtr(tid, node->ptrAddr(ix));
        }
    }
}

template <typename K, typename V, class Interpolate, class RecManager>
size_t istree<K,V,Interpolate,RecManager>::markAndCount(const int tid, const casword_t ptr) {
    if (unlikely(IS_KVPAIR(ptr))) {
        return 1 - CASWORD_TO_KVPAIR(ptr)->empty;
    }
    
    if (unlikely(IS_REBUILDOP(ptr))) {
        auto op = CASWORD_TO_REBUILDOP(ptr);
        return markAndCount(tid, NODE_TO_CASWORD(op->candidate));
        
        // note: the below is not true. if we are here seeing this rebuildop,
        // then we ALREADY marked the node that points to the rebuildop,
        // which means that rebuild op cannot possible change that node
        // to effect the rebuilding.
        
//        // if we come across a rebuildop, and someone else actually does the replacement, then we are in trouble!
//        // so, we need to try to cancel that rebuildop by CASing it back to the original rebuildRoot.
//        // (we dominate the rebuildop from above, so this doesn't threaten progress)
//        // then, whoever is doing that rebuildop will fail the final cas.
//        // (the subtree could be replaced with unmarked nodes when we think it's all marked.)
//        // on the other hand, if we fail to dominate that rebuildop, then that rebuildop finished,
//        // and we can descend into that subtree and do our marking.
//        if (__sync_bool_compare_and_swap(op->parent->ptrAddr(op->index), ptr, NODE_TO_CASWORD(op->candidate))) {
//            // we defeated that rebuildop
//            // proceed to that rebuildop's rebuildRoot to mark it
//            return markAndCount(tid, NODE_TO_CASWORD(op->candidate), someOutputKVPairPtr);
//        } else {
//            // that rebuildop finished
//            // proceed to the replacement subtree to mark it
//            return markAndCount(tid, prov->readPtr(tid, op->parent->ptrAddr(op->index)));
//        }
    }
    
    assert(IS_NODE(ptr));
    auto node = CASWORD_TO_NODE(ptr);
    
    // optimize by taking the sum from node->dirty if we run into a finished subtree
    auto result = __sync_val_compare_and_swap(&node->dirty, 0, 1);
    auto sum = DIRTY_TO_SUM(result);
    if (sum) { // markAndCount has already FINISHED in this subtree, and sum is the count
        return sum;
    }
    
    // recurse over all subtrees
    size_t keyCount = 0;
    for (int i=0;i<node->degree;++i) {
        keyCount += markAndCount(tid, prov->readPtr(tid, node->ptrAddr(i)));
    }
    
    if (keyCount) __sync_bool_compare_and_swap(&node->dirty, 1, SUM_TO_DIRTY(keyCount));
    return keyCount;
}

template <typename K, typename V, class Interpolate, class RecManager>
void istree<K,V,Interpolate,RecManager>::createIdeal(const int tid, casword_t ptr, IdealBuilder * b) {
    if (IS_KVPAIR(ptr)) {
        auto pair = CASWORD_TO_KVPAIR(ptr);
        if (pair->empty) return;
        b->addKV(tid, pair);
    } else if (IS_REBUILDOP(ptr)) {
        auto op = CASWORD_TO_REBUILDOP(ptr);
        createIdeal(tid, NODE_TO_CASWORD(op->candidate), b);
    } else {
        assert(IS_NODE(ptr));
        auto node = CASWORD_TO_NODE(ptr);
        for (int i=0;i<node->degree;++i) {
            createIdeal(tid, prov->readPtr(tid, node->ptrAddr(i)), b);
        }
    }
}

template <typename K, typename V, class Interpolate, class RecManager>
casword_t istree<K,V,Interpolate,RecManager>::createIdeal(const int tid, Node<K,V> * rebuildRoot, const size_t keyCount) {
    if (keyCount == 0) return KVPAIR_TO_CASWORD(createEmptyKVPair(tid));
    IdealBuilder b (keyCount, this);
    createIdeal(tid, NODE_TO_CASWORD(rebuildRoot), &b);
    return b.getCASWord(tid);
}

template <typename K, typename V, class Interpolate, class RecManager>
void istree<K,V,Interpolate,RecManager>::helpRebuild(const int tid, RebuildOperation<K,V> * op) {
    auto keyCount = markAndCount(tid, NODE_TO_CASWORD(op->candidate));
    auto oldWord = REBUILDOP_TO_CASWORD(op);
    casword_t newWord = createIdeal(tid, op->candidate, keyCount);
    if (prov->dcssPtr(tid, (casword_t *) &op->parent->dirty, 0, (casword_t *) op->parent->ptrAddr(op->index), oldWord, newWord).status == DCSS_SUCCESS) {
        GSTATS_ADD_IX(tid, num_complete_rebuild_at_depth, 1, op->depth);
    }
}

template <typename K, typename V, class Interpolate, class RecManager>
void istree<K,V,Interpolate,RecManager>::rebuild(const int tid, Node<K,V> * rebuildRoot, Node<K,V> * parent, int index /* of rebuildRoot in parent */, const int depth) {
    auto op = new RebuildOperation<K,V>(rebuildRoot, parent, index, depth);
    auto ptr = REBUILDOP_TO_CASWORD(op);
    auto old = NODE_TO_CASWORD(op->candidate);
    assert(op->parent == parent);
    if (prov->dcssPtr(tid, (casword_t *) &op->parent->dirty, 0, (casword_t *) op->parent->ptrAddr(op->index), old, ptr).status == DCSS_SUCCESS) {
        helpRebuild(tid, op);
    }
}

template <typename K, typename V, class Interpolate, class RecManager>
int istree<K,V,Interpolate,RecManager>::interpolationSearch(const int tid, const K& key, Node<K,V> * const node) {
    // TODO: redo prefetching, taking into account the fact that l2 adjacent line prefetcher DOESN'T grab an adjacent line
    __builtin_prefetch(&node->degree, 1);
    __builtin_prefetch(&node->maxKey, 1);
    __builtin_prefetch((node->keyAddr(0)), 1);
    __builtin_prefetch((node->keyAddr(0))+(16), 1);

    //assert(node->degree >= 1);
    if (unlikely(node->degree == 1)) {
//        GSTATS_APPEND_D(tid, visited_in_isearch, 1);
        return 0;
    }
    
    const int numKeys = node->degree - 1;
    const K& minKey = node->minKey;
    const K& maxKey = node->maxKey;
    
    if (unlikely(key < minKey)) {
//        GSTATS_APPEND_D(tid, visited_in_isearch, 1);
        return 0;
    }
    if (unlikely(key >= maxKey)) {
//        GSTATS_APPEND_D(tid, visited_in_isearch, 1);
        return numKeys;
    }
    // assert: minKey <= key < maxKey
    int ix = (numKeys * (key - minKey) / (maxKey - minKey));

    __builtin_prefetch((node->keyAddr(0))+(ix), 1); // prefetch approximate key location
    __builtin_prefetch((node->keyAddr(0))+(numKeys+ix), 1); // prefetch approximate pointer location to accelerate later isDcss check
    
    const K& ixKey = node->key(ix);
//    std::cout<<"key="<<key<<" minKey="<<minKey<<" maxKey="<<maxKey<<" ix="<<ix<<" ixKey="<<ixKey<<std::endl;
    if (key < ixKey) {
        // search to the left for node.key[i] <= key, then return i+1
        int i;
        for (i=ix-1;i>=0;--i) {
            if (unlikely(key >= node->key(i))) {
//                GSTATS_APPEND_D(tid, visited_in_isearch, (ix-1) - i + 1);
                return i+1;
            }
        }
        assert(false);
    } else if (key > ixKey) {
        int i;
        for (i=ix+1;i<numKeys;++i) { // recall: degree - 1 keys vs degree pointers
            if (unlikely(key < node->key(i))) {
//                GSTATS_APPEND_D(tid, visited_in_isearch, i - (ix+1) + 1);
                return i;
            }
        }
        assert(false);
    } else {
//        GSTATS_APPEND_D(tid, visited_in_isearch, 1);
        return ix+1;
    }
}

// note: val is unused if t == Erase
template <typename K, typename V, class Interpolate, class RecManager>
V istree<K,V,Interpolate,RecManager>::doUpdate(const int tid, const K& key, const V& val, UpdateType t) {
    const double SAMPLING_PROB = 1.;
    const int MAX_PATH_LENGTH = 64; // in practice, the depth is probably less than 10 even for many billions of keys. max is technically nthreads + O(log log n), but this requires an astronomically unlikely event.
    Node<K,V> * path[MAX_PATH_LENGTH]; // stack to save the path
    int pathLength;
    Node<K,V> * node;

retry:
    pathLength = 0;
    node = root;
    while (true) {
retryNode:
        auto ix = interpolationSearch(tid, key, node);
//        if (node->ptr(ix) & 1) { //////////// debug
//            std::cout<<"ix="<<ix<<std::endl;
//            std::cout<<"node @ "<<(size_t) node<<" node degree="<<node->degree<<" pointers to";
//            for (int i=0;i<node->degree;++i) std::cout<<" "<<(size_t) node->ptr(i);
//            std::cout<<std::endl;
//            std::cout<<"key="<<key<<" node minKey="<<node->minKey<<" maxKey="<<node->maxKey<<std::endl;
//            
//            std::cout<<"addr of node="<<(size_t) node<<std::endl;
//            std::cout<<"offset of capacity=0"<<std::endl;
//            std::cout<<"offset of degree="<<((size_t) &node->degree) - ((size_t) node)<<std::endl;
//            std::cout<<"offset of initSize="<<((size_t) &node->initSize) - ((size_t) node)<<std::endl;
//            std::cout<<"offset of changeSum="<<((size_t) &node->changeSum) - ((size_t) node)<<std::endl;
//            std::cout<<"offset of dirty="<<((size_t) &node->dirty) - ((size_t) node)<<std::endl;
//            std::cout<<"offset of parent="<<((size_t) &node->parent) - ((size_t) node)<<std::endl;
//            std::cout<<"offset of minKey="<<((size_t) &node->minKey) - ((size_t) node)<<std::endl;
//            std::cout<<"offset of maxKey="<<((size_t) &node->maxKey) - ((size_t) node)<<std::endl;
//            if (node->degree >= 2) std::cout<<"offset of key0="<<((size_t) node->keyAddr(0)) - ((size_t) node)<<std::endl;
//            if (node->degree >= 1) std::cout<<"offset of ptr0="<<((size_t) node->ptrAddr(0)) - ((size_t) node)<<std::endl;
//            if (node->degree >= 2) std::cout<<"offset of ptr1="<<((size_t) node->ptrAddr(1)) - ((size_t) node)<<std::endl;
//            std::cout<<"offset of end of node="<<((size_t) (((char *) &node->maxKey) + sizeof(node->maxKey) + (sizeof(K) * (node->capacity - 1) + sizeof(casword_t) * node->capacity))) - ((size_t) node)<<std::endl;
//
//            // bfs debug print
//            std::vector<Node<K,V>*> q;
//            q.push_back(root);
//            int k=0;
//            while (k < q.size()) {
//                auto node = q[k++]; // pop
//                if (node == NULL) {
//                    std::cout<<"null"<<std::endl;
//                    continue;
//                }
//                std::cout<<" degree="<<node->degree;
//                for (int i=0;i<node->degree-1;++i) {
//                    std::cout<<(i==0?":":",")<<node->key(i);
//                }
//                std::cout<<" @ "<<(size_t) node<<std::endl;
//                std::cout<<" ptrs";
//                for (int i=0;i<node->degree;++i) {
//                    std::cout<<(i==0?":":",")<<(size_t)(node->ptr(i)&~0x7)<<(node->ptr(i)&0x1?"d":node->ptr(i)&0x2?"k":node->ptr(i)&0x4?"r":"n");
//                }
//                std::cout<<std::endl;
//
//                for (int i=0;i<node->degree;++i) {
//                    auto ptr = node->ptr(i);
//                    if (IS_KVPAIR(ptr)) {
//
//                    } else if (IS_REBUILDOP(ptr)) {
//
//                    } else {
//                        assert(IS_NODE(ptr));
//                        q.push_back(CASWORD_TO_NODE(ptr));
//                    }
//                }
//            }
//            assert(false);
//        } //////////// debug
        bool affectsChangeSum = true;
        auto word = prov->readPtr(tid, node->ptrAddr(ix));
        if (IS_KVPAIR(word)) {
            auto pair = CASWORD_TO_KVPAIR(word);
            
            V retval = NO_VALUE;
            auto newWord = (casword_t) NULL;
            switch (t) {
                case InsertReplace: case InsertIfAbsent:
                    if (pair->k == INF_KEY) {
                        newWord = KVPAIR_TO_CASWORD(createKVPair(tid, key, val));
                        // retval = NO_VALUE;
                    } else if (pair->k == key) {
                        if (t == InsertIfAbsent) return pair->v;
                        newWord = KVPAIR_TO_CASWORD(createKVPair(tid, key, val));
                        retval = pair->v;
                        affectsChangeSum = false; // note: insert replace should NOT count towards changeSum, because it cannot affect the complexity of operations
                    } else {
                        auto newPair = createKVPair(tid, key, val);
                        auto newNode = createNode(tid, 2);
                        newNode->degree = 2;
                        newNode->initSize = 2;
                        newNode->parent = node;
                        
                        if (key < pair->k) {
                            newNode->key(0) = pair->k;
                            *newNode->ptrAddr(0) = KVPAIR_TO_CASWORD(newPair);
                            *newNode->ptrAddr(1) = KVPAIR_TO_CASWORD(pair);
                        } else {
                            newNode->key(0) = key;
                            *newNode->ptrAddr(0) = KVPAIR_TO_CASWORD(pair);
                            *newNode->ptrAddr(1) = KVPAIR_TO_CASWORD(newPair);
                        }
                        newNode->minKey = newNode->key(0);
                        newNode->maxKey = newNode->key(0);
                        newWord = NODE_TO_CASWORD(newNode);
                        // retval = NO_VALUE;
                    }
                    break;
                case Erase:
                    if (pair->k != key) return NO_VALUE;
                    newWord = KVPAIR_TO_CASWORD(createEmptyKVPair(tid));
                    retval = pair->v;
                    break;
                default:
                    setbench_error("impossible switch case");
                    break;
            }
            assert(newWord);
            assert((newWord & (~7)));
            
            // DCSS that performs the update
            assert(ix >= 0);
            assert(ix < node->degree);
            auto result = prov->dcssPtr(tid, (casword_t *) &node->dirty, 0, (casword_t *) node->ptrAddr(ix), word, newWord);
            switch (result.status) {
                case DCSS_FAILED_ADDR2: // retry from same node
                    goto retryNode;
                    break;
                case DCSS_FAILED_ADDR1: // node is dirty; retry from root
                    goto retry;
                    break;
                case DCSS_SUCCESS:
                    
                    if (!affectsChangeSum) break;
                    
//                    // probabilistically increment the changeSums of ancestors
//                    if (myRNG->next() / (double) std::numeric_limits<uint64_t>::max() < SAMPLING_PROB) {
                        for (int i=0;i<pathLength;++i) {
                            __sync_fetch_and_add(&path[i]->changeSum, 1);
                        }
//                    }
                    
                    // TODO: can we only try to rebuild when we've successfully propagated??? (tried it, and it doesn't seem to help at all--maybe if we had huge counter contention)
                    // TODO: is the num_processes term in the rebuild condition even needed? if there are multiple guys making changes in a subtree, the last guy to finish will see all of those changes.
                    // now, we must determine whether we should rebuild
                    for (int i=0;i<pathLength;++i) {
//                        if ((path[i]->changeSum + (NUM_PROCESSES-1)) / (SAMPLING_PROB * (1 - EPS)) >= REBUILD_FRACTION * path[i]->initSize) {
//                        if (path[i]->changeSum / (SAMPLING_PROB * (1 - EPS)) >= REBUILD_FRACTION * path[i]->initSize) {
                        if (path[i]->changeSum >= REBUILD_FRACTION * path[i]->initSize) {
                            if (i == 0) {
                                GSTATS_ADD_IX(tid, num_try_rebuild_at_depth, 1, 0);
                                rebuild(tid, path[0], root, 0, 0);
                            } else {
                                auto parent = path[i-1];
                                assert(parent->degree > 1);
                                assert(path[i]->degree > 1);
                                auto index = interpolationSearch(tid, path[i]->key(0), parent);

                                // single threaded only debug info
                                if (path[i]->degree == 1 || (TOTAL_THREADS == 1 && CASWORD_TO_NODE(parent->ptr(index)) != path[i])) {
                                    std::cout<<"i="<<i<<std::endl;
                                    std::cout<<"path length="<<pathLength<<std::endl;
                                    std::cout<<"parent@"<<(size_t) parent<<std::endl;
                                    std::cout<<"parent->degree="<<parent->degree<<std::endl;
                                    std::cout<<"parent keys";
                                    for (int j=0;j<parent->degree - 1;++j) std::cout<<" "<<parent->key(j);
                                    std::cout<<std::endl;
                                    std::cout<<"parent ptrs (converted)";
                                    for (int j=0;j<parent->degree;++j) std::cout<<" "<<(parent->ptr(j) & ~7);
                                    std::cout<<std::endl;
                                    std::cout<<"index="<<index<<std::endl;
                                    std::cout<<"parent->ptr(index) (converted)="<<(parent->ptr(index) & ~7)<<std::endl;
                                    std::cout<<"path[i]@"<<(size_t) path[i]<<std::endl;
                                    std::cout<<"path[i]->degree="<<path[i]->degree<<std::endl;
                                    std::cout<<"path[i]->key(0)="<<(path[i]->degree > 1 ? path[i]->key(0) : -1)<<std::endl;
                                    std::cout<<"path[i]->ptr(0)="<<(size_t) path[i]->ptr(0)<<std::endl;
                                    std::cout<<"path[i]->ptr(0) (converted)="<<(path[i]->ptr(0) & ~7)<<std::endl;
                                    std::cout<<"newWord="<<newWord<<std::endl;
                                    std::cout<<"newWord (converted)="<<(newWord & ~7)<<std::endl;
                                    if (IS_KVPAIR(newWord)) {
                                        std::cout<<"new key="<<CASWORD_TO_KVPAIR(newWord)->k<<std::endl;
                                    }
                                    std::cout<<"retval="<<retval<<std::endl;
                                    assert(false);
                                }

                                GSTATS_ADD_IX(tid, num_try_rebuild_at_depth, 1, i);
                                rebuild(tid, path[i], parent, index, i);
                            }
                            break;
                        }
                    }
                    
                    break;
                default:
                    setbench_error("impossible switch case");
                    break;
            }
            return retval;
        } else if (IS_REBUILDOP(word)) {
            //std::cout<<"found supposed rebuildop "<<(size_t) word<<" at path length "<<pathLength<<std::endl;
            helpRebuild(tid, CASWORD_TO_REBUILDOP(word));
            goto retry;
        } else {
            assert(IS_NODE(word));
            node = CASWORD_TO_NODE(word);
            path[pathLength++] = node; // push on stack
            assert(pathLength <= MAX_PATH_LENGTH);
        }
    }
}

template <typename K, typename V, class Interpolate, class RecManager>
Node<K,V>* istree<K,V,Interpolate,RecManager>::createNode(const int tid, const int degree) {
    size_t sz = sizeof(Node<K,V>) + sizeof(K) * (degree - 1) + sizeof(casword_t) * degree;
    Node<K,V> * node = (Node<K,V> *) new char[sz];
//    std::cout<<"node of degree "<<degree<<" allocated size "<<sz<<" @ "<<(size_t) node<<std::endl;
    assert((((size_t) node) & 0x7) == 0);
    node->capacity = degree;
    node->degree = 0;
    node->initSize = 0;
    node->changeSum = 0;
    node->dirty = 0;
    node->parent = NULL;
    assert(node);
//    if (degree == 1) assert(sz + (size_t) node >= (size_t) node->ptrAddr(0));
//    if (degree == 2) assert(sz + (size_t) node >= (size_t) node->ptrAddr(1));
    return node;
}

template <typename K, typename V, class Interpolate, class RecManager>
KVPair<K,V> * istree<K,V,Interpolate,RecManager>::createKVPair(const int tid, const K& key, const V& value) {
    auto result = new KVPair<K,V>(key, value);
//    std::cout<<"kvpair allocated of size "<<sizeof(KVPair<K,V>)<<" with key="<<key<<" @ "<<(size_t) result<<std::endl;
    assert((((size_t) result) & 0x7) == 0);
    assert(result);
    return result;
}

template <typename K, typename V, class Interpolate, class RecManager>
KVPair<K,V> * istree<K,V,Interpolate,RecManager>::createEmptyKVPair(const int tid) {
    auto result = new KVPair<K,V>(INF_KEY, NO_VALUE);
    result->empty = true;
//    std::cout<<"kvpair allocated of size "<<sizeof(KVPair<K,V>)<<" with key="<<key<<" @ "<<(size_t) result<<std::endl;
    assert((((size_t) result) & 0x7) == 0);
    assert(result);
    return result;
}

#endif /* BROWN_EXT_IST_LF_IMPL_H */
