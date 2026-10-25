// SPDX-License-Identifier: GPL-2.0
/*
 * Speaker playback for the Spreadtrum SC8830 family (SC7730) with the codec
 * inside the SC2723S PMIC: Galaxy Tab A 7.0 (SM-T280).
 *
 * A plain ALSA card, one playback stream, S16_LE stereo at 44.1/48 kHz:
 *
 *   buffer --AP DMA--> VBC DA0/DA1 --> codec digital part (AUD_*) -->
 *   ADie digital interface --> DAC --> internal speaker PA (AOL)
 *
 * The VBC (voice band controller) has one ping-pong FIFO per
 * channel (160 samples in the vendor kernel, 64 here), each with its own DMA
 * request (UID 29 left, 30 right). As in the
 * vendor 3.10 kernel (sound/soc/sprd/dai/sprd-pcm.c, drivers/dma/sprd_dma.c)
 * each channel gets a "full" AP DMA channel running a cyclic link list with
 * one node per period, reading every other 16-bit word of the interleaved
 * buffer, one FIFO's worth per request. The block-done interrupt of the left
 * channel ends a period. Power-up sequence of the codec and the PA as in
 * sound/soc/sprd/codec/sprd/v4 (checked with drivers/misc/sc8830-audio-test).
 *
 * The codec is powered on when the stream is opened and off 5 s after it is
 * closed, like the vendor's SND_SOC_PM_DOWN_TIME.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/uio.h>
#include <linux/workqueue.h>
#include <sound/control.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/tlv.h>

/* AP AHB (0x20d00000) */
#define AHB_EB			0x0000
#define  AHB_DMA_EB		BIT(5)

/* AON APB (0x402e0000) */
#define AON_APB_EB0		0x0000
#define  AON_VBC_EB		BIT(19)
#define  AON_AUD_EB		BIT(18)
#define  AON_AUDIF_EB		BIT(17)
#define AON_APB_RST0		0x0008
#define  AON_VBC_SOFT_RST	BIT(20)
#define  AON_AUD_SOFT_RST	BIT(19)
#define  AON_AUDIF_SOFT_RST	BIT(18)
#define AON_SINDRV_CTRL		0x0018
#define  AON_SINDRV_ENA		BIT(0)
#define AON_VBC_CTRL		0x0020	/* who owns VBC and codec: 0 = AP */
#define  AON_AUDIF_CKG_AUTO_EN	BIT(20)
#define  AON_VBC_OWNER_MASK	GENMASK(19, 1)
#define AON_AP_WPROT_EN		0x3040
#define  AON_WPROT_VBC		BIT(6)

/* PMU APB (0x402b0000) */
#define PMU_SLEEP_CTRL		0x00cc
#define  PMU_AP_SLEEP_XTL_ON	BIT(8)
#define PMU_MEM_PD_CFG0		0x0138
#define  PMU_MEM_PD_VBC_AUD	GENMASK(5, 2)

/* VBC (0x40020000) */
#define VBC_PHYS		0x40020000
#define VBDA0			0x0000
#define VBDA1			0x0004
#define VBBUFFSIZE		0x0010
#define  VBDABUFFERSIZE		GENMASK(15, 8)
#define VBDABUFFDTA		0x0018
#define  VBENABLE		BIT(15)
#define  VBDA1DMA_EN		BIT(14)
#define  VBDA0DMA_EN		BIT(13)
#define  RAMSW_EN		BIT(10)
#define  RAMSW_NUMB		BIT(9)
#define VBDACNT			0x0020
#define VBIISSEL		0x003c
#define  VBIISSEL_DA_LRCK	BIT(14)
#define  VBIISSEL_DA_PORT	GENMASK(8, 6)
#define DADGCTL			0x0044	/* digital gain: enable b7 (DA0), b15 (DA1) */
#define  DADG_EN		(BIT(7) | BIT(15))
#define DAHPCTL			0x0048
#define  VBDAC_ALC_EN		BIT(11)
#define DAALCCTL0		0x004c	/* .. DAALCCTL10 at 0x0074 */
#define STCTL0			0x0078
#define STCTL1			0x007c
#define DACSRCCTL		0x0094
#define MIXERCTL		0x0098
#define VBCHNEN			0x00c8
#define  VBDA0_EN		BIT(0)
#define  VBDA1_EN		BIT(1)
/*
 * Samples of each channel per DMA request. The vendor used 160, but then a period
 * must be a multiple of 160 frames and alsa-lib/pipewire ask for 1024: refused
 * with EINVAL. The VBC takes up to 256; 64 fits every power-of-two period.
 */
/*
 * Frames per VBC/DMA request (the FIFO half). Tried on the tablet with the same music:
 * By ear with loud music: 128 sounded better than 16 (the vendor uses 160); the earlier harshness
 * came from level and the ALC. "FIFO Frames" changes it.
 */
#define VBC_FIFO_FRAMES		128
#define VBC_FIFO_MIN		16
#define VBC_FIFO_MAX		256	/* the buffer size field of the VBC is 8 bits */

/* Codec digital part (0x40000000) */
#define AUD_TOP_CTL		0x0000
#define AUD_AUD_CTR		0x0004
#define AUD_I2S_CTL		0x0008
#define  DAC_EN_L		BIT(0)
#define  DAC_EN_R		BIT(2)
#define AUD_DAC_CTL		0x000c
#define  DAC_MUTE_EN		BIT(15)
#define  DAC_RATE		GENMASK(3, 0)
#define AUD_SDM_CTL0		0x0010
#define AUD_DAC_SDM_L		0x0038
#define AUD_DAC_SDM_H		0x003c

/* PMIC registers, offsets in the ADI window at 0x40038000 */
#define ANA_EIC_DATA		0x0100		/* analog EIC: channel 8 is the jack's insert line */
#define ANA_EIC_DMSK		0x0104
#define ANA_EIC_CTRL(n)		(0x0140 + 4 * (n))
#define  EIC_DBNC_EN		BIT(14)
#define  EIC_DBNC_CNT		GENMASK(11, 0)
#define EIC_JACK		8		/* gpio_detect = 312 = a_eic base 304 + 8 */
#define ANA_AUDIFA		0x0400
#define ANA_PMU0		0x0600
#define  PA_SW_EN		BIT(15)
#define  PA_LDO_EN		BIT(14)
#define  PA_EN			BIT(13)
#define  OVP_LDO_EN		BIT(11)
#define  VB_EN			BIT(9)
#define  VBO_EN			BIT(8)
#define  LDOCG_EN		BIT(10)
#define  GND_PLGPD_EN		BIT(4)		/* the jack's ground switch */
#define ANA_PMU1		0x0604
#define  LDOCG_EN_FAL_ILD	BIT(3)
#define  LDOCG_ISET_FAL		BIT(2)
#define  LDOCG_RAMP_EN		BIT(1)
#define  BG_EN			BIT(15)
#define  BG_IBIAS_EN		BIT(14)
#define  VCM_EN			BIT(13)
#define  VCM_BUF_EN		BIT(12)
#define  SEL_VCMI		BIT(11)
#define  VCMI_FAST_EN		BIT(10)
#define  LDOVBO_FAST_EN		BIT(0)
#define ANA_PMU2		0x0608
#define  VCM_V			GENMASK(15, 13)
#define ANA_PMU3		0x060c
#define  PA_AB_I		GENMASK(13, 11)
#define  PA_LDO_V		GENMASK(10, 8)
#define ANA_PMU5		0x0614
#define  PA_OVP_V		GENMASK(15, 13)
#define ANA_PMU6		0x0618
#define  DRV_OCP_AOR_PD		BIT(15)
#define  DRV_OCP_AOL_PD		BIT(14)
#define ANA_CDC0		0x061c
#define  DAC_CLK_EN		BIT(11)
#define  DRV_CLK_EN		BIT(8)
#define ANA_CDC2		0x0624
#define  DACL_EN		BIT(15)
#define  DACR_EN		BIT(14)
#define  DACDC_RMV_EN		BIT(12)
#define  HPL_EN			BIT(8)
#define  HPR_EN			BIT(7)
#define  DIFF_EN		BIT(6)
#define  AOL_EN			BIT(4)
#define ANA_CDC3		0x0628		/* class-G headphone amplifier */
#define  CG_REF_EN		BIT(15)
#define  CG_CHP_EN		BIT(14)
#define  CG_HPL_EN		BIT(13)
#define  CG_HPR_EN		BIT(12)
#define  CG_CHP_OSC		GENMASK(9, 7)
#define  CG_LPW			GENMASK(6, 5)
#define  CG_HP_MODE		BIT(3)
#define  CG_HPCAL_EN		BIT(2)
#define ANA_CDC4		0x062c
#define  PA_D_EN		BIT(15)
#define  PA_DTRI_F		GENMASK(14, 13)
#define  PA_DEMI_EN		BIT(12)
#define ANA_CDC6		0x0634
#define  DACL_AOL		BIT(13)
#define  DACR_AOL		BIT(12)
#define ANA_CDC7		0x0638
#define  HPL_CGL		BIT(11)
#define  HPR_CGR		BIT(10)
#define  DACL_P_HPL		BIT(7)
#define  DACR_P_HPR		BIT(2)
#define ANA_CDC10		0x0644
#define  DACL_G			GENMASK(15, 13)
#define  DACR_G			GENMASK(12, 10)
#define ANA_CDC11		0x0648
#define  AOL_G			GENMASK(15, 12)
#define ANA_CDC12		0x064c
#define  HPL_G			GENMASK(11, 8)
#define  HPR_G			GENMASK(7, 4)
#define ANA_CDC13		0x0650		/* class-G gains, left */
#define ANA_CDC14		0x0654		/* ... and right */
#define  CG_G_1			GENMASK(15, 10)
#define  CG_G_2			GENMASK(9, 7)
#define ANA_CDC16		0x065c
#define  HP_POP_CTL		GENMASK(7, 6)
#define  HP_POP_STEP		GENMASK(5, 3)
#define ANA_HDT0		0x0668		/* headset detection */
#define  HEAD_INS_PD		BIT(10)
#define  HEAD_SDET		GENMASK(6, 5)
#define  HEADDETECT_PD		BIT(7)
#define  HEAD_INS_VREF		GENMASK(9, 8)
#define ANA_HDT1		0x066c
#define  GND_DET_PU_PD		BIT(15)
#define  HEAD_L_INT_PU_PD	BIT(14)
#define ANA_STS0		0x0674
#define  HP_POP_FLG		GENMASK(8, 7)
#define  HEAD_INSERT		BIT(10)
#define  HEAD_INSERT_2		BIT(9)
#define DANGL			0x0680		/* set to 0xffff with the class-G amp, SC2723 AA only */
#define DANGR			0x0684
#define DIG_CFG0		0x068c
#define  AUDIFA_DACL_EN		BIT(9)
#define  AUDIFA_DACR_EN		BIT(11)
#define ANA_GLB_ARM_MODULE_EN	0x0800
#define  ANA_AUD_EN		BIT(4)
#define  ANA_HDT_EN		BIT(5)
#define  ANA_EIC_EN		BIT(3)
#define ANA_GLB_ARM_CLK_EN	0x0804
#define  CLK_AUD_6P5M_EN	BIT(1)
#define  CLK_AUD_HID_EN		BIT(4)
#define  CLK_AUD_HBD_EN		BIT(3)
#define  CLK_AUDIF_EN		BIT(0)
#define ANA_GLB_RTC_CLK_EN	0x0808
#define  RTC_AUD_EN		BIT(4)
#define  RTC_EIC_EN		BIT(3)
#define ANA_GLB_ARM_RST		0x080c
#define  ANA_AUD_RSTS		GENMASK(11, 9)
#define ANA_GLB_AUD_SLP_CTRL	0x08a4
#define ANA_GLB_XTL_WAIT_CTRL	0x08e8
#define  XTL_EN			BIT(8)
#define ANA_GLB_AUDIO_CTRL0	0x08fc
#define  CLK_AUD_6P5M_TX_INV_EN	BIT(1)
#define ANA_GLB_AUDIO_CTRL1	0x0900
#define ANA_GLB_CHIP_ID_LOW	0x093c		/* bits 11:0: version, 0 = AA */
#define ANA_GLB_CHIP_ID_HIGH	0x0940

