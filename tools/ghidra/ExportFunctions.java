// Writes every function of the program as "entry first last" (hex, inclusive byte range of
// each address range of its body) to the file given as the script argument.
// @category CoDPort
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;

public class ExportFunctions extends GhidraScript {
  @Override
  protected void run() throws Exception {
    try (PrintWriter out = new PrintWriter(getScriptArgs()[0])) {
      for (Function fn : currentProgram.getFunctionManager().getFunctions(true)) {
        StringBuilder line = new StringBuilder(String.format("%08x", fn.getEntryPoint().getOffset()));
        for (AddressRange r : fn.getBody().getAddressRanges())
          line.append(String.format(" %08x-%08x", r.getMinAddress().getOffset(), r.getMaxAddress().getOffset()));
        out.println(line);
      }
    }
  }
}
