`timescale 1ns / 1ps

module top
	(
		//extender
		inout [16:0]	J45_aHalf,
		inout [16:0]	J45_bHalf,
		inout [16:0]	J46_aHalf,
		inout [16:0]	J46_bHalf,
		//PL 485
		output [0:0]	RS485_0_DE_tri_o,
		input 			RS485_0_rxd,
		output 			RS485_0_txd,
		output [0:0]	RS485_1_DE_tri_o,
		input 			RS485_1_rxd,
		output 			RS485_1_txd,
		//gpio
		input [0:0]		btns_tri_i,
		output [0:0]	leds_tri_o,
		//mipi
		inout 			cam_i2c_scl_io,
		inout 			cam_i2c_sda_io,		
		inout [0:0]		cam_gpio,		
		input 			mipi_phy_if_clk_n,
		input 			mipi_phy_if_clk_p,
		input [3:0]		mipi_phy_if_data_n,
		input [3:0]		mipi_phy_if_data_p,
		//pl phy	
		output 			mdio_mdc,
		inout 			mdio_mdio_io,
		output [0:0]	phy_reset_n,
		input [3:0]		rgmii_rd,
		input 			rgmii_rx_ctl,
		input 			rgmii_rxc,
		output [3:0]	rgmii_td,
		output 			rgmii_tx_ctl,
		output 			rgmii_txc,
		//ref clk	
		input 			sys_clk_clk_n,
		input 			sys_clk_clk_p,
		//pl uart	
		input 			uart_rxd,
		output 			uart_txd,
		//fan
		output			fan,		
		//pl ddr4
		output                             c0_ddr4_act_n   ,
		output [16:0]                      c0_ddr4_adr     ,
		output [1:0]                       c0_ddr4_ba      ,
		output [0:0]                       c0_ddr4_bg      ,
		output [0:0]                       c0_ddr4_cke     ,
		output [0:0]                       c0_ddr4_odt     ,
		output [0:0]                       c0_ddr4_cs_n    ,
		output [0:0]                       c0_ddr4_ck_t    ,
		output [0:0]                       c0_ddr4_ck_c    ,
		output                             c0_ddr4_reset_n ,
		inout [1:0]                        c0_ddr4_dm_dbi_n,
		inout [15:0]                       c0_ddr4_dq      ,
		inout [1:0]                        c0_ddr4_dqs_c   ,
		inout [1:0]                        c0_ddr4_dqs_t   

    );
	


//assign fan = 1'b1 ;   // 出厂原样：风扇钉死满速。改动见文件末尾。
	
wire 			clk_100m;
wire 	[0:0]	clk_100m_rstn;
wire	[31:0]	status_reg00_status;
wire	[31:0]	status_reg01_status;
wire	[31:0]	status_reg02_status;
wire	[31:0]	status_reg03_status;
wire	[31:0]	status_reg04_status;
wire	[31:0]	status_reg05_status;
wire	[31:0]	status_reg06_status;
wire	[31:0]	status_reg07_status;
wire	[31:0]	status_reg08_status;
wire	[31:0]	status_reg09_status;
wire	[31:0]	status_reg10_status;
wire	[31:0]	status_reg11_status;
wire	[31:0]	status_reg12_status;
wire	[31:0]	status_reg13_status;
wire	[31:0]	status_reg14_status;
wire	[31:0]	status_reg15_status;

	
wire	[31:0]	ddr4_status	;

	
wire 		clk_200m ;
wire		clk_200m_rst ;




ddr4_top pl_ddr4_test
(
	.c0_ddr4_act_n            (c0_ddr4_act_n   ),
	.c0_ddr4_adr              (c0_ddr4_adr     ),
	.c0_ddr4_ba               (c0_ddr4_ba      ),
	.c0_ddr4_bg               (c0_ddr4_bg      ),
	.c0_ddr4_cke              (c0_ddr4_cke     ),
	.c0_ddr4_odt              (c0_ddr4_odt     ),
	.c0_ddr4_cs_n             (c0_ddr4_cs_n    ),
	.c0_ddr4_ck_t             (c0_ddr4_ck_t    ),
	.c0_ddr4_ck_c             (c0_ddr4_ck_c    ),
	.c0_ddr4_reset_n          (c0_ddr4_reset_n ),
	.c0_ddr4_dm_dbi_n         (c0_ddr4_dm_dbi_n),
	.c0_ddr4_dq               (c0_ddr4_dq      ),
	.c0_ddr4_dqs_c            (c0_ddr4_dqs_c   ),
	.c0_ddr4_dqs_t            (c0_ddr4_dqs_t   ),

	.sys_clk_200MHz       	  (clk_200m),
	.sys_rst	          	  (clk_200m_rst	  	 ),
	.status               	  (ddr4_status        )
   );


  

	
	
design_1_wrapper	ps_block
   (
    .J45_aHalf                     (J45_aHalf          ),
    .J45_bHalf                     (J45_bHalf          ),
	.J46_aHalf                     (J46_aHalf			),
	.J46_bHalf                     (J46_bHalf			),
	.RS485_0_DE_tri_o              (RS485_0_DE_tri_o),
	.RS485_0_rxd                   (RS485_0_rxd     ),
	.RS485_0_txd                   (RS485_0_txd     ),
	.RS485_1_DE_tri_o              (RS485_1_DE_tri_o),
	.RS485_1_rxd                   (RS485_1_rxd     ),
	.RS485_1_txd                   (RS485_1_txd     ),
	
    .btns_tri_i                    (btns_tri_i         ),
	.leds_tri_o                    (leds_tri_o         ),
	
    .cam_i2c_scl_io                (cam_i2c_scl_io     ),
    .cam_i2c_sda_io                (cam_i2c_sda_io     ),
	.cam_gpio	           (cam_gpio),
	.mipi_phy_if_clk_n          (mipi_phy_if_clk_n ),
	.mipi_phy_if_clk_p          (mipi_phy_if_clk_p ),
	.mipi_phy_if_data_n         (mipi_phy_if_data_n),
	.mipi_phy_if_data_p         (mipi_phy_if_data_p),
    
    .mdio_mdc                      (mdio_mdc           ),
    .mdio_mdio_io                  (mdio_mdio_io       ),
    .phy_reset_n                   (phy_reset_n          ),
    .rgmii_rd                      (rgmii_rd           ),
    .rgmii_rx_ctl                  (rgmii_rx_ctl       ),
    .rgmii_rxc                     (rgmii_rxc          ),
    .rgmii_td                      (rgmii_td           ),
    .rgmii_tx_ctl                  (rgmii_tx_ctl       ),
    .rgmii_txc                     (rgmii_txc          ),
	.uart_rxd                      (uart_rxd),
	.uart_txd                      (uart_txd),

	
    .status_reg00_status           (ddr4_status	),
    .status_reg01_status           (status_reg01_status	),
    .status_reg02_status           (status_reg02_status	),
    .status_reg03_status           (status_reg03_status	),
    .status_reg04_status           (status_reg04_status),
    .status_reg05_status           (status_reg05_status),
    .status_reg06_status           (status_reg06_status),
    .status_reg07_status           (status_reg07_status),
    .control_reg00_control		   (control_reg00_control),
    .control_reg01_control		   (),
    .control_reg02_control		   (),
    .control_reg03_control		   (),
    .control_reg04_control		   (),
    .control_reg05_control		   (),
    .control_reg06_control		   (),
    .control_reg07_control		   (),
    .sys_clk_clk_p                 (sys_clk_clk_p      ),
    .sys_clk_clk_n                 (sys_clk_clk_n      ),
	.clk_200m_rst				   (clk_200m_rst		),
	.clk_200m				 	   (clk_200m		)
	);	
	

// ============================================================================
// 【风扇温控】—— 出厂 top.v 上唯一的改动
// ============================================================================
// 出厂原样是 `assign fan = 1'b1;`（已注释掉），也就是把风扇钉死在满速。
//
// 这一版换成按结温调速。RTL 与密码 bitstream 里用的是**同一份文件**
// （fpga/fan_ctrl/），那一份在板上带 AXI 观测口验证过温度→占空比确实跟随；
// 这里不再加观测口，是有意的：**这个 bitstream 是要进 BOOT.BIN 开机就跑的**，
// 改动越小越好，BD 一点都不动。要看温度，从 PS 侧的 AMS/hwmon 读即可，
// 那条路不需要动硬件。
//
// ⚠️ 例化放在**文件末尾**，在 clk_200m / clk_200m_rst 的 wire 声明之后。
//    出厂那一版把它写在文件开头、引用了后面才声明的名字 —— Vivado 对这种
//    写法**不报错**，而是新建一条同名的无驱动网络。同样的写法在密码顶层里
//    让板子挂死过两次、断电两次。位置不是风格问题。
//
// 时钟用 clk_200m：它是板载 200 MHz 差分晶振经 IBUF 出来的
// （BD 里 SIGNAME=util_ds_buf_0_IBUF_OUT），**PS 和 DDR 起不起来它都在**，
// 正合适给散热用。复位 clk_200m_rst 在 BD 里标的是 ACTIVE_LOW，直接当 rst_n。
//
// 参数按 200 MHz 折算：
//   PWM_PERIOD  = 200e6 / 25kHz = 8000
//   SYSMON 采样 = 200_000 拍 = 1 ms
//   STALE_LIMIT = 60_000_000 拍 ≈ 0.3 秒（计数器 26 位，上限 6.7e7，别超）
wire [15:0] fan_temp_code;
wire        fan_temp_valid;
wire        fan_sysmon_timeout;

