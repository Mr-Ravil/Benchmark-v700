#!/bin/bash

here=`pwd`
mkdir ${here}/bin 2>/dev/null
cd .. ; cd .. ; make -j all bin_dir=${here}/bin xargs=-DIST_DISABLE_MULTICOUNTER_AT_ROOT
if [ "$?" -ne "0" ]; then
    echo "ERROR compiling"
    exit
fi
cd $here

exp="disable_multicounter"
mkdir $exp 2>/dev/null
../parse.sh null > $exp.csv
cat $exp.csv

step=10000
maxstep=$step
thread_counts=`cd .. ; ./get_thread_counts.sh`
pinning_policy=`cd .. ; ./get_pinning_cluster.sh`
t=30000
num_trials=10
timeout_s=600

for counting in 1 0 ; do
    for ((trial=0;trial<num_trials;++trial)) ; do
        for uhalf in 0.5 5 20 ; do
            for k in 2000000 20000000 200000000 ; do
                for alg in brown_ext_ist_lf ; do # brown_ext_abtree_lf bronson_pext_bst_occ natarajan_ext_bst_lf ; do
                    for n in $thread_counts ; do
                        if ((counting)); then
                            maxstep=$((maxstep+1))
                        else
                            step=$((step+1))
                            if [ "$#" -eq "1" ]; then
                                if [ "$1" -ne "$step" ]; then
                                    continue
                                fi
                            fi
                            f="$exp/step$step.txt"
                            args="-nwork $n -nprefill $n -i $uhalf -d $uhalf -rq 0 -rqsize 1 -k $k -nrq 0 -t $t -pin $pinning_policy"
                            cmd="LD_PRELOAD=../../../lib/libjemalloc.so timeout $timeout_s numactl --interleave=all time bin/ubench_${alg}.alloc_new.reclaim_debra.pool_none.out $args"
                            echo "cmd=$cmd" > $f
                            echo "step=$step" >> $f
                            echo "fname=$f" >> $f
                            eval $cmd >> $f 2>&1
                            maxres=`../grep_maxres.sh $f`
                            echo "maxresident_mb=$maxres" >> $f
                            ../parse.sh $f | tail -1 >> $exp.csv
                            echo -n "step $step/$maxstep: "
                            cat $exp.csv | tail -1
                        fi
                    done
                done
             done
        done
    done
done
