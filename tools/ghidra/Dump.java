// Prints the disassembly and decompiled C of the functions at the given addresses.
// Usage (headless): -postScript Dump.java 0x00767EC8 0x00767F08 ...
// @category CoDPort
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;

public class Dump extends GhidraScript {
  @Override
  protected void run() throws Exception {
    DecompInterface decomp = new DecompInterface();
    decomp.openProgram(currentProgram);
    for (String arg : getScriptArgs()) {
      Address addr = toAddr(arg);
      Function fn = getFunctionContaining(addr);
      if (fn == null) {
        disassemble(addr);
        fn = createFunction(addr, null);
      }
      println("==== " + arg + (fn != null ? " in " + fn.getName() + " @ " + fn.getEntryPoint() : " (no function)"));
      Address start = fn != null ? fn.getEntryPoint() : addr;
      Address end = fn != null ? fn.getBody().getMaxAddress() : addr.add(64);
      for (Instruction in = getInstructionAt(start); in != null && in.getAddress().compareTo(end) <= 0;
           in = in.getNext()) {
        println(in.getAddress() + "  " + in);
      }
      if (fn != null) {
        StringBuilder callers = new StringBuilder();
        for (Function c : fn.getCallingFunctions(monitor)) callers.append(' ').append(c.getEntryPoint());
        println("callers:" + callers);
        DecompileResults res = decomp.decompileFunction(fn, 60, monitor);
        if (res.decompileCompleted()) println(res.getDecompiledFunction().getC());
      }
    }
  }
}
