/**
 * motor_echo.c — lwIP TCP DMA Streaming Server for PMDC Motor v2
 *
 * Architecture (proven circular-buffer pattern):
 * ┌─────────────────────────────────────────────────────────────────┐
 * │  FPGA Fabric                                                    │
 * │  [PMDC_inv_hdl1] ──► [motor_stream_packer] ──► [AXI DMA S2MM]  │
 * │       ▲                                                         │
 * │  [AXI GPIO 7-bit] ── motor_ctrl ─────────────────────────────┘ │
 * └───────────────────────────────────┬─────────────────────────────┘
 *                                     │ DMA writes into next free slot
 *                                     ▼
 * ┌─────────────────────────────────────────────────────────────────┐
 * │  Circular Buffer (DDR) — 8 slots x 100 samples x 4 bytes        │
 * │       ▲                              ▲                          │
 * │   send_idx                       dma_idx                        │
 * │  (TCP sending)               (DMA filling)                      │
 * └─────────────────────────────────────────────────────────────────┘
 *                                     │
 *                                     ▼
 * ┌─────────────────────────────────────────────────────────────────┐
 * │  lwIP TCP (192.168.1.10 : 7) ──► Ethernet ──► PC                │
 * │                                                                  │
 * │  PC ──► TCP port 7 ──► 1-byte control packet                    │
 * │           bit0=reset_n bit1=pwma bit2=pwmb bit3=pwmen           │
 * │           bit4=Tl0 bit5=Tl1 bit6=Tl2  ──► AXI GPIO              │
 * └─────────────────────────────────────────────────────────────────┘
 *
 * Stream sample format (32-bit, little-endian, sfix16_En12):
 *   bits [15:0]  = ia_out_tcp  (armature current, /4096 -> Amps)
 *   bits [31:16] = w_out_tcp   (angular speed,    /4096 -> rad/s)
 *
 * Cache discipline (Cortex-A9 write-back cache):
 *   Flush      before DMA start  (CPU -> DMA handover)
 *   Invalidate after DMA done    (DMA -> CPU handover)
 */

#include <stdio.h>
#include <string.h>

#include "xaxidma.h"
#include "xgpio.h"
#include "xparameters.h"
#include "xil_cache.h"

#include "lwip/err.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"

#if defined (__arm__) || defined (__aarch64__)
#include "xil_printf.h"
#endif

/* ================================================================
 * Configuration
 * ================================================================ */

/** 32-bit samples per DMA transfer / TCP packet (= 1ms of motor data) */
#define SAMPLES_PER_PACKET  100

/** Circular buffer slots. Power of 2. 8 = 7 packets headroom. */
#define NUM_SLOTS           8

/** TCP port — handles both stream TX and motor control RX */
#define TCP_PORT            7

/** AXI GPIO device ID for motor control */
#define MOTOR_GPIO_DEVICE_ID    XPAR_AXI_GPIO_0_DEVICE_ID

/** Default startup state: motor in reset, all PWM/torque = 0 */
#define MOTOR_CTRL_RESET    0x00

/* ================================================================
 * Derived constants
 * ================================================================ */

#define BYTES_PER_PACKET    (SAMPLES_PER_PACKET * sizeof(u32))
#define SLOT_MASK           (NUM_SLOTS - 1)

#if (NUM_SLOTS & (NUM_SLOTS - 1)) != 0
#error "NUM_SLOTS must be a power of 2 (4, 8, 16, ...)"
#endif

/* ================================================================
 * Circular buffer (32-bit samples, 32-byte aligned slots)
 * ================================================================ */

static u32 circ_buf[NUM_SLOTS][SAMPLES_PER_PACKET] __attribute__((aligned(32)));

static u32 dma_idx  = 0;
static u32 send_idx = 0;

/* ================================================================
 * DMA + GPIO + state
 * ================================================================ */

