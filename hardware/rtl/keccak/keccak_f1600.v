// keccak_f1600 —— Keccak-f[1600] 置换核（单轮迭代版）
//
// 【为什么先做 Keccak】
// tools/amdahl.py 的结论：ML-KEM-768 里 SHAKE 占 ~55%、NTT 相关 ~30%。
// 单独做 NTT 端到端只有 1.37×，单独做 Keccak 有 2.09×。
// 所以若目标是端到端性能，Keccak 是第一顺位投资，NTT 之后就该做它。
//
// 【结构：单轮迭代，不是 24 轮全展开】
// 状态 25×64 = 1600 个触发器；组合逻辑只实现**一轮** θ→ρ→π→χ→ι，
// 用 round_cnt 走 24 轮。全展开会是 24 份轮逻辑，面积爆炸且没有必要 ——
// 24 cycle/置换 @100MHz = 240 ns，对 PS 侧的调用频度完全够。
//
// 接口与 ntt_core 同形（写状态 → start → 等 done → 读状态），
// 这样可以直接挂到 pqchsm/accel.h 的寄存器语义上。
// **done 是电平**：置位后保持到下一次 start 才清（理由见 ntt_core.v）。
//
// lane 编号：index = x + 5*y，与 hardware/model/ref_model.py 的 keccak_f1600 一致。
`default_nettype none

module keccak_f1600 (
    input  wire        clk,
    input  wire        rst_n,

    input  wire        start,
    output reg         done,

    // 状态写口（25 个 lane）
    input  wire        wr_en,
    input  wire [4:0]  wr_addr,
    input  wire [63:0] wr_data,

    // 状态读口（组合读）
    input  wire [4:0]  rd_addr,
    output wire [63:0] rd_data
);

    // ---- 状态 ----
    reg [63:0] A [0:24];
    reg [4:0]  round_cnt;
    reg        busy;

    assign rd_data = A[rd_addr];

    // ---- 轮常数 RC[0..23] ----
    function automatic [63:0] rc;
        input [4:0] r;
        begin
            case (r)
            5'd0:  rc = 64'h0000000000000001;  5'd1:  rc = 64'h0000000000008082;
            5'd2:  rc = 64'h800000000000808A;  5'd3:  rc = 64'h8000000080008000;
            5'd4:  rc = 64'h000000000000808B;  5'd5:  rc = 64'h0000000080000001;
            5'd6:  rc = 64'h8000000080008081;  5'd7:  rc = 64'h8000000000008009;
            5'd8:  rc = 64'h000000000000008A;  5'd9:  rc = 64'h0000000000000088;
            5'd10: rc = 64'h0000000080008009;  5'd11: rc = 64'h000000008000000A;
            5'd12: rc = 64'h000000008000808B;  5'd13: rc = 64'h800000000000008B;
            5'd14: rc = 64'h8000000000008089;  5'd15: rc = 64'h8000000000008003;
            5'd16: rc = 64'h8000000000008002;  5'd17: rc = 64'h8000000000000080;
            5'd18: rc = 64'h000000000000800A;  5'd19: rc = 64'h800000008000000A;
            5'd20: rc = 64'h8000000080008081;  5'd21: rc = 64'h8000000000008080;
            5'd22: rc = 64'h0000000080000001;  5'd23: rc = 64'h8000000080008008;
            default: rc = 64'd0;
            endcase
        end
    endfunction

    // 循环左移。rot == 0 要单独处理：Verilog 里 x << 64 的行为不是我们要的。
    function automatic [63:0] rotl;
        input [63:0] x;
        input integer n;
        begin
            rotl = (n == 0) ? x : ((x << n) | (x >> (64 - n)));
        end
    endfunction

    // ρ 的旋转量 r[x][y]，与 ref_model.py 的 R 表一致
    function automatic integer rho_off;
        input integer x;
        input integer y;
        begin
            case (x * 5 + y)
            0:  rho_off = 0;   1:  rho_off = 36;  2:  rho_off = 3;   3:  rho_off = 41;
            4:  rho_off = 18;  5:  rho_off = 1;   6:  rho_off = 44;  7:  rho_off = 10;
            8:  rho_off = 45;  9:  rho_off = 2;   10: rho_off = 62;  11: rho_off = 6;
            12: rho_off = 43;  13: rho_off = 15;  14: rho_off = 61;  15: rho_off = 28;
            16: rho_off = 55;  17: rho_off = 25;  18: rho_off = 21;  19: rho_off = 56;
            20: rho_off = 27;  21: rho_off = 20;  22: rho_off = 39;  23: rho_off = 8;
            default: rho_off = 14;
            endcase
        end
    endfunction

    // ---- 一轮的组合逻辑：θ → ρ → π → χ → ι ----
    reg [63:0] C     [0:4];
    reg [63:0] D     [0:4];
    reg [63:0] Ath   [0:24];   // θ 之后
    reg [63:0] B     [0:24];   // ρ+π 之后
    reg [63:0] Anext [0:24];   // χ+ι 之后

    integer x, y;
    always @(*) begin
        // θ
        for (x = 0; x < 5; x = x + 1) begin
            C[x] = A[x] ^ A[x + 5] ^ A[x + 10] ^ A[x + 15] ^ A[x + 20];
        end
        for (x = 0; x < 5; x = x + 1) begin
            D[x] = C[(x + 4) % 5] ^ rotl(C[(x + 1) % 5], 1);
        end
        for (x = 0; x < 5; x = x + 1) begin
            for (y = 0; y < 5; y = y + 1) begin
                Ath[x + 5 * y] = A[x + 5 * y] ^ D[x];
            end
        end
        // ρ + π：B[y][(2x+3y)%5] = rotl(Ath[x][y], r[x][y])
        for (x = 0; x < 5; x = x + 1) begin
            for (y = 0; y < 5; y = y + 1) begin
                B[y + 5 * ((2 * x + 3 * y) % 5)] = rotl(Ath[x + 5 * y], rho_off(x, y));
            end
        end
        // χ
        for (x = 0; x < 5; x = x + 1) begin
            for (y = 0; y < 5; y = y + 1) begin
                Anext[x + 5 * y] = B[x + 5 * y]
                                 ^ ((~B[((x + 1) % 5) + 5 * y]) & B[((x + 2) % 5) + 5 * y]);
            end
        end
        // ι
        Anext[0] = Anext[0] ^ rc(round_cnt);
    end

    // ---- 时序 ----
    integer i;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            done      <= 1'b0;
            busy      <= 1'b0;
            round_cnt <= 5'd0;
            for (i = 0; i < 25; i = i + 1) begin
                A[i] <= 64'd0;
            end
        end else begin
            if (!busy) begin
                // 空闲：可以写状态；done 保持到下一次 start
                if (wr_en) begin
                    A[wr_addr] <= wr_data;
                end
                if (start) begin
                    done      <= 1'b0;
                    busy      <= 1'b1;
                    round_cnt <= 5'd0;
                end
            end else begin
                for (i = 0; i < 25; i = i + 1) begin
                    A[i] <= Anext[i];
                end
                if (round_cnt == 5'd23) begin
                    busy <= 1'b0;
                    done <= 1'b1;
                end else begin
                    round_cnt <= round_cnt + 5'd1;
                end
            end
        end
    end
endmodule

`default_nettype wire
