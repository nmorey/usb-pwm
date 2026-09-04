/*
 * USB HWMON Fan PWM Controller Driver
 * 
 * Binds to the Arduino Nano R4 (VID 0x2341, PID 0x0063) and exposes
 * its two fan controllers to the Linux hwmon subsystem.
 * 
 * This driver communicates over native USB bulk endpoints using
 * a lightweight request-response text-based protocol.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/usb.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/jiffies.h>

#define USB_PWM_N_FANS      1 // More than 1 is not supported by firmware for now
#define USB_PWM_VENDOR_ID   0x2341
#define USB_PWM_PRODUCT_ID  0x0074  // Nano R4

/* Private driver state and data */
struct usb_pwm_priv {
    struct usb_device *udev;
    struct device *hwmon_dev;
    struct mutex lock; /* Mutex protecting concurrent USB communication */

    __u8 bulk_in_endpointAddr;
    __u8 bulk_out_endpointAddr;
    size_t bulk_in_size;
    size_t bulk_out_size;

    unsigned long last_update; /* Time of last RPM update (jiffies) */
    bool cache_valid;

    long fan_rpm[USB_PWM_N_FANS];
    long pwm_duty[USB_PWM_N_FANS];
};

/* Device ID table for USB core matching */
static const struct usb_device_id usb_pwm_table[] = {
    { USB_DEVICE(USB_PWM_VENDOR_ID, USB_PWM_PRODUCT_ID) },
    { } /* Terminating entry */
};
MODULE_DEVICE_TABLE(usb, usb_pwm_table);

/*
 * Sends a command to the USB device and reads back the response.
 * Synchronous and blocking. Must NOT be called from interrupt context.
 */
static int usb_pwm_send_cmd_recv_resp(struct usb_pwm_priv *priv, const char *cmd,
				      char *resp, size_t resp_max_len)
{
    int retval;
    int actual_length;
    char *buf;
    unsigned timeout = 200; /* 200 ms timeout */

    /* Buffer passed to usb_bulk_msg MUST be dynamically allocated and DMA-safe */
    buf = kmalloc(64, GFP_KERNEL);
    if (!buf)
        return -ENOMEM;

    /* Copy command safely */
    snprintf(buf, 64, "%s", cmd);

    /* Write command to Bulk OUT endpoint */
    retval = usb_bulk_msg(priv->udev,
                          usb_sndbulkpipe(priv->udev, priv->bulk_out_endpointAddr),
                          buf,
                          strlen(buf),
                          &actual_length,
                          1000); /* 1000 ms timeout */
    if (retval < 0) {
        dev_err(&priv->udev->dev, "Bulk write failed: %d\n", retval);
        kfree(buf);
        return retval;
    }

    /* Read response from Bulk IN endpoint, looping to ensure we consume the entire message/line */
    memset(buf, 0, 64);
    actual_length = 0;
    while (actual_length < 63) {
        int len = 0;
        retval = usb_bulk_msg(priv->udev,
                              usb_rcvbulkpipe(priv->udev, priv->bulk_in_endpointAddr),
                              buf + actual_length,
                              63 - actual_length,
                              &len,
                              timeout);
	// We just want to flush following stuff so le'ts recude the timeout
	timeout = 10;

	if (retval < 0) {
            /* If we already received some data, return what we have
	     * (e.g. if device didn't terminate with newline but timed out) */
            if (actual_length > 0 && retval == -ETIMEDOUT) {
		/* We kind of hit timeout all the time due to not getting \n so just ignore silently */
                break;
            }
            dev_err(&priv->udev->dev, "Bulk read failed: %d\n", retval);
            kfree(buf);
            return retval;
        }
        if (len == 0) {
            /* No data read and no error? Break to avoid infinite loop */
            break;
        }
        actual_length += len;
        buf[actual_length] = '\0';

        /* If we found a newline or carriage return, we have read a complete response line */
        if (strchr(buf, '\n') || strchr(buf, '\r')) {
            break;
        }
    }
    snprintf(resp, resp_max_len, "%s", buf);

    kfree(buf);
    return 0;
}

