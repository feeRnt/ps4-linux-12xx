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
	[DMI_SYS_VENDOR]         = "Sony Interactive Entertainment",
	
	[DMI_PRODUCT_NAME]       = "NeverGonna 4",
	[DMI_PRODUCT_VERSION]    = "CUH-GIVEYOUUP",
	[DMI_PRODUCT_SERIAL]     = "MARCANANDDUDES"
	[DMI_PRODUCT_SKU]        = "sku1489666",
	[DMI_PRODUCT_FAMILY]     = "PlayStation",

	[DMI_BOARD_ASSET_TAG]    = "",
	[DMI_BOARD_VENDOR]       = "SCEI",
	[DMI_BOARD_NAME]         = "Belize",
	[DMI_BOARD_VERSION]      = "rev1",
	[DMI_BOARD_SERIAL]       = "PS4HAXSONYCRY0",

	[DMI_BIOS_VENDOR]        = "SIE",
	[DMI_BIOS_VERSION]       = "PS4Homebrew",
	[DMI_BIOS_DATE]          = "21/04/2026",
	[DMI_BIOS_RELEASE]       = "69.0",

	[DMI_CHASSIS_VENDOR]     = "Sony",
	[DMI_CHASSIS_TYPE]       = "3",
	[DMI_CHASSIS_ASSET_TAG]  = "",
	[DMI_CHASSIS_SERIAL]     = "",
	[DMI_CHASSIS_VERSION]    = "",
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
