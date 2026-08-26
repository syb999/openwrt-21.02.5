// SPDX-License-Identifier: GPL-2.0

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#define AR934X_DMA_BASE              0x180a0000
#define AR934X_DMA_SIZE              0x6c
#define AR934X_STEREO_BASE           0x180b0000
#define AR934X_STEREO_SIZE           0x18
#define AR934X_RESET_REG             0x1806001c

#define AR934X_STEREO_REG_CONFIG        0x00
#define AR934X_STEREO_CONFIG_I2S_ENABLE     BIT(21)
#define AR934X_STEREO_CONFIG_SPDIF_ENABLE   BIT(23)
#define AR934X_STEREO_CONFIG_RESET          BIT(19)
#define AR934X_STEREO_CONFIG_I2S_DELAY      BIT(18)
#define AR934X_STEREO_CONFIG_DATA_WORD_SIZE_SHIFT 12
#define AR934X_STEREO_CONFIG_DATA_WORD_SIZE_MASK 0x03
#define AR934X_STEREO_CONFIG_DATA_WORD_16    1
#define AR934X_STEREO_CONFIG_SAMPLE_CNT_CLEAR_TYPE BIT(9)
#define AR934X_STEREO_CONFIG_MASTER          BIT(8)
#define AR934X_STEREO_CONFIG_POSEDGE_MASK    0xff

#define AR934X_STEREO_REG_VOLUME        0x04

#define AR934X_DMA_REG_MBOX_DMA_POLICY         0x10
#define AR934X_DMA_MBOX_DMA_POLICY_TX_FIFO_THRESH_SHIFT 4
#define AR934X_DMA_MBOX_DMA_POLICY_RX_QUANTUM  BIT(1)

#define AR934X_DMA_REG_MBOX0_DMA_RX_DESCRIPTOR_BASE 0x18
#define AR934X_DMA_REG_MBOX0_DMA_RX_CONTROL    0x1c

#define AR934X_DMA_MBOX_DMA_CONTROL_START      BIT(1)
#define AR934X_DMA_MBOX_DMA_CONTROL_STOP       BIT(0)

#define AR934X_DMA_REG_MBOX_INT_STATUS         0x44
#define AR934X_DMA_REG_MBOX_INT_ENABLE         0x4c
#define AR934X_DMA_MBOX0_INT_RX_COMPLETE       BIT(10)

#define AR934X_DMA_REG_MBOX_FIFO_RESET         0x58
#define AR934X_DMA_MBOX0_FIFO_RESET_RX         BIT(2)

#define AR934X_RESET_MBOX              BIT(1)
#define AR934X_RESET_I2S               BIT(0)

#define AR934X_PLL_BASE                0x18050000
#define AR934X_PLL_AUDIO_CONFIG_REG    0x30
#define AR934X_PLL_AUDIO_MOD_REG       0x34
#define AR934X_PLL_CONFIG_EXT_DIV_SHIFT    12
#define AR934X_PLL_CONFIG_POSTPLLPWD_SHIFT 7
#define AR934X_PLL_CONFIG_PLLPWD       BIT(5)
#define AR934X_PLL_CONFIG_BYPASS       BIT(4)
#define AR934X_PLL_CONFIG_REFDIV_MASK  0xf
#define AR934X_PLL_MOD_TGT_DIV_INT_SHIFT  1
#define AR934X_PLL_MOD_TGT_DIV_INT_MASK  0x3f
#define AR934X_PLL_MOD_TGT_DIV_FRAC_SHIFT 11
#define AR934X_PLL_MOD_TGT_DIV_FRAC_MASK 0x3ffff

#define AR934X_AUD_DPLL_BASE           0x18116200
#define AR934X_DPLL_REG_2              0x04
#define AR934X_DPLL_2_RANGE            BIT(31)
#define AR934X_DPLL_2_KI_SHIFT         26
#define AR934X_DPLL_2_KI_MASK          0xf
#define AR934X_DPLL_2_KD_SHIFT         19
#define AR934X_DPLL_2_KD_MASK          0x7f
#define AR934X_DPLL_REG_3              0x08
#define AR934X_DPLL_3_PHASESH_SHIFT    23
#define AR934X_DPLL_3_PHASESH_MASK     0x7f
#define AR934X_DPLL_3_DO_MEAS          BIT(30)
#define AR934X_DPLL_3_SQSUM_DVC_SHIFT  3
#define AR934X_DPLL_3_SQSUM_DVC_MASK   0xfffff
#define AR934X_DPLL_REG_4              0x0c
#define AR934X_DPLL_4_MEAS_DONE        BIT(3)

