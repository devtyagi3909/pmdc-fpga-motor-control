// =============================================================
// motor_stream_packer.v
//
// Drop-in replacement for testing_packer.v (axis_stream_packer_new)
// in the zynq-axi-dma-tcp-streamer block design.
//
// Differences vs original:
//   - 2 inputs (ia, w) instead of 4, packed into 32-bit TDATA
//   - Tick divider: 999  →  10 µs at 100 MHz  (motor Ts = 1e-5)
//     (original was 49 → 1 µs at 50 MHz)
//   - AXI DMA in Vivado must be set to 32-bit stream width
//
// Wire in Vivado block design:
//   in0       ← data_out1  (ia)      [15:0]
//   in1       ← data_out2  (w)       [15:0]
//   in_valid0 ← 1'b1
//   in_valid1 ← 1'b1
//   m_axis_*  → AXI DMA S_AXIS_S2MM
// =============================================================

`timescale 1 ns / 1 ns

module motor_stream_packer (
    input  wire        aclk,
    input  wire        aresetn,

    // 2 × 16-bit motor outputs
    input  wire [15:0] in0,          // data_out1  (ia, sfix16_En12)
    input  wire [15:0] in1,          // data_out2  (w,  sfix16_En12)
    input  wire        in_valid0,
    input  wire        in_valid1,

    // AXI4-Stream master  (32-bit wide)
    output reg  [31:0] m_axis_tdata,
    output reg         m_axis_tvalid,
    output reg         m_axis_tlast,
    input  wire        m_axis_tready
);

// ----------------------------------------------------------
// Tick generator: 10 µs at 100 MHz  (count 0 → 999)
// ----------------------------------------------------------
reg [9:0]  us_div    = 0;
reg        sample_tick = 0;

always @(posedge aclk) begin
    if (!aresetn) begin
        us_div     <= 0;
        sample_tick <= 0;
    end else begin
        if (us_div == 10'd999) begin
            us_div     <= 0;
            sample_tick <= 1;
        end else begin
            us_div     <= us_div + 1;
            sample_tick <= 0;
        end
    end
end

// ----------------------------------------------------------
// AXI-Stream output: 100 samples per frame, TLAST at frame end
// ----------------------------------------------------------
reg [6:0] sample_count = 0;

wire all_valid = in_valid0 & in_valid1;

always @(posedge aclk) begin
    if (!aresetn) begin
        m_axis_tdata  <= 32'd0;
        m_axis_tvalid <= 1'b0;
        m_axis_tlast  <= 1'b0;
        sample_count  <= 7'd0;
    end else begin

        // Backpressure: hold current word until DMA accepts
        if (m_axis_tvalid && !m_axis_tready) begin
            m_axis_tvalid <= 1'b1;   // keep waiting
        end

        // New sample: tick fired AND both inputs valid
        else if (sample_tick && all_valid) begin
            m_axis_tdata  <= {in1, in0};   // [31:16]=w  [15:0]=ia
            m_axis_tvalid <= 1'b1;

            if (sample_count == 7'd99) begin
                m_axis_tlast  <= 1'b1;
                sample_count  <= 7'd0;
            end else begin
                m_axis_tlast  <= 1'b0;
                sample_count  <= sample_count + 7'd1;
            end
        end

        else begin
            m_axis_tvalid <= 1'b0;
            m_axis_tlast  <= 1'b0;
        end

    end
end

endmodule