/*
 * Refreshes the speed cache if it is older than 1 second (1000ms).
 * Holds the private mutex to serialize access to the USB endpoints.
 */
static int usb_pwm_update_cache(struct usb_pwm_priv *priv)
{
    int ret = 0;
    char resp[64];
    unsigned int r1 = 0;

    mutex_lock(&priv->lock);

    /* Cache is valid for 1000ms */
    if (priv->cache_valid && time_before(jiffies, priv->last_update + msecs_to_jiffies(1000))) {
        goto out;
    }

    ret = usb_pwm_send_cmd_recv_resp(priv, "GET\n", resp, sizeof(resp));
    if (ret < 0) {
        dev_err(&priv->udev->dev, "Failed to query fan speeds: %d\n", ret);
        goto out;
    }

    /* Parse response format "RPM1 %u RPM2 %u" */
    if (sscanf(resp, "%u", &r1) == 1) {
        priv->fan_rpm[0] = r1;
        priv->last_update = jiffies;
        priv->cache_valid = true;
        ret = 0;
    } else {
        dev_err(&priv->udev->dev, "Malformed response from device: %s\n", resp);
        ret = -EIO;
    }

out:
    mutex_unlock(&priv->lock);
    return ret;
}

/* HWMON Callback: Read values from the device */
static int usb_pwm_read(struct device *dev, enum hwmon_sensor_types type,
                        u32 attr, int channel, long *val)
{
    struct usb_pwm_priv *priv = dev_get_drvdata(dev);
    int err;

    if (channel < 0 || channel >= USB_PWM_N_FANS)
        return -EINVAL;

    switch (type) {
    case hwmon_fan:
        if (attr == hwmon_fan_input) {
            err = usb_pwm_update_cache(priv);
            if (err < 0)
                return err;
            *val = priv->fan_rpm[channel];
            return 0;
        }
        break;

    case hwmon_pwm:
        if (attr == hwmon_pwm_input) {
            mutex_lock(&priv->lock);
            *val = priv->pwm_duty[channel];
            mutex_unlock(&priv->lock);
            return 0;
        } else if (attr == hwmon_pwm_enable) {
            *val = 1; /* Always returns 1 (manual speed control) */
            return 0;
        }
        break;

    default:
        break;
    }

    return -EOPNOTSUPP;
}

/* HWMON Callback: Write values to the device */
static int usb_pwm_write(struct device *dev, enum hwmon_sensor_types type,
                         u32 attr, int channel, long val)
{
    struct usb_pwm_priv *priv = dev_get_drvdata(dev);
    char cmd[32];
    char resp[16];
    int ret;

    if (channel < 0 || channel >= USB_PWM_N_FANS)
        return -EINVAL;

    if (type != hwmon_pwm)
        return -EOPNOTSUPP;

    if (attr == hwmon_pwm_input) {
        if (val < 0 || val > 255)
            return -EINVAL;

        mutex_lock(&priv->lock);

        /* Format and send command: "SET PWM <val>\n" */
        snprintf(cmd, sizeof(cmd), "SET PWM %ld\n", val);
        ret = usb_pwm_send_cmd_recv_resp(priv, cmd, resp, sizeof(resp));

        if (ret == 0) {
            /* Defensive match: Check if device confirmed receipt with "OK" */
            if (strncmp(resp, "OK", 2) == 0) {
                priv->pwm_duty[channel] = val;
            } else {
                dev_err(&priv->udev->dev, "Device rejected SET PWM command: %s\n", resp);
                ret = -EIO;
            }
        }

        mutex_unlock(&priv->lock);
        return ret;
    } else if (attr == hwmon_pwm_enable) {
        if (val != 1) {
            dev_warn(&priv->udev->dev, "Only manual control mode (1) is supported\n");
            return -EINVAL;
        }
        return 0; /* Already in manual mode, success */
    }

    return -EOPNOTSUPP;
}