#define AR934X_PLL_CFG_12288  ((6 << 12) | (3 << 7) | 1)
#define AR934X_PLL_MOD_12288  ((0x24F76 << 11) | (0x17 << 1))
#define AR934X_PLL_CFG_11289  ((6 << 12) | (3 << 7) | 1)
#define AR934X_PLL_MOD_11289  ((0x2B442 << 11) | (0x15 << 1))

struct ar934x_pcm_desc {
    unsigned int own:1;
    unsigned int eom:1;
    unsigned int rsvd1:6;
    unsigned int size:12;
    unsigned int length:12;
    unsigned int rsvd2:4;
    unsigned int buf_ptr:28;
    unsigned int rsvd3:4;
    unsigned int next_ptr:28;
    unsigned int rsvd4:4;
    unsigned int vuc[36];
    struct list_head list;
    dma_addr_t phys;
};

struct ar934x_runtime {
    struct list_head descs;
    struct ar934x_pcm_desc *last_played;
    unsigned int elapsed;
    unsigned int period_bytes;
    unsigned int stop_delay_ms;
};

struct ar934x_i2s {
    struct device *dev;
    void __iomem *stereo;
    void __iomem *dma;
    void __iomem *reset;
    void __iomem *pll;
    void __iomem *dpll;
    int irq;
    struct dma_pool *desc_pool;
    struct snd_pcm_substream *playback;
    unsigned int mclk_rate;
};

static struct ar934x_i2s *global_i2s;

static inline void stereo_writel(struct ar934x_i2s *i2s, u32 reg, u32 val)
{
    writel(val, i2s->stereo + reg);
}

static inline u32 stereo_readl(struct ar934x_i2s *i2s, u32 reg)
{
    return readl(i2s->stereo + reg);
}

static inline void dma_writel(struct ar934x_i2s *i2s, u32 reg, u32 val)
{
    writel(val, i2s->dma + reg);
}

static inline u32 dma_readl(struct ar934x_i2s *i2s, u32 reg)
{
    return readl(i2s->dma + reg);
}

static void ar934x_mbox_reset(struct ar934x_i2s *i2s)
{
    u32 val;

    val = readl(i2s->reset);
    val |= AR934X_RESET_MBOX;
    writel(val, i2s->reset);
    udelay(100);
    val &= ~AR934X_RESET_MBOX;
    writel(val, i2s->reset);
    udelay(100);

    writel(0xFFFFFFFF, i2s->dma + AR934X_DMA_REG_MBOX_INT_STATUS);
    udelay(50);

    writel(AR934X_DMA_MBOX0_FIFO_RESET_RX,
           i2s->dma + AR934X_DMA_REG_MBOX_FIFO_RESET);
    udelay(50);
    writel(0, i2s->dma + AR934X_DMA_REG_MBOX_FIFO_RESET);
    udelay(50);

    writel(AR934X_DMA_MBOX0_INT_RX_COMPLETE,
           i2s->dma + AR934X_DMA_REG_MBOX_INT_ENABLE);
}

static void ar934x_stereo_reset(struct ar934x_i2s *i2s)
{
    u32 val;

    val = stereo_readl(i2s, AR934X_STEREO_REG_CONFIG);
    val |= AR934X_STEREO_CONFIG_RESET;
    stereo_writel(i2s, AR934X_STEREO_REG_CONFIG, val);
    udelay(50);
    val &= ~AR934X_STEREO_CONFIG_RESET;
    stereo_writel(i2s, AR934X_STEREO_REG_CONFIG, val);
    udelay(50);
}

