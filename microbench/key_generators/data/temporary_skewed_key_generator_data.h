//
// Created by Ravil Galiev on 10.02.2023.
//

#ifndef SETBENCH_TEMPORARY_SKEWED_KEY_GENERATOR_DATA_H
#define SETBENCH_TEMPORARY_SKEWED_KEY_GENERATOR_DATA_H

#include <algorithm>
#include "random_xoshiro256p.h"
#include "plaf.h"
#include "key_generators/key_generator.h"
#include "distributions/distribution.h"
#include "parameters/temporary_skewed_parameters.h"

template<typename K>
class TemporarySkewedKeyGeneratorData : public KeyGeneratorData<K> {
public:
    PAD;
    size_t maxKey;

    int *data;
    size_t *setLengths;
    size_t *setBegins;
    TemporarySkewedParameters *TSParm;
    PAD;

    explicit TemporarySkewedKeyGeneratorData() {}

    TemporarySkewedKeyGeneratorData(const size_t _maxKey, TemporarySkewedParameters *TSParm)
            : maxKey(_maxKey), TSParm(TSParm) {
        data = new int[maxKey];
        for (int i = 0; i < maxKey; i++) {
            data[i] = i + 1;
        }

        std::random_shuffle(data, data + maxKey - 1);

        setLengths = new size_t[TSParm->setCount + 1];
        setBegins = new size_t[TSParm->setCount + 1];
        setBegins[0] = 0;
        for (int i = 0; i < TSParm->setCount; i++) {
            setLengths[i] = (size_t) (maxKey * TSParm->setSizes[i]);
            setBegins[i + 1] = setBegins[i] + setLengths[i];
        }
    }

    K get(size_t index) {
        return data[index];
    }

    ~TemporarySkewedKeyGeneratorData() {
        std::cout << "deleting data" << "\n";

        delete[] data;
        delete[] setLengths;
        delete[] setBegins;
    }
};



#endif //SETBENCH_TEMPORARY_SKEWED_KEY_GENERATOR_DATA_H
