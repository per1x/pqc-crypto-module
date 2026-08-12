// sysmon_drp —— 周期性地从 DRP 口读一个寄存器
//
// 和 SYSMONE4 原语分开，是为了让**这段时序逻辑能进 cocotb 回归**。
// 厂商原语只能在 Vivado 里仿，一旦把 FSM 和原语写在一个文件里，
// 这段"什么时候拉 DEN、DRDY 没来怎么办"的逻辑就永远没有对拍覆盖 ——
// 而它恰恰是最容易写错的部分（DRP 的 DRDY 可能永远不来）。
//
// 所以：本模块是纯 RTL、可仿真；fan_sysmon.v 只负责把它和 SYSMONE4 接起来。
//
// 【DRDY 不来怎么办】
// 超时之后**不重试到死**，而是回到空闲、等下一个采样周期再试，并且**不拉
// valid**。上层（fan_ctrl）看到 valid 长时间不来会强制风扇满速 —— 温度
// 读不到时唯一安全的假设是"可能很热"。
`default_nettype none

module sysmon_drp #(
    parameter integer PERIOD  = 75_000,   // 采样间隔（75 MHz 下 1 ms）
    parameter integer TIMEOUT = 1024,     // 等 DRDY 的上限
    parameter [7:0]   ADDR    = 8'h00     // 0x00 = 片上温度
) (
    input  wire        clk,
    input  wire        rst_n,

    // DRP
    output reg         den,
    output wire [7:0]  daddr,
    output wire [15:0] di,
    output wire        dwe,
    input  wire [15:0] dout,
    input  wire        drdy,

    output reg  [15:0] value,
    output reg         value_valid,   // 单拍脉冲
    output reg         timed_out      // 上一次读超时了（观测用）
);
    assign daddr = ADDR;
    assign di    = 16'd0;
    assign dwe   = 1'b0;              // 只读

    // 只有两个状态：等到下一个采样点、以及等 DRDY。
    localparam [1:0] S_WAIT = 2'd0, S_REQ = 2'd1;

    reg [1:0]  state;
    reg [31:0] tick;
    reg [15:0] tmo;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= S_WAIT; tick <= 32'd0; tmo <= 16'd0;
            den <= 1'b0; value <= 16'd0; value_valid <= 1'b0; timed_out <= 1'b0;
        end else begin
            value_valid <= 1'b0;
            den         <= 1'b0;

            case (state)
            S_WAIT: begin
                if (tick + 32'd1 >= PERIOD) begin
                    tick  <= 32'd0;
                    den   <= 1'b1;        // DEN 只拉一拍
                    tmo   <= 16'd0;
                    state <= S_REQ;
                end else begin
                    tick <= tick + 32'd1;
                end
            end

            // DEN 已经在上一拍拉过了，这里开始等 DRDY
            S_REQ: begin
                if (drdy) begin
                    value       <= dout;
                    value_valid <= 1'b1;
                    timed_out   <= 1'b0;
                    state       <= S_WAIT;
                end else if (tmo + 16'd1 >= TIMEOUT[15:0]) begin
                    // 不重试到死：回去等下一个周期，且**不拉 valid**
                    timed_out <= 1'b1;
                    state     <= S_WAIT;
                end else begin
                    tmo <= tmo + 16'd1;
                end
            end

            default: state <= S_WAIT;
            endcase
        end
    end
endmodule

`default_nettype wire