static void ar934x_module_reset(struct ar934x_i2s *i2s)
{
    u32 val;

    val = readl(i2s->reset);
    val |= AR934X_RESET_I2S;
    writel(val, i2s->reset);
    udelay(100);
    val &= ~AR934X_RESET_I2S;
    writel(val, i2s->reset);
    udelay(100);
}

static void ar934x_free_descriptors(struct ar934x_i2s *i2s,
                                    struct ar934x_runtime *rt)
{
    struct ar934x_pcm_desc *desc, *tmp;
    list_for_each_entry_safe(desc, tmp, &rt->descs, list) {
        list_del(&desc->list);
        dma_pool_free(i2s->desc_pool, desc, desc->phys);
    }
}

static int ar934x_map_descriptors(struct ar934x_i2s *i2s,
                                  struct snd_pcm_substream *substream,
                                  struct snd_pcm_hw_params *params)
{
    struct ar934x_runtime *rt = substream->runtime->private_data;
    struct list_head *head = &rt->descs;
    struct ar934x_pcm_desc *desc, *prev;
    dma_addr_t desc_phys;
    dma_addr_t base = substream->runtime->dma_addr;
    unsigned int period = params_period_bytes(params);
    unsigned int total = params_buffer_bytes(params);
    unsigned int offset = 0;

    ar934x_free_descriptors(i2s, rt);
    rt->period_bytes = period;

    while (offset < total) {
        desc = dma_pool_zalloc(i2s->desc_pool, GFP_KERNEL, &desc_phys);
        if (!desc)
            return -ENOMEM;

        desc->phys = desc_phys;
        desc->own = 1;
        desc->eom = 0;
        desc->size = min(period, total - offset);
        desc->length = desc->size;
        desc->buf_ptr = base + offset;
        desc->next_ptr = 0;

        if (offset == 0) {
                                }

        list_add_tail(&desc->list, head);
        if (desc->list.prev != head) {
            prev = list_prev_entry(desc, list);
            prev->next_ptr = desc->phys;
        }
        offset += desc->size;
    }

    desc = list_first_entry(head, struct ar934x_pcm_desc, list);
    prev = list_last_entry(head, struct ar934x_pcm_desc, list);
    prev->next_ptr = desc->phys;

    return 0;
}

static unsigned int ar934x_reown_descs(struct ar934x_i2s *i2s,
                                       struct ar934x_runtime *rt)
{
    struct ar934x_pcm_desc *desc;
    unsigned int bytes = 0;

    list_for_each_entry(desc, &rt->descs, list) {
        if (!desc->own) {
            desc->own = 1;
            bytes += desc->size;
        }
    }
    return bytes;
}

static struct ar934x_pcm_desc *ar934x_get_last_played(struct ar934x_runtime *rt)
{
    struct ar934x_pcm_desc *desc, *prev;

    prev = list_entry(rt->descs.prev, struct ar934x_pcm_desc, list);
    list_for_each_entry(desc, &rt->descs, list) {
        if (desc->own == 1 && prev->own == 0)
            return desc;
        prev = desc;
    }
    return NULL;
}

static irqreturn_t ar934x_i2s_irq(int irq, void *data)
{
    struct ar934x_i2s *i2s = data;
    struct snd_pcm_substream *substream = i2s->playback;
    struct ar934x_runtime *rt;
    unsigned int period_bytes, played;
    u32 status;

    status = dma_readl(i2s, AR934X_DMA_REG_MBOX_INT_STATUS);

    if (status & AR934X_DMA_MBOX0_INT_RX_COMPLETE)

    writel(status, i2s->dma + AR934X_DMA_REG_MBOX_INT_STATUS);

    if (!(status & AR934X_DMA_MBOX0_INT_RX_COMPLETE))
        return IRQ_NONE;

    if (!substream || !substream->runtime)
        return IRQ_HANDLED;

    rt = substream->runtime->private_data;
    period_bytes = rt->period_bytes;

    rt->last_played = ar934x_get_last_played(rt);
    played = ar934x_reown_descs(i2s, rt);
    rt->elapsed += played;

    if (rt->elapsed >= period_bytes) {
        rt->elapsed %= period_bytes;
        snd_pcm_period_elapsed(substream);
    }

    return IRQ_HANDLED;
}