/* ADI controller (0x40030000), as in sc2723-regulator.c */
#define ADI_GSSI_CFG0		0x1c
#define ADI_RD_CMD		0x24
#define ADI_RD_DATA		0x28
#define  ADI_RD_DATA_BUSY	BIT(31)
#define  ADI_RD_DATA_ADDR	GENMASK(30, 16)
#define  ADI_RD_DATA_VAL	GENMASK(15, 0)
#define ADI_FIFO_STS		0x2c
#define  ADI_FIFO_EMPTY		BIT(10)

/* AP DMA (0x20100000), version r4p0: channels 0-23 standard, 24+ full */
#define DMA_GLB_INT_MSK_STS	0x0014
#define DMA_REQ_CID(uid)	(0x2000 + 4 * ((uid) - 1))
#define DMA_CHN(ch)		(0x1000 + 0x40 * (ch))
#define DMA_CHN_PAUSE		0x00
#define  DMA_PAUSE		BIT(0)
#define  DMA_PAUSED		BIT(16)
#define DMA_CHN_REQ		0x04
#define DMA_CHN_CFG		0x08
#define  DMA_CHN_EN		BIT(0)
#define  DMA_LLIST_EN		BIT(4)
#define  DMA_PRIO_1		(1 << 12)
#define DMA_CHN_INTC		0x0c
#define  DMA_INT_BLK_EN		BIT(1)
#define  DMA_INT_CFGERR_EN	BIT(4)
#define  DMA_INT_EN_MASK	GENMASK(4, 0)
#define  DMA_INT_CLR		GENMASK(28, 24)
#define DMA_CHN_SRC_ADDR	0x10
#define DMA_NODE_WORDS		16	/* a link-list node: one channel's registers */
#define DMA_UID_VB_DA0		29
#define DMA_UID_VB_DA1		30
#define DMA_CH_LEFT		24
#define DMA_CH_RIGHT		25
/*
 * FRG_LEN of each node: 16-bit both sides (width code 1: 0 byte, 1 halfword,
 * 2 word; the vendor's SHORT_WIDTH enum is 2 but dma_r4p0.c writes 1),
 * fragment request, fixed destination
 */
#define DMA_FRG_LEN_VBC(fifo)	((1u << 30) | (1u << 28) | (1u << 21) | (1u << 20) | \
				 ((fifo) * 2))

#define PERIODS_MAX		32
#define BUFFER_BYTES_MAX	(128 * 1024)
#define POWER_OFF_DELAY_MS	5000
/*
 * AOL_G: -24 dB + 3 dB per step (vendor spk_tlv). The speaker is mono and both
 * channels are mixed into it; 10 (+6 dB) seemed to clip, but the distortion
 * stayed at 6 (-6 dB), so it was not the PA.
 */
#define SPK_GAIN_DEFAULT	8	/* 0 dB */
/*
 * Top of the volume control. Userspace sets it to 100 % (Batocera's volume through
 * the TLV), and the register's 15 is +21 dB: it saturated. 8 = 0 dB is clean.
 */
#define SPK_GAIN_MAX		8

enum { MIX_LR, MIX_LEFT, MIX_RIGHT, MIX_DOWNMIX };
enum { OUT_SPEAKER, OUT_HEADPHONE, OUT_BOTH, OUT_AUTO };

struct sc8830_audio {
	struct device *dev;
	void __iomem *vbc, *codec, *aon, *pmu, *adi, *pmic, *dma, *ahb;
	phys_addr_t pmic_phys;
	u32 addr_mask;
	spinlock_t adi_lock;

	struct snd_card *card;
	struct snd_pcm_substream *substream;
	u32 *nodes[2];			/* link lists, left and right */
	dma_addr_t nodes_phys[2];
	unsigned int irqs;

	/* bring-up: level of the data that the DMA plays, from the DMA buffer itself */
	struct delayed_work level;
	unsigned int lv_n, lv_clip;
	int lv_peak_l, lv_peak_r;


	struct mutex lock;		/* power state and the controls */
	bool powered;
	int spk_gain;
	unsigned int fifo;		/* frames per VBC/DMA request */
	bool spk_on;
	/* bring-up knobs, the ALSA controls of the same names */
	int mix;			/* MIX_*: left + right, left only, right only, downmix */
	int boost;			/* extra speaker amp gain steps, added to the volume */
	bool write_mode;		/* no mmap: user space writes, so .copy can downmix */
	int dac_gain;			/* ANA_CDC10 DAC gain, 0 mute .. 7 = 0 dB */
	bool alc;			/* VBC limiter */
	int vbc_dg;			/* VBC digital gain, 0 = off */
	struct delayed_work power_off;
	/* headphone (3.5 mm jack) */
	int out;			/* OUT_*: where the sound goes */
	int hp_gain;			/* ANA_CDC12 HPL_G/HPR_G, 0 .. 15 */
	bool hp_up;			/* class-G amplifier is on */
	bool hp_cg_link;		/* HPL_CGL/HPR_CGR switches of the vendor mixer */
	bool jack_invert;		/* polarity of the insert line, unconfirmed */
	int jack_raw;			/* last EIC jack line, -1 unknown */
	u16 jack_eic, jack_sts;		/* last logged EIC data and ANA_STS0 insert bits */
	unsigned int jack_logs;
	struct delayed_work jack;	/* polls the jack, also while nothing plays */
	bool chip_aa;			/* SC2723 version AA: DANGL/DANGR with the class-G amp */
};

/* DAALCCTL0-10 of the vendor's default DA profile (vbc_da_eq_profile_default) */
static const u32 vbc_alc_default[] = {
	0x01e0, 0x2000, 0x04fe, 0x001f, 0xffe2, 0x7fff, 0x028c, 0x0010, 0x04dd,
	0x0000, 0x0062,
};

static void upd(void __iomem *base, u32 reg, u32 mask, u32 val)
{
	writel((readl(base + reg) & ~mask) | (val & mask), base + reg);
}

static int adi_read(struct sc8830_audio *sa, u32 reg, u16 *val)
{
	u32 phys = sa->pmic_phys + reg, data;
	int ret;

	writel(phys, sa->adi + ADI_RD_CMD);
	ret = readl_poll_timeout_atomic(sa->adi + ADI_RD_DATA, data,
					!(data & ADI_RD_DATA_BUSY), 1, 2000);
	if (ret)
		return ret;
	if ((FIELD_GET(ADI_RD_DATA_ADDR, data) & sa->addr_mask) !=
	    (phys & sa->addr_mask))
		return -EIO;
	*val = FIELD_GET(ADI_RD_DATA_VAL, data);
	return 0;
}

static void adi_upd(struct sc8830_audio *sa, u32 reg, u16 mask, u16 val)
{
	unsigned long flags;
	u16 old = 0;
	u32 sts;

	spin_lock_irqsave(&sa->adi_lock, flags);
	if (adi_read(sa, reg, &old))
		dev_warn(sa->dev, "ADI read 0x%03x failed\n", reg);
	writel((old & ~mask) | (val & mask), sa->pmic + reg);
	if (readl_poll_timeout_atomic(sa->adi + ADI_FIFO_STS, sts,
				      sts & ADI_FIFO_EMPTY, 1, 1000))
		dev_warn(sa->dev, "ADI write 0x%03x timed out\n", reg);
	spin_unlock_irqrestore(&sa->adi_lock, flags);
}

static u16 adi_get(struct sc8830_audio *sa, u32 reg)
{
	unsigned long flags;
	u16 val = 0xdead;

	spin_lock_irqsave(&sa->adi_lock, flags);
	adi_read(sa, reg, &val);
	spin_unlock_irqrestore(&sa->adi_lock, flags);
	return val;
}

/* ---- codec power ------------------------------------------------------- */

static bool want_spk(struct sc8830_audio *sa);

