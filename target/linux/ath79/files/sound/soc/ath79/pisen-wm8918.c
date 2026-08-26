// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <sound/core.h>
#include <sound/soc.h>
#include <sound/pcm_params.h>
#include <sound/wm8904.h>

static struct clk *mclk;
static struct clk_lookup *mclk_lookup;
static struct platform_device *card_pdev;

static int pisen_register_mclk(void)
{
    mclk = clk_register_fixed_rate(NULL, "wm8918_mclk", NULL, 0, 12288000);
    if (IS_ERR(mclk)) {
        pr_err("pisen-wm8918: failed to register MCLK\n");
        return PTR_ERR(mclk);
    }

    mclk_lookup = clkdev_create(mclk, "mclk", NULL);
    if (!mclk_lookup) {
        clk_unregister_fixed_rate(mclk);
        mclk = NULL;
        return -ENOMEM;
    }

    pr_info("pisen-wm8918: MCLK registered (12.288MHz)\n");
    return 0;
}

static void pisen_unregister_mclk(void)
{
    if (mclk_lookup) {
        clkdev_drop(mclk_lookup);
        mclk_lookup = NULL;
    }
    if (mclk) {
        clk_unregister_fixed_rate(mclk);
        mclk = NULL;
    }
}

static int pisen_wm8918_hw_params(struct snd_pcm_substream *substream,
                                  struct snd_pcm_hw_params *params)
{
    struct snd_soc_pcm_runtime *rtd = substream->private_data;
    unsigned int mclk_rate;
    int ret;

    switch (params_rate(params)) {
    case 48000:
        mclk_rate = 12288000;
        break;
    case 44100:
    case 22050:
        mclk_rate = 11289600;
        break;
    default:
        pr_err("pisen-wm8918: unsupported rate %d\n", params_rate(params));
        return -EINVAL;
    }

    ret = snd_soc_dai_set_sysclk(rtd->cpu_dai, 0, mclk_rate,
                                 SND_SOC_CLOCK_OUT);
    if (ret < 0) {
        pr_err("cpu_dai set_sysclk failed: %d\n", ret);
        return ret;
    }

    msleep(50);

    ret = snd_soc_dai_set_sysclk(rtd->codec_dai, 1 ,
                                 mclk_rate, SND_SOC_CLOCK_IN);
    if (ret < 0) {
        pr_err("codec_dai set_sysclk(MCLK) failed: %d\n", ret);
        return ret;
    }

    return 0;
}

static int pisen_wm8918_codec_init(struct snd_soc_pcm_runtime *rtd)
{
    struct snd_soc_component *component = rtd->codec_dai->component;
    int ret;

    pr_info("pisen-wm8918: codec init\n");

    ret = snd_soc_dai_set_fmt(rtd->codec_dai,
                              SND_SOC_DAIFMT_I2S |
                              SND_SOC_DAIFMT_NB_NF |
                              SND_SOC_DAIFMT_CBS_CFS);
    if (ret < 0) {
        pr_err("Failed to set codec DAI format: %d\n", ret);
        return ret;
    }

    snd_soc_component_write(component, 0x1E, 0x30);
    snd_soc_component_write(component, 0x1F, 0x30);

    pr_info("pisen-wm8918: codec initialized in slave mode\n");
    return 0;
}

static const struct snd_soc_dapm_route pisen_wm8918_routes[] = {
    { "AIFINL", NULL, "Playback" },
    { "AIFINR", NULL, "Playback" },
};

static const struct snd_soc_ops pisen_wm8918_ops = {
    .hw_params = pisen_wm8918_hw_params,
};

static struct snd_soc_dai_link_component pisen_wm8918_cpu[] = {
    { .dai_name = "qca-ar934x-i2s" }
};

static struct snd_soc_dai_link_component pisen_wm8918_codec[] = {
    { .dai_name = "wm8904-hifi" }
};

static struct snd_soc_dai_link_component pisen_wm8918_platform[] = {
    { }
};

