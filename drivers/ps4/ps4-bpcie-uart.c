#include <linux/serial_8250.h>
#include <linux/serial_core.h>
#include <linux/serial_reg.h>

#include "baikal.h"
//Ns8250
void bpcie_uart_remove(struct bpcie_dev *sc);

int bpcie_uart_init(struct bpcie_dev *sc)
{
	int i;
	struct uart_8250_port uart;

	for (i = 0; i < BPCIE_NR_UARTS; i++) {
		sc->serial_line[i] = -1;
	}

	for (i = 0; i < BPCIE_NR_UARTS; i++) {
		uint32_t off = BPCIE_RGN_UART_BASE + (i << 12);
		memset(&uart, 0, sizeof(uart));
		uart.port.irq		= bpcie_irqnum(sc, BPCIE_SUBFUNC_UART0 + i);
		uart.port.uartclk	= 58500000;
		uart.port.flags		= UPF_SHARE_IRQ;
		uart.port.iotype	= UPIO_MEM32;
		uart.port.mapbase	= pci_resource_start(sc->pdev, 2) + off;
		uart.port.membase	= sc->bar2 + off;
		uart.port.regshift	= 2;
		uart.port.dev		= &sc->pdev->dev;

		sc->serial_line[i] = serial8250_register_8250_port(&uart);
		if (sc->serial_line[i] < 0) {
			sc_err("Failed to register serial port %d\n", i);
			bpcie_uart_remove(sc);
			return -EIO;
		}
	}
	return 0;
}

void bpcie_uart_remove(struct bpcie_dev *sc)
{
	int i;
	for (i = 0; i < BPCIE_NR_UARTS; i++) {
		if (sc->serial_line[i] >= 0) {
			serial8250_unregister_port(sc->serial_line[i]);
			sc->serial_line[i] = -1;
		}
	}
}

#ifdef CONFIG_PM
void bpcie_uart_suspend(struct bpcie_dev *sc, pm_message_t state)
{
	int i;
	for (i = 0; i < BPCIE_NR_UARTS; i++)
		if (sc->serial_line[i] >= 0)
			serial8250_suspend_port(sc->serial_line[i]);
}

void bpcie_uart_resume(struct bpcie_dev *sc)
{
	int i;
	for (i = 0; i < BPCIE_NR_UARTS; i++)
		if (sc->serial_line[i] >= 0)
			serial8250_resume_port(sc->serial_line[i]);
}
#endif