static const struct snd_pcm_hardware ar934x_pcm_hardware = {
    .info = SNDRV_PCM_INFO_MMAP |
            SNDRV_PCM_INFO_MMAP_VALID |
            SNDRV_PCM_INFO_INTERLEAVED |
            SNDRV_PCM_INFO_BLOCK_TRANSFER,
    .formats = SNDRV_PCM_FMTBIT_S16_BE,
    .rates = SNDRV_PCM_RATE_48000,
    .rate_min = 48000,
    .rate_max = 48000,
    .channels_min = 2,
    .channels_max = 2,
    .buffer_bytes_max = 65536,
    .period_bytes_min = 64,
    .period_bytes_max = 4095,
    .periods_min = 16,
    .periods_max = 256,
};

static int ar934x_pcm_open(struct snd_pcm_substream *substream)
{
    struct ar934x_runtime *rt;

    rt = kzalloc(sizeof(*rt), GFP_KERNEL);
    if (!rt)
        return -ENOMEM;

    INIT_LIST_HEAD(&rt->descs);
    substream->runtime->private_data = rt;
    snd_soc_set_runtime_hwparams(substream, &ar934x_pcm_hardware);

    return 0;
}

static int ar934x_pcm_close(struct snd_pcm_substream *substream)
{
    struct ar934x_runtime *rt = substream->runtime->private_data;

    if (rt) {
        ar934x_free_descriptors(global_i2s, rt);
        kfree(rt);
    }
    return 0;
}

static int ar934x_pcm_hw_params(struct snd_pcm_substream *substream,
                                struct snd_pcm_hw_params *params)
{
    struct ar934x_i2s *i2s = global_i2s;
    int ret;

    ret = snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(params));
    if (ret < 0)
        return ret;

    ret = ar934x_map_descriptors(i2s, substream, params);
    if (ret)
        return ret;

    return 0;
}

static int ar934x_pcm_hw_free(struct snd_pcm_substream *substream)
{
    struct ar934x_i2s *i2s = global_i2s;
    struct ar934x_runtime *rt = substream->runtime->private_data;

    ar934x_free_descriptors(i2s, rt);
    return snd_pcm_lib_free_pages(substream);
}

static int ar934x_pcm_prepare(struct snd_pcm_substream *substream)
{
    struct ar934x_i2s *i2s = global_i2s;
    struct ar934x_runtime *rt = substream->runtime->private_data;
    struct ar934x_pcm_desc *first;
    u32 val;

    rt->elapsed = 0;

    ar934x_mbox_reset(i2s);

    val = dma_readl(i2s, AR934X_DMA_REG_MBOX_DMA_POLICY);
    val |= AR934X_DMA_MBOX_DMA_POLICY_RX_QUANTUM |
           (6 << AR934X_DMA_MBOX_DMA_POLICY_TX_FIFO_THRESH_SHIFT);
    dma_writel(i2s, AR934X_DMA_REG_MBOX_DMA_POLICY, val);

    first = list_first_entry(&rt->descs, struct ar934x_pcm_desc, list);
    dma_writel(i2s, AR934X_DMA_REG_MBOX0_DMA_RX_DESCRIPTOR_BASE,
               first->phys);

    dma_writel(i2s, AR934X_DMA_REG_MBOX_INT_ENABLE,
               AR934X_DMA_MBOX0_INT_RX_COMPLETE);

    return 0;
}

static int ar934x_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
    struct ar934x_i2s *i2s = global_i2s;

    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:

        dma_writel(i2s, AR934X_DMA_REG_MBOX0_DMA_RX_CONTROL,
                   AR934X_DMA_MBOX_DMA_CONTROL_START);
        dma_readl(i2s, AR934X_DMA_REG_MBOX0_DMA_RX_CONTROL);

                break;

    case SNDRV_PCM_TRIGGER_STOP:
                dma_writel(i2s, AR934X_DMA_REG_MBOX0_DMA_RX_CONTROL,
                   AR934X_DMA_MBOX_DMA_CONTROL_STOP);
        break;

    default:
        return -EINVAL;
    }

    return 0;
}

