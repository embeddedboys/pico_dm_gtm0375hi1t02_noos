// Copyright (c) 2024 embeddedboys developers

// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:

// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"

#include "ili9488.h" /* where we get x,y resolution */
#include "ns2009.h"

#define pr_debug printf

#define NS2009_ADDR      0x48
#define NS2009_DEF_SPEED 400000
#define NS2009_PIN_SCL  27
#define NS2009_PIN_SDA  26
#define NS2009_PIN_IRQ  21

#define NS2009_CMD_READ_X 0xC0
#define NS2009_CMD_READ_Y 0xD0

#define NS2009_DISABLE_IRQ (1 << 2)

typedef enum {
    NS2009_RESOLUTION_8BIT = 8,
    NS2009_RESOLUTION_12BIT = 12,
} ns2009_res_t;

enum {
    NS2009_POWER_MODE_NORMAL,
    NS2009_POWER_MODE_LOW_POWER,
};

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

struct ns2009_data {
    struct {
        uint8_t addr;
        i2c_inst_t *master;
        uint32_t speed;

        uint8_t scl_pin;
        uint8_t sda_pin;
    } i2c;

    uint8_t irq_pin;

    u16            tft_x_res;
    u16            tft_y_res;

    u16            rtp_x_res;   /* Res Touch X resolution */
    u16            rtp_y_res;
    u16            rtp_x_width;
    u16            rtp_y_width;
    int            rtp_x_offs;  /* Res Touch X offset */
    int            rtp_y_offs;
    float          rtp_x_sc;    /* Res Touch X scale */
    float          rtp_y_sc;
    ns2009_res_t   res;

    ns2009_direction_t dir;   /* direction set */
    bool invert_x;
    bool invert_y;
    bool switch_xy;
    uint16_t (*read_x)(struct ns2009_data *priv);
    uint16_t (*read_y)(struct ns2009_data *priv);
} g_ns2009_data;

static void ns2009_write_reg(struct ns2009_data *priv, uint8_t reg, uint8_t val)
{
    uint16_t buf = val << 8 | reg;
    i2c_write_blocking(priv->i2c.master, priv->i2c.addr, (uint8_t *)&buf, sizeof(buf), false);
}
#define write_reg ns2009_write_reg

static uint8_t ns2009_read_reg(struct ns2009_data *priv, uint8_t reg)
{
    uint8_t val;
    i2c_write_blocking(priv->i2c.master, priv->i2c.addr, &reg, 1, true);
    i2c_read_blocking(priv->i2c.master, priv->i2c.addr, &val, 1, false);
    return val;
}
#define read_reg ns2009_read_reg

static uint16_t __ns2009_read_x(struct ns2009_data *priv)
{
    uint8_t val = read_reg(priv, NS2009_CMD_READ_X);
    u16 this_x = 0;

    if (priv->invert_x)
        this_x = (priv->tft_x_res - (val * priv->tft_x_res) / (1 << priv->res));
    else
        this_x = (val * priv->tft_x_res) / (1 << priv->res);

    pr_debug("x : %d, sc_x : %f\n", this_x, priv->rtp_x_sc);
    this_x += priv->rtp_x_offs;
    this_x *= priv->rtp_x_sc;
    pr_debug("x : %d, sc_x : %f\n", this_x, priv->rtp_x_sc);

    return this_x;
}

uint16_t ns2009_read_x(void)
{
    return g_ns2009_data.read_x(&g_ns2009_data);
}

static uint16_t __ns2009_read_y(struct ns2009_data *priv)
{
    uint8_t val = read_reg(priv, NS2009_CMD_READ_Y);
    pr_debug("val : %d\n", val);
    u16 this_y = 0;

    if (priv->invert_y)
        this_y = (priv->tft_y_res - (val * priv->tft_y_res) / (1 << priv->res));
    else
        this_y = (val * priv->tft_y_res) / (1 << priv->res);

    pr_debug("y : %d, sc_y : %f\n", this_y, priv->rtp_y_sc);
    this_y += priv->rtp_y_offs;
    this_y *= priv->rtp_y_sc;
    pr_debug("y : %d, sc_y : %f\n", this_y, priv->rtp_y_sc);

    return this_y;
}