static void apply_route(struct sc8830_audio *sa)
{
	u16 on = (sa->mix != MIX_RIGHT ? DACL_AOL : 0) |
		 (sa->mix == MIX_LR || sa->mix == MIX_RIGHT ? DACR_AOL : 0);

	if (sa->mix == MIX_DOWNMIX)
		on = DACL_AOL;
	if (!want_spk(sa))	/* the amp at its lowest gain (AOL_G 0) is still audible */
		on = 0;

	adi_upd(sa, ANA_CDC6, DACL_AOL | DACR_AOL, on);
}

static void apply_vbc(struct sc8830_audio *sa)
{
	upd(sa->vbc, DADGCTL, 0xffff,
	    sa->vbc_dg ? ((0x80 | sa->vbc_dg) << 8) | (0x80 | sa->vbc_dg) : 0x1818 & ~DADG_EN);
	upd(sa->vbc, DAHPCTL, VBDAC_ALC_EN, sa->alc ? VBDAC_ALC_EN : 0);
}

static void set_gains(struct sc8830_audio *sa, int dac, int aol)
{
	adi_upd(sa, ANA_CDC10, DACL_G | DACR_G,
		FIELD_PREP(DACL_G, dac) | FIELD_PREP(DACR_G, dac));
	adi_upd(sa, ANA_CDC11, AOL_G, FIELD_PREP(AOL_G, aol));
}

static bool jack_inserted(struct sc8830_audio *sa)
{
	return sa->jack_raw >= 0 && (!!sa->jack_raw != sa->jack_invert);
}

static bool want_hp(struct sc8830_audio *sa)
{
	return sa->out == OUT_HEADPHONE || sa->out == OUT_BOTH ||
	       (sa->out == OUT_AUTO && jack_inserted(sa));
}

static bool want_spk(struct sc8830_audio *sa)
{
	return sa->spk_on && (sa->out == OUT_SPEAKER || sa->out == OUT_BOTH ||
			      (sa->out == OUT_AUTO && !jack_inserted(sa)));
}

static void apply_speaker_gain(struct sc8830_audio *sa)
{
	bool spk = want_spk(sa);

	/* the DACs also feed the headphones: they stay open while either output is on */
	set_gains(sa, spk || sa->hp_up ? sa->dac_gain : 0,
		  spk ? min(15, sa->spk_gain + sa->boost) : 0);
}

/*
 * Headphone jack: the SC2723S has its own class-G amplifier (ANA_CDC3, CDC12-14, PMU0/1, HDT1).
 * Same order as the vendor's DAPM: bias ramp (HP POP), class-G rail, HPL/HPR switches, DAC into
 * the mixers, gains up one step at a time, then the amplifier outputs.
 */
static void hp_set_gain(struct sc8830_audio *sa, int g)
{
	adi_upd(sa, ANA_CDC12, HPL_G | HPR_G, FIELD_PREP(HPL_G, g) | FIELD_PREP(HPR_G, g));
}

static void hp_pop_wait(struct sc8830_audio *sa, const char *what)
{
	u16 sts = 0;
	int i;

	for (i = 0; i < 60; i++) {
		sts = adi_get(sa, ANA_STS0);
		if (FIELD_GET(HP_POP_FLG, sts) == 3)
			break;
		msleep(10);
	}
	dev_info(sa->dev, "HP pop %s: flag %lu after %d ms (STS0=%04x)\n", what,
		 FIELD_GET(HP_POP_FLG, sts), i * 10, sts);
}

static void hp_on(struct sc8830_audio *sa)
{
	int g;

	if (sa->hp_up)
		return;
	adi_upd(sa, ANA_CDC16, HP_POP_STEP | HP_POP_CTL,
		FIELD_PREP(HP_POP_STEP, 4) | FIELD_PREP(HP_POP_CTL, 1));	/* ramp up */
	hp_pop_wait(sa, "up");
	adi_upd(sa, ANA_CDC16, HP_POP_CTL, FIELD_PREP(HP_POP_CTL, 3));	/* hold */

	/* sprd_inter_headphone_pa(): class-G reference, rail, charge pump */
	adi_upd(sa, ANA_CDC3, CG_LPW | CG_CHP_OSC, FIELD_PREP(CG_LPW, 0) | FIELD_PREP(CG_CHP_OSC, 1));
	adi_upd(sa, ANA_CDC3, CG_REF_EN, CG_REF_EN);
	adi_upd(sa, ANA_PMU0, LDOCG_EN, LDOCG_EN);
	adi_upd(sa, ANA_CDC3, CG_CHP_EN, CG_CHP_EN);
	adi_upd(sa, ANA_PMU1, LDOCG_EN_FAL_ILD | LDOCG_ISET_FAL | LDOCG_RAMP_EN,
		LDOCG_EN_FAL_ILD | LDOCG_ISET_FAL | LDOCG_RAMP_EN);
	adi_upd(sa, ANA_HDT1, HEAD_L_INT_PU_PD, HEAD_L_INT_PU_PD);
	if (sa->chip_aa) {
		adi_upd(sa, DANGL, 0xffff, 0xffff);
		adi_upd(sa, DANGR, 0xffff, 0xffff);
	}
	adi_upd(sa, ANA_CDC13, CG_G_1 | CG_G_2, FIELD_PREP(CG_G_1, 0x10) | FIELD_PREP(CG_G_2, 7));
	adi_upd(sa, ANA_CDC14, CG_G_1 | CG_G_2, FIELD_PREP(CG_G_1, 0x10) | FIELD_PREP(CG_G_2, 7));
	msleep(30);

	/* HPL/HPR switches, differential mode, bias ramp off; then DAC L -> HPL and DAC R -> HPR */
	hp_set_gain(sa, 0);
	adi_upd(sa, ANA_CDC2, HPL_EN | HPR_EN | DIFF_EN, HPL_EN | HPR_EN | DIFF_EN);
	adi_upd(sa, ANA_CDC16, HP_POP_CTL, 0);
	adi_upd(sa, ANA_CDC7, DACL_P_HPL | DACR_P_HPR, DACL_P_HPL | DACR_P_HPR);
	for (g = 1; g <= sa->hp_gain; g++) {
		hp_set_gain(sa, g);
		msleep(4);
	}

	/* sprd_inter_headphone_pa_post(): mode, calibration, outputs */
	adi_upd(sa, ANA_CDC3, CG_HP_MODE, 0);
	adi_upd(sa, ANA_CDC3, CG_HPCAL_EN, CG_HPCAL_EN);
	msleep(10);
	adi_upd(sa, ANA_CDC3, CG_HPL_EN | CG_HPR_EN, CG_HPL_EN | CG_HPR_EN);
	msleep(20);
	if (sa->hp_cg_link)
		adi_upd(sa, ANA_CDC7, HPL_CGL | HPR_CGR, HPL_CGL | HPR_CGR);

	sa->hp_up = true;
	dev_info(sa->dev, "headphone amplifier on, gain %d | PMU0=%04x PMU1=%04x PMU3=%04x CDC2=%04x CDC3=%04x CDC7=%04x CDC12=%04x CDC13=%04x CDC16=%04x HDT0=%04x HDT1=%04x STS0=%04x DANGL=%04x\n",
		 sa->hp_gain, adi_get(sa, ANA_PMU0), adi_get(sa, ANA_PMU1), adi_get(sa, ANA_PMU3),
		 adi_get(sa, ANA_CDC2), adi_get(sa, ANA_CDC3), adi_get(sa, ANA_CDC7),
		 adi_get(sa, ANA_CDC12), adi_get(sa, ANA_CDC13), adi_get(sa, ANA_CDC16),
		 adi_get(sa, ANA_HDT0), adi_get(sa, ANA_HDT1), adi_get(sa, ANA_STS0), adi_get(sa, DANGL));
}

static void hp_off(struct sc8830_audio *sa)
{
	int g;

	if (!sa->hp_up)
		return;
	for (g = sa->hp_gain - 1; g >= 0; g--) {
		hp_set_gain(sa, g);
		msleep(2);
	}
	adi_upd(sa, ANA_CDC7, HPL_CGL | HPR_CGR, 0);
	adi_upd(sa, ANA_CDC3, CG_HPL_EN | CG_HPR_EN, 0);
	adi_upd(sa, ANA_CDC3, CG_HPCAL_EN | CG_HP_MODE, 0);
	adi_upd(sa, ANA_CDC16, HP_POP_STEP | HP_POP_CTL,
		FIELD_PREP(HP_POP_STEP, 3) | FIELD_PREP(HP_POP_CTL, 2));	/* ramp down */
	hp_pop_wait(sa, "down");
	adi_upd(sa, ANA_CDC7, DACL_P_HPL | DACR_P_HPR, 0);
	adi_upd(sa, ANA_CDC2, HPL_EN | HPR_EN | DIFF_EN, 0);
	adi_upd(sa, ANA_CDC16, HP_POP_CTL, 0);
	adi_upd(sa, ANA_CDC3, CG_CHP_EN | CG_REF_EN, 0);
	adi_upd(sa, ANA_PMU0, LDOCG_EN, 0);
	adi_upd(sa, ANA_PMU1, LDOCG_EN_FAL_ILD | LDOCG_ISET_FAL | LDOCG_RAMP_EN, 0);
	if (sa->chip_aa) {
		adi_upd(sa, DANGL, 0xffff, 0);
		adi_upd(sa, DANGR, 0xffff, 0);
	}

	sa->hp_up = false;
	dev_info(sa->dev, "headphone amplifier off\n");
}

/*
 * The headset detector (vendor: headset_sprd_sc2723.c). Its insert line reaches the analog EIC,
 * channel 8 (the DTS's gpio_detect = 312, irq_trigger_level_detect = 1: high = plugged in).
 */
