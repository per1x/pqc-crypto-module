// fan_ctrl_axi —— 风扇控制器的 AXI4-Lite 观测口
//
// ⚠️ **这是观测口，不是控制口。** 拔掉整条 AXI，风扇照样按温度自己调速 ——
//    散热不能依赖软件活着（理由见 fan_ctrl.v 的文件头）。
//
// 有这个口是为了**能证明温控真的在工作**：上板之后要看到"空载 25%、
// 加负载温度上去、占空比跟着上去"，没有一个能读温度和占空比的口，
// 就只能靠耳朵judge，那不算验证。
//
// 覆盖位（OVR）只用于调试，而且**盖不过过温强制**（fan_ctrl 里保证）。
//
// 【寄存器表】（偏移，32 位）
//   0x00 VERSION  R
//   0x04 STATUS   R  [15:0]=温度 ADC 码 [23:16]=占空比% [26:24]=档位
//                    [27]=强制满速 [28]=SYSMON 超时
//   0x08 TEMP_C   R  [15:0]=换算好的摄氏度×10（软件不用自己算）
//   0x0C OVR      RW [0]=覆盖使能 [15:8]=覆盖占空比 0..100
`default_nettype none

module fan_ctrl_axi #(
    parameter [31:0] VERSION = 32'h0001_0000
) (
    input  wire        clk,
    input  wire        rst_n,

    input  wire [7:0]  s_axi_awaddr,
    input  wire [2:0]  s_axi_awprot,
    input  wire        s_axi_awvalid,
    output wire        s_axi_awready,
    input  wire [31:0] s_axi_wdata,
    input  wire [3:0]  s_axi_wstrb,
    input  wire        s_axi_wvalid,
    output wire        s_axi_wready,
    output reg  [1:0]  s_axi_bresp,
    output reg         s_axi_bvalid,
    input  wire        s_axi_bready,

    input  wire [7:0]  s_axi_araddr,
    input  wire [2:0]  s_axi_arprot,
    input  wire        s_axi_arvalid,
    output wire        s_axi_arready,
    output reg  [31:0] s_axi_rdata,
    output reg  [1:0]  s_axi_rresp,
    output reg         s_axi_rvalid,
    input  wire        s_axi_rready,

    // 来自 fan_ctrl
    input  wire [15:0] cur_temp,
    input  wire [7:0]  cur_duty,
    input  wire [2:0]  cur_step,
    input  wire        forced_full,
    input  wire        sysmon_timeout,

    // 去往 fan_ctrl
    output reg         ovr_en,
    output reg  [7:0]  ovr_duty
);
    localparam [1:0] RESP_OKAY = 2'b00;

    // ---- 摄氏度×10 ----
    // T = code/65536 × 502.9098 − 273.8195，乘 10 之后：
    //   T×10 = code × 5029.098/65536 − 2738.195
    // 用整数近似：code × 5029 / 65536 ≈ code × 5029 >> 16，误差 < 0.02°C，
    // 对"给人看的温度"足够。软件因此不用自己搬那个公式。
    wire [31:0] scaled = (cur_temp * 32'd5029) >> 16;
    wire signed [31:0] tc10 = $signed(scaled) - 32'sd2738;
    wire [15:0] temp_c10 = (tc10 < 0) ? 16'd0 : tc10[15:0];

    // ================= 写 =================
    reg aw_got, w_got;
    reg [7:0]  aw_addr_r;
    reg [31:0] w_data_r;

    assign s_axi_awready = !aw_got && !s_axi_bvalid;
    assign s_axi_wready  = !w_got  && !s_axi_bvalid;

    wire wr_now = (aw_got || (s_axi_awvalid && s_axi_awready))
                  && (w_got || (s_axi_wvalid && s_axi_wready)) && !s_axi_bvalid;
    wire [7:0]  wr_addr = (s_axi_awvalid && s_axi_awready) ? s_axi_awaddr : aw_addr_r;
    wire [31:0] wr_data = (s_axi_wvalid && s_axi_wready) ? s_axi_wdata : w_data_r;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            aw_got <= 1'b0; w_got <= 1'b0; aw_addr_r <= 8'd0; w_data_r <= 32'd0;
            s_axi_bvalid <= 1'b0; s_axi_bresp <= RESP_OKAY;
            ovr_en <= 1'b0; ovr_duty <= 8'd0;
        end else begin
            if (s_axi_awvalid && s_axi_awready) begin
                aw_got <= 1'b1; aw_addr_r <= s_axi_awaddr;
            end
            if (s_axi_wvalid && s_axi_wready) begin
                w_got <= 1'b1; w_data_r <= s_axi_wdata;
            end
            if (wr_now) begin
                aw_got <= 1'b0; w_got <= 1'b0;
                s_axi_bvalid <= 1'b1; s_axi_bresp <= RESP_OKAY;
                if (wr_addr[5:2] == 4'h3) begin      // 0x0C OVR
                    ovr_en   <= wr_data[0];
                    ovr_duty <= wr_data[15:8];
                end
            end
            if (s_axi_bvalid && s_axi_bready) s_axi_bvalid <= 1'b0;
        end
    end

    // ================= 读 =================
    assign s_axi_arready = !s_axi_rvalid;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            s_axi_rvalid <= 1'b0; s_axi_rresp <= RESP_OKAY; s_axi_rdata <= 32'd0;
        end else begin
            if (s_axi_arvalid && s_axi_arready) begin
                s_axi_rvalid <= 1'b1;
                s_axi_rresp  <= RESP_OKAY;
                case (s_axi_araddr[5:2])
                4'h0: s_axi_rdata <= VERSION;
                4'h1: s_axi_rdata <= {3'd0, sysmon_timeout, forced_full,
                                      cur_step, cur_duty, cur_temp};
                4'h2: s_axi_rdata <= {16'd0, temp_c10};
                4'h3: s_axi_rdata <= {16'd0, ovr_duty, 7'd0, ovr_en};
                default: s_axi_rdata <= 32'd0;
                endcase
            end
            if (s_axi_rvalid && s_axi_rready) s_axi_rvalid <= 1'b0;
        end
    end

    // 地址只译 [5:2]（16 个 32 位寄存器），高位与字节内偏移按设计忽略；
    // OVR 只用到 wdata 的 [0] 与 [15:8]。这些"有意不用"要显式吸收掉，
    // 否则 lint 报 UNUSEDSIGNAL —— 而那条告警在别处是能抓到真漏接的。
    wire _unused = &{1'b0, s_axi_awprot, s_axi_arprot, s_axi_wstrb,
                     s_axi_araddr[7:6], s_axi_araddr[1:0],
                     wr_addr[7:6], wr_addr[1:0],
                     wr_data[31:16], wr_data[7:1], 1'b0};

endmodule

`default_nettype wire