static XAxiDma AxiDma;
static XGpio   MotorGpio;
static u8      motor_ctrl_shadow = MOTOR_CTRL_RESET;

typedef enum {
    STATE_IDLE,
    STATE_DMA_RUNNING,
    STATE_SEND_PENDING,
} AppState;

static AppState        app_state     = STATE_IDLE;
static struct tcp_pcb *client_pcb    = NULL;
static u32             overrun_count = 0;

/* ================================================================
 * Forward declarations
 * ================================================================ */

static int   start_dma_into_slot(u32 slot);
static void  on_dma_complete(void);
static err_t tcp_sent_cb(void *arg, struct tcp_pcb *tpcb, u16_t len);
static err_t tcp_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static void  tcp_error_cb(void *arg, err_t err);

/* ================================================================
 * Circular buffer helpers
 * ================================================================ */

static inline u32 slots_filled(void)
{
    return (dma_idx - send_idx) & SLOT_MASK;
}

static inline u32 slots_free(void)
{
    return NUM_SLOTS - 1 - slots_filled();
}

/* ================================================================
 * DMA helpers
 * ================================================================ */

static int start_dma_into_slot(u32 slot)
{
    UINTPTR addr = (UINTPTR)circ_buf[slot & SLOT_MASK];
    int     status;

    Xil_DCacheFlushRange(addr, BYTES_PER_PACKET);

    status = XAxiDma_SimpleTransfer(
        &AxiDma, addr, BYTES_PER_PACKET, XAXIDMA_DEVICE_TO_DMA);

    if (status != XST_SUCCESS) {
        xil_printf("[DMA] Transfer into slot %lu failed: %d\n\r",
                   (unsigned long)(slot & SLOT_MASK), status);
        return -1;
    }

    app_state = STATE_DMA_RUNNING;
    return 0;
}

static void on_dma_complete(void)
{
    u32 completed = dma_idx & SLOT_MASK;

    Xil_DCacheInvalidateRange((UINTPTR)circ_buf[completed], BYTES_PER_PACKET);

    dma_idx = (dma_idx + 1) & SLOT_MASK;

    if (dma_idx == send_idx) {
        overrun_count++;
        xil_printf("[CIRC] Overrun #%lu — dropping oldest slot\n\r",
                   (unsigned long)overrun_count);
        send_idx = (send_idx + 1) & SLOT_MASK;
    }

    if (slots_free() > 0) {
        start_dma_into_slot(dma_idx);
    } else {
        xil_printf("[CIRC] Buffer full — DMA paused\n\r");
        app_state = STATE_SEND_PENDING;
    }
}

/* ================================================================
 * send_samples — call every main loop iteration
 * ================================================================ */

void send_samples(void)
{
    if (!client_pcb) return;

    switch (app_state) {

        case STATE_DMA_RUNNING:
            if (XAxiDma_Busy(&AxiDma, XAXIDMA_DEVICE_TO_DMA)) {
                break;
            }
            on_dma_complete();
            /* FALLTHROUGH */

        case STATE_SEND_PENDING:
            while (slots_filled() > 0) {

                if (tcp_sndbuf(client_pcb) < (u16_t)BYTES_PER_PACKET) {
                    app_state = STATE_SEND_PENDING;
                    return;
                }

                u32 slot = send_idx & SLOT_MASK;

                err_t err = tcp_write(
                    client_pcb, circ_buf[slot],
                    (u16_t)BYTES_PER_PACKET, TCP_WRITE_FLAG_COPY);

                if (err != ERR_OK) {
                    xil_printf("[TCP] tcp_write error: %d\n\r", err);
                    app_state = STATE_SEND_PENDING;
                    return;
                }

                send_idx = (send_idx + 1) & SLOT_MASK;

                if (!XAxiDma_Busy(&AxiDma, XAXIDMA_DEVICE_TO_DMA) &&
                    app_state == STATE_SEND_PENDING) {
                    start_dma_into_slot(dma_idx);
                }
            }

            tcp_output(client_pcb);
            app_state = STATE_DMA_RUNNING;
            break;

        case STATE_IDLE:
        default:
            break;
    }
}

