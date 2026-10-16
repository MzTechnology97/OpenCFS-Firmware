// Post-analysis export for the CFS application.
// Args: <output_dir>
// Writes functions.tsv, callgraph.tsv, strings.tsv and decompiled C split
// into decomp/<entry>.c. Output is for private analysis only.
//@category CFS

import java.io.File;
import java.io.PrintWriter;
import java.util.Set;
import java.util.TreeSet;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;

public class CfsExport extends GhidraScript {

	@Override
	public void run() throws Exception {
		File out = new File(getScriptArgs()[0]);
		File decompDir = new File(out, "decomp");
		decompDir.mkdirs();

		DecompInterface ifc = new DecompInterface();
		ifc.openProgram(currentProgram);

		try (PrintWriter fn = new PrintWriter(new File(out, "functions.tsv"), "UTF-8");
				PrintWriter cg = new PrintWriter(new File(out, "callgraph.tsv"), "UTF-8")) {
			fn.println("entry\tname\tsize\tcallers\tcallees\tperipherals\tstrings");
			cg.println("caller\tcallee");
			for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
				monitor.checkCancelled();
				Set<String> periph = new TreeSet<>();
				Set<String> strs = new TreeSet<>();
				for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(),
					true)) {
					for (Reference r : ins.getReferencesFrom()) {
						collect(r.getToAddress(), periph, strs, 0);
					}
				}
				Set<Function> callees = f.getCalledFunctions(monitor);
				for (Function c : callees) {
					cg.println(f.getEntryPoint() + "\t" + c.getEntryPoint());
				}
				fn.println(f.getEntryPoint() + "\t" + f.getName() + "\t" +
					f.getBody().getNumAddresses() + "\t" +
					f.getCallingFunctions(monitor).size() + "\t" + callees.size() + "\t" +
					String.join(",", periph) + "\t" + String.join(" | ", strs));

				DecompileResults res = ifc.decompileFunction(f, 60, monitor);
				try (PrintWriter c = new PrintWriter(
					new File(decompDir, f.getEntryPoint().toString() + ".c"), "UTF-8")) {
					c.println("// " + f.getName() + " @ " + f.getEntryPoint());
					c.println(res.decompileCompleted() ? res.getDecompiledFunction().getC()
							: "// decompile failed: " + res.getErrorMessage());
				}
			}
		}

		try (PrintWriter st = new PrintWriter(new File(out, "strings.tsv"), "UTF-8")) {
			st.println("address\tstring\treferenced_by");
			for (Data d : currentProgram.getListing().getDefinedData(true)) {
				StringDataInstance s = StringDataInstance.getStringDataInstance(d);
				if (s == StringDataInstance.NULL_INSTANCE) {
					continue;
				}
				Set<String> users = new TreeSet<>();
				for (Reference r : getReferencesTo(d.getAddress())) {
					Function u = getFunctionContaining(r.getFromAddress());
					users.add(u != null ? u.getName() : r.getFromAddress().toString());
				}
				String v = s.getStringValue();
				st.println(d.getAddress() + "\t" +
					(v == null ? "" : v.replace("\t", "\\t").replace("\n", "\\n").replace("\r", "\\r")) +
					"\t" + String.join(",", users));
			}
		}
		ifc.dispose();
		println("exported " + currentProgram.getFunctionManager().getFunctionCount() +
			" functions to " + out);
	}

	/** Records peripheral registers and strings reached directly or through one literal-pool pointer. */
	private void collect(Address to, Set<String> periph, Set<String> strs, int depth)
			throws Exception {
		MemoryBlock b = getMemoryBlock(to);
		if (b == null) {
			return;
		}
		if (b.getName().equals("PERIPH") || b.getName().equals("SCS")) {
			Symbol base = currentProgram.getSymbolTable().getPrimarySymbol(to);
			if (base == null) {
				for (long back = 0; back <= 0x400 && base == null; back += 4) {
					Address a = to.subtract(back);
					Symbol s = currentProgram.getSymbolTable().getPrimarySymbol(a);
					if (s != null && !s.getName().startsWith("DAT_")) {
						periph.add(s.getName() + "+0x" + Long.toHexString(back));
						return;
					}
				}
				periph.add(to.toString());
			}
			else {
				periph.add(base.getName());
			}
			return;
		}
		Data d = getDataAt(to);
		if (d == null) {
			return;
		}
		StringDataInstance s = StringDataInstance.getStringDataInstance(d);
		if (s != StringDataInstance.NULL_INSTANCE && s.getStringValue() != null) {
			strs.add(s.getStringValue().replace("\t", " ").replace("\n", "\\n"));
			return;
		}
		if (depth == 0 && d.isPointer() && d.getValue() instanceof Address) {
			collect((Address) d.getValue(), periph, strs, 1);
		}
	}
}
