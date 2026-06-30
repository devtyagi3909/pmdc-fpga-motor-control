# Usage with Vitis IDE:
# In Vitis IDE create a Single Application Debug launch configuration,
# change the debug type to 'Attach to running target' and provide this 
# tcl script in 'Execute Script' option.
# Path of this script: C:\Users\devty\Downloads\pmdcverilogmodel\pmdc\pmdc_dma\vitis_workspace\pmdc_server_system\_ide\scripts\debugger_pmdc_server-default.tcl
# 
# 
# Usage with xsct:
# To debug using xsct, launch xsct and run below command
# source C:\Users\devty\Downloads\pmdcverilogmodel\pmdc\pmdc_dma\vitis_workspace\pmdc_server_system\_ide\scripts\debugger_pmdc_server-default.tcl
# 
connect -url tcp:127.0.0.1:3121
targets -set -nocase -filter {name =~"APU*"}
rst -system
after 3000
targets -set -filter {jtag_cable_name =~ "Digilent Zybo Z7 210351BDF8F6A" && level==0 && jtag_device_ctx=="jsn-Zybo Z7-210351BDF8F6A-23727093-0"}
fpga -file C:/Users/devty/Downloads/pmdcverilogmodel/pmdc/pmdc_dma/vitis_workspace/pmdc_server/_ide/bitstream/system_wrapper.bit
targets -set -nocase -filter {name =~"APU*"}
loadhw -hw C:/Users/devty/Downloads/pmdcverilogmodel/pmdc/pmdc_dma/vitis_workspace/system_wrapper/export/system_wrapper/hw/system_wrapper.xsa -mem-ranges [list {0x40000000 0xbfffffff}] -regs
configparams force-mem-access 1
targets -set -nocase -filter {name =~"APU*"}
source C:/Users/devty/Downloads/pmdcverilogmodel/pmdc/pmdc_dma/vitis_workspace/pmdc_server/_ide/psinit/ps7_init.tcl
ps7_init
ps7_post_config
targets -set -nocase -filter {name =~ "*A9*#0"}
dow C:/Users/devty/Downloads/pmdcverilogmodel/pmdc/pmdc_dma/vitis_workspace/pmdc_server/Debug/pmdc_server.elf
configparams force-mem-access 0
targets -set -nocase -filter {name =~ "*A9*#0"}
con
