// axi4lite_regs —— 加速器控制/状态寄存器的 AXI4-Lite 从机
//
// AXI 是 ARM 的公开标准，不是某一家 FPGA 厂商的接口，所以这一层同样保持厂商中立：
// 纯可推断逻辑，不例化任何厂商的 AXI 互连原语或 IP 封装。
//
// 【寄存器映射契约】与 include/pqchsm/accel.h 中软件侧的定义一一对应。
//
//   偏移  名字      软件权限  硬件行为
//   0x00  CTRL      W        [0] START      写 1 触发，**硬件自清**，读回恒为 0
//                            [1] SOFT_RESET 写 1 复位数据通路，硬件自清
//   0x04  STATUS    R        [0] DONE  电平锁存：完成时置位，保持到下一次 START
//                            [1] BUSY  运算进行中
//                            [2] ERR   本次命令出错，与 DONE 同时置位
//   0x08  MODE      RW       操作码
//   0x0C  PARAM     RW       参数集
//   0x10  IN_LEN    RW       输入字节数
//   0x14  OUT_LEN   R        输出字节数，由硬件回填
//   0x18  ERRCODE   R        出错时的细分原因，由硬件回填
//   0x1C  VERSION   R        常量，取值 VERSION 参数
//
// 几条约定值得单独说明，因为它们决定了软件轮询能否工作：
//
//   START 自清 —— 软件写 1 之后不需要再写 0 清除。若不自清，软件读改写 CTRL
//   的任何一次操作都会重新触发一次运算。
//
//   DONE 电平锁存 —— 而不是 1 周期脉冲。软件在任意时刻采样 STATUS 都必须能看到
//   完成信号；脉冲语义下轮询会漏掉。清除时机是下一次 START，不是读 STATUS，
//   这样"读一次 STATUS 就把状态弄没了"的竞态不存在。
//
//   STATUS / OUT_LEN / ERRCODE 只由硬件写 —— 软件对这三个地址的写被忽略并返回
//   OKAY（而不是 SLVERR）：AXI4-Lite 上对只读寄存器写入是良性操作，
//   返回错误反而会让不了解该约定的主机停在错误处理里。
//
//   未映射地址 —— 读返回 0、写被忽略，响应一律 OKAY。地址译码只看 [4:2]，
//   所以 0x20 以上会回绕到同一组寄存器；真实系统里由地址空间大小限制访问范围。
`default_nettype none

module axi4lite_regs #(
    parameter [31:0] VERSION = 32'h0001_0000
) (
    input  wire        clk,
    input  wire        rst_n,

    // ---- AXI4-Lite 从机 ----
    input  wire [7:0]  s_axi_awaddr,
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
    input  wire        s_axi_arvalid,
    output wire        s_axi_arready,

    output reg  [31:0] s_axi_rdata,
    output reg  [1:0]  s_axi_rresp,
    output reg         s_axi_rvalid,
    input  wire        s_axi_rready,

    // ---- 面向数据通路 ----
    output wire        start,          // 单周期脉冲
    output wire        soft_reset,     // 单周期脉冲
    output wire [31:0] mode,
    output wire [31:0] param,
    output wire [31:0] in_len,

    input  wire        done_set,       // 数据通路完成，置位 DONE
    input  wire        busy,
    input  wire        err_set,        // 与 done_set 同拍给出
    input  wire [31:0] out_len_in,
    input  wire [31:0] errcode_in,
    input  wire        out_we          // 回填 OUT_LEN / ERRCODE 的写使能
);
    localparam [2:0] A_CTRL    = 3'h0;
    localparam [2:0] A_STATUS  = 3'h1;
    localparam [2:0] A_MODE    = 3'h2;
    localparam [2:0] A_PARAM   = 3'h3;
    localparam [2:0] A_IN_LEN  = 3'h4;
    localparam [2:0] A_OUT_LEN = 3'h5;
    localparam [2:0] A_ERRCODE = 3'h6;
    localparam [2:0] A_VERSION = 3'h7;

    reg [31:0] reg_mode, reg_param, reg_in_len, reg_out_len, reg_errcode;
    reg        st_done, st_err;
    reg        start_r, soft_reset_r;

    assign mode       = reg_mode;
    assign param      = reg_param;
    assign in_len     = reg_in_len;
    assign start      = start_r;
    assign soft_reset = soft_reset_r;

    // ---- 写通道：AW 与 W 各自握手，两边都到齐才落笔 ----
    reg        aw_hold, w_hold;
    reg [7:0]  aw_addr;
    reg [31:0] w_data;
    reg [3:0]  w_strb;

    wire aw_go = s_axi_awvalid && !aw_hold;
    wire w_go  = s_axi_wvalid  && !w_hold;
    assign s_axi_awready = !aw_hold;
    assign s_axi_wready  = !w_hold;

    wire        wr_fire = (aw_hold || aw_go) && (w_hold || w_go) && !s_axi_bvalid;
    wire [7:0]  wr_addr = aw_hold ? aw_addr : s_axi_awaddr;
    wire [31:0] wr_data = w_hold  ? w_data  : s_axi_wdata;
    wire [3:0]  wr_strb = w_hold  ? w_strb  : s_axi_wstrb;

    // 按字节选通合成写入值
    function automatic [31:0] apply_strb;
        input [31:0] old_val;
        input [31:0] new_val;
        input [3:0]  strb;
        begin
            apply_strb = {strb[3] ? new_val[31:24] : old_val[31:24],
                          strb[2] ? new_val[23:16] : old_val[23:16],
                          strb[1] ? new_val[15:8]  : old_val[15:8],
                          strb[0] ? new_val[7:0]   : old_val[7:0]};
        end
    endfunction

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            aw_hold      <= 1'b0;
            w_hold       <= 1'b0;
            aw_addr      <= 8'd0;
            w_data       <= 32'd0;
            w_strb       <= 4'd0;
            s_axi_bvalid <= 1'b0;
            s_axi_bresp  <= 2'b00;
            reg_mode     <= 32'd0;
            reg_param    <= 32'd0;
            reg_in_len   <= 32'd0;
            start_r      <= 1'b0;
            soft_reset_r <= 1'b0;
        end else begin
            // START / SOFT_RESET 是单周期脉冲，硬件自清
            start_r      <= 1'b0;
            soft_reset_r <= 1'b0;

            if (aw_go && !wr_fire) begin
                aw_hold <= 1'b1;
                aw_addr <= s_axi_awaddr;
            end
            if (w_go && !wr_fire) begin
                w_hold <= 1'b1;
                w_data <= s_axi_wdata;
                w_strb <= s_axi_wstrb;
            end

            if (wr_fire) begin
                aw_hold      <= 1'b0;
                w_hold       <= 1'b0;
                s_axi_bvalid <= 1'b1;
                s_axi_bresp  <= 2'b00;          // 未映射与只读地址同样返回 OKAY
                case (wr_addr[4:2])
                A_CTRL: begin
                    start_r      <= wr_strb[0] && wr_data[0];
                    soft_reset_r <= wr_strb[0] && wr_data[1];
                end
                A_MODE:   reg_mode   <= apply_strb(reg_mode,   wr_data, wr_strb);
                A_PARAM:  reg_param  <= apply_strb(reg_param,  wr_data, wr_strb);
                A_IN_LEN: reg_in_len <= apply_strb(reg_in_len, wr_data, wr_strb);
                default: ;                      // STATUS / OUT_LEN / ERRCODE / VERSION 只读
                endcase
            end

            if (s_axi_bvalid && s_axi_bready) begin
                s_axi_bvalid <= 1'b0;
            end
        end
    end

    // ---- 状态位：DONE 电平锁存，START 时清 ----
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            st_done     <= 1'b0;
            st_err      <= 1'b0;
            reg_out_len <= 32'd0;
            reg_errcode <= 32'd0;
        end else if (start_r || soft_reset_r) begin
            st_done <= 1'b0;
            st_err  <= 1'b0;
        end else begin
            if (out_we) begin
                reg_out_len <= out_len_in;
                reg_errcode <= errcode_in;
            end
            if (done_set) begin
                st_done <= 1'b1;
                st_err  <= err_set;
            end
        end
    end

    // ---- 读通道 ----
    reg [7:0] ar_addr;
    reg       ar_hold;
    assign s_axi_arready = !ar_hold && !s_axi_rvalid;

    wire [31:0] rd_mux =
        (ar_addr[4:2] == A_STATUS)  ? {29'd0, st_err, busy, st_done} :
        (ar_addr[4:2] == A_MODE)    ? reg_mode :
        (ar_addr[4:2] == A_PARAM)   ? reg_param :
        (ar_addr[4:2] == A_IN_LEN)  ? reg_in_len :
        (ar_addr[4:2] == A_OUT_LEN) ? reg_out_len :
        (ar_addr[4:2] == A_ERRCODE) ? reg_errcode :
        (ar_addr[4:2] == A_VERSION) ? VERSION :
                                      32'd0;    // CTRL 只写，读回 0

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            ar_hold      <= 1'b0;
            ar_addr      <= 8'd0;
            s_axi_rvalid <= 1'b0;
            s_axi_rdata  <= 32'd0;
            s_axi_rresp  <= 2'b00;
        end else begin
            if (s_axi_arvalid && s_axi_arready) begin
                ar_addr <= s_axi_araddr;
                ar_hold <= 1'b1;
            end else if (ar_hold && !s_axi_rvalid) begin
                s_axi_rdata  <= rd_mux;
                s_axi_rresp  <= 2'b00;
                s_axi_rvalid <= 1'b1;
                ar_hold      <= 1'b0;
            end
            if (s_axi_rvalid && s_axi_rready) begin
                s_axi_rvalid <= 1'b0;
            end
        end
    end
endmodule

`default_nettype wire
