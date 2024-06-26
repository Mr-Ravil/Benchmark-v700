package contention.abstractions;

public class ParseArgument {
    public int pointer;
    public String[] args;

    public ParseArgument(String[] args) {
        this.args = args;
        this.pointer = 0;
    }

    public String getCurrent() {
        return args[pointer];
    }

    public String getNext() {
        return args[++pointer];
    }

    public ParseArgument next() {
        pointer++;
        return this;
    }

    public boolean hasNext() {
        return pointer < args.length;
    }
}
