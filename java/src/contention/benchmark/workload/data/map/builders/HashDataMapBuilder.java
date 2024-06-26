package contention.benchmark.workload.data.map.builders;

import contention.benchmark.workload.data.map.abstractions.DataMap;
import contention.benchmark.workload.data.map.abstractions.DataMapBuilder;
import contention.benchmark.workload.data.map.impls.HashDataMap;
import contention.benchmark.workload.data.map.impls.hash.HashFunction;

import static contention.benchmark.tools.StringFormat.indentedTitleWithData;

public class HashDataMapBuilder extends DataMapBuilder {
    public HashFunction hashFunction;

    public HashDataMapBuilder setHashFunction(HashFunction hashFunction) {
        this.hashFunction = hashFunction;
        return this;
    }

    @Override
    public HashDataMapBuilder init(int range) {
        return this;
    }

    @Override
    public DataMap build() {
        return new HashDataMap(hashFunction);
    }

    @Override
    public StringBuilder toStringBuilder(int indents) {
        return new StringBuilder(indentedTitleWithData("Type", "HashDataMap", indents));

    }
}