static void jack_eic_enable(struct sc8830_audio *sa)
{
	adi_upd(sa, ANA_GLB_ARM_MODULE_EN, ANA_EIC_EN, ANA_EIC_EN);
	adi_upd(sa, ANA_GLB_RTC_CLK_EN, RTC_EIC_EN, RTC_EIC_EN);
	/* the vendor's debounce for EIC 8 (DBNC_CNT8_VALUE), and the line's data unmasked */
	adi_upd(sa, ANA_EIC_CTRL(EIC_JACK), EIC_DBNC_EN | EIC_DBNC_CNT,
		EIC_DBNC_EN | FIELD_PREP(EIC_DBNC_CNT, 25));
	adi_upd(sa, ANA_EIC_DMSK, BIT(EIC_JACK), BIT(EIC_JACK));
}

/* the detector itself lives in the audio block: set up again each time the codec powers on */
static void jack_enable(struct sc8830_audio *sa)
{
	adi_upd(sa, ANA_GLB_ARM_MODULE_EN, ANA_HDT_EN, ANA_HDT_EN);
	adi_upd(sa, ANA_GLB_ARM_CLK_EN, CLK_AUD_HID_EN | CLK_AUD_HBD_EN,
		CLK_AUD_HID_EN | CLK_AUD_HBD_EN);
	adi_upd(sa, ANA_HDT0, HEAD_INS_PD | HEADDETECT_PD | HEAD_SDET | HEAD_INS_VREF,
		FIELD_PREP(HEAD_SDET, 1) | FIELD_PREP(HEAD_INS_VREF, 3));
	jack_eic_enable(sa);
}

/* returns true if the jack line changed; logs the EIC and the codec's own insert flags too */
static bool jack_poll(struct sc8830_audio *sa)
{
	u16 eic = adi_get(sa, ANA_EIC_DATA);
	u16 sts = sa->powered ? adi_get(sa, ANA_STS0) & (HEAD_INSERT | HEAD_INSERT_2) : sa->jack_sts;
	int raw;

	if (eic == 0xdead)
		return false;
	if ((eic != sa->jack_eic || sts != sa->jack_sts) && sa->jack_logs < 100) {
		sa->jack_logs++;
		dev_info(sa->dev, "jack: EIC data %04x -> %04x, STS0 insert bits %03x -> %03x\n",
			 sa->jack_eic, eic, sa->jack_sts, sts);
	}
	sa->jack_eic = eic;
	sa->jack_sts = sts;
	raw = !!(eic & BIT(EIC_JACK));
	if (raw == sa->jack_raw)
		return false;
	dev_info(sa->dev, "jack: %s (line %d -> %d)\n", raw != sa->jack_invert ? "plugged in" : "out",
		 sa->jack_raw, raw);
	sa->jack_raw = raw;
	return true;
}

/* put the amplifiers where the output setting says; the codec lock is held */
static void apply_outputs(struct sc8830_audio *sa)
{
	if (!sa->powered)
		return;
	if (want_hp(sa))
		hp_on(sa);
	else
		hp_off(sa);
	apply_route(sa);
	apply_speaker_gain(sa);
}

static void jack_work(struct work_struct *work)
{
	struct sc8830_audio *sa = container_of(work, struct sc8830_audio, jack.work);

	mutex_lock(&sa->lock);
	if (jack_poll(sa) && sa->out == OUT_AUTO)
		apply_outputs(sa);
	mutex_unlock(&sa->lock);
	schedule_delayed_work(&sa->jack, msecs_to_jiffies(300));
}

static void codec_power_on(struct sc8830_audio *sa)
{
	/* VBC and codec digital part: ownership (AP, not the modem), clocks, resets */
	upd(sa->aon, AON_VBC_CTRL, AON_VBC_OWNER_MASK, 0);
	upd(sa->aon, AON_AP_WPROT_EN, AON_WPROT_VBC, 0);
	upd(sa->aon, AON_APB_EB0, AON_VBC_EB | AON_AUD_EB | AON_AUDIF_EB,
	    AON_VBC_EB | AON_AUD_EB);
	upd(sa->aon, AON_VBC_CTRL, AON_AUDIF_CKG_AUTO_EN, AON_AUDIF_CKG_AUTO_EN);
	upd(sa->aon, AON_SINDRV_CTRL, AON_SINDRV_ENA, AON_SINDRV_ENA);
	upd(sa->aon, AON_APB_RST0, AON_VBC_SOFT_RST | AON_AUD_SOFT_RST |
	    AON_AUDIF_SOFT_RST, ~0u);
	udelay(10);
	upd(sa->aon, AON_APB_RST0, AON_VBC_SOFT_RST | AON_AUD_SOFT_RST |
	    AON_AUDIF_SOFT_RST, 0);
	upd(sa->pmu, PMU_SLEEP_CTRL, PMU_AP_SLEEP_XTL_ON, PMU_AP_SLEEP_XTL_ON);
	upd(sa->pmu, PMU_MEM_PD_CFG0, PMU_MEM_PD_VBC_AUD, 0);

	/* PMIC: audio block and its clocks */
	adi_upd(sa, ANA_GLB_ARM_MODULE_EN, ANA_AUD_EN, ANA_AUD_EN);
	adi_upd(sa, ANA_GLB_AUD_SLP_CTRL, 0xffff, 0);
	adi_upd(sa, ANA_GLB_ARM_RST, ANA_AUD_RSTS, ANA_AUD_RSTS);
	udelay(10);
	adi_upd(sa, ANA_GLB_ARM_RST, ANA_AUD_RSTS, 0);
	adi_upd(sa, ANA_GLB_ARM_CLK_EN, CLK_AUD_6P5M_EN | CLK_AUDIF_EN,
		CLK_AUD_6P5M_EN | CLK_AUDIF_EN);
	adi_upd(sa, ANA_GLB_AUDIO_CTRL0, CLK_AUD_6P5M_TX_INV_EN,
		CLK_AUD_6P5M_TX_INV_EN);
	adi_upd(sa, ANA_AUDIFA, BIT(4), 0);	/* SC2723S: no ADSYNC reuse */
	adi_upd(sa, ANA_GLB_RTC_CLK_EN, RTC_AUD_EN, RTC_AUD_EN);
	adi_upd(sa, ANA_GLB_XTL_WAIT_CTRL, XTL_EN, XTL_EN);

	/* LDO chain VB > BG > BG_IBIAS > VCOM > VCOM_BUF, and VBO */
	adi_upd(sa, ANA_PMU0, VB_EN, VB_EN);
	adi_upd(sa, ANA_PMU1, BG_EN, BG_EN);
	adi_upd(sa, ANA_PMU1, BG_IBIAS_EN, BG_IBIAS_EN);
	adi_upd(sa, ANA_PMU2, VCM_V, FIELD_PREP(VCM_V, 3));	/* battery >= 3.7 V */
	adi_upd(sa, ANA_PMU1, VCM_EN, VCM_EN);
	adi_upd(sa, ANA_PMU1, VCM_BUF_EN, VCM_BUF_EN);
	adi_upd(sa, ANA_PMU0, VBO_EN, VBO_EN);
	msleep(20);

	/* sprd_codec_analog_open() and sprd_codec_digital_open() */
	adi_upd(sa, ANA_PMU1, SEL_VCMI | VCMI_FAST_EN, SEL_VCMI | VCMI_FAST_EN);
	adi_upd(sa, ANA_CDC2, DACDC_RMV_EN, DACDC_RMV_EN);
	adi_upd(sa, ANA_PMU1, LDOVBO_FAST_EN, LDOVBO_FAST_EN);
	writel(0x500, sa->codec + AUD_SDM_CTL0);
	writel(0x100, sa->codec + AUD_SDM_CTL0);
	upd(sa->codec, AUD_DAC_CTL, DAC_MUTE_EN, 0);
	upd(sa->codec, AUD_DAC_SDM_L, 0xffff, 0x9999);
	upd(sa->codec, AUD_DAC_SDM_H, 0xff, 0x1);
	/*
	 * CONFIG_SPRD_INTERNAL_HEADSET_SUPPORT in sprd_codec_analog_open() and the headset driver's
	 * USE_HEADSET_WATERPROOF probe: the jack's ground switch on, HEAD_L_INT pull-up off
	 */
	adi_upd(sa, ANA_HDT1, GND_DET_PU_PD | HEAD_L_INT_PU_PD, HEAD_L_INT_PU_PD);
	adi_upd(sa, ANA_PMU0, GND_PLGPD_EN, GND_PLGPD_EN);

	/* DAPM order of the vendor driver: clocks, DAC switches, mixer into AOL */
	adi_upd(sa, ANA_CDC0, DAC_CLK_EN, DAC_CLK_EN);
	adi_upd(sa, ANA_CDC0, DRV_CLK_EN, DRV_CLK_EN);
	upd(sa->codec, AUD_TOP_CTL, DAC_EN_L | DAC_EN_R, DAC_EN_L | DAC_EN_R);
	adi_upd(sa, DIG_CFG0, AUDIFA_DACL_EN | AUDIFA_DACR_EN,
		AUDIFA_DACL_EN | AUDIFA_DACR_EN);
	adi_upd(sa, ANA_CDC2, AOL_EN, AOL_EN);
	adi_upd(sa, ANA_PMU0, PA_SW_EN, PA_SW_EN);
	apply_route(sa);
	adi_upd(sa, ANA_CDC2, DACL_EN | DACR_EN, DACL_EN | DACR_EN);
	set_gains(sa, 0, 0);	/* silent while the amp comes up: the vendor raises the gains last */

	/* sprd_inter_speaker_pa(): class AB, supplied straight from VBAT */
	adi_upd(sa, ANA_PMU3, PA_AB_I | PA_LDO_V,
		FIELD_PREP(PA_AB_I, 0) | FIELD_PREP(PA_LDO_V, 7));
	adi_upd(sa, ANA_PMU5, PA_OVP_V, FIELD_PREP(PA_OVP_V, 7));
	adi_upd(sa, ANA_PMU0, OVP_LDO_EN, OVP_LDO_EN);
	/*
	 * Android's "Inter PA Config" 0x130 (/system/etc/tiny_hw.xml): class AB, no LDO, DEMI on,
	 * DTRI_F 3. sprd_codec_pa_demi_en() writes PMU6 0x400 with it, then opens the AOL/AOR OCP.
	 */
	adi_upd(sa, ANA_PMU6, 0xffff, 0x400);
	adi_upd(sa, ANA_PMU6, DRV_OCP_AOL_PD | DRV_OCP_AOR_PD, 0);
	adi_upd(sa, ANA_CDC4, PA_D_EN | PA_DEMI_EN | PA_DTRI_F,
		PA_DEMI_EN | FIELD_PREP(PA_DTRI_F, 3));
	msleep(20);
	adi_upd(sa, ANA_PMU0, PA_EN, PA_EN);

	/*
	 * Soft start against the thump of the first sound after the codec was off: let the amp and
	 * its bias settle, open the DAC, then raise the amp gain a 3 dB step at a time.
	 */
	msleep(30);
	jack_enable(sa);
	msleep(10);
	jack_poll(sa);
	{
		int dac = sa->dac_gain;
		int aol = want_spk(sa) ? min(15, sa->spk_gain + sa->boost) : 0, g;

		set_gains(sa, dac, 0);
		for (g = 1; g <= aol; g++) {
			msleep(4);
			set_gains(sa, dac, g);
		}
	}

	sa->powered = true;
	apply_outputs(sa);
	dev_info(sa->dev, "codec on, output %d, jack flag %d\n", sa->out, sa->jack_raw);
}

