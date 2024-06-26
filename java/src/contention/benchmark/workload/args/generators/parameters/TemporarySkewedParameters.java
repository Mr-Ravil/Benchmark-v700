package contention.benchmark.workload.args.generators.parameters;

import contention.abstractions.ParseArgument;
import contention.benchmark.workload.data.map.abstractions.DataMapBuilder;
import contention.benchmark.workload.data.map.builders.ArrayDataMapBuilder;

import java.util.Arrays;

/**
 * n — { xi — yi — ti — rti } // либо   n — rt — { xi — yi — ti }
 * n — количество элементов
 * xi — процент элементов i-ого множества
 * yi — вероятность чтения элемента из i-ого множества
 * // 100% - yi — чтение остальных элементов
 * ti — время / количество итераций работы в режиме горячего вызова i-ого множества
 * rti / rt (relax time) — время / количество итераций работы в обычном режиме (равномерное распределение на все элементы)
 * // rt — если relax time всегда одинаковый
 * // rti — relax time после горячей работы с i-ым множеством
 */
public class TemporarySkewedParameters {
    public DataMapBuilder dataMapBuilder = new ArrayDataMapBuilder();

    public int setNumber = 0;
    public double[] setSizes;
    public double[] hotProbs;
    public int[] hotTimes;
    public int[] relaxTimes;
    public int hotTime = -1;
    public int relaxTime = -1;

    public void setSetNumber(final int setNumber) {
        this.setNumber = setNumber;
        setSizes = new double[setNumber];
        hotProbs = new double[setNumber];
        hotTimes = new int[setNumber];
        relaxTimes = new int[setNumber];

        /*
         * if hotTimes[i] == -1, we will use hotTime
         * relaxTime analogically
         */
        Arrays.fill(hotTimes, hotTime);
        Arrays.fill(relaxTimes, relaxTime);
    }

    public void setSetSize(final int i, final double setSize) {
        assert (i < setNumber);
        setSizes[i] = setSize;
    }

    public void setHotProb(final int i, final double hotProb) {
        assert (i < setNumber);
        hotProbs[i] = hotProb;
    }

    public void setHotTime(final int i, final int hotTime) {
        assert (i < setNumber);
        hotTimes[i] = hotTime;
    }

    public void setRelaxTimes(final int i, final int relaxTime) {
        assert (i < setNumber);
        relaxTimes[i] = relaxTime;
    }

    public void setCommonHotTime(final int hotTime) {
        this.hotTime = hotTime;

        for (int i = 0; i < setNumber; ++i) {
            if (hotTimes[i] == -1) {
                hotTimes[i] = this.hotTime;
            }
        }
    }

    public void setCommonRelaxTime(final int relaxTime) {
        this.relaxTime = relaxTime;

        for (int i = 0; i < setNumber; ++i) {
            if (relaxTimes[i] == -1) {
                relaxTimes[i] = this.relaxTime;
            }
        }
    }

    public int[] setsLengths;
    public int[] setsBegins;

    public void init(int range) {
        setsLengths = new int[setNumber + 1];
        setsBegins = new int[setNumber + 1];
        setsBegins[0] = 0;

        for (int i = 0; i < setNumber; i++) {
            setsLengths[i] = (int) (range * setSizes[i]);
            setsBegins[i + 1] = setsBegins[i] + setsLengths[i];
        }
    }

    public boolean parseArg(ParseArgument args) {
        switch (args.getCurrent()) {
            case "-set-count" -> setSetNumber(Integer.parseInt(args.getNext()));
            case "-rt" -> setCommonRelaxTime(Integer.parseInt(args.getNext()));
            case "-ht" -> setCommonHotTime(Integer.parseInt(args.getNext()));
            case "-si" -> setSetSize(Integer.parseInt(args.getNext()), Double.parseDouble(args.getNext()));
            case "-pi" -> setHotProb(Integer.parseInt(args.getNext()), Double.parseDouble(args.getNext()));
            case "-hti" -> setHotTime(Integer.parseInt(args.getNext()), Integer.parseInt(args.getNext()));
            case "-rti" -> setRelaxTimes(Integer.parseInt(args.getNext()), Integer.parseInt(args.getNext()));
            default -> {
                return false;
            }
        }
        return true;
    }

    public StringBuilder toStringBuilder() {
        //TODO toStringBuilder
        StringBuilder params = new StringBuilder();
        params
                .append("\n")
                .append("  Key Generator:           \tTEMPORARY_SKEWED")
                .append("\n")
                .append("  Number of Sets:          \t")
                .append(this.setNumber)
                .append("\n")
                .append("  Sets sizes:              \t")
                .append("\n");
        for (int i = 0; i < setNumber; i++) {
            params
                .append("    Set size ").append(i).append(":              \t")
                .append(this.setSizes[i]).append("\n");
        }

        params
                .append("  Common Hot Time:         \t")
                .append(this.hotTime)
                .append("\n")
                .append("  Hot Times:               \t")
                .append("\n");
        for (int i = 0; i < setNumber; i++) {
            params
                .append("    Hot Time ").append(i).append(":              \t")
                .append(this.setSizes[i]).append("\n");
        }

        params
                .append("  Hot Probabilities:       \t")
                .append("\n");
        for (int i = 0; i < setNumber; i++) {
            params
                 .append("    Hot Probability ").append(i).append(":      \t")
                 .append(this.setSizes[i]).append("\n");
        }

        params
                .append("  Common Relax Time:       \t")
                .append(this.relaxTime)
                .append("\n")
                .append("  Relax Times:             \t");

        for (int i = 0; i < setNumber; i++) {
            params.append("\n")
                .append("    Relax Time ").append(i).append(":            \t")
                .append(this.setSizes[i]);
        }

        return params;
    }
}
