# 
# Usage: To re-create this platform project launch xsct with below options.
# xsct C:\Users\devty\Downloads\pmdcverilogmodel\pmdc\pmdc_dma\vitis_workspace\system_wrapper\platform.tcl
# 
# OR launch xsct and run below command.
# source C:\Users\devty\Downloads\pmdcverilogmodel\pmdc\pmdc_dma\vitis_workspace\system_wrapper\platform.tcl
# 
# To create the platform in a different location, modify the -out option of "platform create" command.
# -out option specifies the output directory of the platform project.

platform create -name {system_wrapper}\
-hw {C:\Users\devty\Downloads\pmdcverilogmodel\pmdc\pmdc_dma\system_wrapper.xsa}\
-out {C:/Users/devty/Downloads/pmdcverilogmodel/pmdc/pmdc_dma/vitis_workspace}

platform write
domain create -name {standalone_ps7_cortexa9_0} -display-name {standalone_ps7_cortexa9_0} -os {standalone} -proc {ps7_cortexa9_0} -runtime {cpp} -arch {32-bit} -support-app {lwip_echo_server}
platform generate -domains 
platform active {system_wrapper}
domain active {zynq_fsbl}
domain active {standalone_ps7_cortexa9_0}
platform generate -quick
platform generate
platform active {system_wrapper}
bsp reload
domain active {zynq_fsbl}
bsp reload
domain active {standalone_ps7_cortexa9_0}
bsp setlib -name libmetal -ver 2.5
bsp removelib -name libmetal
bsp write
bsp write
bsp config phy_link_speed "CONFIG_LINKSPEED1000"
bsp config phy_link_speed "CONFIG_LINKSPEED100"
bsp config n_rx_descriptors "64"
bsp write
bsp reload
catch {bsp regenerate}
platform generate -domains standalone_ps7_cortexa9_0 
platform active {system_wrapper}
