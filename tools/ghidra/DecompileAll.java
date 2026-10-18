// Decompile every function to one C file, for searching ("// <entry> <name>" before each).
// Args: <output path>
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;

public class DecompileAll extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(getScriptArgs()[0])) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (monitor.isCancelled()) break;
                DecompileResults r = decomp.decompileFunction(f, 60, monitor);
                out.printf("// %08X %s%n", f.getEntryPoint().getOffset(), f.getName());
                out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// (decompile failed)");
            }
        }
        decomp.dispose();
    }
}