// DCLK_DIV=40 → ADCCLK = 200 MHz/40 = 5.0 MHz，在 SYSMONE4 的上限之内。
// ⚠️ 这个数**必须跟着时钟走**：密码那一版是 75 MHz 用 16，这一版 200 MHz 用 40。
//    写小了 ADCCLK 超限，ADC 不转换，而 DRP 照样应答、寄存器里照样有个
//    看着合理的温度 —— 一点症状都没有（见 fan_sysmon.v 文件头）。
fan_sysmon #(.PERIOD(200_000), .DCLK_DIV(40)) u_fan_sysmon (
	.clk            (clk_200m),
	.rst_n          (clk_200m_rst),
	.temp_code      (fan_temp_code),
	.temp_valid     (fan_temp_valid),
	.sysmon_timeout (fan_sysmon_timeout),
	.dbg_req        (1'b0),          // 这一版没有观测口
	.dbg_addr       (8'd0),
	.dbg_data       (),
	.dbg_valid      (),
	.dbg_timeout    ());

fan_ctrl #(
	.PWM_PERIOD  (8000),
	.STALE_LIMIT (60_000_000),
	.STUCK_LIMIT (30_000)
) u_fan_ctrl (
	.clk         (clk_200m),
	.rst_n       (clk_200m_rst),
	.temp_code   (fan_temp_code),
	.temp_valid  (fan_temp_valid),
	.ovr_en      (1'b0),          // 没有 AXI 口，覆盖恒关
	.ovr_duty    (8'd0),
	.cur_temp    (),
	.cur_duty    (),
	.cur_step    (),
	.forced_full (),
	.sensor_stuck(),
	.fan_pin     (fan));


endmodule
