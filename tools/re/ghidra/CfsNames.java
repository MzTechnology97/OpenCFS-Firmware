// Applies tools/ghidra/names.tsv (address<TAB>name) to the current program.
// Renames the function at the address, or adds a label when there is none.
// Args: <names.tsv>
//@category CFS

import java.nio.file.Files;
import java.nio.file.Paths;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class CfsNames extends GhidraScript {

	@Override
	public void run() throws Exception {
		int renamed = 0, labelled = 0;
		for (String line : Files.readAllLines(Paths.get(getScriptArgs()[0]))) {
			String[] f = line.trim().split("\t");
			if (f.length != 2 || f[0].startsWith("#")) {
				continue;
			}
			Address a = toAddr(Long.parseLong(f[0], 16));
			Function fn = getFunctionAt(a);
			if (fn != null) {
				fn.setName(f[1], SourceType.USER_DEFINED);
				renamed++;
			}
			else {
				createLabel(a, f[1], true, SourceType.USER_DEFINED);
				labelled++;
			}
		}
		println("names applied: " + renamed + " functions, " + labelled + " labels");
	}
}
