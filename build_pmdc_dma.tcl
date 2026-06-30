################################################################
# build_pmdc_dma.tcl
#
# Builds the FULL Vivado project for PMDC motor v2 streaming
# over Ethernet/TCP on a Zybo Z7-20.
#
# Architecture:
#   ZYNQ7 PS (ARM)  <--AXI-Lite-->  AXI DMA (S2MM) + AXI GPIO(7b)
#         |                              |
#      S_AXI_HP0 <---- M_AXI_S2MM -------'
#         |
#       DDR <--- circular buffer, read by C code, sent over TCP
#
#   pmdc_dma_top (RTL) --AXI4-Stream--> axi_dma_0/S_AXIS_S2MM
#   axi_gpio_0/gpio_io_o[6:0] --> pmdc_dma_top/motor_ctrl
#
# USAGE:
#   1. Put ALL .v files (motor model + pmdc_dma_top.v +
#      motor_stream_packer.v) in one folder along with this script
#   2. cd into that folder in Vivado Tcl Console
#   3. source build_pmdc_dma.tcl
################################################################

set proj_name "pmdc_fpga_dma"
set proj_dir  "[pwd]/vivado_proj"
set part      "xc7z020clg400-1"
set board     "digilentinc.com:zybo-z7-20:part0:1.1"

create_project ${proj_name} ${proj_dir} -part ${part} -force
set_property board_part ${board} [current_project]
set_property target_language Verilog [current_project]

# ----------------------------------------------------------
# 1. Add ALL RTL source files (motor model + wrapper + packer)
# ----------------------------------------------------------
set rtl_files [glob -nocomplain *.v]
add_files -norecurse $rtl_files

# ----------------------------------------------------------
# 2. Create Block Design
# ----------------------------------------------------------
create_bd_design "system"

# ---- ZYNQ7 Processing System ----
create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7:5.5 processing_system7_0

apply_bd_automation -rule xilinx.com:bd_rule:processing_system7 \
    -config {make_external "FIXED_IO, DDR, PS_PORTS" \
             Master "Disable" Slave "Disable" \
             apply_board_preset "1"} \
    [get_bd_cells processing_system7_0]

# Enable FCLK_CLK0=100MHz, HP0 (for DMA->DDR), fabric interrupt
set_property -dict [list \
    CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {100} \
    CONFIG.PCW_USE_S_AXI_HP0 {1} \
    CONFIG.PCW_USE_FABRIC_INTERRUPT {1} \
    CONFIG.PCW_IRQ_F2P_INTR {1} \
] [get_bd_cells processing_system7_0]

# ---- AXI DMA (S2MM only, 32-bit, no scatter-gather) ----
create_bd_cell -type ip -vlnv xilinx.com:ip:axi_dma:7.1 axi_dma_0
set_property -dict [list \
    CONFIG.c_include_mm2s {0} \
    CONFIG.c_include_sg {0} \
    CONFIG.c_sg_include_stscntrl_strm {0} \
    CONFIG.c_m_axi_s2mm_data_width {32} \
    CONFIG.c_s_axis_s2mm_tdata_width {32} \
] [get_bd_cells axi_dma_0]

# ---- AXI GPIO (7-bit motor control, all outputs) ----
create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio:2.0 axi_gpio_0
set_property -dict [list \
    CONFIG.C_GPIO_WIDTH {7} \
    CONFIG.C_ALL_OUTPUTS {1} \
] [get_bd_cells axi_gpio_0]

# ---- PMDC RTL module (motor model + packer) ----
create_bd_cell -type module -reference pmdc_dma_top pmdc_dma_top_0

# ----------------------------------------------------------
# 3. AXI4-Lite control connections (auto-creates interconnect
#    + proc_sys_reset)
# ----------------------------------------------------------
apply_bd_automation -rule xilinx.com:bd_rule:axi4 \
    -config { Master "/processing_system7_0/M_AXI_GP0" Clk "Auto" } \
    [get_bd_intf_pins axi_dma_0/S_AXI_LITE]

