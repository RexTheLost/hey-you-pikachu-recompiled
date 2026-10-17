// Dump every function Ghidra found: "<entry> <size> <name>" (hex), one per line.
// Args: <output path>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import java.io.PrintWriter;

public class DumpFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        try (PrintWriter out = new PrintWriter(getScriptArgs()[0])) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                long entry = f.getEntryPoint().getOffset();
                long end = f.getBody().getMaxAddress().getOffset() + 1;
                out.printf("%08X %X %s%n", entry, end - entry, f.getName());
            }
        }
    }
}
