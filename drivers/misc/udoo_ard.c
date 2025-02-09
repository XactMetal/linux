/*
 * udoo_ard.c
 * UDOO quad/dual Arduino flash erase / CPU resetter
 *
 * Copyright (C) 2013-2015 Aidilab srl
 * Author: UDOO Team <social@udoo.org>
 * Author: Giuseppe Pagano <giuseppe.pagano@seco.com>
 * Author: Francesco Montefoschi <francesco.monte@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/errno.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/kernel.h>
#include <linux/workqueue.h>
#include <linux/fs.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/uaccess.h>

#define DRIVER_NAME              "udoo_ard"
#define PINCTRL_DEFAULT          "default"
#define AUTH_TOKEN               0x5A5A
#define MAX_MSEC_SINCE_LAST_IRQ  400
#define GRAY_TIME_BETWEEN_RESET  10000 // In this time we can't accept new erase/reset code

static struct workqueue_struct *erase_reset_wq;
typedef struct {
    struct work_struct erase_reset_work;
    struct pinctrl *pinctrl;
    struct pinctrl_state *pins_default;
    int    step;
    int    cmdcode;
    int    erase_reset_lock;
    int    gpio_bossac_clk;
    int    gpio_bossac_dat;
    int    gpio_ard_erase;
    int    gpio_ard_reset;
    unsigned long    last_int_time_in_ns;
    unsigned long    last_int_time_in_sec;
} erase_reset_work_t;

erase_reset_work_t *work;
static u32 origTX, origRX; // original UART4 TX/RX pad control registers
static int major; // for /dev/udoo_ard
static struct class *udoo_class;

static struct platform_device_id udoo_ard_devtype[] = {
    {
        /* keep it for coldfire */
        .name = DRIVER_NAME,
        .driver_data = 0,
    }, {
        /* sentinel */
    }
};
MODULE_DEVICE_TABLE(platform, udoo_ard_devtype);

static const struct of_device_id udoo_ard_dt_ids[] = {
    { .compatible = "udoo,imx6q-udoo-ard", .data = &udoo_ard_devtype[0], },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, udoo_ard_dt_ids);

static void disable_serial(void)
{
    u32 addrTX;
    void __iomem *_addrTX;

    printk("[bossac] Disable UART4 serial port.\n");

    addrTX = 0x20E01F8;
    _addrTX = ioremap(addrTX, 8);

    origTX = __raw_readl(_addrTX);
    origRX = __raw_readl(_addrTX + 0x4);

    __raw_writel(0x15, _addrTX);
    __raw_writel(0x15, _addrTX + 0x4);

    iounmap(_addrTX);
}

static void enable_serial(void)
{
    u32 addrTX;
    void __iomem *_addrTX;

    printk("[bossac] Enable UART4 serial port.\n");

    addrTX = 0x20E01F8;
    _addrTX = ioremap(addrTX, 8);

    __raw_writel(origTX, _addrTX);
    __raw_writel(origRX, _addrTX + 0x4);

    iounmap(_addrTX);
}

static void erase_reset(void)
{
    printk("[bossac] UDOO ERASE and RESET on Sam3x started.\n");

    gpio_direction_input(work->gpio_ard_erase);
    gpio_set_value(work->gpio_ard_reset, 1);
    msleep(1);

    gpio_direction_output(work->gpio_ard_erase, 1);
    msleep(300);
    gpio_direction_input(work->gpio_ard_erase);

    msleep(10);
    gpio_set_value(work->gpio_ard_reset, 0);

    msleep(80);
    gpio_set_value(work->gpio_ard_reset, 1);

    printk("[bossac] UDOO ERASE and RESET on Sam3x EXECUTED.\n");
}

static void shutdown_sam3x(void)
{
    printk("[bossac] RESET on Sam3x.\n");

    gpio_set_value(work->gpio_ard_reset, 0);
}

static void erase_reset_wq_function( struct work_struct *work2)
{
    disable_serial();
    erase_reset();
    msleep(GRAY_TIME_BETWEEN_RESET);

    work->erase_reset_lock = 0;
}

/*
 * Called everytime the gpio_bossac_clk signal toggles.
 * If the auth token (16 bit) is found, we look for the command code (4 bit).
 * The code 0x0F is sent by Bossac to trigger an erase/reset (to achieve this,
 * erase_reset_wq is scheduled). Before starting to program the flash, we disable
 * the UART4 serial port, otherwise there is too noise on the serial lines (the
 * programming port and UART4 port are connected together, see hw schematics).
 * When Bossac finishes to flash/verify, the code 0x00 is sent which re-enables
 * the UART4 port.
 */