static snd_pcm_uframes_t ar934x_pcm_pointer(struct snd_pcm_substream *substream)
{
    struct ar934x_runtime *rt = substream->runtime->private_data;

    if (rt->last_played == NULL)
        return 0;

    return bytes_to_frames(substream->runtime,
                           rt->last_played->buf_ptr -
                           substream->runtime->dma_addr);
}

static const struct snd_pcm_ops ar934x_pcm_ops = {
    .open = ar934x_pcm_open,
    .close = ar934x_pcm_close,
    .ioctl = snd_pcm_lib_ioctl,
    .hw_params = ar934x_pcm_hw_params,
    .hw_free = ar934x_pcm_hw_free,
    .prepare = ar934x_pcm_prepare,
    .trigger = ar934x_pcm_trigger,
    .pointer = ar934x_pcm_pointer,
    .mmap = snd_pcm_lib_default_mmap,
};

static int ar934x_i2s_startup(struct snd_pcm_substream *substream,
                              struct snd_soc_dai *dai)
{
    struct ar934x_i2s *i2s = snd_soc_dai_get_drvdata(dai);
    u32 config;

    i2s->playback = substream;

    config = AR934X_STEREO_CONFIG_SPDIF_ENABLE |
             AR934X_STEREO_CONFIG_I2S_ENABLE |
             AR934X_STEREO_CONFIG_MASTER |
             AR934X_STEREO_CONFIG_SAMPLE_CNT_CLEAR_TYPE;

    stereo_writel(i2s, AR934X_STEREO_REG_CONFIG, config);
    ar934x_stereo_reset(i2s);
    stereo_writel(i2s, AR934X_STEREO_REG_CONFIG, config);

    return 0;
}

static void ar934x_i2s_shutdown(struct snd_pcm_substream *substream,
                                struct snd_soc_dai *dai)
{
    struct ar934x_i2s *i2s = snd_soc_dai_get_drvdata(dai);
    i2s->playback = NULL;
}

static int ar934x_i2s_hw_params(struct snd_pcm_substream *substream,
                                struct snd_pcm_hw_params *params,
                                struct snd_soc_dai *dai)
{
    struct ar934x_i2s *i2s = snd_soc_dai_get_drvdata(dai);
    u32 config;
    u32 rate = params_rate(params);
    u32 mclk, posedge;

    switch (rate) {
    case 48000:
        mclk = 12288000;
        break;
    case 44100:
    case 22050:
        mclk = 11289600;
        break;
    default:
        dev_err(i2s->dev, "Unsupported rate %u\n", rate);
        return -EINVAL;
    }

    posedge = mclk / (rate * 128);
    if (posedge < 1 || posedge > 255) {
        dev_err(i2s->dev, "posedge out of range: %d\n", posedge);
        return -EINVAL;
    }

    config = stereo_readl(i2s, AR934X_STEREO_REG_CONFIG);
    config &= ~(AR934X_STEREO_CONFIG_DATA_WORD_SIZE_MASK <<
                AR934X_STEREO_CONFIG_DATA_WORD_SIZE_SHIFT);
    config &= ~AR934X_STEREO_CONFIG_POSEDGE_MASK;

    config |= AR934X_STEREO_CONFIG_DATA_WORD_16 <<
              AR934X_STEREO_CONFIG_DATA_WORD_SIZE_SHIFT;
    config |= posedge & AR934X_STEREO_CONFIG_POSEDGE_MASK;
    config |= AR934X_STEREO_CONFIG_I2S_ENABLE;
    config |= AR934X_STEREO_CONFIG_MASTER;

    stereo_writel(i2s, AR934X_STEREO_REG_CONFIG, config);
    ar934x_stereo_reset(i2s);
    stereo_writel(i2s, AR934X_STEREO_REG_CONFIG, config);

    ar934x_stereo_reset(i2s);

    return 0;
}