/* ================================================================
 * TCP callbacks
 * ================================================================ */

/**
 * tcp_recv_cb — receives motor control byte(s) from PC
 *
 * Each received byte is a complete control word:
 *   bit0=reset_n bit1=pwma bit2=pwmb bit3=pwmen
 *   bit4=Tl0 bit5=Tl1 bit6=Tl2
 *
 * If multiple bytes arrive, only the LAST one is applied
 * (always reflects the most recent command).
 */
static err_t tcp_recv_cb(void *arg, struct tcp_pcb *tpcb,
                         struct pbuf *p, err_t err)
{
    (void)arg;

    if (!p) {
        xil_printf("[TCP] Client closed connection\n\r");
        tcp_close(tpcb);
        client_pcb = NULL;
        return ERR_OK;
    }

    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }

    if (p->tot_len >= 1) {
        u8 buf[8];
        u16_t copy_len = p->tot_len > 8 ? 8 : p->tot_len;
        pbuf_copy_partial(p, buf, copy_len, 0);

        u8 ctrl = buf[copy_len - 1] & 0x7F;   /* lower 7 bits used */
        motor_ctrl_shadow = ctrl;

        XGpio_DiscreteWrite(&MotorGpio, 1, (u32)ctrl);

        xil_printf("[CTRL] 0x%02X  reset_n=%d pwma=%d pwmb=%d pwmen=%d Tl=%d%d%d\n\r",
                   ctrl,
                   (ctrl >> 0) & 1, (ctrl >> 1) & 1,
                   (ctrl >> 2) & 1, (ctrl >> 3) & 1,
                   (ctrl >> 6) & 1, (ctrl >> 5) & 1, (ctrl >> 4) & 1);
    }

    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t tcp_sent_cb(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    (void)arg; (void)tpcb; (void)len;

    if (app_state == STATE_SEND_PENDING) {
        send_samples();
    }
    return ERR_OK;
}

static void tcp_error_cb(void *arg, err_t err)
{
    (void)arg;

    xil_printf("[TCP] Connection lost (err=%d)\n\r", err);
    xil_printf("[TCP] Session overruns: %lu\n\r", (unsigned long)overrun_count);

    client_pcb    = NULL;
    overrun_count = 0;

    /* Safety: hold motor in reset when PC disconnects */
    motor_ctrl_shadow = MOTOR_CTRL_RESET;
    XGpio_DiscreteWrite(&MotorGpio, 1, MOTOR_CTRL_RESET);
    xil_printf("[SAFETY] Motor reset on disconnect\n\r");

    dma_idx  = 0;
    send_idx = 0;

    app_state = STATE_DMA_RUNNING;
}

err_t accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    (void)arg;

    if (err != ERR_OK || !newpcb) {
        xil_printf("[TCP] Accept error: %d\n\r", err);
        return ERR_VAL;
    }

    if (client_pcb != NULL) {
        xil_printf("[TCP] Rejecting second client\n\r");
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    client_pcb = newpcb;

    tcp_recv(newpcb, tcp_recv_cb);
    tcp_nagle_disable(newpcb);
    tcp_sent(newpcb, tcp_sent_cb);
    tcp_err(newpcb,  tcp_error_cb);

    xil_printf("[TCP] Client connected\n\r");
    xil_printf("[CIRC] %d slots x %d bytes = %d bytes\n\r",
               NUM_SLOTS, (int)BYTES_PER_PACKET,
               (int)(NUM_SLOTS * BYTES_PER_PACKET));

    send_samples();
    return ERR_OK;
}

/* ================================================================
 * print_app_header
 * ================================================================ */