static irqreturn_t udoo_bossac_req(int irq, void *dev_id)
{
    int retval, auth_bit, expected_bit, msec_since_last_irq;
    u64 nowsec;
    unsigned long rem_nsec;
    erase_reset_work_t *erase_reset_work;

    auth_bit = 0;
    if (gpio_get_value(work->gpio_bossac_dat) != 0x0) {
        auth_bit = 1;
    }

    erase_reset_work = (erase_reset_work_t *)work;

    nowsec = local_clock();
    rem_nsec = do_div(nowsec, 1000000000) ;
    msec_since_last_irq = (((unsigned long)nowsec * 1000) + rem_nsec/1000000 ) - (((unsigned long)erase_reset_work->last_int_time_in_sec * 1000) + erase_reset_work->last_int_time_in_ns/1000000);

    if (msec_since_last_irq > MAX_MSEC_SINCE_LAST_IRQ) {
        erase_reset_work->step = 0;
#ifdef DEBUG
        printk("[bossac] Reset authentication timeout!\n");
#endif
    }

#ifdef DEBUG
    printk("[bossac] STEP %d -> 0x%d \n", erase_reset_work->step, auth_bit);
#endif
    erase_reset_work->last_int_time_in_ns = rem_nsec;
    erase_reset_work->last_int_time_in_sec = nowsec;

    if ( erase_reset_work->step < 16 ) {  // Authenticating received token bit.
        expected_bit = (( AUTH_TOKEN >> erase_reset_work->step ) & 0x01 );
        if ( auth_bit == expected_bit ) {
            erase_reset_work->step = erase_reset_work->step + 1;
        } else {
            erase_reset_work->step = 0;
        }
    } else { // Passed all authentication step. Receiving command code.
        erase_reset_work->cmdcode = erase_reset_work->cmdcode | (auth_bit << (erase_reset_work->step - 16));
        erase_reset_work->step = erase_reset_work->step + 1;
    }

#ifdef DEBUG
    printk("erase_reset_work->erase_reset_lock = %d \n", erase_reset_work->erase_reset_lock);
#endif
    if ( erase_reset_work->step == 20 ) {  // Passed authentication and code acquiring step.
#ifdef DEBUG
        printk("[bossac] Received code = 0x%04x \n", erase_reset_work->cmdcode);
#endif
        if (erase_reset_work->cmdcode == 0xF) {
            if (erase_reset_work->erase_reset_lock == 0) {
		erase_reset_work->erase_reset_lock = 1;
		retval = queue_work( erase_reset_wq, (struct work_struct *)work );
            } else {
#ifdef DEBUG
                printk("Erase and reset operation already in progress. Do nothing.\n");
#endif
            }
        } else {
            enable_serial();
        }
        erase_reset_work->step = 0;
        erase_reset_work->cmdcode = 0;
    }

    return IRQ_HANDLED;
}

/*
 * Takes control of clock, data, erase, reset GPIOs.
 */
static int gpio_setup(void)
{
    int ret;

    ret = gpio_request(work->gpio_bossac_clk, "BOSSA_CLK");
    if (ret) {
        printk(KERN_ERR "request BOSSA_CLK IRQ\n");
        return -1;
    } else {
        gpio_direction_input(work->gpio_bossac_clk);
    }

    ret = gpio_request(work->gpio_bossac_dat, "BOSSA_DAT");
    if (ret) {
        printk(KERN_ERR "request BOSSA_DAT IRQ\n");
        return -1;
    } else {
        gpio_direction_input(work->gpio_bossac_dat);
    }

    ret = gpio_request(work->gpio_ard_erase, "BOSSAC");
    if (ret) {
        printk(KERN_ERR "request GPIO FOR ARDUINO ERASE\n");
        return -1;
    } else {
        gpio_direction_input(work->gpio_ard_erase);
    }

    ret = gpio_request(work->gpio_ard_reset, "BOSSAC");
    if (ret) {
        printk(KERN_ERR "request GPIO FOR ARDUINO RESET\n");
        return -1;
    } else {
        gpio_direction_output(work->gpio_ard_reset, 1);
    }

    return 0;
}