static int ar934x_i2s_set_sysclk(struct snd_soc_dai *dai, int clk_id,
                                 unsigned int freq, int dir)
{
    struct ar934x_i2s *i2s = snd_soc_dai_get_drvdata(dai);
    u32 config, mod;

    if (dir == SND_SOC_CLOCK_IN)
        return 0;

    i2s->mclk_rate = freq;

    switch (freq) {
    case 12288000:
        config = AR934X_PLL_CFG_12288;
        mod    = AR934X_PLL_MOD_12288;
        break;
    case 11289600:
        config = AR934X_PLL_CFG_11289;
        mod    = AR934X_PLL_MOD_11289;
        break;
    default:
        dev_warn(i2s->dev, "Unsupported MCLK %u Hz\n", freq);
        return -EINVAL;
    }

    {
        u32 t;

        t = readl(i2s->pll + AR934X_PLL_AUDIO_CONFIG_REG);
        t |= AR934X_PLL_CONFIG_PLLPWD;

        t &= ~BIT(31);
        writel(t, i2s->pll + AR934X_PLL_AUDIO_CONFIG_REG);
        udelay(100);

        t = readl(i2s->pll + AR934X_PLL_AUDIO_CONFIG_REG);
        t &= ~((0x7 << 12) | (0x7 << 7) | (0xf << 0));
        t |= config;
        t &= ~AR934X_PLL_CONFIG_PLLPWD;
        t &= ~AR934X_PLL_CONFIG_BYPASS;
        writel(t, i2s->pll + AR934X_PLL_AUDIO_CONFIG_REG);

        writel(mod, i2s->pll + AR934X_PLL_AUDIO_MOD_REG);

        t = readl(i2s->dpll + AR934X_DPLL_REG_2);
        t |= AR934X_DPLL_2_RANGE;
        t &= ~(AR934X_DPLL_2_KI_MASK << AR934X_DPLL_2_KI_SHIFT);
        t &= ~(AR934X_DPLL_2_KD_MASK << AR934X_DPLL_2_KD_SHIFT);
        t |= (4 << AR934X_DPLL_2_KI_SHIFT);
        t |= (0x3d << AR934X_DPLL_2_KD_SHIFT);
        writel(t, i2s->dpll + AR934X_DPLL_REG_2);

        t = readl(i2s->dpll + AR934X_DPLL_REG_3);
        t &= ~(AR934X_DPLL_3_PHASESH_MASK << AR934X_DPLL_3_PHASESH_SHIFT);
        t |= (6 << AR934X_DPLL_3_PHASESH_SHIFT);
        writel(t, i2s->dpll + AR934X_DPLL_REG_3);
        udelay(100);
    }

    {
        u32 dpll3, dpll4, sqsum;
        int tries = 0;

        do {
            u32 t;

            sqsum = 0xffffffff;
            dpll4 = 0;

            t = readl(i2s->dpll + AR934X_DPLL_REG_3);
            t &= ~AR934X_DPLL_3_DO_MEAS;
            writel(t, i2s->dpll + AR934X_DPLL_REG_3);

            t = readl(i2s->pll + AR934X_PLL_AUDIO_CONFIG_REG);
            t |= AR934X_PLL_CONFIG_PLLPWD;

        t &= ~BIT(31);
            writel(t, i2s->pll + AR934X_PLL_AUDIO_CONFIG_REG);
            udelay(100);

            t = readl(i2s->pll + AR934X_PLL_AUDIO_CONFIG_REG);
            t &= ~((0x7 << 12) | (0x7 << 7) | (0xf << 0));
            t |= config;
            t &= ~AR934X_PLL_CONFIG_PLLPWD;
            t &= ~AR934X_PLL_CONFIG_BYPASS;
            writel(t, i2s->pll + AR934X_PLL_AUDIO_CONFIG_REG);

            writel(mod, i2s->pll + AR934X_PLL_AUDIO_MOD_REG);

            t = readl(i2s->dpll + AR934X_DPLL_REG_2);
            t |= AR934X_DPLL_2_RANGE;
            t &= ~(AR934X_DPLL_2_KI_MASK << AR934X_DPLL_2_KI_SHIFT);
            t &= ~(AR934X_DPLL_2_KD_MASK << AR934X_DPLL_2_KD_SHIFT);
            t |= (4 << AR934X_DPLL_2_KI_SHIFT);
            t |= (0x3d << AR934X_DPLL_2_KD_SHIFT);
            writel(t, i2s->dpll + AR934X_DPLL_REG_2);

            t = readl(i2s->dpll + AR934X_DPLL_REG_3);
            t &= ~(AR934X_DPLL_3_PHASESH_MASK << AR934X_DPLL_3_PHASESH_SHIFT);
            t |= (6 << AR934X_DPLL_3_PHASESH_SHIFT);
            writel(t, i2s->dpll + AR934X_DPLL_REG_3);
            udelay(100);

            t = readl(i2s->dpll + AR934X_DPLL_REG_3);
            t &= ~AR934X_DPLL_3_DO_MEAS;
            writel(t, i2s->dpll + AR934X_DPLL_REG_3);

            dpll3 = readl(i2s->dpll + AR934X_DPLL_REG_3);
            writel(dpll3 | AR934X_DPLL_3_DO_MEAS,
                   i2s->dpll + AR934X_DPLL_REG_3);
            {
                int cnt = 0;
                while (cnt++ < 1000) {
                    dpll4 = readl(i2s->dpll + AR934X_DPLL_REG_4);
                    if (dpll4 & AR934X_DPLL_4_MEAS_DONE)
                        break;
                    udelay(10);
                }
            }
            sqsum = (readl(i2s->dpll + AR934X_DPLL_REG_3) >>
                     AR934X_DPLL_3_SQSUM_DVC_SHIFT) &
                    AR934X_DPLL_3_SQSUM_DVC_MASK;
        } while (++tries < 100 && sqsum >= 0x40000);

        dev_info(i2s->dev, "Audio PLL meas: tries=%d MEAS_DONE=%s SQSUM=0x%x (%s)\n",
                 tries, (dpll4 & AR934X_DPLL_4_MEAS_DONE) ? "yes" : "NO",
                 sqsum, sqsum < 0x40000 ? "LOCKED" : "NOT-LOCKED");
        dev_info(i2s->dev, "Audio PLL regs: CFG=0x%08x MOD=0x%08x DPLL2=0x%08x DPLL3=0x%08x DPLL4=0x%08x\n",
                 readl(i2s->pll + AR934X_PLL_AUDIO_CONFIG_REG),
                 readl(i2s->pll + AR934X_PLL_AUDIO_MOD_REG),
                 readl(i2s->dpll + AR934X_DPLL_REG_2),
                 readl(i2s->dpll + AR934X_DPLL_REG_3),
                 readl(i2s->dpll + AR934X_DPLL_REG_4));
    }

        return 0;
}