/* HWMON Callback: Determine visibility of sysfs attributes */
static umode_t usb_pwm_is_visible(const void *data, enum hwmon_sensor_types type,
                                  u32 attr, int channel)
{
    if (channel < 0 || channel >= USB_PWM_N_FANS)
        return 0;

    switch (type) {
    case hwmon_fan:
        if (attr == hwmon_fan_input)
            return 0444; /* Read-only for fan RPM */
        break;

    case hwmon_pwm:
        if (attr == hwmon_pwm_input || attr == hwmon_pwm_enable)
            return 0644; /* Read-write for PWM values and enable mode */
        break;

    default:
        break;
    }
    return 0;
}

/* HWMON Subsystem Declarations */
static const struct hwmon_ops usb_pwm_hwmon_ops = {
    .is_visible = usb_pwm_is_visible,
    .read = usb_pwm_read,
    .write = usb_pwm_write,
};

static const u32 usb_pwm_fan_config[] = {
    [0 ... USB_PWM_N_FANS - 1 ] = HWMON_F_INPUT,
    0
};

static const u32 usb_pwm_pwm_config[] = {
    [0 ... USB_PWM_N_FANS - 1 ] = HWMON_P_INPUT | HWMON_P_ENABLE,
    0
};

static const struct hwmon_channel_info usb_pwm_fan_info = {
    .type = hwmon_fan,
    .config = usb_pwm_fan_config,
};
// Make sure no more hwmon fans than actual fans
_Static_assert(sizeof(usb_pwm_fan_config) / sizeof(usb_pwm_fan_config[0]) ==
	       sizeof(((struct usb_pwm_priv *)0)->fan_rpm) / 
	       sizeof(((struct usb_pwm_priv *)0)->fan_rpm[0]) + 1);

static const struct hwmon_channel_info usb_pwm_pwm_info = {
    .type = hwmon_pwm,
    .config = usb_pwm_pwm_config,
};
// Make sure no more hwmon pwm than actual fans
_Static_assert(sizeof(usb_pwm_fan_config) / sizeof(usb_pwm_fan_config[0]) ==
	       sizeof(((struct usb_pwm_priv *)0)->fan_rpm) / 
	       sizeof(((struct usb_pwm_priv *)0)->fan_rpm[0]) + 1);

static const struct hwmon_channel_info *const usb_pwm_channel_info[] = {
    &usb_pwm_fan_info,
    &usb_pwm_pwm_info,
    NULL
};

static const struct hwmon_chip_info usb_pwm_chip_info = {
    .ops = &usb_pwm_hwmon_ops,
    .info = usb_pwm_channel_info,
};

