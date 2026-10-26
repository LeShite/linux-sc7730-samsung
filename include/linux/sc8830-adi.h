/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SC8830_ADI_H
#define _LINUX_SC8830_ADI_H

#include <linux/spinlock.h>

/*
 * One lock for the ADI bus of the SC8830 family. A read is two steps on shared registers (the
 * address goes to RD_CMD, the answer comes back in RD_DATA), so two drivers reading at once get
 * each other's data; a read-modify-write on garbage then wipes the register. Every driver that
 * touches the PMIC through the ADI holds this around each read and each read-modify-write.
 */
extern raw_spinlock_t sc8830_adi_lock;

#endif