static struct snd_soc_dai_link pisen_wm8918_link = {
    .name = "WM8918",
    .stream_name = "Playback",
    .cpus = pisen_wm8918_cpu,
    .num_cpus = ARRAY_SIZE(pisen_wm8918_cpu),
    .codecs = pisen_wm8918_codec,
    .num_codecs = ARRAY_SIZE(pisen_wm8918_codec),
    .platforms = pisen_wm8918_platform,
    .num_platforms = ARRAY_SIZE(pisen_wm8918_platform),
    .init = pisen_wm8918_codec_init,
    .ops = &pisen_wm8918_ops,
    .dai_fmt = SND_SOC_DAIFMT_I2S |
               SND_SOC_DAIFMT_NB_NF |
               SND_SOC_DAIFMT_CBS_CFS,
};

static struct snd_soc_card pisen_wm8918_card = {
    .name = "pisen-wm8918",
    .owner = THIS_MODULE,
    .dai_link = &pisen_wm8918_link,
    .num_links = 1,
    .dapm_routes = pisen_wm8918_routes,
    .num_dapm_routes = ARRAY_SIZE(pisen_wm8918_routes),
};

static int pisen_wm8918_card_probe(struct platform_device *pdev)
{
    struct device_node *i2s_np, *codec_np;
    int ret;

    dev_info(&pdev->dev, "probing card\n");

    i2s_np = of_find_compatible_node(NULL, NULL, "qca,ar934x-i2s");
    codec_np = of_find_compatible_node(NULL, NULL, "wlf,wm8904");
    if (!i2s_np || !codec_np) {
        dev_err(&pdev->dev, "DT audio nodes not found (i2s=%p, codec=%p)\n",
                i2s_np, codec_np);
        return -ENODEV;
    }

    pisen_wm8918_link.cpus->of_node = i2s_np;
    pisen_wm8918_link.platforms->of_node = i2s_np;
    pisen_wm8918_link.codecs->of_node = codec_np;

    pisen_wm8918_card.dev = &pdev->dev;

    ret = snd_soc_register_card(&pisen_wm8918_card);
    if (ret) {
        dev_err(&pdev->dev, "snd_soc_register_card failed: %d\n", ret);
        return ret;
    }

    dev_info(&pdev->dev, "card registered\n");
    return 0;
}

static int pisen_wm8918_card_remove(struct platform_device *pdev)
{
    snd_soc_unregister_card(&pisen_wm8918_card);
    return 0;
}

static struct platform_driver pisen_wm8918_driver = {
    .probe = pisen_wm8918_card_probe,
    .remove = pisen_wm8918_card_remove,
    .driver = {
        .name = "pisen-wm8918",
    },
};

static int __init pisen_wm8918_module_init(void)
{
    int ret;

    pr_info("pisen-wm8918: loading\n");

    ret = pisen_register_mclk();
    if (ret)
        return ret;

    card_pdev = platform_device_register_simple("pisen-wm8918", -1, NULL, 0);
    if (IS_ERR(card_pdev)) {
        ret = PTR_ERR(card_pdev);
        pr_err("pisen-wm8918: failed to register card device\n");
        goto err_mclk;
    }

    ret = platform_driver_register(&pisen_wm8918_driver);
    if (ret) {
        platform_device_unregister(card_pdev);
        card_pdev = NULL;
        pr_err("pisen-wm8918: failed to register card driver\n");
        goto err_mclk;
    }

    pr_info("pisen-wm8918: initialized\n");
    return 0;

err_mclk:
    pisen_unregister_mclk();
    return ret;
}

static void __exit pisen_wm8918_module_exit(void)
{
    pr_info("pisen-wm8918: unloading\n");

    if (card_pdev)
        platform_device_unregister(card_pdev);

    platform_driver_unregister(&pisen_wm8918_driver);
    pisen_unregister_mclk();
}

module_init(pisen_wm8918_module_init);
module_exit(pisen_wm8918_module_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PISEN WMB001N WM8918 ASoC machine driver");
MODULE_ALIAS("platform:pisen-wm8918");
