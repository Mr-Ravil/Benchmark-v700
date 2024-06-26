package contention.benchmark.workload.distributions.abstractions;

public interface MutableDistribution extends Distribution {
    void setRange(int range);

    default int next(int range) {
        setRange(range);
        return next();
    }
}
