// sysmon_drp —— 周期性地从 DRP 口读温度，外加一个软件可用的任意地址读窗口
//
// 和 SYSMONE4 原语分开，是为了让**这段时序逻辑能进 cocotb 回归**。
// 厂商原语只能在 Vivado 里仿，一旦把 FSM 和原语写在一个文件里，
// 这段"什么时候拉 DEN、DRDY 没来怎么办"的逻辑就永远没有对拍覆盖 ——
// 而它恰恰是最容易写错的部分（DRP 的 DRDY 可能永远不来）。
//
// 所以：本模块是纯 RTL、可仿真；fan_sysmon.v 只负责把它和 SYSMONE4 接起来。
//
// ============================================================================
// 【DRDY 不来怎么办】
// ============================================================================
// 超时之后**不重试到死**，而是回到空闲、等下一个采样周期再试，并且**不拉
// valid**。上层（fan_ctrl）看到 valid 长时间不来会强制风扇满速 —— 温度
// 读不到时唯一安全的假设是"可能很热"。
//
// ============================================================================
// 【为什么加这个 dbg 读窗口 —— 一次真机教训】
// ============================================================================
// 第一版只会读 0x00 这一个地址。上板之后温度码读出来是 39924（换算 32.5°C），
// 数值**看着完全合理**，风扇也老老实实停在最低档 —— 但那个数在五分钟、
// 六十次采样里**一个比特都没变过**，而同期 PS 侧 AMS 报的结温在 24.9~29.1°C
// 之间正常抖动。也就是说：DRP 有应答、寄存器有值、而 ADC 根本没在转换。
//
// 当时没有任何办法从软件侧看 SYSMON 的配置与状态寄存器，只能靠猜，
// 而猜一次要重新出一版 bitstream（三十多分钟）。所以补上这个窗口：
// 软件写一个 DRP 地址就能把 0x3F（标志）、0x40/41/42（配置）、
// 0x20/0x24（最高/最低温记录）读回来 —— 最后那两个尤其关键，
// 它们要是还停在复位值，就说明 ADC 一次都没转换过。
//
// 读窗口**不影响温度那条路**：dbg 读的结果只进 dbg_data，绝不拉 value_valid。
// 否则调试的人一读寄存器，风扇就拿到一个假温度。
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
    output reg  [7:0]  daddr,
    output wire [15:0] di,
    output wire        dwe,
    input  wire [15:0] dout,
    input  wire        drdy,

    output reg  [15:0] value,
    output reg         value_valid,   // 单拍脉冲
    output reg         timed_out,     // 上一次周期读超时了（观测用）

    // ---- 软件调试读窗口 ----
    input  wire        dbg_req,       // 单拍脉冲：请求读 dbg_addr
    input  wire [7:0]  dbg_addr,
    output reg  [15:0] dbg_data,
    output reg         dbg_valid,     // 电平：dbg_data 有效（新请求时清零）
    output reg         dbg_timeout
);
    assign di  = 16'd0;
    assign dwe = 1'b0;                // 只读

    // 只有两个状态：等到下一个采样点、以及等 DRDY。
    localparam [1:0] S_WAIT = 2'd0, S_REQ = 2'd1;

    reg [1:0]  state;
    reg [31:0] tick;
    reg [15:0] tmo;
    reg        is_dbg;     // 在途的这一笔是调试读还是温度读
    reg        dbg_pend;   // 有调试读在排队

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= S_WAIT; tick <= 32'd0; tmo <= 16'd0;
            den <= 1'b0; daddr <= ADDR;
            value <= 16'd0; value_valid <= 1'b0; timed_out <= 1'b0;
            is_dbg <= 1'b0; dbg_pend <= 1'b0;
            dbg_data <= 16'd0; dbg_valid <= 1'b0; dbg_timeout <= 1'b0;
        end else begin
            value_valid <= 1'b0;
            den         <= 1'b0;

            // 调试请求随时可以进来，排队等 FSM 空闲
            if (dbg_req) begin
                dbg_pend    <= 1'b1;
                dbg_valid   <= 1'b0;   // 新请求，旧数据立刻作废
                dbg_timeout <= 1'b0;
            end

            case (state)
            S_WAIT: begin
                // 温度读优先：它关系到散热；调试读晚一个采样周期无所谓
                if (tick + 32'd1 >= PERIOD) begin
                    tick   <= 32'd0;
                    daddr  <= ADDR;
                    den    <= 1'b1;        // DEN 只拉一拍
                    tmo    <= 16'd0;
                    is_dbg <= 1'b0;
                    state  <= S_REQ;
                end else begin
                    tick <= tick + 32'd1;
                    if (dbg_pend) begin
                        dbg_pend <= 1'b0;
                        daddr    <= dbg_addr;
                        den      <= 1'b1;
                        tmo      <= 16'd0;
                        is_dbg   <= 1'b1;
                        state    <= S_REQ;
                    end
                end
            end

            // DEN 已经在上一拍拉过了，这里开始等 DRDY
            S_REQ: begin
                if (drdy) begin
                    if (is_dbg) begin
                        // ⚠️ 调试读**绝不**拉 value_valid：
                        //    否则一读寄存器就等于给风扇喂一个假温度。
                        dbg_data  <= dout;
                        dbg_valid <= 1'b1;
                    end else begin
                        value       <= dout;
                        value_valid <= 1'b1;
                        timed_out   <= 1'b0;
                    end
                    state <= S_WAIT;
                end else if (tmo + 16'd1 >= TIMEOUT[15:0]) begin
                    // 不重试到死：回去等下一个周期，且**不拉 valid**
                    if (is_dbg) dbg_timeout <= 1'b1;
                    else        timed_out   <= 1'b1;
                    state <= S_WAIT;
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
