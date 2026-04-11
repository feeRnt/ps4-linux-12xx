// SPDX-License-Identifier: GPL-2.0-only
/*
 * PlayStation 4 DMI spoof fallback
 *
 * The PS4 exposes SMBIOS 2.1, but DMI strings may be blank or malformed.
 * ACPI tables still expose stable OEM identity, so use ACPI only to
 * confirm "this is a PS4", then inject a single generic PS4 fallback.
 */

#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/string.h>

#include "ps4-dmi.h"

static bool detected_ps4 __initdata;

static int __init ps4_fadt_probe(struct acpi_table_header *hdr)
{
	/*
	 * Generic PS4 probe:
	 * all logs provided so far expose Sony Interactive Entertainment
	 * ACPI OEM identity. Keep this broad and PS4-only.
	 */
	if (memcmp(hdr->oem_id, "SIE   ", ACPI_OEM_ID_SIZE) == 0)
		detected_ps4 = true;

	return 0;
}

bool __init ps4_dmi_is_ps4(void)
{
	detected_ps4 = false;
	acpi_table_parse(ACPI_SIG_FADT, ps4_fadt_probe);

	return detected_ps4;
}

static const char * const ps4_strings[DMI_STRING_MAX] = {
	[DMI_SYS_VENDOR]      = "Sony Interactive Entertainment",
	[DMI_PRODUCT_NAME]    = "PlayStation 4",
	[DMI_PRODUCT_VERSION] = "PS4",
	[DMI_PRODUCT_FAMILY]  = "PlayStation",
	[DMI_BOARD_VENDOR]    = "SIE",
	[DMI_BOARD_NAME]      = "PlayStation 4",
	[DMI_BIOS_VENDOR]     = "SIE",
	[DMI_BIOS_VERSION]    = "ORBIS",
	[DMI_BIOS_DATE]       = "09/01/2021",
	[DMI_CHASSIS_VENDOR]  = "Sony",
	[DMI_CHASSIS_TYPE]    = "3",
};

void __init ps4_dmi_populate(const char *ident[DMI_STRING_MAX])
{
	int i;

	pr_info("ps4_dmi: injecting generic PS4 fallback DMI strings\n");

	for (i = 0; i < DMI_STRING_MAX; i++) {
		if (ps4_strings[i])
			ident[i] = ps4_strings[i];
	}
}
