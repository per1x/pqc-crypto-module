// sync_fifo —— 同步 FIFO（单时钟，first-word-fall-through，可擦除）
//
// 通用件，不属于任何一个算法核。TRNG 用它缓冲调理后的随机字，之后 SHA-3 /
// ML-KEM 的数据通路也会用到。
//
// FWFT（首字直通）：队首数据在 rd_valid 拉高的同一拍就出现在 rd_data 上，
// 不需要先发一次读请求再等一拍。理由是它直接对上 AXI4-Lite 的读时序 ——
// 软件读 RDATA 寄存器时，数据必须在同一次事务里返回。
//
// 【flush 是真擦除，不是只挪指针】
// 只把读写指针归零，存储单元里的旧数据还在 —— 对普通 FIFO 无所谓，对一个
// 装过随机数/密钥材料的 FIFO 就不一样了：后续的部分写入、扫描链、或者哪天
// 有人加了个调试读口，都可能把它捞出来。密码边界里的 zeroize 要求的是
// 数据真的没了。所以 flush 会启动一次逐地址覆零的扫描（DEPTH 个周期），
// 扫描期间读写口都关闭。
//
// WIPE_ON_FLUSH=0 可以退回成只挪指针的普通 FIFO，给非敏感数据通路用。
//
// DEPTH 必须是 2 的幂：指针回绕靠自然溢出，多一位区分空满。
`default_nettype none

module sync_fifo #(
    parameter integer WIDTH          = 32,
    parameter integer DEPTH          = 16,
    parameter integer WIPE_ON_FLUSH  = 1
) (
    input  wire             clk,
    input  wire             rst_n,
    input  wire             flush,          // 拉高一拍即启动擦除

    input  wire             wr_en,
    input  wire [WIDTH-1:0] wr_data,
    output wire             wr_ready,       // 未满且不在擦除中

    input  wire             rd_en,
    output wire [WIDTH-1:0] rd_data,
    output wire             rd_valid,       // 非空且不在擦除中

    output wire             wiping,
    output wire [$clog2(DEPTH):0] level
);
    localparam integer AW = $clog2(DEPTH);

    reg [WIDTH-1:0] mem [0:DEPTH-1];
    reg [AW:0]      wptr, rptr;             // 多一位用于区分空/满

    reg             wipe_run;
    reg [AW:0]      wipe_addr;

    wire full  = (wptr[AW] != rptr[AW]) && (wptr[AW-1:0] == rptr[AW-1:0]);
    wire empty = (wptr == rptr);

    assign wiping   = wipe_run;
    assign wr_ready = !full  && !wipe_run;
    assign rd_valid = !empty && !wipe_run;
    assign rd_data  = mem[rptr[AW-1:0]];
    assign level    = wptr - rptr;

    integer i;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            wptr      <= {(AW+1){1'b0}};
            rptr      <= {(AW+1){1'b0}};
            wipe_run  <= 1'b0;
            wipe_addr <= {(AW+1){1'b0}};
            for (i = 0; i < DEPTH; i = i + 1) begin
                mem[i] <= {WIDTH{1'b0}};
            end
        end else if (flush) begin
            wptr      <= {(AW+1){1'b0}};
            rptr      <= {(AW+1){1'b0}};
            wipe_addr <= {(AW+1){1'b0}};
            wipe_run  <= (WIPE_ON_FLUSH != 0);
        end else if (wipe_run) begin
            mem[wipe_addr[AW-1:0]] <= {WIDTH{1'b0}};
            if (wipe_addr == DEPTH[AW:0] - 1'b1) begin
                wipe_run <= 1'b0;
            end else begin
                wipe_addr <= wipe_addr + 1'b1;
            end
        end else begin
            if (wr_en && wr_ready) begin
                mem[wptr[AW-1:0]] <= wr_data;
                wptr <= wptr + 1'b1;
            end
            if (rd_en && rd_valid) begin
                rptr <= rptr + 1'b1;
            end
        end
    end

endmodule

`default_nettype wire