static const struct snd_soc_dai_ops ar934x_i2s_dai_ops = {
    .startup = ar934x_i2s_startup,
    .shutdown = ar934x_i2s_shutdown,
    .hw_params = ar934x_i2s_hw_params,
    .set_sysclk = ar934x_i2s_set_sysclk,
};

static struct snd_soc_dai_driver ar934x_i2s_dai = {
    .name = "qca-ar934x-i2s",
    .playback = {

        .stream_name = "Playback",
        .channels_min = 2,
        .channels_max = 2,
        .rates = SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_44100 |
                 SNDRV_PCM_RATE_22050,
        .formats = SNDRV_PCM_FMTBIT_S16_BE,
    },
    .ops = &ar934x_i2s_dai_ops,
};

static int ar934x_pcm_new(struct snd_soc_pcm_runtime *rtd)
{

    snd_pcm_lib_preallocate_pages_for_all(rtd->pcm,
            SNDRV_DMA_TYPE_DEV,
            rtd->card->dev,
            ar934x_pcm_hardware.buffer_bytes_max,
            ar934x_pcm_hardware.buffer_bytes_max);
    return 0;
}

static const struct snd_soc_component_driver ar934x_i2s_component = {
    .name = "qca-ar934x-i2s",
    .ops = &ar934x_pcm_ops,
    .pcm_new = ar934x_pcm_new,
};

