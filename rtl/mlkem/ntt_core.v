// ntt_core —— ML-KEM 的 256 点 NTT / INTT 核（首版：1 蝶形/周期）
//
// 这是一版**行为级但结构清晰**的实现：每周期做一个蝶形，系数放在一块
// 256×16 的寄存器阵列里。按 tools/cycle_budget.py 的表，1 蝶形并行约
// 910 cycles @150MHz ≈ 6.1 µs —— 与路线图 §5.8.1 的算例一致。
// 提高并行度（4/8 蝶形）需要把系数拆成多个 bank，那是下一步的事。
//
// 接口刻意做成"写系数 → start → 等 done → 读系数"，与 pqchsm/accel.h 的
// 寄存器语义对得上，这样 Verilator 仿真出来的核可以直接挂到 accel transport 上。
//
// ⚠️ ML-KEM 的 NTT 只做 **7 层**（到 2 次多项式为止，不是完整 8 层）。
// 逆变换最后要乘 f = mont^2/128 = 1441，所以 invntt(ntt(x)) ≡ x·2^16 (mod q)，
// **不是恒等** —— 这一条最容易被当成 bug，见 model/ref_model.py 的说明。
`default_nettype none

module ntt_core (
    input  wire               clk,
    input  wire               rst_n,

    input  wire               start,      // 单周期脉冲
    input  wire               inverse,    // 0 = 正变换，1 = 逆变换
    output reg                done,

    // 系数写口（done 之后 / start 之前用）
    input  wire               wr_en,
    input  wire  [7:0]        wr_addr,
    input  wire signed [15:0] wr_data,

    // 系数读口（组合读）
    input  wire  [7:0]        rd_addr,
    output wire signed [15:0] rd_data
);

    localparam signed [15:0] Q    =  16'sd3329;
    localparam signed [15:0] QINV = -16'sd3327;
    localparam signed [15:0] BARV =  16'sd20159;   // ((1<<26)+q/2)/q
    localparam signed [15:0] FINV =  16'sd1441;    // mont^2/128

    // ---- zeta ROM（Montgomery 域，与 pq-crystals 的表一致）----
    reg signed [15:0] zetas [0:127];
    initial begin
        zetas[0]=-16'sd1044; zetas[1]=-16'sd758;  zetas[2]=-16'sd359;  zetas[3]=-16'sd1517;
        zetas[4]= 16'sd1493; zetas[5]= 16'sd1422; zetas[6]= 16'sd287;  zetas[7]= 16'sd202;
        zetas[8]=-16'sd171;  zetas[9]= 16'sd622;  zetas[10]=16'sd1577; zetas[11]=16'sd182;
        zetas[12]=16'sd962;  zetas[13]=-16'sd1202;zetas[14]=-16'sd1474;zetas[15]=16'sd1468;
        zetas[16]=16'sd573;  zetas[17]=-16'sd1325;zetas[18]=16'sd264;  zetas[19]=16'sd383;
        zetas[20]=-16'sd829; zetas[21]=16'sd1458; zetas[22]=-16'sd1602;zetas[23]=-16'sd130;
        zetas[24]=-16'sd681; zetas[25]=16'sd1017; zetas[26]=16'sd732;  zetas[27]=16'sd608;
        zetas[28]=-16'sd1542;zetas[29]=16'sd411;  zetas[30]=-16'sd205; zetas[31]=-16'sd1571;
        zetas[32]=16'sd1223; zetas[33]=16'sd652;  zetas[34]=-16'sd552; zetas[35]=16'sd1015;
        zetas[36]=-16'sd1293;zetas[37]=16'sd1491; zetas[38]=-16'sd282; zetas[39]=-16'sd1544;
        zetas[40]=16'sd516;  zetas[41]=-16'sd8;   zetas[42]=-16'sd320; zetas[43]=-16'sd666;
        zetas[44]=-16'sd1618;zetas[45]=-16'sd1162;zetas[46]=16'sd126;  zetas[47]=16'sd1469;
        zetas[48]=-16'sd853; zetas[49]=-16'sd90;  zetas[50]=-16'sd271; zetas[51]=16'sd830;
        zetas[52]=16'sd107;  zetas[53]=-16'sd1421;zetas[54]=-16'sd247; zetas[55]=-16'sd951;
        zetas[56]=-16'sd398; zetas[57]=16'sd961;  zetas[58]=-16'sd1508;zetas[59]=-16'sd725;
        zetas[60]=16'sd448;  zetas[61]=-16'sd1065;zetas[62]=16'sd677;  zetas[63]=-16'sd1275;
        zetas[64]=-16'sd1103;zetas[65]=16'sd430;  zetas[66]=16'sd555;  zetas[67]=16'sd843;
        zetas[68]=-16'sd1251;zetas[69]=16'sd871;  zetas[70]=16'sd1550; zetas[71]=16'sd105;
        zetas[72]=16'sd422;  zetas[73]=16'sd587;  zetas[74]=16'sd177;  zetas[75]=-16'sd235;
        zetas[76]=-16'sd291; zetas[77]=-16'sd460; zetas[78]=16'sd1574; zetas[79]=16'sd1653;
        zetas[80]=-16'sd246; zetas[81]=16'sd778;  zetas[82]=16'sd1159; zetas[83]=-16'sd147;
        zetas[84]=-16'sd777; zetas[85]=16'sd1483; zetas[86]=-16'sd602; zetas[87]=16'sd1119;
        zetas[88]=-16'sd1590;zetas[89]=16'sd644;  zetas[90]=-16'sd872; zetas[91]=16'sd349;
        zetas[92]=16'sd418;  zetas[93]=16'sd329;  zetas[94]=-16'sd156; zetas[95]=-16'sd75;
        zetas[96]=16'sd817;  zetas[97]=16'sd1097; zetas[98]=16'sd603;  zetas[99]=16'sd610;
        zetas[100]=16'sd1322;zetas[101]=-16'sd1285;zetas[102]=-16'sd1465;zetas[103]=16'sd384;
        zetas[104]=-16'sd1215;zetas[105]=-16'sd136;zetas[106]=16'sd1218;zetas[107]=-16'sd1335;
        zetas[108]=-16'sd874;zetas[109]=16'sd220; zetas[110]=-16'sd1187;zetas[111]=-16'sd1659;
        zetas[112]=-16'sd1185;zetas[113]=-16'sd1530;zetas[114]=-16'sd1278;zetas[115]=16'sd794;
        zetas[116]=-16'sd1510;zetas[117]=-16'sd854;zetas[118]=-16'sd870;zetas[119]=16'sd478;
        zetas[120]=-16'sd108;zetas[121]=-16'sd308;zetas[122]=16'sd996; zetas[123]=16'sd991;
        zetas[124]=16'sd958; zetas[125]=-16'sd1460;zetas[126]=16'sd1522;zetas[127]=16'sd1628;
    end

    // ---- 系数存储 ----
    reg signed [15:0] mem [0:255];
    assign rd_data = mem[rd_addr];

    // ---- 组合算子 ----
    // 显式写出每一步的位宽：Verilator 是 2-state，隐式截断在这里会真的算错，
    // 而 Icarus 的 4-state 可能"碰巧"对上 —— 两个仿真器都要跑就是为了逼出这类问题。
    function automatic signed [15:0] mont;
        input signed [31:0] a;
        reg signed [15:0] m;
        reg signed [31:0] prod;
        reg signed [31:0] diff;
        begin
            m    = $signed(a[15:0]) * QINV;      // 低 16 位相乘后截断（对应 C 的 (int16_t) 赋值）
            prod = $signed({{16{m[15]}}, m}) * $signed({{16{Q[15]}}, Q});
            diff = a - prod;
            mont = diff[31:16];                  // 算术右移 16 == 取高 16 位
        end
    endfunction

    function automatic signed [15:0] barr;
        input signed [15:0] a;
        reg signed [31:0] t;
        reg signed [15:0] t16;
        begin
            t    = ($signed({{16{BARV[15]}}, BARV}) * $signed({{16{a[15]}}, a})
                    + 32'sd33554432) >>> 26;
            t16  = t[15:0];
            barr = a - t16 * Q;
        end
    endfunction

    // ---- 状态机 ----
    localparam S_IDLE = 3'd0, S_RUN = 3'd1, S_SCALE = 3'd2, S_DONE = 3'd3;

    reg [2:0] state;
    reg [8:0] len;        // 128..2（正）或 2..128（逆）
    reg [8:0] grp;        // 当前组的起始下标
    reg [8:0] j;          // 组内偏移
    reg [7:0] k;          // zeta 下标
    reg       inv_r;
    reg [8:0] scale_i;

    wire [8:0] j_hi = j + len;
    wire signed [15:0] a_val = mem[j[7:0]];
    wire signed [15:0] b_val = mem[j_hi[7:0]];
    wire signed [15:0] zeta  = zetas[k[6:0]];

    // CT（正）与 GS（逆）两种蝶形
    wire signed [31:0] ct_prod = $signed({{16{zeta[15]}}, zeta}) * $signed({{16{b_val[15]}}, b_val});
    wire signed [15:0] ct_t  = mont(ct_prod);
    wire signed [15:0] ct_a  = a_val + ct_t;
    wire signed [15:0] ct_b  = a_val - ct_t;
    wire signed [15:0] gs_a  = barr(a_val + b_val);
    wire signed [15:0] gs_diff = b_val - a_val;
    wire signed [31:0] gs_prod = $signed({{16{zeta[15]}}, zeta}) * $signed({{16{gs_diff[15]}}, gs_diff});
    wire signed [15:0] gs_b  = mont(gs_prod);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= S_IDLE;
            done  <= 1'b0;
        end else begin
            case (state)
            S_IDLE: begin
                done <= 1'b0;
                if (wr_en) begin
                    mem[wr_addr] <= wr_data;
                end
                if (start) begin
                    inv_r <= inverse;
                    len   <= inverse ? 9'd2 : 9'd128;
                    grp   <= 9'd0;
                    j     <= 9'd0;
                    k     <= inverse ? 8'd127 : 8'd1;
                    state <= S_RUN;
                end
            end

            S_RUN: begin
                // 一个周期一个蝶形
                if (inv_r) begin
                    mem[j[7:0]]    <= gs_a;
                    mem[j_hi[7:0]] <= gs_b;
                end else begin
                    mem[j[7:0]]    <= ct_a;
                    mem[j_hi[7:0]] <= ct_b;
                end

                if (j + 1 < grp + len) begin
                    j <= j + 1;
                end else begin
                    // 一组做完，换 zeta
                    k <= inv_r ? (k - 8'd1) : (k + 8'd1);
                    if (grp + 2 * len < 9'd256) begin
                        grp <= grp + 2 * len;
                        j   <= grp + 2 * len;
                    end else begin
                        // 一层做完
                        if (inv_r) begin
                            if (len == 9'd128) begin
                                scale_i <= 9'd0;
                                state   <= S_SCALE;
                            end else begin
                                len <= len << 1;
                                grp <= 9'd0;
                                j   <= 9'd0;
                            end
                        end else begin
                            if (len == 9'd2) begin
                                scale_i <= 9'd0;
                                state   <= S_SCALE;
                            end else begin
                                len <= len >> 1;
                                grp <= 9'd0;
                                j   <= 9'd0;
                            end
                        end
                    end
                end
            end

            S_SCALE: begin
                // 正变换：最后统一 Barrett 归约；逆变换：乘 f
                mem[scale_i[7:0]] <= inv_r
                    ? mont($signed({{16{FINV[15]}}, FINV})
                           * $signed({{16{mem[scale_i[7:0]][15]}}, mem[scale_i[7:0]]}))
                    : barr(mem[scale_i[7:0]]);
                if (scale_i == 9'd255) begin
                    state <= S_DONE;
                end else begin
                    scale_i <= scale_i + 1;
                end
            end

            S_DONE: begin
                done  <= 1'b1;
                state <= S_IDLE;
            end

            default: state <= S_IDLE;
            endcase
        end
    end
endmodule

`default_nettype wire