void print_app_header(void)
{
    xil_printf("\n\r\n\r----- PMDC Motor TCP Streaming Server -----\n\r");
    xil_printf("  Samples/packet     : %d\n\r", SAMPLES_PER_PACKET);
    xil_printf("  Bytes/packet       : %d  (32-bit/sample)\n\r", (int)BYTES_PER_PACKET);
    xil_printf("  Sample period      : 10 us  (Ts model)\n\r");
    xil_printf("  Packet period      : ~1 ms\n\r");
    xil_printf("  Circular buf slots : %d\n\r", NUM_SLOTS);
    xil_printf("  TCP port           : %d\n\r", TCP_PORT);
    xil_printf("  Format: bits[15:0]=ia_out_tcp  bits[31:16]=w_out_tcp\n\r");
    xil_printf("          (sfix16_En12, divide by 4096 for physical units)\n\r");
    xil_printf("  Control byte (PC->FPGA, 1 byte):\n\r");
    xil_printf("    bit0=reset_n bit1=pwma bit2=pwmb bit3=pwmen\n\r");
    xil_printf("    bit4=Tl0 bit5=Tl1 bit6=Tl2\n\r");
    xil_printf("--------------------------------------------\n\r");
}

/* ================================================================
 * start_application
 * ================================================================ */

int start_application(void)
{
    XAxiDma_Config *CfgPtr;
    struct tcp_pcb  *pcb;
    err_t  err;
    int    status, i;

    /* ── Init AXI GPIO for motor control ───────────────── */

    status = XGpio_Initialize(&MotorGpio, MOTOR_GPIO_DEVICE_ID);
    if (status != XST_SUCCESS) {
        xil_printf("[GPIO] Init failed: %d\n\r", status);
        return -1;
    }

    XGpio_SetDataDirection(&MotorGpio, 1, 0x00);  /* all outputs */

    /* Hold motor in reset until PC sends first control byte */
    motor_ctrl_shadow = MOTOR_CTRL_RESET;
    XGpio_DiscreteWrite(&MotorGpio, 1, MOTOR_CTRL_RESET);

    xil_printf("[GPIO] Motor control ready (held in reset)\n\r");

    /* ── Init AXI DMA ────────────────────────────────────── */

    CfgPtr = XAxiDma_LookupConfig(XPAR_AXIDMA_0_DEVICE_ID);
    if (!CfgPtr) {
        xil_printf("[DMA] Config not found\n\r");
        return -1;
    }

    status = XAxiDma_CfgInitialize(&AxiDma, CfgPtr);
    if (status != XST_SUCCESS) {
        xil_printf("[DMA] Init failed: %d\n\r", status);
        return -1;
    }

    XAxiDma_IntrDisable(&AxiDma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
    XAxiDma_IntrDisable(&AxiDma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DMA_TO_DEVICE);

    /* ── Flush all slots, start first transfer ──────────── */

    for (i = 0; i < NUM_SLOTS; i++) {
        Xil_DCacheFlushRange((UINTPTR)circ_buf[i], BYTES_PER_PACKET);
    }

    dma_idx  = 0;
    send_idx = 0;

    status = start_dma_into_slot(0);
    if (status != 0) {
        xil_printf("[DMA] First transfer failed\n\r");
        return -1;
    }

    xil_printf("[DMA] Started — filling slot 0\n\r");

    /* ── TCP server ──────────────────────────────────────── */

    pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) {
        xil_printf("[TCP] PCB alloc failed\n\r");
        return -1;
    }

    err = tcp_bind(pcb, IP_ANY_TYPE, TCP_PORT);
    if (err != ERR_OK) {
        xil_printf("[TCP] Bind failed: %d\n\r", err);
        return -2;
    }

    pcb = tcp_listen(pcb);
    if (!pcb) {
        xil_printf("[TCP] Listen failed\n\r");
        return -3;
    }

    tcp_accept(pcb, accept_callback);

    xil_printf("[TCP] Listening on port %d\n\r", TCP_PORT);
    return 0;
}