uint16_t ns2009_read_y(void)
{
    return g_ns2009_data.read_y(&g_ns2009_data);
}

static bool __ns2009_is_pressed(struct ns2009_data *priv)
{
    return !gpio_get(priv->irq_pin);
}

bool ns2009_is_pressed(void)
{
    return __ns2009_is_pressed(&g_ns2009_data);
}

static void swap_float(float *a, float *b)
{
    float temp = *a;
    *a = *b;
    *b = temp;
}

static void __ns2009_set_dir(struct ns2009_data *priv, ns2009_direction_t dir)
{
    priv->dir = dir;

    if (dir & NS2009_DIR_INVERT_X)
        priv->invert_x = true;
    else
        priv->invert_x = false;

    if (dir & NS2009_DIR_INVERT_Y)
        priv->invert_y = true;
    else
        priv->invert_y = false;

    if (dir & NS2009_DIR_SWITCH_XY) {
        priv->switch_xy = true;

        priv->read_x = __ns2009_read_y;
        priv->read_y = __ns2009_read_x;

        bool invert_tmp = priv->invert_x;
        priv->invert_x = priv->invert_y;
        priv->invert_y = invert_tmp;

        u16 offs_tmp = priv->rtp_x_offs;
        priv->rtp_x_offs = priv->rtp_y_offs;
        priv->rtp_y_offs = offs_tmp;
        swap_float(&priv->rtp_x_sc, &priv->rtp_y_sc);

        u16 res_tmp = priv->tft_x_res;
        priv->tft_x_res = priv->tft_y_res;
        priv->tft_y_res = res_tmp;
    } else {
        priv->switch_xy = false;
        priv->read_x = __ns2009_read_x;
        priv->read_y = __ns2009_read_y;
    }
}

void ns2009_set_dir(ns2009_direction_t dir)
{
    __ns2009_set_dir(&g_ns2009_data, dir);
}

static void ns2009_hw_init(struct ns2009_data *priv)
{
    i2c_init(priv->i2c.master, NS2009_DEF_SPEED);

    gpio_set_function(priv->i2c.scl_pin, GPIO_FUNC_I2C);
    gpio_set_function(priv->i2c.sda_pin, GPIO_FUNC_I2C);

    gpio_pull_up(priv->i2c.scl_pin);
    gpio_pull_up(priv->i2c.sda_pin);

    gpio_init(priv->irq_pin);
    gpio_set_dir(priv->irq_pin, GPIO_IN);
    gpio_pull_up(priv->irq_pin);

    /* initialize touch direction */
    __ns2009_set_dir(priv, priv->dir);
}

static int ns2009_probe(struct ns2009_data *priv)
{
    priv->i2c.master  = i2c1;
    priv->i2c.addr    = NS2009_ADDR;
    priv->i2c.scl_pin = NS2009_PIN_SCL;
    priv->i2c.sda_pin = NS2009_PIN_SDA;

    priv->irq_pin     = NS2009_PIN_IRQ;

    priv->tft_x_res = ILI9488_X_RES;
    priv->tft_y_res = ILI9488_Y_RES;

    priv->invert_x = false;
    priv->invert_y = false;

    priv->rtp_x_width = 80;
    priv->rtp_y_width = 54;
    priv->rtp_x_res = 415;
    priv->rtp_y_res = 285;
    priv->rtp_x_offs = 5;
    priv->rtp_y_offs = -20;
    priv->res = NS2009_RESOLUTION_8BIT;

    priv->rtp_x_sc = (float)((float)priv->tft_x_res / (float)priv->rtp_x_res);
    priv->rtp_y_sc = (float)((float)priv->tft_y_res / (float)priv->rtp_y_res);

    priv->dir = NS2009_DIR_SWITCH_XY | NS2009_DIR_INVERT_Y;

    ns2009_hw_init(priv);

    return 0;
}

int ns2009_driver_init(void)
{
    printf("ns2009_driver_init\n");
    ns2009_probe(&g_ns2009_data);
    return 0;
}