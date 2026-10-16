#ifndef BK_ACPI_PM_H
#define BK_ACPI_PM_H
#include "types.h"
#define ACPI_PM_HZ 3579545U
/* Optional fixed-frequency wall clock. Initialization requires a running PIT. */
bool acpi_pm_init(void);
bool acpi_pm_read(uint32_t *counter);
uint32_t acpi_pm_mask(void);
#endif
