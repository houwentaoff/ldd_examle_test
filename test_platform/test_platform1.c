/*
 * =====================================================================================
 *       Copyright (c), 2013-2020, xxx.
 *       Filename:  test_platform1.c
 *
 *    Description:  测试在bsp中和在当前驱动中直接注册platform device
 *                  2中platform方法
 *         Others:  相关的api:
 *                  platform_driver_register platform_driver_unregister
 *                  platform_get_drvdata platform_set_drvdata
 *                  和dts的关系:
 *                  1. dts被解析时自动识别成一个platform 
 *                  2. 在probe中解析dts中描述的管脚，时钟等相关信息并进行初始化处理
 *                  3. 在硬件发生变化时，只需要改变不同的dts文件,并不用修改驱动代码
 *
 *        Version:  1.0
 *        Created:  Sunday, August 14, 2016 10:48:35 CST
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  Joy. Hou (hwt), 544088192@qq.com
 *   Organization:  xxx
 *
 * =====================================================================================
 */
#if 0

/*-----------------------------------------------------------------------------
 *  bsp中的定义如下
 *-----------------------------------------------------------------------------*/
static struct platform_device *smdkc210_devices[] __initdata = {
    &s3c_device_hsmmc0,
    &s3c_device_hsmmc1,
    &s3c_device_hsmmc2,
    &s3c_device_hsmmc3,
    &s3c_device_i2c1,
    &s3c_device_rtc,
    &s3c_device_wdt,
    &exynos4_device_ac97,
    &exynos4_device_i2s0,
    &exynos4_device_pd[PD_MFC],
    &exynos4_device_pd[PD_G3D],
    &exynos4_device_pd[PD_LCD0],
    &exynos4_device_pd[PD_LCD1],
    &exynos4_device_pd[PD_CAM],
    &exynos4_device_pd[PD_TV],
    &exynos4_device_pd[PD_GPS],
    &exynos4_device_sysmmu,
    &samsung_asoc_dma,
    &smdkc210_smsc911x,
};
/*  */
platform_add_devices(smdkc210_devices, ARRAY_SIZE(smdkc210_devices));
#endif

#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/reboot.h>
#include <linux/workqueue.h>   
/**
 * @brief platform device的私有数据
 */
struct test_data {
    const char * const str;
};
struct test_data test_platform_data = {
    .str  = "this is test platform data!"
};
/**
 * @brief 不加会导致rmsmod时候内核报错
 *
 * @param dev
 */
static void platform_test_release(struct device * dev)
{
    return ;
}
struct platform_device test_device = {
    .name  = "test platform",
    .dev   = {
        .release       = platform_test_release,
        .platform_data = &test_platform_data,
    }
};

static int dm9000_drv_remove(struct platform_device *pdev)
{
    struct test_data *pdata = pdev->dev.platform_data;// platform的void data
    char *rbuff = platform_get_drvdata(pdev);

    printk("==>%s data[%s]\n", __func__, pdata->str);
    
    printk("<==%s rbuff[%s]\n", __func__, rbuff);
    if (rbuff) {
        kfree(rbuff);
    }
    platform_set_drvdata(pdev, NULL);
    return 0;
}
static struct work_struct work;   
static void work_func(struct work_struct *work)
{                                                                                                                                                               
    printk("work queue wake  exec!!!\n");
    kernel_restart(NULL); //是可以重启的, 只要不放在init- probe-等函数中就行
    return ;
}

static int dm90001_probe(struct platform_device *pdev)
{
    int ret = 0;
    struct test_data *pdata = pdev->dev.platform_data;
    char * rbuff = NULL;
    INIT_WORK(&work, work_func);  
    printk("==>%s data[%s]\n", __func__, pdata->str);
    printk("111111111111\n");
    schedule_work(&work); 
//    kernel_restart(NULL); //该语句阻塞, 目前不会重启设备.(原因为　该API会调用每个设备的shutdown ,而insmod未结束会卡住这里，无法调用该去掉的shutdown?)
    printk("2222222222222\n");

    rbuff = kmalloc(100, GFP_KERNEL);
    if (rbuff == NULL ) {
        ret = -ENOBUFS;
        goto out_free;
    }
    platform_set_drvdata(pdev, rbuff);
    strcpy(rbuff, "just test set/get drv data!!");

    printk("<==%s data[%s]rbuff[%s]\n", __func__, pdata->str,
            rbuff);

    return ret;
out_free:
    kfree(rbuff);    
    return ret;
}
static struct platform_driver test_platform_driver = {
    .driver= {
        .name  = "test platform",
        .owner = THIS_MODULE,
//        .pm    = &dm9000_drv_pm_ops,
    },
    .probe   = dm90001_probe,
    .remove  = dm9000_drv_remove,
};
static int test_platform_device_init(void)
{
    return platform_device_register(&test_device);
}
static void test_platform_device_exit(void)
{
    platform_device_unregister(&test_device);
}
static int __init test_platform_init(void)
{
    printk(KERN_INFO "==>%s init\n", __func__);
    test_platform_device_init();
    return platform_driver_register(&test_platform_driver);
}

static void __exit test_platform_exit(void)
{
    test_platform_device_exit();
    platform_driver_unregister(&test_platform_driver);//该函数会触发dm9000_drv_remove
    printk(KERN_INFO "<==%s exit\n", __func__);
}

module_init(test_platform_init);
module_exit(test_platform_exit);
MODULE_AUTHOR("Joy.Hou Chengdu China");
MODULE_DESCRIPTION("test platform driver");
MODULE_LICENSE("GPL");