static void codec_power_off(struct sc8830_audio *sa)
{
	hp_off(sa);
	set_gains(sa, 0, 0);	/* the vendor drops the gains first, then the amp */
	msleep(5);
	adi_upd(sa, ANA_PMU0, PA_EN, 0);
	msleep(2);
	adi_upd(sa, ANA_PMU0, PA_SW_EN | OVP_LDO_EN, 0);
	adi_upd(sa, ANA_CDC2, DACL_EN | DACR_EN | AOL_EN, 0);
	adi_upd(sa, ANA_CDC6, DACL_AOL | DACR_AOL, 0);
	adi_upd(sa, DIG_CFG0, AUDIFA_DACL_EN | AUDIFA_DACR_EN, 0);
	upd(sa->codec, AUD_TOP_CTL, DAC_EN_L | DAC_EN_R, 0);
	adi_upd(sa, ANA_CDC0, DAC_CLK_EN | DRV_CLK_EN, 0);
	adi_upd(sa, ANA_PMU0, VBO_EN | VB_EN, 0);
	adi_upd(sa, ANA_PMU1, VCM_BUF_EN | VCM_EN | BG_IBIAS_EN | BG_EN, 0);
	adi_upd(sa, ANA_GLB_ARM_CLK_EN, CLK_AUD_6P5M_EN | CLK_AUDIF_EN, 0);
	adi_upd(sa, ANA_GLB_ARM_MODULE_EN, ANA_AUD_EN, 0);
	upd(sa->aon, AON_APB_EB0, AON_VBC_EB | AON_AUD_EB, 0);

	sa->powered = false;
	dev_info(sa->dev, "codec off\n");
}

/*
 * Every 250 ms scan the DMA ring (what dmix has mixed so far) for its peak and for samples at
 * full scale, and every 2 s log the worst case of the window, if there was any sound. Tells if
 * the data is already clipped on its way to the DAC or if the distortion comes later (analog).
 */
static void level_work(struct work_struct *work)
{
	struct sc8830_audio *sa = container_of(work, struct sc8830_audio, level.work);
	struct snd_pcm_substream *ss = sa->substream;
	const s16 *buf;
	unsigned int i, frames;

	if (!ss || !ss->runtime || !ss->runtime->dma_area)
		return;
	buf = (const s16 *)ss->runtime->dma_area;
	frames = ss->runtime->dma_bytes / 4;
	for (i = 0; i < frames; i++) {
		int l = abs(buf[2 * i]), r = abs(buf[2 * i + 1]);

		if (l > sa->lv_peak_l)
			sa->lv_peak_l = l;
		if (r > sa->lv_peak_r)
			sa->lv_peak_r = r;
		if (l >= 32000)
			sa->lv_clip++;
	}
	if (++sa->lv_n % 8 == 0) {
		if (sa->lv_peak_l || sa->lv_peak_r)
			dev_info(sa->dev, "level: peak L=%d (%d%% of full scale) R=%d, left samples >= 32000: %u\n",
				 sa->lv_peak_l, sa->lv_peak_l * 100 / 32768, sa->lv_peak_r, sa->lv_clip);
		sa->lv_peak_l = sa->lv_peak_r = 0;
		sa->lv_clip = 0;
	}
	schedule_delayed_work(&sa->level, msecs_to_jiffies(250));
}

static void power_off_work(struct work_struct *work)
{
	struct sc8830_audio *sa = container_of(work, struct sc8830_audio,
					       power_off.work);

	mutex_lock(&sa->lock);
	if (sa->powered && !sa->substream)
		codec_power_off(sa);
	mutex_unlock(&sa->lock);
}

/* ---- DMA ---------------------------------------------------------------- */

static void __iomem *chn(struct sc8830_audio *sa, int ch, u32 reg)
{
	return sa->dma + DMA_CHN(ch) + reg;
}

/*
 * One node per period: the node's registers from SRC_ADDR on are what the
 * channel loads; LLIST_PTR points at the next node's SRC_ADDR word, and the
 * last node at the first one, so the list never ends.
 */
static void dma_build_list(struct sc8830_audio *sa, int side,
			   struct snd_pcm_runtime *runtime)
{
	u32 period = frames_to_bytes(runtime, runtime->period_size);
	u32 blk = period / 2;			/* bytes of one channel per period */
	unsigned int i, n = runtime->periods;
	u32 *node;

	for (i = 0; i < n; i++) {
		node = sa->nodes[side] + i * DMA_NODE_WORDS;
		memset(node, 0, DMA_NODE_WORDS * 4);
		node[DMA_CHN_CFG / 4] = DMA_PRIO_1 | DMA_LLIST_EN;
		node[4] = runtime->dma_addr + side * 2 + i * period;	/* src */
		node[5] = VBC_PHYS + (side ? VBDA1 : VBDA0);		/* dest */
		node[6] = DMA_FRG_LEN_VBC(sa->fifo);			/* frg_len */
		node[7] = blk;						/* blk_len */
		node[8] = blk;						/* trsc_len */
		node[9] = 4;				/* src step: every other word */
		node[12] = sa->nodes_phys[side] + ((i + 1) % n) * DMA_NODE_WORDS * 4 +
			   DMA_CHN_SRC_ADDR;				/* llist_ptr */
	}
}

static void dma_setup_channel(struct sc8830_audio *sa, int side, bool irq)
{
	int ch = side ? DMA_CH_RIGHT : DMA_CH_LEFT;
	u32 *node0 = sa->nodes[side];
	int i;

	writel(0, chn(sa, ch, DMA_CHN_CFG));
	writel(0, chn(sa, ch, DMA_CHN_PAUSE));
	writel(0, chn(sa, ch, DMA_CHN_REQ));
	for (i = 4; i < DMA_NODE_WORDS; i++)
		writel(0, chn(sa, ch, i * 4));
	writel(node0[4], chn(sa, ch, 0x10));
	writel(node0[5], chn(sa, ch, 0x14));
	writel(sa->nodes_phys[side] + DMA_CHN_SRC_ADDR, chn(sa, ch, 0x30));
	writel(DMA_INT_CLR | (irq ? DMA_INT_BLK_EN | DMA_INT_CFGERR_EN : 0),
	       chn(sa, ch, DMA_CHN_INTC));
	writel(DMA_PRIO_1 | DMA_LLIST_EN, chn(sa, ch, DMA_CHN_CFG));
	writel(ch + 1, sa->dma + DMA_REQ_CID(side ? DMA_UID_VB_DA1 : DMA_UID_VB_DA0));
}

static void dma_stop_channel(struct sc8830_audio *sa, int side)
{
	int ch = side ? DMA_CH_RIGHT : DMA_CH_LEFT;
	u32 v;

	upd(chn(sa, ch, 0), DMA_CHN_PAUSE, DMA_PAUSE, DMA_PAUSE);
	readl_poll_timeout_atomic(chn(sa, ch, DMA_CHN_PAUSE), v, v & DMA_PAUSED, 1, 1000);
	upd(chn(sa, ch, 0), DMA_CHN_CFG, DMA_CHN_EN, 0);
	writel(0, chn(sa, ch, DMA_CHN_PAUSE));
	writel(DMA_INT_CLR, chn(sa, ch, DMA_CHN_INTC));
	writel(0, sa->dma + DMA_REQ_CID(side ? DMA_UID_VB_DA1 : DMA_UID_VB_DA0));
}

static irqreturn_t sc8830_dma_irq(int irq, void *data)
{
	struct sc8830_audio *sa = data;
	u32 sts = readl(sa->dma + DMA_GLB_INT_MSK_STS);
	u32 ours = BIT(DMA_CH_LEFT) | BIT(DMA_CH_RIGHT);

	if (!(sts & ours))
		return IRQ_NONE;
	if (sts & BIT(DMA_CH_LEFT))
		upd(chn(sa, DMA_CH_LEFT, 0), DMA_CHN_INTC, DMA_INT_CLR, DMA_INT_CLR);
	if (sts & BIT(DMA_CH_RIGHT))
		upd(chn(sa, DMA_CH_RIGHT, 0), DMA_CHN_INTC, DMA_INT_CLR, DMA_INT_CLR);

	if ((sts & BIT(DMA_CH_LEFT)) && sa->substream) {
		sa->irqs++;
		if (sa->irqs <= 6 || sa->irqs % 500 == 0) {
			struct snd_pcm_runtime *rt = sa->substream->runtime;

			dev_info_ratelimited(sa->dev, "period %u: hw=%lu appl=%lu\n", sa->irqs,
					     (unsigned long)rt->status->hw_ptr, (unsigned long)rt->control->appl_ptr);
		}
		snd_pcm_period_elapsed(sa->substream);
	}
	return IRQ_HANDLED;
}

/* ---- PCM ---------------------------------------------------------------- */