static ssize_t device_write(struct file *filp, const char *buff, size_t len, loff_t *off)
{
    char msg[10];
    long res;

    if (len > 10)
		return -EINVAL;


	res = copy_from_user(msg, buff, len);
    if (res) {
        return -EFAULT;
    }
	msg[len] = '\0';

    if (strcmp(msg, "erase")==0) {
        erase_reset();
    } else if (strcmp(msg, "shutdown")==0) {
        shutdown_sam3x();
    } else if (strcmp(msg, "uartoff")==0) {
        disable_serial();
    } else if (strcmp(msg, "uarton")==0) {
        enable_serial();
    } else {
        printk("[bossac] udoo_ard invalid operation! %s", msg);
    }

	return len;
}

static struct file_operations fops = {
    .write = device_write,
};

/*
 * If a fdt udoo_ard entry is found, we register an IRQ on bossac clock line
 * and we create /dev/udoo_ard
 */
static int udoo_ard_probe(struct platform_device *pdev)
{
    int retval;
    struct device *temp_class;
    struct platform_device *bdev;
    struct device_node *np;

    bdev = kzalloc(sizeof(*bdev), GFP_KERNEL);
    np = pdev->dev.of_node;

    if (!np)
            return -ENODEV;

    work = (erase_reset_work_t *)kmalloc(sizeof(erase_reset_work_t), GFP_KERNEL);
    if (work) {
	    work->gpio_ard_reset = of_get_named_gpio(np, "bossac-reset-gpio", 0);
	    work->gpio_ard_erase = of_get_named_gpio(np, "bossac-erase-gpio", 0);
	    work->gpio_bossac_clk = of_get_named_gpio(np, "bossac-clk-gpio", 0);
	    work->gpio_bossac_dat = of_get_named_gpio(np, "bossac-dat-gpio", 0);
	    work->pinctrl = devm_pinctrl_get(&pdev->dev);
        work->pins_default = pinctrl_lookup_state(work->pinctrl, PINCTRL_DEFAULT);
    } else {
	    printk("[bossac] Failed to allocate data structure.");
	    return -ENOMEM;
    }

    pinctrl_select_state(work->pinctrl, work->pins_default);
    gpio_setup();

    printk("[bossac] Registering IRQ %d for BOSSAC Arduino erase/reset operation\n", gpio_to_irq(work->gpio_bossac_clk));
    retval = request_irq(gpio_to_irq(work->gpio_bossac_clk), udoo_bossac_req, IRQF_TRIGGER_FALLING, "UDOO", bdev);

    major = register_chrdev(major, "udoo_ard", &fops);
    if (major < 0) {
		printk(KERN_ERR "[bossac] Cannot get major for UDOO Ard\n");
		return -EBUSY;
	}

    udoo_class = class_create(THIS_MODULE, "udoo_ard");
	if (IS_ERR(udoo_class)) {
		return PTR_ERR(udoo_class);
	}

	temp_class = device_create(udoo_class, NULL, MKDEV(major, 0), NULL, "udoo_ard");
	if (IS_ERR(temp_class)) {
		return PTR_ERR(temp_class);
	}

    printk("[bossac] Created device file /dev/udoo_ard\n");

    erase_reset_wq = create_workqueue("erase_reset_queue");
    if (erase_reset_wq) {

        /* Queue some work (item 1) */
        if (work) {
            INIT_WORK( (struct work_struct *)work, erase_reset_wq_function );
            work->step = 1;
            work->cmdcode = 0;
            work->last_int_time_in_ns = 0;
            work->last_int_time_in_sec = 0;
            work->erase_reset_lock = 0;
            //  retval = queue_work( erase_reset_wq, (struct work_struct *)work );
        }
    }
    return  0;
}

static int udoo_ard_remove(struct platform_device *pdev)
{
    printk("[bossac] Unloading UDOO ard driver.\n");
    free_irq(gpio_to_irq(work->gpio_bossac_clk), NULL);

    gpio_free(work->gpio_ard_reset);
    gpio_free(work->gpio_ard_erase);
    gpio_free(work->gpio_bossac_clk);
    gpio_free(work->gpio_bossac_dat);

    device_destroy(udoo_class, MKDEV(major, 0));
    class_destroy(udoo_class);
    unregister_chrdev(major, "udoo_ard");

    return 0;
}

static struct platform_driver udoo_ard_driver = {
    .driver = {
        .name   = DRIVER_NAME,
        .owner  = THIS_MODULE,
        .of_match_table = udoo_ard_dt_ids,
    },
    .id_table = udoo_ard_devtype,
    .probe  = udoo_ard_probe,
    .remove = udoo_ard_remove,
};

module_platform_driver(udoo_ard_driver);

MODULE_ALIAS("platform:"DRIVER_NAME);
MODULE_LICENSE("GPL");