static int ar934x_i2s_probe(struct platform_device *pdev)
{
    struct ar934x_i2s *i2s;
    int ret;

    dev_info(&pdev->dev, "probe: starting\n");

    i2s = devm_kzalloc(&pdev->dev, sizeof(*i2s), GFP_KERNEL);
    if (!i2s)
        return -ENOMEM;

    i2s->dev = &pdev->dev;
    global_i2s = i2s;

    i2s->stereo = devm_ioremap(&pdev->dev, AR934X_STEREO_BASE,
                               AR934X_STEREO_SIZE);
    if (!i2s->stereo) {
        dev_err(&pdev->dev, "Failed to map stereo registers\n");
        return -ENOMEM;
    }

    i2s->dma = devm_ioremap(&pdev->dev, AR934X_DMA_BASE,
                            AR934X_DMA_SIZE);
    if (!i2s->dma) {
        dev_err(&pdev->dev, "Failed to map DMA registers\n");
        return -ENOMEM;
    }

    i2s->reset = devm_ioremap(&pdev->dev, AR934X_RESET_REG, 4);
    if (!i2s->reset) {
        dev_err(&pdev->dev, "Failed to map reset registers\n");
        return -ENOMEM;
    }

    i2s->pll = devm_ioremap(&pdev->dev, AR934X_PLL_BASE, 0x4c);
    if (!i2s->pll) {
        dev_err(&pdev->dev, "Failed to map PLL registers\n");
        return -ENOMEM;
    }

    i2s->dpll = devm_ioremap(&pdev->dev, AR934X_AUD_DPLL_BASE, 0x10);
    if (!i2s->dpll) {
        dev_err(&pdev->dev, "Failed to map audio DPLL registers\n");
        return -ENOMEM;
    }

    i2s->desc_pool = dma_pool_create("ar934x-i2s-desc", &pdev->dev,
                                     sizeof(struct ar934x_pcm_desc),
                                     4, 0);
    if (!i2s->desc_pool) {
        dev_err(&pdev->dev, "Failed to create DMA pool\n");
        return -ENOMEM;
    }

    ar934x_module_reset(i2s);

    i2s->irq = platform_get_irq(pdev, 0);
    if (i2s->irq < 0) {
        dev_err(&pdev->dev, "No IRQ in DT: %d\n", i2s->irq);
        ret = i2s->irq;
        goto err_pool;
    }
    ret = request_irq(i2s->irq, ar934x_i2s_irq, IRQF_SHARED,
                      "qca-ar934x-i2s", i2s);
    if (ret) {
        dev_err(&pdev->dev, "Failed to request IRQ %d: %d\n",
                i2s->irq, ret);
        goto err_pool;
    }

    platform_set_drvdata(pdev, i2s);

    ret = devm_snd_soc_register_component(&pdev->dev,
                                          &ar934x_i2s_component,
                                          &ar934x_i2s_dai, 1);
    if (ret < 0) {
        dev_err(&pdev->dev, "Failed to register component: %d\n", ret);
        goto err_irq;
    }

    dev_info(&pdev->dev, "AR934x I2S registered\n");
    return 0;

err_irq:
    free_irq(i2s->irq, i2s);
err_pool:
    dma_pool_destroy(i2s->desc_pool);
    return ret;
}

static int ar934x_i2s_remove(struct platform_device *pdev)
{
    struct ar934x_i2s *i2s = platform_get_drvdata(pdev);

    if (i2s) {
        free_irq(i2s->irq, i2s);
        dma_pool_destroy(i2s->desc_pool);
        global_i2s = NULL;
    }
    return 0;
}

static const struct of_device_id ar934x_i2s_of_match[] = {
    { .compatible = "qca,ar934x-i2s" },
    { }
};
MODULE_DEVICE_TABLE(of, ar934x_i2s_of_match);

static struct platform_driver ar934x_i2s_driver = {
    .probe = ar934x_i2s_probe,
    .remove = ar934x_i2s_remove,
    .driver = {
        .name = "qca-ar934x-i2s",
        .of_match_table = ar934x_i2s_of_match,
    },
};

module_platform_driver(ar934x_i2s_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("QCA AR934x I2S Driver");
MODULE_ALIAS("platform:qca-ar934x-i2s");