static const struct snd_pcm_hardware sc8830_pcm_hw = {
	/* MMAP is added in open() unless the "Write Mode" control is on (the downmix needs .copy) */
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000,
	.rate_min = 44100,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = BUFFER_BYTES_MAX,
	.period_bytes_min = 4 * VBC_FIFO_MIN,
	.period_bytes_max = BUFFER_BYTES_MAX / 2,
	.periods_min = 2,
	.periods_max = PERIODS_MAX,
};

static int sc8830_pcm_open(struct snd_pcm_substream *substream)
{
	struct sc8830_audio *sa = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int ret;

	runtime->hw = sc8830_pcm_hw;
	if (!sa->write_mode)
		runtime->hw.info |= SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID;
	/* a period must be whole VBC requests of "fifo" frames */
	runtime->hw.period_bytes_min = 4 * sa->fifo;
	ret = snd_pcm_hw_constraint_step(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
					 4 * sa->fifo);
	if (ret < 0)
		return ret;
	ret = snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		return ret;

	cancel_delayed_work_sync(&sa->power_off);
	mutex_lock(&sa->lock);
	if (!sa->powered)
		codec_power_on(sa);
	sa->substream = substream;
	mutex_unlock(&sa->lock);
	return 0;
}

static int sc8830_pcm_close(struct snd_pcm_substream *substream)
{
	struct sc8830_audio *sa = snd_pcm_substream_chip(substream);

	mutex_lock(&sa->lock);
	sa->substream = NULL;
	mutex_unlock(&sa->lock);
	schedule_delayed_work(&sa->power_off, msecs_to_jiffies(POWER_OFF_DELAY_MS));
	return 0;
}

static int sc8830_pcm_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct sc8830_audio *sa = snd_pcm_substream_chip(substream);


	/* codec DAC rate codes: 2 = 44.1 kHz, 1 = 48 kHz */
	upd(sa->codec, AUD_DAC_CTL, DAC_RATE, params_rate(params) == 44100 ? 2 : 1);
	return 0;
}

/* bring-up aid: what the hardware holds when a stream starts */
static void dump_regs(struct sc8830_audio *sa, struct snd_pcm_runtime *runtime)
{
	dev_info(sa->dev, "%u Hz, period %lu x %u | VBC BUF=%08x DTA=%08x IIS=%08x SWT=%08x DG=%08x HP=%08x ST=%08x/%08x SRC=%08x MIX=%08x\n",
		 runtime->rate, runtime->period_size, runtime->periods,
		 readl(sa->vbc + VBBUFFSIZE), readl(sa->vbc + VBDABUFFDTA),
		 readl(sa->vbc + VBIISSEL), readl(sa->vbc + 0x38),
		 readl(sa->vbc + DADGCTL), readl(sa->vbc + DAHPCTL),
		 readl(sa->vbc + STCTL0), readl(sa->vbc + STCTL1),
		 readl(sa->vbc + DACSRCCTL), readl(sa->vbc + MIXERCTL));
	dev_info(sa->dev, "AUD TOP=%08x CTR=%08x I2S=%08x DAC=%08x | PMU0=%04x PMU1=%04x PMU3=%04x CDC0=%04x CDC2=%04x CDC4=%04x CDC6=%04x CDC10=%04x CDC11=%04x DIG0=%04x\n",
		 readl(sa->codec + AUD_TOP_CTL), readl(sa->codec + AUD_AUD_CTR),
		 readl(sa->codec + AUD_I2S_CTL), readl(sa->codec + AUD_DAC_CTL),
		 adi_get(sa, ANA_PMU0), adi_get(sa, ANA_PMU1), adi_get(sa, ANA_PMU3),
		 adi_get(sa, ANA_CDC0), adi_get(sa, ANA_CDC2), adi_get(sa, ANA_CDC4),
		 adi_get(sa, ANA_CDC6), adi_get(sa, ANA_CDC10), adi_get(sa, ANA_CDC11),
		 adi_get(sa, DIG_CFG0));
}

static int sc8830_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct sc8830_audio *sa = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int side, i;

	upd(sa->vbc, VBDABUFFDTA, VBENABLE | VBDA0DMA_EN | VBDA1DMA_EN, 0);
	upd(sa->vbc, VBCHNEN, VBDA0_EN | VBDA1_EN, 0);
	dma_stop_channel(sa, 0);
	dma_stop_channel(sa, 1);

	upd(sa->vbc, VBBUFFSIZE, VBDABUFFERSIZE,
	    FIELD_PREP(VBDABUFFERSIZE, sa->fifo - 1));
	upd(sa->vbc, VBIISSEL, VBIISSEL_DA_PORT | VBIISSEL_DA_LRCK,
	    VBIISSEL_DA_LRCK);			/* port 0: internal codec */
	/*
	 * As the vendor driver with its default profile: digital gain off, the
	 * ALC limiter on with its default parameters (the EQ stays off: flat)
	 */
	for (i = 0; i < ARRAY_SIZE(vbc_alc_default); i++)
		writel(vbc_alc_default[i], sa->vbc + DAALCCTL0 + 4 * i);
	writel(0x0183, sa->vbc + STCTL0);	/* side tone off */
	writel(0x0183, sa->vbc + STCTL1);
	writel(0, sa->vbc + DACSRCCTL);
	writel(0, sa->vbc + MIXERCTL);
	writel(0, sa->vbc + DAHPCTL);
	apply_vbc(sa);

	for (side = 0; side < 2; side++) {
		dma_build_list(sa, side, runtime);
		dma_setup_channel(sa, side, side == 0);
	}
	sa->irqs = 0;
	dump_regs(sa, runtime);
	return 0;
}

static int sc8830_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct sc8830_audio *sa = snd_pcm_substream_chip(substream);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		upd(chn(sa, DMA_CH_LEFT, 0), DMA_CHN_CFG, DMA_CHN_EN, DMA_CHN_EN);
		upd(chn(sa, DMA_CH_RIGHT, 0), DMA_CHN_CFG, DMA_CHN_EN, DMA_CHN_EN);
		upd(sa->vbc, VBCHNEN, VBDA0_EN | VBDA1_EN, VBDA0_EN | VBDA1_EN);
		upd(sa->vbc, VBDABUFFDTA, VBENABLE | VBDA0DMA_EN | VBDA1DMA_EN,
		    VBENABLE | VBDA0DMA_EN | VBDA1DMA_EN);
		sa->lv_n = 0;
		sa->lv_peak_l = sa->lv_peak_r = 0;
		sa->lv_clip = 0;
		schedule_delayed_work(&sa->level, msecs_to_jiffies(250));
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
		cancel_delayed_work(&sa->level);
		{
			struct snd_pcm_runtime *rt = substream->runtime;

			/* bring-up aid: did this stream ever carry data? (appl moves only if it was written) */
			dev_info_ratelimited(sa->dev, "stop: state=%d periods=%u hw=%lu appl=%lu buffer=%lu period=%lu\n",
					     rt->status->state, sa->irqs, (unsigned long)rt->status->hw_ptr,
					     (unsigned long)rt->control->appl_ptr, rt->buffer_size, rt->period_size);
		}
		upd(sa->vbc, VBDABUFFDTA, VBENABLE | VBDA0DMA_EN | VBDA1DMA_EN, 0);
		upd(sa->vbc, VBCHNEN, VBDA0_EN | VBDA1_EN, 0);
		dma_stop_channel(sa, 0);
		dma_stop_channel(sa, 1);
		dev_dbg(sa->dev, "stop after %u periods\n", sa->irqs);
		return 0;
	default:
		return -EINVAL;
	}
}

static snd_pcm_uframes_t sc8830_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct sc8830_audio *sa = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	u32 src = readl(chn(sa, DMA_CH_LEFT, DMA_CHN_SRC_ADDR));
	u32 off = src - runtime->dma_addr;

	/* the left channel reads one 16-bit word of each 4-byte frame */
	if (off >= runtime->dma_bytes)
		return 0;
	return bytes_to_frames(runtime, off & ~3u);
}

/*
 * The speaker is mono and its amplifier input sums both DACs, which clipped loud stereo music
 * (found by ear: left only was clean). "Downmix" writes (L+R)/2 to both channels and the
 * analog route uses the left DAC only: everything is kept and there is headroom.
 */
static int sc8830_pcm_copy(struct snd_pcm_substream *substream, int channel,
			   unsigned long pos, struct iov_iter *iter, unsigned long bytes)
{
	struct sc8830_audio *sa = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	s16 *dst = (s16 *)(runtime->dma_area + pos);
	unsigned long i, frames = bytes / 4;

	if (copy_from_iter(dst, bytes, iter) != bytes)
		return -EFAULT;
	if (sa->mix == MIX_DOWNMIX) {
		for (i = 0; i < frames; i++) {
			s16 avg = ((int)dst[2 * i] + dst[2 * i + 1]) >> 1;

			dst[2 * i] = avg;
			dst[2 * i + 1] = avg;
		}
	}
	return 0;
}

static const struct snd_pcm_ops sc8830_pcm_ops = {
	.open = sc8830_pcm_open,
	.close = sc8830_pcm_close,
	.hw_params = sc8830_pcm_hw_params,
	.prepare = sc8830_pcm_prepare,
	.trigger = sc8830_pcm_trigger,
	.copy = sc8830_pcm_copy,
	.pointer = sc8830_pcm_pointer,
};

/* ---- controls ----------------------------------------------------------- */

static int spk_vol_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = SPK_GAIN_MAX;
	return 0;
}

static int spk_vol_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct sc8830_audio *sa = snd_kcontrol_chip(kc);

	uc->value.integer.value[0] = sa->spk_gain;
	return 0;
}

static int spk_vol_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct sc8830_audio *sa = snd_kcontrol_chip(kc);
	long v = uc->value.integer.value[0];
	int changed;

	if (v < 0 || v > SPK_GAIN_MAX)
		return -EINVAL;
	mutex_lock(&sa->lock);
	changed = sa->spk_gain != v;
	sa->spk_gain = v;
	if (changed && sa->powered)
		apply_speaker_gain(sa);
	mutex_unlock(&sa->lock);
	return changed;
}

