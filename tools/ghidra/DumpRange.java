// Prints the decompiled C of every function whose entry lies in [start, end).
// Usage (headless): -postScript DumpRange.java 0x00753000 0x00755000
// @category CoDPort
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DumpRange extends GhidraScript {
  @Override
  protected void run() throws Exception {
    String[] args = getScriptArgs();
    Address start = toAddr(args[0]), end = toAddr(args[1]);
    DecompInterface decomp = new DecompInterface();
    decomp.openProgram(currentProgram);
    for (Function fn : currentProgram.getFunctionManager().getFunctions(start, true)) {
      if (fn.getEntryPoint().compareTo(end) >= 0) break;
      StringBuilder callers = new StringBuilder();
      for (Function c : fn.getCallingFunctions(monitor)) callers.append(' ').append(c.getEntryPoint());
      println("==== " + fn.getEntryPoint() + " callers:" + callers);
      DecompileResults res = decomp.decompileFunction(fn, 60, monitor);
      if (res.decompileCompleted()) println(res.getDecompiledFunction().getC());
    }
  }
}
