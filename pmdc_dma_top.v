// =============================================================
// pmdc_dma_top.v
//
// Combines:
//   - PMDC_inv_hdl1 (v2 motor model, with torque control)
//   - clk_enable divider (10us @ 100MHz, matches Ts=1e-5)
//   - motor_stream_packer (packs ia_out_tcp + w_out_tcp -> AXIS)
//
// This module is instantiated as an RTL module reference inside
// the Vivado Block Design. Its AXI4-Stream master connects to
// axi_dma_0 / S_AXIS_S2MM. Its motor_ctrl input connects to
// axi_gpio_0 / gpio_io_o[6:0].
//
// motor_ctrl bit map (matches motor_echo.c control byte):
//   bit 0 = reset_n   (1 = motor running, 0 = held in reset)
//   bit 1 = pwma
//   bit 2 = pwmb
//   bit 3 = pwmen
//   bit 4 = Tl0
//   bit 5 = Tl1
//   bit 6 = Tl2
//
// b0,b1,b2,d_en on PMDC_inv_hdl1 are tied to 0 (legacy serial
// readback path, unused — ia_out_tcp/w_out_tcp used directly)
// =============================================================

`timescale 1 ns / 1 ns

module pmdc_dma_top (
    input  wire        s_axi_aclk,
    input  wire        s_axi_aresetn,

    input  wire [6:0]  motor_ctrl,

    output wire [31:0] m_axis_tdata,
    output wire        m_axis_tvalid,
    output wire        m_axis_tlast,
    input  wire        m_axis_tready
);

    // ------------------------------------------------------
    // Decode control bits
    // ------------------------------------------------------
    wire reset_n = motor_ctrl[0];
    wire pwma    = motor_ctrl[1];
    wire pwmb    = motor_ctrl[2];
    wire pwmen   = motor_ctrl[3];
    wire Tl0     = motor_ctrl[4];
    wire Tl1     = motor_ctrl[5];
    wire Tl2     = motor_ctrl[6];

    // Active-high reset for the motor model
    wire reset = ~reset_n | ~s_axi_aresetn;

    // ------------------------------------------------------
    // clk_enable divider: pulse every 1000 cycles
    //   100 MHz / 1000 = 10 us  (matches model Ts = 1e-5)
    // ------------------------------------------------------
    reg [9:0] ce_cnt;
    reg       clk_enable;

    always @(posedge s_axi_aclk) begin
        if (reset) begin
            ce_cnt     <= 10'd0;
            clk_enable <= 1'b0;
        end else if (ce_cnt == 10'd999) begin
            ce_cnt     <= 10'd0;
            clk_enable <= 1'b1;
        end else begin
            ce_cnt     <= ce_cnt + 10'd1;
            clk_enable <= 1'b0;
        end
    end

    // ------------------------------------------------------
    // PMDC motor model (v2)
    // ------------------------------------------------------
    wire              ce_out;
    wire [15:0]       data_out_unused;
    wire              T_x0_unused, T_x1_unused, PWMen_o_unused;
    wire signed [15:0] ia_out_tcp;
    wire signed [15:0] w_out_tcp;

    PMDC_inv_hdl1 u_pmdc (
        .clk        (s_axi_aclk),
        .reset      (reset),
        .clk_enable (clk_enable),
        .pwma       (pwma),
        .pwmb       (pwmb),
        .pwmen      (pwmen),
        .b0         (1'b0),
        .b1         (1'b0),
        .b2         (1'b0),
        .d_en       (1'b0),
        .Tl0        (Tl0),
        .Tl1        (Tl1),
        .Tl2        (Tl2),
        .ce_out     (ce_out),
        .data_out   (data_out_unused),
        .T_x0       (T_x0_unused),
        .T_x1       (T_x1_unused),
        .PWMen_o    (PWMen_o_unused),
        .ia_out_tcp (ia_out_tcp),
        .w_out_tcp  (w_out_tcp)
    );

    // ------------------------------------------------------
    // AXI4-Stream packer -> AXI DMA S2MM
    // ------------------------------------------------------
    motor_stream_packer u_packer (
        .aclk          (s_axi_aclk),
        .aresetn       (reset_n & s_axi_aresetn),
        .in0           (ia_out_tcp),
        .in1           (w_out_tcp),
        .in_valid0     (1'b1),
        .in_valid1     (1'b1),
        .m_axis_tdata  (m_axis_tdata),
        .m_axis_tvalid (m_axis_tvalid),
        .m_axis_tlast  (m_axis_tlast),
        .m_axis_tready (m_axis_tready)
    );

endmodule