apply_bd_automation -rule xilinx.com:bd_rule:axi4 \
    -config { Master "/processing_system7_0/M_AXI_GP0" Clk "Auto" } \
    [get_bd_intf_pins axi_gpio_0/S_AXI]

# ----------------------------------------------------------
# 4. AXI4 datapath: DMA S2MM -> PS HP0 -> DDR
# ----------------------------------------------------------
apply_bd_automation -rule xilinx.com:bd_rule:axi4 \
    -config { Master "/axi_dma_0/M_AXI_S2MM" Slave "/processing_system7_0/S_AXI_HP0" \
              ddr_seg "Auto" intc_ip "New AXI Interconnect" Clk "Auto" } \
    [get_bd_intf_pins processing_system7_0/S_AXI_HP0]

# ----------------------------------------------------------
# 5. AXI4-Stream: pmdc_dma_top -> axi_dma S2MM
# ----------------------------------------------------------
connect_bd_intf_net [get_bd_intf_pins pmdc_dma_top_0/m_axis] \
                     [get_bd_intf_pins axi_dma_0/S_AXIS_S2MM]

# ----------------------------------------------------------
# 6. Clock / reset / control wiring
# ----------------------------------------------------------
connect_bd_net [get_bd_pins processing_system7_0/FCLK_CLK0] \
               [get_bd_pins pmdc_dma_top_0/s_axi_aclk]

# peripheral_aresetn from the auto-generated proc_sys_reset
set rst_pin [get_bd_pins -of_objects [get_bd_cells -filter {VLNV =~ "*proc_sys_reset*"}] \
              -filter {NAME == "peripheral_aresetn"}]
connect_bd_net [lindex $rst_pin 0] [get_bd_pins pmdc_dma_top_0/s_axi_aresetn]

# GPIO -> motor control (7-bit direct connect)
connect_bd_net [get_bd_pins axi_gpio_0/gpio_io_o] \
               [get_bd_pins pmdc_dma_top_0/motor_ctrl]

# DMA interrupt -> PS
connect_bd_net [get_bd_pins axi_dma_0/s2mm_introut] \
               [get_bd_pins processing_system7_0/IRQ_F2P]

# ----------------------------------------------------------
# 7. Address map
# ----------------------------------------------------------
assign_bd_address -target_address_space [get_bd_addr_spaces processing_system7_0/Data] \
    [get_bd_addr_segs axi_dma_0/S_AXI_LITE/Reg] -force

assign_bd_address -target_address_space [get_bd_addr_spaces processing_system7_0/Data] \
    [get_bd_addr_segs axi_gpio_0/S_AXI/Reg] -force

assign_bd_address -target_address_space [get_bd_addr_spaces axi_dma_0/Data_S2MM] \
    [get_bd_addr_segs processing_system7_0/S_AXI_HP0/HP0_DDR_LOWOCM] -force

# ----------------------------------------------------------
# 8. Validate, save, wrapper
# ----------------------------------------------------------
validate_bd_design
save_bd_design

make_wrapper -files [get_files system.bd] -top
add_files -norecurse [glob ${proj_dir}/${proj_name}.gen/sources_1/bd/system/hdl/system_wrapper.v]
set_property top system_wrapper [current_fileset]

puts ""
puts "========================================================="
puts " Block design created: system.bd"
puts " Top set to: system_wrapper"
puts ""
puts " GPIO->motor_ctrl bit map (write via XGpio_DiscreteWrite):"
puts "   bit0=reset_n  bit1=pwma  bit2=pwmb  bit3=pwmen"
puts "   bit4=Tl0      bit5=Tl1   bit6=Tl2"
puts ""
puts " DMA stream: 32-bit/sample = {w_out_tcp[15:0], ia_out_tcp[15:0]}"
puts " 100 samples/packet, 10us/sample -> 1ms/packet"
puts ""
puts " Next: launch_runs synth_1 -jobs 4 ; wait_on_run synth_1"
puts "       launch_runs impl_1 -to_step write_bitstream -jobs 4"
puts "========================================================="
