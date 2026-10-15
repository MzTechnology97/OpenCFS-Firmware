// Pre-analysis setup for a CFS image (GD32F303): app at 0x08003000 or loader at 0x08000000.
// Args: <sram_init.bin>
// Adds SRAM (initialized from the unpacked scatter image), peripheral and
// system-control blocks, GD32F30x peripheral labels and the vector table.
//@category CFS

import java.io.File;
import java.io.FileInputStream;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

public class CfsSetup extends GhidraScript {


	static final Object[][] PERIPHERALS = {
		{ "TIMER1", 0x40000000L }, { "TIMER2", 0x40000400L }, { "TIMER3", 0x40000800L },
		{ "TIMER4", 0x40000C00L }, { "TIMER5", 0x40001000L }, { "TIMER6", 0x40001400L },
		{ "TIMER11", 0x40001800L }, { "TIMER12", 0x40001C00L }, { "TIMER13", 0x40002000L },
		{ "RTC", 0x40002800L }, { "WWDGT", 0x40002C00L }, { "FWDGT", 0x40003000L },
		{ "SPI1", 0x40003800L }, { "SPI2", 0x40003C00L }, { "USART1", 0x40004400L },
		{ "USART2", 0x40004800L }, { "UART3", 0x40004C00L }, { "UART4", 0x40005000L },
		{ "I2C0", 0x40005400L }, { "I2C1", 0x40005800L }, { "USBD", 0x40005C00L },
		{ "CAN0", 0x40006400L }, { "BKP", 0x40006C00L }, { "PMU", 0x40007000L },
		{ "DAC", 0x40007400L }, { "AFIO", 0x40010000L }, { "EXTI", 0x40010400L },
		{ "GPIOA", 0x40010800L }, { "GPIOB", 0x40010C00L }, { "GPIOC", 0x40011000L },
		{ "GPIOD", 0x40011400L }, { "GPIOE", 0x40011800L }, { "GPIOF", 0x40011C00L },
		{ "GPIOG", 0x40012000L }, { "ADC0", 0x40012400L }, { "ADC1", 0x40012800L },
		{ "TIMER0", 0x40012C00L }, { "SPI0", 0x40013000L }, { "TIMER7", 0x40013400L },
		{ "USART0", 0x40013800L }, { "ADC2", 0x40013C00L }, { "TIMER8", 0x40014C00L },
		{ "TIMER9", 0x40015000L }, { "TIMER10", 0x40015400L }, { "SDIO", 0x40018000L },
		{ "DMA0", 0x40020000L }, { "DMA1", 0x40020400L }, { "RCU", 0x40021000L },
		{ "FMC", 0x40022000L }, { "CRC", 0x40023000L },
		{ "SysTick", 0xE000E010L }, { "NVIC", 0xE000E100L }, { "SCB", 0xE000ED00L },
		{ "DBG", 0xE0042000L },
	};

	static final String[] SYSTEM_VECTORS = {
		"__initial_sp", "Reset_Handler", "NMI_Handler", "HardFault_Handler",
		"MemManage_Handler", "BusFault_Handler", "UsageFault_Handler", null, null, null, null,
		"SVC_Handler", "DebugMon_Handler", null, "PendSV_Handler", "SysTick_Handler",
	};

	static final String[] IRQS = {
		"WWDGT", "LVD", "TAMPER", "RTC", "FMC", "RCU", "EXTI0", "EXTI1", "EXTI2", "EXTI3",
		"EXTI4", "DMA0_Ch0", "DMA0_Ch1", "DMA0_Ch2", "DMA0_Ch3", "DMA0_Ch4", "DMA0_Ch5",
		"DMA0_Ch6", "ADC0_1", "USBD_HP_CAN0_TX", "USBD_LP_CAN0_RX0", "CAN0_RX1", "CAN0_EWMC",
		"EXTI5_9", "TIMER0_BRK_TIMER8", "TIMER0_UP_TIMER9", "TIMER0_TRG_CMT_TIMER10",
		"TIMER0_Channel", "TIMER1", "TIMER2", "TIMER3", "I2C0_EV", "I2C0_ER", "I2C1_EV",
		"I2C1_ER", "SPI0", "SPI1", "USART0", "USART1", "USART2", "EXTI10_15", "RTC_Alarm",
		"USBD_WKUP", "TIMER7_BRK_TIMER11", "TIMER7_UP_TIMER12", "TIMER7_TRG_CMT_TIMER13",
		"TIMER7_Channel", "ADC2", "EXMC", "SDIO", "TIMER4", "SPI2", "UART3", "UART4",
		"TIMER5", "TIMER6", "DMA1_Ch0", "DMA1_Ch1", "DMA1_Ch2", "DMA1_Ch3_4",
	};

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 1) {
			throw new IllegalArgumentException("usage: CfsSetup.java <sram_init.bin>");
		}
		Memory mem = currentProgram.getMemory();
		final long FLASH = mem.getBlock("FLASH").getStart().getOffset();
		File sram = new File(args[0]);
		try (FileInputStream in = new FileInputStream(sram)) {
			MemoryBlock b = mem.createInitializedBlock("SRAM", toAddr(0x20000000L), in,
				sram.length(), monitor, false);
			b.setPermissions(true, true, true);
		}
		volatileBlock("PERIPH", 0x40000000L, 0x24000L);
		volatileBlock("SCS", 0xE0000000L, 0x100000L);

		for (Object[] p : PERIPHERALS) {
			createLabel(toAddr((Long) p[1]), (String) p[0], true, SourceType.USER_DEFINED);
		}

		int vectors = SYSTEM_VECTORS.length + IRQS.length;
		for (int i = 0; i < vectors; i++) {
			Address slot = toAddr(FLASH + 4L * i);
			createData(slot, PointerDataType.dataType);
			String name = i < SYSTEM_VECTORS.length ? SYSTEM_VECTORS[i]
					: IRQS[i - SYSTEM_VECTORS.length] + "_IRQHandler";
			if (i == 0 || name == null) {
				continue;
			}
			long target = getInt(slot) & 0xFFFFFFFFL;
			if (target == 0) {
				continue;
			}
			Address entry = toAddr(target & ~1L);
			createLabel(slot, "vec_" + name, true, SourceType.USER_DEFINED);
			if (getFunctionAt(entry) == null) {
				disassemble(entry);
				createFunction(entry, null);
			}
			// Several vectors share Default_Handler; keep the first name only.
			if (getFunctionAt(entry) != null &&
				getFunctionAt(entry).getName().startsWith("FUN_")) {
				getFunctionAt(entry).setName(name, SourceType.USER_DEFINED);
			}
		}
		addEntryPoint(toAddr(getInt(toAddr(FLASH + 4)) & ~1L));
		println("CFS memory map and " + vectors + " vectors set up");
	}

	private void volatileBlock(String name, long start, long len) throws Exception {
		MemoryBlock b = currentProgram.getMemory().createUninitializedBlock(name,
			toAddr(start), len, false);
		b.setPermissions(true, true, false);
		b.setVolatile(true);
	}
}