static int spk_sw_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct sc8830_audio *sa = snd_kcontrol_chip(kc);

	uc->value.integer.value[0] = sa->spk_on;
	return 0;
}

static int spk_sw_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct sc8830_audio *sa = snd_kcontrol_chip(kc);
	bool on = !!uc->value.integer.value[0];
	int changed;

	mutex_lock(&sa->lock);
	changed = sa->spk_on != on;
	sa->spk_on = on;
	if (changed && sa->powered) {
		apply_route(sa);
		apply_speaker_gain(sa);
	}
	mutex_unlock(&sa->lock);
	return changed;
}

static int fifo_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = VBC_FIFO_MIN;
	uinfo->value.integer.max = VBC_FIFO_MAX;
	uinfo->value.integer.step = 16;
	return 0;
}

static int fifo_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct sc8830_audio *sa = snd_kcontrol_chip(kc);

	uc->value.integer.value[0] = sa->fifo;
	return 0;
}

/* bring-up aid: frames per request; takes effect the next time the stream opens */
static int fifo_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct sc8830_audio *sa = snd_kcontrol_chip(kc);
	long v = uc->value.integer.value[0];
	int changed;

	if (v < VBC_FIFO_MIN || v > VBC_FIFO_MAX || v % 16)
		return -EINVAL;
	mutex_lock(&sa->lock);
	if (sa->substream) {
		mutex_unlock(&sa->lock);
		return -EBUSY;
	}
	changed = sa->fifo != v;
	sa->fifo = v;
	mutex_unlock(&sa->lock);
	return changed;
}

/* bring-up knobs: integers and one enum, applied at once when the codec is on */
static int mix_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *uinfo)
{
	static const char * const names[] = { "Left+Right", "Left", "Right", "Downmix" };

	return snd_ctl_enum_info(uinfo, 1, ARRAY_SIZE(names), names);
}

static int mix_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	uc->value.enumerated.item[0] = ((struct sc8830_audio *)snd_kcontrol_chip(kc))->mix;
	return 0;
}

static int mix_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct sc8830_audio *sa = snd_kcontrol_chip(kc);
	unsigned int v = uc->value.enumerated.item[0];
	int changed;

	if (v > MIX_DOWNMIX)
		return -EINVAL;
	mutex_lock(&sa->lock);
	changed = sa->mix != v;
	sa->mix = v;
	if (changed && sa->powered)
		apply_route(sa);
	mutex_unlock(&sa->lock);
	return changed;
}

static int write_mode_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	uc->value.integer.value[0] = ((struct sc8830_audio *)snd_kcontrol_chip(kc))->write_mode;
	return 0;
}

static int write_mode_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct sc8830_audio *sa = snd_kcontrol_chip(kc);
	bool on = !!uc->value.integer.value[0];
	int changed;

	mutex_lock(&sa->lock);
	if (sa->substream) {
		mutex_unlock(&sa->lock);
		return -EBUSY;
	}
	changed = sa->write_mode != on;
	sa->write_mode = on;
	mutex_unlock(&sa->lock);
	return changed;
}

static int boost_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 7;
	return 0;
}

static int boost_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	uc->value.integer.value[0] = ((struct sc8830_audio *)snd_kcontrol_chip(kc))->boost;
	return 0;
}

static int boost_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct sc8830_audio *sa = snd_kcontrol_chip(kc);
	long v = uc->value.integer.value[0];
	int changed;

	if (v < 0 || v > 7)
		return -EINVAL;
	mutex_lock(&sa->lock);
	changed = sa->boost != v;
	sa->boost = v;
	if (changed && sa->powered)
		apply_speaker_gain(sa);
	mutex_unlock(&sa->lock);
	return changed;
}

static int dac_gain_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 7;
	return 0;
}

static int dac_gain_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	uc->value.integer.value[0] = ((struct sc8830_audio *)snd_kcontrol_chip(kc))->dac_gain;
	return 0;
}

static int dac_gain_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct sc8830_audio *sa = snd_kcontrol_chip(kc);
	long v = uc->value.integer.value[0];
	int changed;

	if (v < 0 || v > 7)
		return -EINVAL;
	mutex_lock(&sa->lock);
	changed = sa->dac_gain != v;
	sa->dac_gain = v;
	if (changed && sa->powered)
		apply_speaker_gain(sa);
	mutex_unlock(&sa->lock);
	return changed;
}

static int alc_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	uc->value.integer.value[0] = ((struct sc8830_audio *)snd_kcontrol_chip(kc))->alc;
	return 0;
}

static int alc_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct sc8830_audio *sa = snd_kcontrol_chip(kc);
	bool on = !!uc->value.integer.value[0];
	int changed;

	mutex_lock(&sa->lock);
	changed = sa->alc != on;
	sa->alc = on;
	if (changed && sa->powered)
		apply_vbc(sa);
	mutex_unlock(&sa->lock);
	return changed;
}

static int vbc_dg_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 127;
	return 0;
}

static int vbc_dg_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	uc->value.integer.value[0] = ((struct sc8830_audio *)snd_kcontrol_chip(kc))->vbc_dg;
	return 0;
}

static int vbc_dg_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct sc8830_audio *sa = snd_kcontrol_chip(kc);
	long v = uc->value.integer.value[0];
	int changed;

	if (v < 0 || v > 127)
		return -EINVAL;
	mutex_lock(&sa->lock);
	changed = sa->vbc_dg != v;
	sa->vbc_dg = v;
	if (changed && sa->powered)
		apply_vbc(sa);
	mutex_unlock(&sa->lock);
	return changed;
}

/*
 * Headphone jack controls. Not named "Output Select", "Headphone ..." or "Headphone Jack": PipeWire's
 * alsa-card-profile drives controls with those names (its speaker path sets the headphone volume to
 * its minimum) and left the jack silent.
 */
static int out_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *uinfo)
{
	static const char * const names[] = { "Speaker", "Headphone", "Both", "Auto" };

	return snd_ctl_enum_info(uinfo, 1, ARRAY_SIZE(names), names);
}

static int out_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	uc->value.enumerated.item[0] = ((struct sc8830_audio *)snd_kcontrol_chip(kc))->out;
	return 0;
}

static int out_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct sc8830_audio *sa = snd_kcontrol_chip(kc);
	unsigned int v = uc->value.enumerated.item[0];
	int changed;

	if (v > OUT_AUTO)
		return -EINVAL;
	mutex_lock(&sa->lock);
	changed = sa->out != v;
	sa->out = v;
	if (changed)
		apply_outputs(sa);
	mutex_unlock(&sa->lock);
	return changed;
}

static int hp_vol_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 15;
	return 0;
}

static int hp_vol_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	uc->value.integer.value[0] = ((struct sc8830_audio *)snd_kcontrol_chip(kc))->hp_gain;
	return 0;
}

static int hp_vol_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct sc8830_audio *sa = snd_kcontrol_chip(kc);
	long v = uc->value.integer.value[0];
	int changed;

	if (v < 0 || v > 15)
		return -EINVAL;
	mutex_lock(&sa->lock);
	changed = sa->hp_gain != v;
	sa->hp_gain = v;
	if (changed && sa->hp_up)
		hp_set_gain(sa, v);
	mutex_unlock(&sa->lock);
	return changed;
}

static int hp_cg_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	uc->value.integer.value[0] = ((struct sc8830_audio *)snd_kcontrol_chip(kc))->hp_cg_link;
	return 0;
}

static int hp_cg_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct sc8830_audio *sa = snd_kcontrol_chip(kc);
	bool on = !!uc->value.integer.value[0];
	int changed;

	mutex_lock(&sa->lock);
	changed = sa->hp_cg_link != on;
	sa->hp_cg_link = on;
	if (changed && sa->hp_up)
		adi_upd(sa, ANA_CDC7, HPL_CGL | HPR_CGR, on ? HPL_CGL | HPR_CGR : 0);
	mutex_unlock(&sa->lock);
	return changed;
}

static int jack_inv_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	uc->value.integer.value[0] = ((struct sc8830_audio *)snd_kcontrol_chip(kc))->jack_invert;
	return 0;
}

static int jack_inv_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct sc8830_audio *sa = snd_kcontrol_chip(kc);
	bool on = !!uc->value.integer.value[0];
	int changed;

	mutex_lock(&sa->lock);
	changed = sa->jack_invert != on;
	sa->jack_invert = on;
	if (changed)
		apply_outputs(sa);
	mutex_unlock(&sa->lock);
	return changed;
}

static int jack_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	uc->value.integer.value[0] = ((struct sc8830_audio *)snd_kcontrol_chip(kc))->jack_raw > 0;
	return 0;
}

static const DECLARE_TLV_DB_SCALE(spk_tlv, -2400, 300, 0);

static const struct snd_kcontrol_new sc8830_controls[] = {
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Speaker Playback Volume",
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |
			  SNDRV_CTL_ELEM_ACCESS_TLV_READ,
		.tlv.p = spk_tlv,
		.info = spk_vol_info,
		.get = spk_vol_get,
		.put = spk_vol_put,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Speaker Mix",
		.info = mix_info,
		.get = mix_get,
		.put = mix_put,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Write Mode",
		.info = snd_ctl_boolean_mono_info,
		.get = write_mode_get,
		.put = write_mode_put,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Speaker Boost",
		.info = boost_info,
		.get = boost_get,
		.put = boost_put,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "DAC Gain",
		.info = dac_gain_info,
		.get = dac_gain_get,
		.put = dac_gain_put,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "ALC Switch",
		.info = snd_ctl_boolean_mono_info,
		.get = alc_get,
		.put = alc_put,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "VBC Digital Gain",
		.info = vbc_dg_info,
		.get = vbc_dg_get,
		.put = vbc_dg_put,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "FIFO Frames",
		.info = fifo_info,
		.get = fifo_get,
		.put = fifo_put,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Speaker Playback Switch",
		.info = snd_ctl_boolean_mono_info,
		.get = spk_sw_get,
		.put = spk_sw_put,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Aux Output",
		.info = out_info,
		.get = out_get,
		.put = out_put,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Aux Gain",
		.info = hp_vol_info,
		.get = hp_vol_get,
		.put = hp_vol_put,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Aux CG Link",
		.info = snd_ctl_boolean_mono_info,
		.get = hp_cg_get,
		.put = hp_cg_put,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Aux Jack Invert",
		.info = snd_ctl_boolean_mono_info,
		.get = jack_inv_get,
		.put = jack_inv_put,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Aux Jack Flag",
		.access = SNDRV_CTL_ELEM_ACCESS_READ,
		.info = snd_ctl_boolean_mono_info,
		.get = jack_get,
	},
};

