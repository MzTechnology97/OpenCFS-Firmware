// Creates functions for code reached only through pointers (RT-Thread device ops,
// timer/IRQ callbacks, MSH command tables). A word counts as a code pointer when it
// is odd (Thumb), lands inside FLASH, and the target starts with a PUSH prologue.
// Then re-runs auto-analysis on the changes.
//@category CFS

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

public class CfsFixups extends GhidraScript {

	@Override
	public void run() throws Exception {
		Memory mem = currentProgram.getMemory();
		MemoryBlock flash = mem.getBlock("FLASH");
		int created = 0;
		for (String blockName : new String[] { "FLASH", "SRAM" }) {
			MemoryBlock b = mem.getBlock(blockName);
			for (Address a = b.getStart(); a.compareTo(b.getEnd().subtract(3)) < 0; a = a.add(4)) {
				long w = mem.getInt(a) & 0xFFFFFFFFL;
				if ((w & 1) == 0) {
					continue;
				}
				Address t = toAddr(w & ~1L);
				if (!flash.contains(t) || getFunctionAt(t) != null) {
					continue;
				}
				if (getFunctionContaining(t) != null && getInstructionAt(t) == null) {
					continue;
				}
				if (!isPushPrologue(mem, t)) {
					continue;
				}
				clearListing(t);
				if (disassemble(t) && createFunction(t, null) != null) {
					created++;
				}
			}
		}
		analyzeChanges(currentProgram);
		println("pointer-reached functions created: " + created);
	}

	private boolean isPushPrologue(Memory mem, Address t) throws Exception {
		int h = mem.getShort(t) & 0xFFFF;
		if (((h & 0xFE00) == 0xB400 && (h & 0x0100) != 0) || h == 0xED2D) {
			return true; // PUSH {..., lr}
		}
		int h2 = mem.getShort(t.add(2)) & 0xFFFF;
		return h == 0xE92D && (h2 & 0x4000) != 0; // PUSH.W {..., lr}
	}
}