/* USB Probe Callback */
static int usb_pwm_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
    struct usb_device *udev = interface_to_usbdev(interface);
    struct usb_host_interface *iface_desc;
    struct usb_endpoint_descriptor *endpoint;
    struct usb_pwm_priv *priv;
    struct device *dev = &interface->dev;
    int i;
    int retval;

    iface_desc = interface->cur_altsetting;
    
    /* 
     * Arduino Nano R4 CDC-ACM device has two interfaces:
     * - Interface 0: Control/Communication Class
     * - Interface 1: Data Class (contains Bulk IN/OUT endpoints)
     * We only want to handle and register hwmon on Interface 1,
     * but we bind to both to prevent cdc_acm from claiming either.
     */
    if (iface_desc->desc.bInterfaceNumber != 1) {
        dev_info(dev, "USB PWM: Claimed control interface %d\n", iface_desc->desc.bInterfaceNumber);
        return 0; /* Bind successful, no hwmon setup required */
    }

    /* Allocate device state memory */
    priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;

    priv->udev = usb_get_dev(udev);
    mutex_init(&priv->lock);

    /* Discover bulk endpoints on Interface 1 dynamically */
    for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i) {
        endpoint = &iface_desc->endpoint[i].desc;

        if (usb_endpoint_is_bulk_in(endpoint)) {
            priv->bulk_in_endpointAddr = endpoint->bEndpointAddress;
            priv->bulk_in_size = usb_endpoint_maxp(endpoint);
            dev_info(dev, "USB PWM: Discovered Bulk IN endpoint: 0x%02X (size: %zu)\n",
                     priv->bulk_in_endpointAddr, priv->bulk_in_size);
        }

        if (usb_endpoint_is_bulk_out(endpoint)) {
            priv->bulk_out_endpointAddr = endpoint->bEndpointAddress;
            priv->bulk_out_size = usb_endpoint_maxp(endpoint);
            dev_info(dev, "USB PWM: Discovered Bulk OUT endpoint: 0x%02X (size: %zu)\n",
                     priv->bulk_out_endpointAddr, priv->bulk_out_size);
        }
    }

    if (!priv->bulk_in_endpointAddr || !priv->bulk_out_endpointAddr) {
        dev_err(dev, "USB PWM: Could not find both bulk IN and bulk OUT endpoints\n");
        retval = -ENODEV;
        goto error;
    }

    /* Initialize state defaults */
    priv->pwm_duty[0] = 255; /* Default to 100% duty cycle */
    priv->pwm_duty[1] = 255;
    priv->cache_valid = false;

    /* Assert DTR/RTS to tell the Arduino's CDC ACM stack that we are ready to communicate */
    retval = usb_control_msg(priv->udev,
                             usb_sndctrlpipe(priv->udev, 0),
                             0x22, /* SET_CONTROL_LINE_STATE */
                             0x21, /* USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE */
                             0x03, /* DTR (0x01) | RTS (0x02) */
                             0,    /* Control Interface 0 */
                             NULL, 0,
                             1000);
    if (retval < 0) {
        dev_warn(dev, "USB PWM: Failed to set control line state (DTR/RTS): %d\n", retval);
    } else {
        dev_info(dev, "USB PWM: Asserted DTR/RTS (control line state set successfully)\n");
    }

    /* Register device into the Linux hwmon subsystem */
    priv->hwmon_dev = devm_hwmon_device_register_with_info(dev,
                                                           "usb_pwm_controller",
                                                           priv,
                                                           &usb_pwm_chip_info,
                                                           NULL);
    if (IS_ERR(priv->hwmon_dev)) {
        dev_err(dev, "USB PWM: Failed to register hwmon device\n");
        retval = PTR_ERR(priv->hwmon_dev);
        goto error;
    }

    usb_set_intfdata(interface, priv);
    dev_info(dev, "USB PWM: HWMON device registered successfully\n");
    return 0;

error:
    usb_put_dev(priv->udev);
    return retval;
}

/* USB Disconnect Callback */
static void usb_pwm_disconnect(struct usb_interface *interface)
{
    struct usb_pwm_priv *priv;
    struct usb_host_interface *iface_desc = interface->cur_altsetting;

    if (iface_desc->desc.bInterfaceNumber != 1) {
        dev_info(&interface->dev, "USB PWM: Released control interface %d\n", iface_desc->desc.bInterfaceNumber);
        return; /* Interface 0 has no private structures, nothing to do */
    }

    priv = usb_get_intfdata(interface);
    usb_set_intfdata(interface, NULL);

    if (priv) {
        usb_put_dev(priv->udev);
    }
    dev_info(&interface->dev, "USB PWM: HWMON device disconnected\n");
}

/* Driver structure linking probe, disconnect and ID matching */
static struct usb_driver usb_pwm_driver = {
    .name = "usb_pwm_hwmon",
    .probe = usb_pwm_probe,
    .disconnect = usb_pwm_disconnect,
    .id_table = usb_pwm_table,
};

module_usb_driver(usb_pwm_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nathan Morey");
MODULE_DESCRIPTION("USB HWMON Fan PWM Controller Driver");
MODULE_VERSION("1.0");
