package contention.benchmark.workload.thread.loops.abstractions;

import contention.abstractions.DataStructure;
import contention.benchmark.workload.stop.condition.StopCondition;

import java.lang.reflect.Method;

public abstract class ThreadLoopBuilder {
    public int quantity;
    transient public StopCondition stopCondition;

    public ThreadLoopBuilder setQuantity(int quantity) {
        this.quantity = quantity;
        return this;
    }

    public abstract ThreadLoopBuilder init(int range);

    public abstract ThreadLoop build(int threadId, DataStructure<Integer> dataStructure,
                                     Method[] methods, StopCondition stopCondition);

    public StringBuilder toStringBuilder() {
        return toStringBuilder(1);
    }

    public abstract StringBuilder toStringBuilder(int indents);

}