/*
 * Bring-up: /sys/devices/platform/soc/.../40020000.audio/adi. Reading dumps the codec's analog
 * registers, the PMIC globals the codec uses and the EIC; writing "offset mask value" (hex)
 * updates one PMIC register, so a script can try register settings without a new kernel.
 */
static const u16 adi_dump_regs[] = {
	ANA_EIC_DATA, ANA_EIC_DMSK, ANA_EIC_CTRL(EIC_JACK),
	ANA_GLB_ARM_MODULE_EN, ANA_GLB_ARM_CLK_EN, ANA_GLB_RTC_CLK_EN, ANA_GLB_AUDIO_CTRL0,
	ANA_GLB_AUDIO_CTRL1, ANA_GLB_CHIP_ID_LOW, ANA_GLB_CHIP_ID_HIGH,
};

static ssize_t adi_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct sc8830_audio *sa = dev_get_drvdata(dev);
	int len = 0, i;
	u16 reg;

	mutex_lock(&sa->lock);
	len += sysfs_emit_at(buf, len, "powered %d hp_up %d out %d jack %d\n", sa->powered,
			     sa->hp_up, sa->out, sa->jack_raw);
	for (reg = ANA_PMU0; reg <= DIG_CFG0 + 0x18; reg += 4)
		len += sysfs_emit_at(buf, len, "%03x=%04x%c", reg, adi_get(sa, reg),
				     (reg - ANA_PMU0) % 32 == 28 ? '\n' : ' ');
	len += sysfs_emit_at(buf, len, "\n");
	for (i = 0; i < ARRAY_SIZE(adi_dump_regs); i++)
		len += sysfs_emit_at(buf, len, "%03x=%04x ", adi_dump_regs[i],
				     adi_get(sa, adi_dump_regs[i]));
	mutex_unlock(&sa->lock);
	len += sysfs_emit_at(buf, len, "\n");
	return len;
}

static ssize_t adi_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct sc8830_audio *sa = dev_get_drvdata(dev);
	unsigned int reg, mask, val;

	if (sscanf(buf, "%x %x %x", &reg, &mask, &val) != 3 || reg >= 0x1000 || reg % 4 ||
	    mask > 0xffff || val > 0xffff)
		return -EINVAL;
	mutex_lock(&sa->lock);
	adi_upd(sa, reg, mask, val);
	dev_info(dev, "adi: %03x mask %04x <- %04x, now %04x\n", reg, mask, val, adi_get(sa, reg));
	mutex_unlock(&sa->lock);
	return count;
}
static DEVICE_ATTR_RW(adi);

static struct attribute *sc8830_attrs[] = {
	&dev_attr_adi.attr,
	NULL
};

static const struct attribute_group sc8830_attr_group = {
	.attrs = sc8830_attrs,
};

/* ---- probe -------------------------------------------------------------- */

static void __iomem *map(struct platform_device *pdev, const char *name,
			 phys_addr_t *phys)
{
	struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!res)
		return IOMEM_ERR_PTR(-EINVAL);
	if (phys)
		*phys = res->start;
	/* not exclusive: the PMIC, timer, SMP and GPU code map some of these too */
	return devm_ioremap(&pdev->dev, res->start, resource_size(res)) ?:
	       IOMEM_ERR_PTR(-ENOMEM);
}

static int sc8830_audio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sc8830_audio *sa;
	struct snd_card *card;
	struct snd_pcm *pcm;
	u32 cfg0, bits;
	int irq, ret, i;

	ret = snd_devm_card_new(dev, SNDRV_DEFAULT_IDX1, "SC7730", THIS_MODULE,
				sizeof(*sa), &card);
	if (ret)
		return ret;
	sa = card->private_data;
	sa->dev = dev;
	sa->card = card;
	spin_lock_init(&sa->adi_lock);
	mutex_init(&sa->lock);
	INIT_DELAYED_WORK(&sa->power_off, power_off_work);
	INIT_DELAYED_WORK(&sa->level, level_work);
	INIT_DELAYED_WORK(&sa->jack, jack_work);
	platform_set_drvdata(pdev, sa);
	sa->spk_gain = SPK_GAIN_DEFAULT;
	sa->fifo = VBC_FIFO_FRAMES;
	sa->spk_on = true;
	/*
	 * Tuned by ear. The vendor's ALC limiter is off. The speaker amp input sums both DACs and clipped
	 * stereo, so only the left DAC is routed. DAC at 0 dB; the amp at 0 dB too (boost 0): +6 dB was
	 * clean with the synthetic test music but real music distorted, +12 dB distorted even that.
	 * The stereo downmix (MIX_DOWNMIX with write_mode on: no mmap, .copy mixes) worked with aplay but
	 * left Batocera silent through PipeWire, so it is off by default.
	 */
	sa->dac_gain = 7;
	sa->alc = false;
	sa->boost = 0;
	sa->mix = MIX_LEFT;
	sa->write_mode = false;
	/* both outputs until the insert flag's polarity is confirmed on the hardware */
	sa->out = OUT_BOTH;
	sa->hp_gain = 6;	/* the vendor's Headset volume levels run from 4 to 11 */
	sa->jack_raw = -1;

	sa->vbc = map(pdev, "vbc", NULL);
	sa->codec = map(pdev, "codec", NULL);
	sa->aon = map(pdev, "aon", NULL);
	sa->pmu = map(pdev, "pmu", NULL);
	sa->adi = map(pdev, "adi", NULL);
	sa->pmic = map(pdev, "pmic", &sa->pmic_phys);
	sa->dma = map(pdev, "dma", NULL);
	sa->ahb = map(pdev, "ahb", NULL);
	if (IS_ERR(sa->vbc) || IS_ERR(sa->codec) || IS_ERR(sa->aon) ||
	    IS_ERR(sa->pmu) || IS_ERR(sa->adi) || IS_ERR(sa->pmic) ||
	    IS_ERR(sa->dma) || IS_ERR(sa->ahb))
		return -ENOMEM;

	cfg0 = readl(sa->adi + ADI_GSSI_CFG0);
	bits = (cfg0 & 0x3f) - ((cfg0 >> 11) & 0x1f) - 1;
	if (bits > 13)
		bits = 9;
	sa->addr_mask = GENMASK(bits + 1, 0);

	{
		u16 hi = adi_get(sa, ANA_GLB_CHIP_ID_HIGH), lo = adi_get(sa, ANA_GLB_CHIP_ID_LOW);

		/* the vendor's sci_get_ana_chip_ver() == AUDIO_2723_VER_AA */
		sa->chip_aa = hi == 0x2723 && !(lo & 0xfff);
		dev_info(dev, "PMIC chip id %04x%04x, version %03x%s\n", hi, lo, lo & 0xfff,
			 sa->chip_aa ? " (AA: DANGL/DANGR with the headphone amp)" : "");
	}

	/* the AP DMA must be clocked before any of its registers is touched */
	dev_info(dev, "AP DMA clock was %s\n",
		 readl(sa->ahb + AHB_EB) & AHB_DMA_EB ? "on" : "off");
	upd(sa->ahb, AHB_EB, AHB_DMA_EB, AHB_DMA_EB);

	for (i = 0; i < 2; i++) {
		sa->nodes[i] = dmam_alloc_coherent(dev, PERIODS_MAX * DMA_NODE_WORDS * 4,
						   &sa->nodes_phys[i], GFP_KERNEL);
		if (!sa->nodes[i])
			return -ENOMEM;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	ret = devm_request_irq(dev, irq, sc8830_dma_irq, 0, "sc8830-audio", sa);
	if (ret)
		return ret;

	strscpy(card->driver, "SC8830", sizeof(card->driver));
	strscpy(card->shortname, "SC7730 audio", sizeof(card->shortname));
	strscpy(card->longname, "Spreadtrum SC7730 VBC + SC2723S codec, speaker",
		sizeof(card->longname));

	ret = snd_pcm_new(card, "SC2723S", 0, 1, 0, &pcm);
	if (ret)
		return ret;
	pcm->private_data = sa;
	strscpy(pcm->name, "SC2723S speaker", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &sc8830_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_DEV, dev,
				       BUFFER_BYTES_MAX, BUFFER_BYTES_MAX);

	for (i = 0; i < ARRAY_SIZE(sc8830_controls); i++) {
		ret = snd_ctl_add(card, snd_ctl_new1(&sc8830_controls[i], sa));
		if (ret)
			return ret;
	}

	ret = snd_card_register(card);
	if (ret)
		return ret;
	ret = devm_device_add_group(dev, &sc8830_attr_group);
	if (ret)
		dev_warn(dev, "no adi file in sysfs: %d\n", ret);

	mutex_lock(&sa->lock);
	jack_eic_enable(sa);
	mutex_unlock(&sa->lock);
	schedule_delayed_work(&sa->jack, msecs_to_jiffies(500));
	dev_info(dev, "card registered\n");
	return 0;
}

static const struct of_device_id sc8830_audio_of_match[] = {
	{ .compatible = "sprd,sc8830-audio" },
	{ }
};
MODULE_DEVICE_TABLE(of, sc8830_audio_of_match);

static struct platform_driver sc8830_audio_driver = {
	.probe = sc8830_audio_probe,
	.driver = {
		.name = "sc8830-audio",
		.of_match_table = sc8830_audio_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(sc8830_audio_driver);

MODULE_DESCRIPTION("Spreadtrum SC8830 VBC and SC2723S codec speaker playback");
MODULE_LICENSE("GPL");
