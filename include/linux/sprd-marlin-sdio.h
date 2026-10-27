/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SPRD_MARLIN_SDIO_H
#define _LINUX_SPRD_MARLIN_SDIO_H

#include <linux/types.h>

struct marlin_sdio;

struct marlin_sdio *marlin_sdio_get(void);
struct device *marlin_sdio_dev(struct marlin_sdio *m);
int marlin_sdio_chn_status(struct marlin_sdio *m, u16 *status);
int marlin_sdio_chn_len(struct marlin_sdio *m, unsigned int chn);
int marlin_sdio_chn_read(struct marlin_sdio *m, unsigned int chn, void *buf, int len);
int marlin_sdio_chn_write(struct marlin_sdio *m, unsigned int chn, const void *buf, int len);
void marlin_sdio_set_irq_cb(struct marlin_sdio *m, void (*cb)(void *), void *data);

#if IS_ENABLED(CONFIG_SPRD_SC2331_WLAN)
int sc2331_wlan_start(struct marlin_sdio *m);
void sc2331_wlan_stop(void);
#else
static inline int sc2331_wlan_start(struct marlin_sdio *m) { return -ENODEV; }
static inline void sc2331_wlan_stop(void) { }
#endif

#endif
